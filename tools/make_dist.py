#!/usr/bin/env python3
"""Build + package a M5Burner-ready merged firmware image for adarkroom.

M5Burner (and any "flash the whole chip at 0x0" tool) wants a single .bin
that already contains bootloader + partition table + boot_app0 + app image
at their real flash offsets, not the four separate files PlatformIO
produces. This script drives that: `pio run -e adarkroom` (optional), then
`esptool.py merge_bin` with the exact offsets/flags this board needs, then
verifies the result and prints a summary.

Offsets/flags (ESP32-S3, esptool.py v4.9.0 — bundled with the PlatformIO
espressif32 platform):
  0x0     bootloader.bin
  0x8000  partitions.bin
  0xe000  boot_app0.bin      (OTA data-select stub, from the Arduino framework)
  0x10000 firmware.bin
--flash_mode dio --flash_freq 80m --flash_size 16MB — `dio` is PlatformIO's
own downgrade of board esp32s3box's native `qio` for this board+size combo
(board_build.arduino.memory_type = qio_opi in platformio.ini); using `dio`
here matches what a normal `pio run -t upload` actually writes over USB, so
the merged image boots identically to a USB flash.

Usage (venv python):
  ~/.ai-desk-card/venv/Scripts/python.exe tools/make_dist.py [--skip-build]

Output:
  dist/adarkroom-<version>-merged.bin   - flash the whole image at 0x0
  dist/adarkroom-<version>-launcher.bin - app-only image for bmorcelli/
                                           Launcher (or any third-party boot
                                           manager); copy to SD, install via
                                           Launcher
<version> is read from platformio.ini's -DCARD_VERSION build flag (e.g.
"0.4.2-adarkroom" -> "0.4.2").
"""
from __future__ import annotations
import argparse
import hashlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENV = "adarkroom"
BUILD_DIR = ROOT / ".pio" / "build" / ENV


def read_version(ini_path: Path) -> str:
    text = ini_path.read_text(encoding="utf-8")
    m = re.search(r'CARD_VERSION="\\"([0-9][0-9.]*)-adarkroom\\""', text)
    if not m:
        print(f"[version] couldn't find -DCARD_VERSION in {ini_path}")
        sys.exit(2)
    return m.group(1)


def find_pio() -> str | None:
    which = shutil.which("pio")
    if which:
        return which
    home = Path.home()
    for candidate in (
        home / ".ai-desk-card" / "venv" / "Scripts" / "pio.exe",  # Windows
        home / ".ai-desk-card" / "venv" / "bin" / "pio",          # macOS/Linux
    ):
        if candidate.exists():
            return str(candidate)
    return None


def run(cmd: list[str], desc: str) -> None:
    print(f"[{desc}] {' '.join(str(c) for c in cmd)}")
    result = subprocess.run(cmd)
    if result.returncode != 0:
        print(f"[{desc}] failed (exit {result.returncode})")
        sys.exit(result.returncode)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-build", action="store_true",
                     help="reuse the existing .pio/build/%s output instead "
                          "of running pio run first" % ENV)
    args = ap.parse_args()

    # Line-buffer stdout even when redirected/logged, so our own [step]
    # messages interleave correctly with subprocess (pio/esptool) output
    # instead of all landing at the end in one flush.
    sys.stdout.reconfigure(line_buffering=True)

    version = read_version(ROOT / "platformio.ini")
    print(f"[version] {version}")

    if not args.skip_build:
        pio = find_pio()
        if not pio:
            print("[build] couldn't find `pio` on PATH or in "
                  "~/.ai-desk-card/venv — install it or pass --skip-build")
            return 2
        run([pio, "run", "-e", ENV], "build")
    else:
        print("[build] skipped (--skip-build)")

    bootloader = BUILD_DIR / "bootloader.bin"
    partitions = BUILD_DIR / "partitions.bin"
    firmware = BUILD_DIR / "firmware.bin"
    for f in (bootloader, partitions, firmware):
        if not f.exists():
            print(f"[build] missing {f} — run without --skip-build, or "
                  f"`pio run -e {ENV}` first")
            return 2

    esptool = (Path.home() / ".platformio" / "packages" / "tool-esptoolpy"
               / "esptool.py")
    if not esptool.exists():
        print(f"[esptool] not found: {esptool}")
        print("          install the espressif32 platform via PlatformIO first")
        return 2

    boot_app0 = (Path.home() / ".platformio" / "packages"
                 / "framework-arduinoespressif32" / "tools" / "partitions"
                 / "boot_app0.bin")
    if not boot_app0.exists():
        print(f"[boot_app0] not found: {boot_app0}")
        print("            install the espressif32 Arduino framework via "
              "PlatformIO first")
        return 2

    dist_dir = ROOT / "dist"
    dist_dir.mkdir(exist_ok=True)
    out = dist_dir / f"adarkroom-{version}-merged.bin"

    run([sys.executable, str(esptool), "--chip", "esp32s3", "merge_bin",
         "-o", str(out),
         "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB",
         "0x0", str(bootloader),
         "0x8000", str(partitions),
         "0xe000", str(boot_app0),
         "0x10000", str(firmware)],
        "merge")

    print(f"[verify] checking {out}")
    data = out.read_bytes()
    if not data:
        print("[verify] output file is empty")
        return 1
    if data[0] != 0xE9:
        print(f"[verify] bad magic at offset 0: {data[0]:#04x}")
        return 1
    expected_size = 0x10000 + firmware.stat().st_size
    if len(data) != expected_size:
        print(f"[verify] size mismatch: got {len(data)}, expected {expected_size} "
              f"(0x10000 + firmware.bin)")
        return 1
    print(f"[verify] magic=0xE9 size={len(data)} (0x10000 + firmware.bin) OK")

    run([sys.executable, str(esptool), "--chip", "esp32s3", "image_info",
         str(out)], "image_info")

    sha256 = hashlib.sha256(data).hexdigest()
    print(f"[done] {out}")
    print(f"[done] size={len(data)} ({len(data) / 1024:.1f} KiB)")
    print(f"[done] sha256={sha256}")
    print("[done] flash the whole file starting at 0x0 (M5Burner: single "
          "image, address 0x0)")

    # App-only image for third-party boot managers (e.g. bmorcelli/Launcher):
    # Launcher owns bootloader + partition table + its own OTA app slot, and
    # installs a user-picked app-only .bin into that slot. An ESP32 app image
    # is position-independent w.r.t. partition offset, so firmware.bin as-is
    # boots fine there — no merge/offsets needed, just a copy under a name
    # that says what it's for.
    launcher_out = dist_dir / f"adarkroom-{version}-launcher.bin"
    shutil.copyfile(firmware, launcher_out)
    print(f"[launcher] copied {firmware} -> {launcher_out}")

    launcher_data = launcher_out.read_bytes()
    if not launcher_data:
        print("[launcher] output file is empty")
        return 1
    if launcher_data[0] != 0xE9:
        print(f"[launcher] bad magic at offset 0: {launcher_data[0]:#04x}")
        return 1
    if len(launcher_data) <= 512 * 1024:
        print(f"[launcher] suspiciously small: {len(launcher_data)} bytes")
        return 1
    print(f"[launcher] magic=0xE9 size={len(launcher_data)} "
          f"({len(launcher_data) / 1024:.1f} KiB) OK")
    print("[launcher] for bmorcelli/Launcher: copy to SD card and install "
          "via Launcher (do not flash at 0x0 directly)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
