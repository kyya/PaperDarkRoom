#!/usr/bin/env python3
"""Grab the card's current screen (FBGet) and save it as a PNG.

Two transports, one wire format:

  BLE (default)  write "fb:get" to CTRL, read the reply off STAT notifications.
                 No new GATT characteristic — CTRL already carries the "adr:"
                 debug commands, and adding one would invalidate cached GATT
                 tables on paired hosts (README's GATT-cache trap).
  USB (--serial) write "fb:get\\n" to the CDC port, read the reply back.
                 Needs no pairing at all, which matters because a lost bond
                 (reflashed NVS, host that forgot the device) tends to happen
                 exactly when you need to see the screen.

WIRE FORMAT (firmware: ble_link::fbSend / main.cpp pollSerialFbGet)
  "FB <w> <h> <len>"   ASCII header
  <len bytes>          binary payload
The payload is the SAME 4bpp + RLE encoding the art blobs use, so the decoder
below is the mirror of tools/gen_event_art.py's rle()/pack4bpp():
  0x00 <n> <n bytes>   literal run
  <n>  <val>           n copies of val
each byte holding two pixels, HIGH nibble = the LEFT one, nibble * 17 -> 8-bit
grey. A typical UI frame leaves the device as ~2KB instead of 518KB.

Usage:
  python tools/fbget.py [-o shot.png]                  # BLE  (needs bleak)
  python tools/fbget.py --serial /dev/cu.usbmodemXXXX  # USB  (needs pyserial)
both need pillow.

For BLE the device must be awake and already bonded, and no other central may
hold the link (one peripheral, one central) — same preconditions as ble_ota.py.
"""
from __future__ import annotations
import argparse
import asyncio
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ble_protocol import CTRL_UUID, STAT_UUID, DEVICE_NAME  # noqa: E402


def unrle(data: bytes, expect: int) -> bytes:
    """RLE stream -> packed 4bpp bytes. Mirrors gen_event_art.py's encoder."""
    out = bytearray()
    i = 0
    while i < len(data) and len(out) < expect:
        head = data[i]
        if head == 0:                       # literal packet
            n = data[i + 1]
            out += data[i + 2:i + 2 + n]
            i += 2 + n
        else:                               # run packet
            out += bytes((data[i + 1],)) * head
            i += 2
    return bytes(out)


def to_image(packed: bytes, w: int, h: int):
    """Packed 4bpp (two pixels per byte, high nibble left) -> 8-bit L image."""
    from PIL import Image
    px = bytearray(w * h)
    row_bytes = w // 2
    for y in range(h):
        base, o = y * row_bytes, y * w
        for xb in range(row_bytes):
            if base + xb >= len(packed):
                break
            b = packed[base + xb]
            px[o + xb * 2]     = (b >> 4) * 17
            px[o + xb * 2 + 1] = (b & 0x0F) * 17
    return Image.frombytes("L", (w, h), bytes(px))


async def grab(out_path: str, scan_timeout: float, wait: float) -> int:
    from bleak import BleakClient, BleakScanner   # only the BLE path needs it
    print(f"[scan] looking for '{DEVICE_NAME}' ({scan_timeout:.0f}s)…")
    dev = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=scan_timeout)
    if not dev:
        print("[scan] not found — is it awake and not held by another central?")
        return 2
    print(f"[scan] found {dev.address}")

    state = {"w": 0, "h": 0, "len": 0, "buf": bytearray(), "err": None}
    done = asyncio.Event()

    def on_stat(_c, payload: bytearray):
        # Before the header lands, only ASCII status lines are expected; after
        # it, every notify is payload until `len` bytes have arrived.
        if state["len"] == 0:
            try:
                line = bytes(payload).decode("ascii")
            except UnicodeDecodeError:
                return                       # stray binary, not ours
            if line.startswith("FBERR"):
                state["err"] = line
                done.set()
            elif line.startswith("FB "):
                _, w, h, ln = line.split()
                state.update(w=int(w), h=int(h), len=int(ln))
                print(f"[fb] {w}x{h}, {int(ln)} bytes encoded")
            return
        state["buf"] += bytes(payload)
        if len(state["buf"]) >= state["len"]:
            done.set()

    async with BleakClient(dev, timeout=20.0) as client:
        await client.start_notify(STAT_UUID, on_stat)
        await client.write_gatt_char(CTRL_UUID, b"fb:get", response=True)
        try:
            await asyncio.wait_for(done.wait(), wait)
        except asyncio.TimeoutError:
            got, want = len(state["buf"]), state["len"]
            print(f"[fb] timed out after {got}/{want or '?'} bytes")
            return 3
        await client.stop_notify(STAT_UUID)

    if state["err"]:
        print(f"[fb] device reported {state['err']}")
        return 4
    w, h = state["w"], state["h"]
    packed = unrle(bytes(state["buf"]), w * h // 2)
    if len(packed) != w * h // 2:
        print(f"[fb] warning: decoded {len(packed)} of {w * h // 2} packed bytes")
    to_image(packed, w, h).save(out_path)
    ratio = (w * h // 2) / state["len"] if state["len"] else 0
    print(f"[fb] wrote {out_path} ({w}x{h}, {ratio:.1f}x compression)")
    return 0


def grab_serial(port: str, out_path: str, wait: float) -> int:
    """USB CDC path: write the request, read the header line, then `len` bytes."""
    import serial                       # pyserial, only needed for --serial
    with serial.Serial(port, 115200, timeout=wait) as s:
        s.reset_input_buffer()
        s.write(b"fb:get\n")
        s.flush()
        # Boot chatter and [hb] heartbeats share this pipe, so skip lines until
        # the header shows up rather than assuming the first one is ours.
        deadline = time.time() + wait
        w = h = ln = 0
        while time.time() < deadline:
            line = s.readline().decode("ascii", "replace").strip()
            if not line:
                continue
            if line.startswith("FBERR"):
                print(f"[fb] device reported {line}")
                return 4
            if line.startswith("FB "):
                try:
                    _, a, b, c = line.split()
                    w, h, ln = int(a), int(b), int(c)
                except ValueError:
                    continue
                break
        if not ln:
            print("[fb] no FB header within the timeout")
            return 3
        print(f"[fb] {w}x{h}, {ln} bytes encoded")
        buf = bytearray()
        while len(buf) < ln and time.time() < deadline:
            chunk = s.read(ln - len(buf))
            if not chunk:
                break
            buf += chunk
    if len(buf) < ln:
        print(f"[fb] short read: {len(buf)}/{ln} bytes")
        return 3
    packed = unrle(bytes(buf), w * h // 2)
    to_image(packed, w, h).save(out_path)
    print(f"[fb] wrote {out_path} ({w}x{h}, {(w * h // 2) / ln:.1f}x compression)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--out", default="shot.png", help="output PNG")
    ap.add_argument("--serial", metavar="PORT",
                    help="grab over USB CDC instead of BLE (no pairing needed)")
    ap.add_argument("--scan", type=float, default=30.0, help="scan timeout, s")
    ap.add_argument("--wait", type=float, default=60.0, help="transfer timeout, s")
    args = ap.parse_args()
    if args.serial:
        return grab_serial(args.serial, args.out, args.wait)
    return asyncio.run(grab(args.out, args.scan, args.wait))


if __name__ == "__main__":
    sys.exit(main())
