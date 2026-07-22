#!/usr/bin/env python3
"""BLE debug-command sender for the M5PaperS3 (adarkroom firmware) — a tiny
BLE central that injects game commands over the existing encrypted link.

Reuses the CTRL characteristic instead of adding a new one (the Windows GATT
cache trap — a new characteristic forces a remove + re-pair; see README.md): a
write starting with ASCII "adr:" is intercepted by the firmware as a game
command, not a binary frame header (the header's u32 LE `total` is a small
count and can never collide with 0x3A726461 == "adr:" LE). Fire and forget —
the device beeps + repaints on success, so there's no ack to wait for:

  scan -> connect (existing bond) -> write "adr:give <res> <amount>" to CTRL
  (response=True) -> print & exit.

Usage (venv python — bleak):
  ~/.ai-desk-card/venv/bin/python tools/adr_cmd.py give iron 300
  ~/.ai-desk-card/venv/bin/python tools/adr_cmd.py give-all 1000000
  ~/.ai-desk-card/venv/bin/python tools/adr_cmd.py reset   # WIPE save, new game

<res> is a RES_KEY english key from src/game_data.h (wood/fur/iron/coal/steel/
scales/teeth/…). Multi-word keys are sent with '_' for the space
(cured_meat / energy_cell / alien_alloy) — the firmware un-escapes them before
matching — and a `give <res>` arg with a literal space is normalized to that
form too. `give-all` sweeps every RES_KEY in one connection (a ~0.3s pause per
write lets the firmware save + repaint keep up). <amount> is whole units,
1..1000000. The device must be awake (power button / USB) and already bonded.
Don't run this while another BLE central is mid-session with the device (two
centrals, one peripheral).
"""
# PEP 563: defer annotation evaluation so any PEP 604 `X | None` hints parse as
# strings on the Mac venv's CLT python 3.9 (matches tools/ble_ota.py).
from __future__ import annotations
import argparse
import asyncio
import sys
from pathlib import Path

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ble_protocol import CTRL_UUID as CTRL, DEVICE_NAME as NAME   # noqa: E402

# Every RES_KEY from src/game_data.h, multi-word keys pre-escaped with '_' for
# the space (the firmware swaps '_' back to ' ' before matching). Kept in sync
# BY HAND with the RES_KEY table — 19 resources, order irrelevant for give-all.
RES_KEYS = [
    "wood", "fur", "meat", "bait", "leather", "cured_meat",
    "iron", "coal", "sulphur", "steel", "teeth", "scales", "cloth", "charm",
    "bullets", "medicine", "energy_cell", "alien_alloy", "compass",
]

AMOUNT_MAX = 1000000
GIVE_ALL_GAP_S = 0.3   # pause between writes so the firmware save+repaint keeps up


async def scan(name: str, timeout: float):
    print(f"[scan] looking for '{name}' ({timeout:.0f}s)…")
    return await BleakScanner.find_device_by_name(name, timeout=timeout)


async def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scan", type=float, default=30.0,
                    help="scan timeout, seconds (default: %(default)s)")
    sub = ap.add_subparsers(dest="verb", required=True)
    g = sub.add_parser("give", help="inject one resource")
    g.add_argument("res", help="RES_KEY resource key (wood/fur/iron/…)")
    g.add_argument("amount", type=int, help=f"whole units, 1..{AMOUNT_MAX}")
    ga = sub.add_parser("give-all", help="inject every resource")
    ga.add_argument("amount", type=int, help=f"whole units, 1..{AMOUNT_MAX}")
    sub.add_parser("reset", help="GM factory wipe — delete save + start over "
                                 "(IRREVERSIBLE, no confirmation)")
    args = ap.parse_args()

    # Build the work list: each item is a raw ASCII command line to write.
    if args.verb == "reset":
        # No args. Fire-and-forget wipe: the firmware deletes the save, resets to
        # a fresh dark room, beeps, and jumps to the Room. IRREVERSIBLE — the user
        # asked for it, so no interactive confirm, just a loud heads-up.
        print("[cmd] !! adr:reset — this WIPES the save and starts a NEW game. "
              "This cannot be undone. Sending now.")
        work = ["adr:reset"]
    else:
        if not 1 <= args.amount <= AMOUNT_MAX:
            print(f"[cmd] amount out of range (1..{AMOUNT_MAX}): {args.amount}")
            return 2
        # Normalize any space/underscore in a single `give` res to the wire form.
        if args.verb == "give-all":
            work = [f"adr:give {res} {args.amount}" for res in RES_KEYS]
        else:
            work = [f"adr:give {args.res.replace(' ', '_')} {args.amount}"]

    dev = await scan(NAME, args.scan)
    if not dev:
        print("[scan] not found — is the device awake? (power button / USB)")
        return 1
    print(f"[scan] found {dev.address}")

    try:
        async with BleakClient(dev, timeout=20.0) as client:
            for i, cmd in enumerate(work):
                await client.write_gatt_char(CTRL, cmd.encode("ascii"),
                                             response=True)
                print(f"[cmd] ({i + 1}/{len(work)}) sent '{cmd}'")
                if i + 1 < len(work):
                    await asyncio.sleep(GIVE_ALL_GAP_S)
            print("[cmd] done — device beeps + repaints on each success")
    except BleakError as e:
        print(f"[ble] error: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
