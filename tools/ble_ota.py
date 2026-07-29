#!/usr/bin/env python3
"""BLE OTA firmware pusher for the M5PaperS3 (adarkroom firmware) — its own
BLE central.

Streams a firmware .bin to the device over the existing BLE link, no USB:
  scan -> connect (existing bond) -> write the 8-byte OTA header (<II
  total|crc32, zlib CRC32) -> stream the image over the DATA characteristic at
  mtu-3 chunks (acknowledged writes) -> watch STATUS for `ota=ok` -> the device
  reboots into the new image -> rescan and confirm STATUS `fw=` changed.

The DATA chunks are ACKNOWLEDGED writes (response=True), not
write-without-response: the firmware writes each chunk into flash from its BLE
RX callback (esp_ota_write, which lazily erases a 4 KB sector on first touch,
stalling the callback), and write-without-response has no back-pressure, so a
chunk arriving during an erase stall overruns the peripheral's ACL RX buffer
and is dropped — with no per-chunk ack or retransmit, one lost chunk hangs the
whole transfer (recv freezes short of `total`, never finalizes). See the long
comment at the streaming loop. Acknowledged writes keep at most one chunk in
flight, so nothing can be dropped at any image size.

The game firmware and the companion dashboard firmware share the same BLE GATT
UUID set and OTA slot layout on purpose, so this script can also cross-flash a
dashboard build onto the same device and back.

Usage (venv python — bleak):
  ~/.ai-desk-card/venv/bin/python tools/ble_ota.py [--fw PATH] [--scan 30]
                                                   [--timeout 120]

The device must be awake (power button / USB) and already bonded. Don't run
this while another BLE central is mid-session with the device: two centrals,
one peripheral. After the first flash that adds the OTA characteristic,
Windows/WinRT may need the device removed + re-paired to drop its cached GATT
table (see README.md).

`--throttle` (ms/chunk, default 0) adds an extra per-chunk delay. It is now a
manual escape hatch only: acknowledged writes already gate the send on the
peripheral, which superseded the old macOS pacing hack (CoreBluetooth applied
no back-pressure to write-without-response and would overrun the ESP32's RX
buffer). Leave it at 0 unless a specific link needs slowing.
"""
# PEP 563: defer annotation evaluation so the PEP 604 `X | None` return hints
# below parse as strings — the Mac venv runs CLT python 3.9, which would else
# raise TypeError on `str | None` at def time. No runtime behavior change.
from __future__ import annotations
import argparse
import asyncio
import struct
import sys
import time
import zlib
from pathlib import Path

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ble_protocol import (OTA_UUID as OTA, DATA_UUID as DATA,   # noqa: E402
                          STAT_UUID as STAT, DEVICE_NAME as NAME)

DEFAULT_FW = ".pio/build/adarkroom/firmware.bin"


def parse_status(line: str) -> dict:
    toks = {}
    for tok in line.split():
        k, sep, v = tok.partition("=")
        if sep:
            toks[k] = v
    return toks


async def scan(name: str, timeout: float):
    print(f"[scan] looking for '{name}' ({timeout:.0f}s)…")
    return await BleakScanner.find_device_by_name(name, timeout=timeout)


async def read_fw_version(dev, scan_timeout: float) -> str | None:
    """Connect, grab one STATUS, return its fw= (None on any failure)."""
    got = {"v": None}
    seen = asyncio.Event()

    def on_stat(_c, payload: bytearray):
        line = payload.decode(errors="replace")
        if line.startswith("STATUS"):
            v = parse_status(line).get("fw")
            if v:
                got["v"] = v
                seen.set()

    try:
        async with BleakClient(dev, timeout=20.0) as client:
            await client.start_notify(STAT, on_stat)
            try:
                await asyncio.wait_for(seen.wait(), 10.0)
            except asyncio.TimeoutError:
                pass
    except (BleakError, OSError) as e:
        # OSError covers the WinRT post-reboot reconnect race: the device just
        # rebooted and start_notify's CCCD write can throw
        # "operation was canceled" (WinError -2147023673) before the encrypted
        # link settles. Caller retries.
        print(f"[verify] reconnect failed: {e}")
    return got["v"]


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fw", default=DEFAULT_FW,
                    help="firmware .bin (default: %(default)s)")
    ap.add_argument("--scan", type=float, default=30.0,
                    help="per-scan timeout, seconds (default: %(default)s)")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="verify wait after streaming, seconds "
                         "(default: %(default)s)")
    ap.add_argument("--progress", type=int, default=64,
                    help="log every N chunks (default: %(default)s)")
    ap.add_argument("--throttle", type=float, default=0.0,
                    help="extra delay per chunk, ms. Normally 0: DATA chunks now "
                         "use acknowledged writes (response=True), whose ATT "
                         "back-pressure already keeps at most one chunk in "
                         "flight, so the old macOS pacing hack is obsolete. Kept "
                         "as a manual escape hatch (default: %(default)s)")
    args = ap.parse_args()

    fw = Path(args.fw)
    if not fw.is_absolute():
        fw = Path(__file__).resolve().parent.parent / fw
    if not fw.exists():
        print(f"[fw] not found: {fw}")
        print("     build it first: pio run -e adarkroom")
        return 2
    image = fw.read_bytes()
    total = len(image)
    crc = zlib.crc32(image) & 0xFFFFFFFF
    print(f"[fw] {fw}")
    print(f"[fw] {total} B ({total / 1024:.1f} KiB) crc={crc:08x}")

    dev = await scan(NAME, args.scan)
    if not dev:
        print("[scan] not found — is the device awake? (power button / USB)")
        return 1
    print(f"[scan] found {dev.address}")

    ota_last = {"v": None}
    fw_before = {"v": None}
    failed = {"reason": None}
    done = asyncio.Event()
    disconnected = asyncio.Event()

    def on_stat(_c, payload: bytearray):
        line = payload.decode(errors="replace")
        if not line.startswith("STATUS"):
            return
        toks = parse_status(line)
        if fw_before["v"] is None and "fw" in toks:
            fw_before["v"] = toks["fw"]
        ota = toks.get("ota")
        if ota and ota != ota_last["v"]:
            ota_last["v"] = ota
            print(f"[ota] {ota}")
        if ota == "ok":
            done.set()
        elif ota and ota.startswith("err"):
            failed["reason"] = ota
            done.set()

    def on_disc(_c):
        disconnected.set()

    try:
        async with BleakClient(dev, timeout=20.0,
                               disconnected_callback=on_disc) as client:
            # Windows/WinRT doesn't auto-pair on connect the way it used to
            # once an old bond exists; explicit pair() (Just Works, no
            # prompt) gets the link encrypted so the OTA/DATA characteristic
            # writes below don't fail with ATT "Insufficient
            # Authentication"/"Insufficient Encryption". Tolerate failure
            # (e.g. already paired, or platforms where pair() isn't needed)
            # and keep going — the writes below will surface a clearer error
            # if the link truly isn't authenticated.
            try:
                await client.pair()
            except (BleakError, NotImplementedError, OSError) as e:
                print(f"[pair] {e} — continuing anyway")

            await client.start_notify(STAT, on_stat)
            await asyncio.sleep(2.5)   # catch a STATUS to learn the current fw=
            print(f"[ota] current fw={fw_before['v']}")

            mtu = client.mtu_size
            chunk = max(20, mtu - 3)
            print(f"[ota] mtu={mtu} chunk={chunk}")

            header = struct.pack("<II", total, crc)
            await client.write_gatt_char(OTA, header, response=True)
            print(f"[ota] header sent (total={total} crc={crc:08x}) — streaming…")

            t0 = time.time()
            nchunks = (total + chunk - 1) // chunk
            # ACKNOWLEDGED writes (response=True), NOT write-without-response.
            # The firmware streams each DATA chunk straight into flash from the
            # BLE RX callback (esp_ota_write). esp_ota_begin(OTA_SIZE_UNKNOWN)
            # erases the OTA partition lazily, one 4 KB sector at a time, on the
            # first write that reaches each sector — and that erase runs INSIDE
            # the RX callback, stalling it for milliseconds. With
            # write-without-response there is no ATT-layer back-pressure, so a
            # chunk that arrives during a sector-erase stall overruns the
            # peripheral's ACL RX buffer and is silently dropped. The transfer
            # has no per-chunk ack and no retransmit, so a single dropped chunk
            # hangs forever: the device's recv counter freezes one chunk short
            # of `total`, never finalizes, never errors.
            #
            # This bit us the moment the image grew past a 4 KB boundary the
            # previous builds never reached: a 1,385,600-byte image first writes
            # into sector 338 (offset 0x151000 = 1,384,448), forcing one MORE
            # late-stream erase than the ~1,383,7xx images that always flashed;
            # the chunk right after that erase (chunk 2694, image bytes
            # 1,384,716..1,385,230) got dropped every run, freezing recv at
            # 1,385,086 = total - 514 = 514*2694 + 370. The trailing 370-byte
            # chunk still landed, which is exactly why recv is not a multiple of
            # 514 — proof one MID-stream chunk was lost, not the last one.
            #
            # response=True makes the host wait for the ATT Write Response the
            # firmware only sends AFTER the callback (incl. any erase) returns,
            # so at most one chunk is ever in flight: the RX buffer can't
            # overflow and no chunk can be dropped, at any image size. Slower
            # (one round-trip per chunk) but lossless; the DATA characteristic
            # advertises PROPERTY_WRITE alongside PROPERTY_WRITE_NR, so the
            # already-installed firmware accepts it with no firmware change.
            for ci, off in enumerate(range(0, total, chunk)):
                if disconnected.is_set():
                    print("[ota] disconnected mid-transfer — aborting")
                    return 1
                # The device can reject the image mid-stream (e.g. the
                # firmware's foreign-layout guard in ble_link.cpp otaFail()).
                # STATUS notifies every 2s, so failed["reason"] flips within
                # one cycle; without this check we'd keep pushing acknowledged
                # writes for the rest of the ~1.4 MB image — several more
                # minutes of pure waste — before noticing after the loop.
                if failed["reason"]:
                    print(f"[ota] device reported {failed['reason']} — "
                          f"aborting stream")
                    return 1
                await client.write_gatt_char(DATA, image[off:off + chunk],
                                             response=True)
                if args.throttle:
                    await asyncio.sleep(args.throttle / 1000)
                if (ci + 1) % args.progress == 0 or ci + 1 == nchunks:
                    sent = min(off + chunk, total)
                    el = max(time.time() - t0, 1e-3)
                    print(f"[ota] {sent}/{total} ({100.0 * sent / total:.0f}%) "
                          f"{sent / 1024 / el:.1f} KiB/s")
            print(f"[ota] image streamed in {time.time() - t0:.1f}s "
                  f"— waiting for verify…")

            try:
                await asyncio.wait_for(done.wait(), args.timeout)
            except asyncio.TimeoutError:
                if disconnected.is_set():
                    print("[ota] disconnected before ota=ok — aborting")
                else:
                    print(f"[ota] no ota=ok within {args.timeout:.0f}s")
                return 1
            if failed["reason"]:
                print(f"[ota] device reported {failed['reason']}")
                return 1
            print("[ota] device accepted image (ota=ok) — it reboots now")
    except BleakError as e:
        print(f"[ble] error: {e}")
        return 1

    # The device reboots into the new image; rescan + read the new fw=. The
    # FIRST reconnect after a reboot races WinRT's encrypted reconnect —
    # start_notify can throw OSError before the link settles — so retry the
    # whole confirm (rescan + connect + read) up to 3 rounds, 5 s apart.
    # read_fw_version swallows OSError/BleakError/timeout and returns None.
    print("[verify] waiting for the device to reboot…")
    fw_after = None
    for attempt in range(1, 4):
        await asyncio.sleep(5.0)
        dev2 = await scan(NAME, args.scan)
        if not dev2:
            print(f"[verify] attempt {attempt}/3: device not seen yet")
            continue
        fw_after = await read_fw_version(dev2, args.scan)
        if fw_after:
            break
        print(f"[verify] attempt {attempt}/3: couldn't read STATUS — retrying")

    print(f"[verify] fw before={fw_before['v']} after={fw_after}")
    if fw_after and fw_after != fw_before["v"]:
        print("[verify] OK — firmware updated ✓")
        return 0
    if fw_after and fw_after == fw_before["v"]:
        # We DID reconnect and read a version, and it's unchanged — a genuine
        # failure (image rejected, or the app-level rollback flipped it back).
        print("[verify] fw= unchanged — image failed or rolled back")
        return 1
    # Never got a reading in 3 rounds. The push + reboot themselves succeeded
    # (ota=ok landed above), we just couldn't reconnect to confirm the version
    # — distinct from a push failure, and the update most likely took.
    print("[verify] OTA was pushed and the device rebooted, but the version "
          "confirm failed after 3 tries — check the device screen manually")
    return 3


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
