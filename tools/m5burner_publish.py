#!/usr/bin/env python3
"""Upload + publish one dist image as a new version of the M5Burner entry.

Adapted from bmorcelli/Launcher's support_files/m5burner_post.py (MIT) and cut
down from "a table of 12 boards" to "this repo's single firmware entry".
Upstream: https://github.com/bmorcelli/Launcher

API flow (all of it undocumented, reverse-engineered by the Launcher project):
  1. POST https://uiflow2.m5stack.com/api/v1/account/login  {email, password}
     -> sets cookie `m5_auth_token`, which is then also sent as a *header* on
        the m5burner-api host (different domain, the cookie won't ride along).
  2. GET  {api}/api/admin/firmware          -> your own entries, each with
        `fid`, the display `name`, and the `versions` list.
  3. POST {api}/api/admin/firmware          -> multipart upload of a new
        version (the other form fields have to echo the existing entry's
        metadata, or the entry gets rewritten with blanks).
  4. PUT  {api}/api/admin/firmware/{fid}/publish/{file}/1   -> publish it.
     Note `{file}` is the *server-assigned* filename from the version entry
     (e.g. "df8f...c0d.bin"), not the version string.

WHICH ARTIFACT: the merged image, `dist/adarkroom-<version>-merged.bin`.
  - M5Burner (desktop) writes an entry's .bin to flash at 0x0 as a whole-chip
    image, so it needs bootloader + partition table + boot_app0 + app already
    stitched together. That is exactly what tools/make_dist.py emits as
    `-merged.bin`, and it is what the manually-created 0.14.0 version on the
    entry already is (CDN content-length 1446608 == our merged.bin, byte for
    byte 0x10000 larger than the app-only launcher.bin).
  - bmorcelli/Launcher's OTA installer consumes the *same* merged image: it
    does an HTTP Range request into the M5Burner CDN copy starting at the
    app's source offset (0x10000 for a merged image) and writes only that
    slice into a Launcher-managed OTA slot (see installFirmwareDynamic ->
    flashRawRangeFromHttp in Launcher's src/onlineLauncher.cpp). Launcher
    itself ships merged images to M5Burner for the same reason (its
    support_files/merge.py runs `esptool merge-bin` on bootloader/partitions/
    app before upload). So merged serves both consumers.
  - `dist/adarkroom-<version>-launcher.bin` (app-only) is for SD-card
    sideloading into Launcher and must NOT be uploaded here: M5Burner would
    flash a bare app image at 0x0 and brick the board. This script refuses a
    bin without a partition-table magic at 0x8000 for that reason.

Credentials come from the environment, never from the repo:
    M5BURNER_USER / M5BURNER_PWD   (or --user / --password to override)

Usage:
    export M5BURNER_USER=... M5BURNER_PWD=...
    python tools/m5burner_publish.py --tag v0.15.0 --dry-run   # verify first
    python tools/m5burner_publish.py --tag v0.15.0

Requirements: Python 3.8+, `pip install requests`.
"""
from __future__ import annotations
import argparse
import json
import os
import re
import sys
from pathlib import Path

try:
    import requests
except ImportError:
    print("[deps] this script needs `requests`: pip install requests")
    sys.exit(2)

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_NAME = "PaperDarkRoom"
DEFAULT_API = "https://m5burner-api.m5stack.com"
LOGIN_URL = "https://uiflow2.m5stack.com/api/v1/account/login"

# ESP32 partition-table entry magic, at the fixed 0x8000 flash offset. Present
# in a merged image, absent in an app-only one -- see the WHICH ARTIFACT note.
PART_TABLE_OFFSET = 0x8000
PART_TABLE_MAGIC = b"\xaa\x50"


def read_version(ini_path: Path) -> str:
    """Same -DCARD_VERSION probe tools/make_dist.py uses."""
    text = ini_path.read_text(encoding="utf-8")
    m = re.search(r'CARD_VERSION="\\"([0-9][0-9.]*)-adarkroom\\""', text)
    if not m:
        print(f"[version] couldn't find -DCARD_VERSION in {ini_path}")
        sys.exit(2)
    return m.group(1)


def check_merged_image(path: Path) -> None:
    data = path.read_bytes()[: PART_TABLE_OFFSET + len(PART_TABLE_MAGIC)]
    if not data or data[0] != 0xE9:
        print(f"[bin] {path} is not an ESP32 image (no 0xE9 magic at offset 0)")
        sys.exit(2)
    if data[PART_TABLE_OFFSET : PART_TABLE_OFFSET + 2] != PART_TABLE_MAGIC:
        print(f"[bin] {path} has no partition table at {PART_TABLE_OFFSET:#x} — "
              "this looks like the app-only launcher.bin. M5Burner flashes an "
              "entry's image at 0x0, so it must be the *merged* one "
              "(dist/adarkroom-<version>-merged.bin).")
        sys.exit(2)


def _format_headers(headers) -> str:
    return "\n".join(f"  {k}: {v}" for k, v in headers.items())


def _safe_body(response) -> str:
    if "application/json" in response.headers.get("Content-Type", ""):
        try:
            return json.dumps(response.json(), indent=2)
        except Exception:
            pass
    try:
        text = response.text
        return text if len(text) <= 4096 else text[:4096] + "\n... (truncated)"
    except Exception:
        return f"<binary {len(response.content)} bytes>"


class RequestLogger:
    """Logs every request/response pair, with credentials redacted."""

    def __init__(self, output_path):
        self._file = open(output_path, "w", encoding="utf-8")
        self._counter = 0

    def attach(self, session):
        session.hooks["response"].append(self._on_response)

    def _on_response(self, response, *args, **kwargs):
        self._counter += 1
        req = response.request

        safe_req_headers = dict(req.headers)
        for h in list(safe_req_headers):
            if h.lower() in ("authorization", "m5_auth_token", "cookie"):
                safe_req_headers[h] = "***"

        safe_resp_headers = dict(response.headers)
        for h in list(safe_resp_headers):
            if h.lower() == "set-cookie":
                safe_resp_headers[h] = "***"

        req_body = ""
        if req.body:
            if req.url.startswith(LOGIN_URL):
                # The login payload carries the password in clear text.
                req_body = '{"email": "***", "password": "***"}'
            elif isinstance(req.body, bytes):
                req_body = f"<binary {len(req.body)} bytes>"
            else:
                body = req.body
                req_body = body if len(body) <= 4096 else body[:4096] + "\n... (truncated)"

        self._file.write(
            f"\n{'='*72}\n"
            f"[{self._counter}] {req.method} {req.url}\n"
            f"--- REQUEST HEADERS ---\n{_format_headers(safe_req_headers)}\n"
            f"--- REQUEST BODY ---\n{req_body or '(empty)'}\n"
            f"--- RESPONSE {response.status_code} ---\n"
            f"--- RESPONSE HEADERS ---\n{_format_headers(safe_resp_headers)}\n"
            f"--- RESPONSE BODY ---\n{_safe_body(response)}\n"
        )
        self._file.flush()

    def close(self):
        self._file.close()


class M5BurnerClient:
    def __init__(self, api_base_url=DEFAULT_API, logger=None):
        self.api_base_url = api_base_url.rstrip("/")
        self.session = requests.Session()
        if logger:
            logger.attach(self.session)

    def login(self, username, password):
        print(f"[login] as {username} ...")
        response = self.session.post(LOGIN_URL, json={"email": username, "password": password})
        if response.status_code != 200:
            raise RuntimeError(f"login failed: HTTP {response.status_code}")

        token = self.session.cookies.get("m5_auth_token")
        if not token:
            raise RuntimeError("login returned 200 but no m5_auth_token cookie")
        # The admin API lives on another host, where the cookie is not sent.
        self.session.headers.update({"m5_auth_token": token})
        print("[login] ok")

    def list_firmware(self):
        url = f"{self.api_base_url}/api/admin/firmware"
        response = self.session.get(url)
        if response.status_code != 200:
            raise RuntimeError(f"listing firmware failed: HTTP {response.status_code} - {response.text}")
        return response.json()

    def find_firmware(self, name=None, fid=None):
        firmwares = self.list_firmware()
        for fw in firmwares:
            if (fid and fw.get("fid") == fid) or (not fid and fw.get("name") == name):
                return fw
        raise RuntimeError(
            f"no entry matching {'fid ' + fid if fid else 'name ' + repr(name)} in your account. "
            f"Your entries: {[fw.get('name') for fw in firmwares]}. "
            "Create the entry once in the M5Burner GUI first."
        )

    def upload_version(self, firmware, version, binary_path):
        url = f"{self.api_base_url}/api/admin/firmware"
        # Every metadata field has to be echoed back or the entry loses it.
        data = {
            "name": firmware.get("name", ""),
            "description": firmware.get("description", ""),
            "category": firmware.get("category", ""),
            "author": firmware.get("author", ""),
            "version": version,
            "github": firmware.get("github", ""),
            "cover": "null",
        }
        print(f"[upload] {binary_path} as version {version} ...")
        with open(binary_path, "rb") as f:
            response = self.session.post(url, data=data, files={"firmware": f})
        if response.status_code != 200:
            raise RuntimeError(f"upload failed: HTTP {response.status_code} - {response.text}")
        print("[upload] ok")

    def publish_version(self, fid, version):
        # publish/{...}/1 wants the server-assigned file name, not the version.
        firmware = self.find_firmware(fid=fid)
        versions = firmware.get("versions") or []
        file_id = next((v.get("file") for v in versions if v.get("version") == version), None)
        if not file_id:
            raise RuntimeError(
                f"version {version} not found on the entry after upload. "
                f"Present: {[v.get('version') for v in versions]}"
            )
        url = f"{self.api_base_url}/api/admin/firmware/{fid}/publish/{file_id}/1"
        print(f"[publish] version {version} (file={file_id}) ...")
        response = self.session.put(url)
        if response.status_code != 200:
            raise RuntimeError(f"publish failed: HTTP {response.status_code} - {response.text}")
        result = response.json()
        if result.get("status") != 1:
            raise RuntimeError(f"publish rejected: {result}")
        print("[publish] ok")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--user", default=os.environ.get("M5BURNER_USER"),
                        help="M5Burner account email (default: $M5BURNER_USER)")
    parser.add_argument("--password", default=os.environ.get("M5BURNER_PWD"),
                        help="M5Burner account password (default: $M5BURNER_PWD)")
    parser.add_argument("--tag", help="release tag, e.g. v0.15.0; a leading 'v' is "
                                      "stripped so the M5Burner version reads 0.15.0. "
                                      "Default: -DCARD_VERSION from platformio.ini")
    parser.add_argument("--name", default=DEFAULT_NAME,
                        help=f"M5Burner entry name to publish under (default: {DEFAULT_NAME}). "
                             "The entry must already exist — it can only be created in "
                             "the M5Burner GUI, not through this API.")
    parser.add_argument("--fid", help="target the entry by fid instead of by name")
    parser.add_argument("--bin", help="image to upload (default: "
                                      "dist/adarkroom-<version>-merged.bin)")
    parser.add_argument("--api-base-url", default=DEFAULT_API,
                        help=f"admin API base URL (default: {DEFAULT_API})")
    parser.add_argument("--dry-run", action="store_true",
                        help="log in, resolve the entry and the bin, print what would "
                             "happen, then stop without uploading or publishing")
    parser.add_argument("--output", metavar="FILE",
                        help="write a redacted HTTP request/response log here")
    args = parser.parse_args()

    if not args.user or not args.password:
        print("[auth] set M5BURNER_USER and M5BURNER_PWD (or pass --user/--password)")
        return 2

    version = (args.tag or read_version(ROOT / "platformio.ini")).lstrip("vV")
    bin_path = Path(args.bin) if args.bin else ROOT / "dist" / f"adarkroom-{version}-merged.bin"
    if not bin_path.exists():
        print(f"[bin] not found: {bin_path} — run tools/make_dist.py first")
        return 2
    check_merged_image(bin_path)
    print(f"[plan] version={version} bin={bin_path} ({bin_path.stat().st_size} bytes)")

    logger = RequestLogger(args.output) if args.output else None
    if logger:
        print(f"[log] HTTP traffic -> {args.output}")

    client = M5BurnerClient(args.api_base_url, logger=logger)
    try:
        client.login(args.user, args.password)
        firmware = client.find_firmware(name=args.name, fid=args.fid)
        fid = firmware["fid"]
        existing = [v.get("version") for v in firmware.get("versions") or []]
        print(f"[entry] {firmware.get('name')} fid={fid} category={firmware.get('category')} "
              f"versions={existing}")

        if version in existing:
            print(f"[abort] version {version} already exists on this entry. "
                  "Bump -DCARD_VERSION / pass a different --tag, or delete it in the GUI.")
            return 1

        if args.dry_run:
            print(f"[dry-run] would upload {bin_path} as version {version}, then publish it")
            return 0

        client.upload_version(firmware, version, bin_path)
        client.publish_version(fid, version)
    except Exception as e:
        print(f"[error] {e}")
        return 1
    finally:
        if logger:
            logger.close()

    print(f"[done] {firmware.get('name')} {version} is live on M5Burner")
    return 0


if __name__ == "__main__":
    sys.exit(main())
