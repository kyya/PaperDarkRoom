#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Generate src/event_art_data.h — the 13 random-event illustrations, packed for
# the 16-grey e-ink panel.
#
# Source art lives OUTSIDE the repo (docs/event-art/<key>_nb2.png, 1376x768 RGB
# pen-and-ink line drawings; docs/event-art/ is gitignored). Only the generated
# header is committed, so this tool is the record of how the blob was made.
#
# Pipeline, per image:
#   1. RGB -> L (luma).
#   2. LANCZOS downscale 1376x768 -> 492x276 (see EVENT_ART_W/H rationale in the
#      generated header). LANCZOS matters here: the art is high-frequency ink
#      hatching being shrunk 2.8x, and a box/bilinear filter turns the hatching
#      into mush. Lanczos keeps the strokes as clean anti-aliased greys, which is
#      exactly the signal the 16-grey panel can show.
#   3. Quantise 8-bit luma -> 4-bit (0..15), NO dither by default. These are ink
#      drawings on paper-white: the tone that exists is anti-aliasing produced in
#      step 2, and it already lands on the 16-level ladder almost exactly. An
#      ordered/Bayer dither (--dither bayer, kept for A/B) only sprinkles noise
#      into the large flat white areas and breaks up the thin strokes — strictly
#      worse at this size. Rounding is round-half-up so paper white (255) stays
#      level 15 and ink black (0) stays level 0.
#   4. Pack 4bpp, two pixels per byte, HIGH nibble = left (even x) pixel. Width
#      492 is even, so every row is exactly 246 bytes with no padding.
#   5. RLE the packed byte stream (format documented in the generated header).
#      The art is mostly paper white, so this pays for itself ~4x: 883 KB raw ->
#      ~210 KB, which is the difference between a 4.4 MB and a 1.1 MB header.
#
# Usage (from the repo root):
#   python tools/gen_event_art.py
#   python tools/gen_event_art.py --dither bayer --preview-only   # A/B the dither
#
# 480p greyscale previews of exactly what the firmware will show are written to
# docs/event-art/final/ (also gitignored) for eyeballing before committing.

import argparse
import os
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is required: python -m pip install pillow")

ART_W = 492
ART_H = 276

# adr::EventId order (src/events_data.h) — the pointer table is indexed by it,
# so this list must stay in that exact order.
EVENTS = [
    ("EV_NOMAD",       "ev_nomad"),
    ("EV_NOISES_OUT",  "ev_noises_out"),
    ("EV_NOISES_IN",   "ev_noises_in"),
    ("EV_BEGGAR",      "ev_beggar"),
    ("EV_SHADY",       "ev_shady"),
    ("EV_WANDER_WOOD", "ev_wander_wood"),
    ("EV_WANDER_FUR",  "ev_wander_fur"),
    ("EV_RUINED_TRAP", "ev_ruined_trap"),
    ("EV_FIRE",        "ev_fire"),
    ("EV_BEAST",       "ev_beast"),
    ("EV_SICK_MAN",    "ev_sick_man"),
    ("EV_SICKNESS",    "ev_sickness"),
    ("EV_PLAGUE",      "ev_plague"),
]

# 4x4 Bayer matrix, scaled to the 1/16-of-a-level offsets an ordered dither needs
# at 16 output levels. Only used with --dither bayer.
BAYER4 = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]


def quantize(img, dither, w=ART_W, h=ART_H):
    """8-bit L image -> list of 0..15 nibbles, row-major."""
    px = img.load()
    out = bytearray(w * h)
    i = 0
    for y in range(h):
        for x in range(w):
            v = px[x, y]
            if dither == "bayer":
                # v/255*15 in 1/16-level steps, plus the Bayer threshold.
                t = (v * 15 * 16 + BAYER4[y & 3][x & 3] * 255) // (255 * 16)
                q = t if t <= 15 else 15
            else:
                q = (v * 15 + 127) // 255      # round half up
            out[i] = q
            i += 1
    return out


def pack4bpp(nibbles, w=ART_W, h=ART_H):
    """Row-major nibbles -> 4bpp bytes, high nibble = even x."""
    out = bytearray(w // 2 * h)
    o = 0
    for y in range(h):
        base = y * w
        for x in range(0, w, 2):
            out[o] = (nibbles[base + x] << 4) | nibbles[base + x + 1]
            o += 1
    return bytes(out)


def rle(data):
    """RLE the packed stream. Packets:
         0x00 <len> <len bytes>   literal, len 1..255
         <n>  <val>               run of n copies of val, n 1..255
    """
    out = bytearray()
    i, n = 0, len(data)
    while i < n:
        # How long does the run starting at i go? (capped at 255)
        j = i
        while j + 1 < n and data[j + 1] == data[i] and (j - i) < 254:
            j += 1
        run = j - i + 1
        if run >= 3:
            out += bytes((run, data[i]))
            i = j + 1
            continue
        # Otherwise gather literals until a run of >=3 starts.
        lit = bytearray()
        k = i
        while k < n and len(lit) < 255:
            r = 1
            while k + r < n and data[k + r] == data[k] and r < 3:
                r += 1
            if r >= 3:
                break
            lit.append(data[k])
            k += 1
        out += bytes((0, len(lit))) + lit
        i = k
    return bytes(out)


def unrle(data, expect):
    """Reference decoder — proves the C decoder's contract before we ship it."""
    out = bytearray()
    i = 0
    while i < len(data):
        h = data[i]
        if h == 0:
            ln = data[i + 1]
            out += data[i + 2:i + 2 + ln]
            i += 2 + ln
        else:
            out += bytes((data[i + 1],)) * h
            i += 2
    assert len(out) == expect, (len(out), expect)
    return bytes(out)


def c_array(name, blob):
    lines = [f"static const uint8_t {name}[{len(blob)}] = {{"]
    for off in range(0, len(blob), 16):
        chunk = blob[off:off + 16]
        lines.append("    " + "".join(f"0x{b:02X}," for b in chunk))
    lines.append("};")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--src", default=os.path.join(root, "docs", "event-art"))
    ap.add_argument("--out", default=os.path.join(root, "src", "event_art_data.h"))
    ap.add_argument("--preview", default=os.path.join(root, "docs", "event-art", "final"))
    ap.add_argument("--dither", choices=["none", "bayer"], default="none")
    ap.add_argument("--preview-only", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.preview, exist_ok=True)
    blobs, raw_total = [], 0
    for sym, key in EVENTS:
        path = os.path.join(args.src, key + "_nb2.png")
        if not os.path.exists(path):
            sys.exit(f"missing source art: {path}")
        img = Image.open(path).convert("L").resize((ART_W, ART_H), Image.LANCZOS)
        nib = quantize(img, args.dither)
        # Preview = exactly the 16 levels the panel will drive, expanded x17.
        Image.frombytes("L", (ART_W, ART_H), bytes(v * 17 for v in nib)).save(
            os.path.join(args.preview, f"{key}_{args.dither}.png"))
        packed = pack4bpp(nib)
        comp = rle(packed)
        unrle(comp, len(packed))
        raw_total += len(packed)
        blobs.append((sym, key, comp))
        print(f"  {key:<16} raw {len(packed):>6}  rle {len(comp):>6}  "
              f"{len(packed) / len(comp):.2f}x")

    total = sum(len(b) for _, _, b in blobs)
    print(f"  {'TOTAL':<16} raw {raw_total:>6}  rle {total:>6}  "
          f"{raw_total / total:.2f}x")
    if args.preview_only:
        print(f"previews -> {args.preview} (no header written)")
        return

    with open(args.out, "w", newline="\n") as f:
        f.write(HEADER_TOP.format(w=ART_W, h=ART_H, w2=ART_W // 2,
                                  count=len(EVENTS), raw=raw_total, total=total,
                                  ratio=raw_total / total))
        for sym, key, blob in blobs:
            f.write(f"\n// {key} — {len(blob)} bytes RLE\n")
            f.write(c_array("EVENT_ART_" + sym, blob) + "\n")
        f.write("\n// Indexed by adr::EventId; nullptr = no art for that event.\n")
        f.write(f"static const uint8_t* const EVENT_ART[{len(EVENTS)}]"
                " __attribute__((unused)) = {\n")
        for sym, _, _ in blobs:
            f.write(f"    EVENT_ART_{sym},\n")
        f.write("};\n")
    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes of source)")


HEADER_TOP = """\
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Random-event illustrations, {count} of them, one per adr::EventId. MACHINE
// GENERATED by tools/gen_event_art.py from docs/event-art/<key>_nb2.png — DO NOT
// EDIT BY HAND (regenerate and re-view the previews in docs/event-art/final/
// instead). The source PNGs live outside the repo; see the tool's header for the
// full pipeline (LANCZOS downscale, no dither, 4bpp, RLE).
//
// Include from exactly ONE .cpp (event_modal.cpp) — it carries the whole blob.
//
// FORMAT
//   Each image is {w}x{h} pixels of 4-bit greyscale, 0 = ink black .. 15 = paper
//   white. Pixels are packed two per byte, HIGH nibble = the LEFT (even x) pixel;
//   {w} is even so a row is exactly {w2} bytes with no padding. The panel canvas is
//   grayscale_8bit, so a nibble expands to a canvas byte as nib * 17 (15 -> 255).
//
//   That packed byte stream is then run-length encoded. The decoder walks packets:
//     0x00 <len> <len bytes>   literal run, len = 1..255
//     <n>  <val>               n copies of val, n = 1..255
//   Decoding is a single forward pass with no scratch buffer, so the unpacker can
//   stream straight into the canvas (event_modal::drawArt). Ink-on-white art is
//   very run-friendly: {raw} bytes raw -> {total} bytes encoded ({ratio:.2f}x), which is
//   what keeps this header near 1 MB of source instead of 4.4 MB.
#pragma once
#include <stdint.h>

// See event_modal.cpp for the vertical budget these dimensions come out of.
constexpr int EVENT_ART_W = {w};
constexpr int EVENT_ART_H = {h};
"""

if __name__ == "__main__":
    main()
