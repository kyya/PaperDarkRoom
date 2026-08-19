#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# Generate src/enemy_art_data.h — the combat portraits, packed for the 16-grey
# e-ink panel. Same encoding as the event plates (RLE over 4bpp), so the pixel
# pipeline is imported from gen_event_art.py rather than restated; only the
# geometry and the subject table differ.
#
# 492x620 — full CONTENT_W wide, tall enough to spend the 1-row attack grid's
# leftover (fight_modal lockArtHeight: 1 row -> 620px of plate). A 420px plate
# left ~200px of paper-white air above and below the picture in the common
# fight, indistinguishable from the drawing's own padding, so the subject
# looked like it was floating in a hole. 620 fills that slot; heavier loadouts
# crop it from the centre (art::blit srcY0/rows) the way they always did.
#
#     buttons   rows   room for the plate   (3 columns)
#      1- 3      1            620  -> 620 (fills)
#      4- 6      2            530  -> 530 (90px crop)
#      7- 9      3            440  -> 440
#     10-12      4            350  -> 350
#
# COMPOSE FOR THE CROP: keep the subject vertically centred and treat the top
# and bottom edges as trim. Landscape drawings (lizard) letterbox rather than
# losing a head or a tail to a cover-fit.
#
# SUBJECTS (see fight_modal.cpp / world_state.h Combat.enemyId):
#   - the 11 random-encounter enemies are indexed by adr::EncounterId
#   - a setpiece enemy carries enemyId 0xFF and only a glyph, so it falls back to
#     one generic plate per glyph actually reachable there ('R'/'E'/'T'/'D') — 4
# Source art lives OUTSIDE the repo at docs/enemy-art/<key>.png, exactly like the
# event plates. Any subject whose source is missing is emitted as a PLACEHOLDER
# (framed box + its glyph) so the layout and the decoder can be flown on real
# hardware before a single drawing exists.
#
# Usage (from the repo root):
#   python tools/gen_enemy_art.py                 # placeholders for missing art
#   python tools/gen_enemy_art.py --preview-only  # eyeball, write no header

import argparse
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is required: python -m pip install pillow")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_event_art import quantize, pack4bpp, rle, unrle, c_array  # noqa: E402

ART_W = 492
ART_H = 620
# If a cover-fit would shave more than this fraction off EACH side, letterbox
# instead. Landscape subjects (the lizard is 2:1) must not lose a snout.
MAX_COVER_SIDE = 0.08
# The height that survives the tightest crop (6-row attack grid). Kept in sync
# with fight_modal.cpp ART_H_MIN — it is the safe area the art must compose for.
MIN_CROP_H = 110

# adr::EncounterId order (src/combat_data.h) — the pointer table is indexed by
# it, so this list must stay in that exact order.
ENCOUNTERS = [
    ("E_SNARLING_BEAST", "snarling_beast", 'R'),
    ("E_GAUNT_MAN",      "gaunt_man",      'E'),
    ("E_STRANGE_BIRD",   "strange_bird",   'R'),
    ("E_TWO_HEADED",     "two_headed",     'K'),
    ("E_SHIVERING_MAN",  "shivering_man",  'E'),
    ("E_MAN_EATER",      "man_eater",      'T'),
    ("E_SCAVENGER",      "scavenger",      'E'),
    ("E_LIZARD",         "lizard",         'T'),
    ("E_FERAL_TERROR",   "feral_terror",   'T'),
    ("E_SOLDIER",        "soldier",        'D'),
    ("E_SNIPER",         "sniper",         'D'),
]

# Setpiece fallback, one per glyph actually used by src/setpieces_data.h. 'K' is
# NOT among them: the only enemy carrying it is the two-headed creature, a random
# encounter, which resolves through ENEMY_ART by id and never reaches this table.
GLYPHS = [('R', "glyph_r"), ('E', "glyph_e"), ('T', "glyph_t"), ('D', "glyph_d")]


def placeholder(glyph, label):
    """A stand-in plate: framed box, corner ticks, the glyph, the subject key.

    Deliberately high-contrast and edge-to-edge so a real-hardware flash shows
    exactly where the plate lands and how big it is — the whole point of
    shipping placeholders before the art exists.
    """
    img = Image.new("L", (ART_W, ART_H), 255)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, ART_W - 1, ART_H - 1], outline=0, width=2)
    for cx, cy in ((0, 0), (ART_W - 12, 0), (0, ART_H - 12), (ART_W - 12, ART_H - 12)):
        d.rectangle([cx, cy, cx + 11, cy + 11], fill=0)

    # SAFE AREA: the centred band that survives even the tightest crop (a 6-row
    # attack grid). Anything outside it can be letterboxed away, so the real art
    # must keep its subject inside these marks. Drawn as dashed rules + a mid
    # grey so a flashed placeholder shows exactly what a crop would keep.
    top = (ART_H - MIN_CROP_H) // 2
    bot = top + MIN_CROP_H
    for yy in (top, bot):
        for xx in range(4, ART_W - 4, 16):
            d.rectangle([xx, yy - 1, xx + 8, yy], fill=96)
    d.text((8, top + 4), f"safe {MIN_CROP_H}px", fill=96)

    # The glyph, blown up from the bitmap default face so it reads at a glance.
    big = Image.new("L", (24, 24), 255)
    ImageDraw.Draw(big).text((7, 6), glyph, fill=0)
    img.paste(big.resize((96, 96), Image.NEAREST), (ART_W // 2 - 48, ART_H // 2 - 48))
    d.text((8, 10), label[:40], fill=0)
    d.text((8, ART_H - 20), "TOP/BOTTOM MAY BE CROPPED", fill=0)
    # A 16-step ladder inside the safe band: proves every grey the panel can show
    # survives quantise -> pack -> RLE -> the on-device decoder, and stays visible
    # at any crop.
    for i in range(16):
        d.rectangle([ART_W - 24 - i * 18, bot - 26, ART_W - 10 - i * 18,
                     bot - 8], fill=i * 17)
    return img


# Everything at or above this luma becomes true paper before quantisation.
#
# Generated art arrives as a photograph of a drawing: the "white" ground is
# 246..252 speckle, not 255. Quantised, that speckle alternates between level 14
# and 15 — visually identical to paper, but it destroys the RLE, because a run
# can only span bytes that are equal. Clamping the top end first is what took
# the reference plate from 27% to 84% pure-white and its encoded size from 96KB
# to 30KB, with no visible change: every pixel this moves was already reading as
# white on a 16-level panel.
#
# 225 is chosen to sit below the speckle floor and above the lightest real ink.
# Lower starts eating the faintest strokes; higher leaves speckle behind.
WHITE_POINT = 225


def flatten_paper(im):
    """Clamp near-white to paper, stretch what is left back over the range."""
    lut = [255 if v >= WHITE_POINT else int(v * 255 / WHITE_POINT)
           for v in range(256)]
    return im.point(lut)


# Paper-grain scans register as "ink" all the way to the edge at WHITE_POINT.
# A darker cut finds the actual subject; 160 sits above the lightest strokes
# we measured and well below the textured ground.
INK_BBOX_THR = 160


def ink_bbox(im):
    """Axis-aligned box of real strokes, ignoring grey paper grain."""
    mask = im.point(lambda v: 255 if v < INK_BBOX_THR else 0)
    bb = mask.getbbox()
    return bb if bb else (0, 0, im.width, im.height)


def fit_plate(im):
    """Trim sparse paper, then cover-fit — or letterbox if cover would clip.

    The 1-row plate is taller than most of the source drawings. Cover-fitting
    a wide subject (lizard ~2:1) into 492x620 would shave a third off each
    side; letterboxing keeps the animal whole and spends the leftover as
    paper, which is already the panel's ground.
    """
    x0, y0, x1, y1 = ink_bbox(im)
    bw, bh = x1 - x0, y1 - y0
    pad = max(8, int(0.04 * max(bw, bh)))
    x0 = max(0, x0 - pad)
    y0 = max(0, y0 - pad)
    x1 = min(im.width, x1 + pad)
    y1 = min(im.height, y1 + pad)
    crop = flatten_paper(im.crop((x0, y0, x1, y1)))
    bw, bh = crop.size
    cover = max(ART_W / bw, ART_H / bh)
    side = max(0.0, (bw * cover - ART_W) / 2.0 / (bw * cover))
    s = min(ART_W / bw, ART_H / bh) if side > MAX_COVER_SIDE else cover
    nw = max(1, round(bw * s))
    nh = max(1, round(bh * s))
    resized = crop.resize((nw, nh), Image.LANCZOS)
    plate = Image.new("L", (ART_W, ART_H), 255)
    plate.paste(resized, ((ART_W - nw) // 2, (ART_H - nh) // 2))
    return plate


def build(key, glyph, srcdir):
    """Load <srcdir>/<key>.png, fit to the plate, or synthesise a stand-in."""
    path = os.path.join(srcdir, key + ".png")
    if not os.path.exists(path):
        return placeholder(glyph, key), True
    return fit_plate(Image.open(path).convert("L")), False


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--src", default=os.path.join(root, "docs", "enemy-art"))
    ap.add_argument("--out", default=os.path.join(root, "src", "enemy_art_data.h"))
    ap.add_argument("--preview", default=os.path.join(root, "docs", "enemy-art", "final"))
    ap.add_argument("--dither", choices=["none", "bayer"], default="none")
    ap.add_argument("--preview-only", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.preview, exist_ok=True)
    subjects = [(sym, key, ch) for sym, key, ch in ENCOUNTERS]
    subjects += [("GLYPH_" + g, key, g) for g, key in GLYPHS]

    blobs, raw_total, nph = [], 0, 0
    for sym, key, glyph in subjects:
        img, is_ph = build(key, glyph, args.src)
        nph += is_ph
        nib = quantize(img, args.dither, ART_W, ART_H)
        Image.frombytes("L", (ART_W, ART_H), bytes(v * 17 for v in nib)).save(
            os.path.join(args.preview, f"{key}.png"))
        packed = pack4bpp(nib, ART_W, ART_H)
        comp = rle(packed)
        unrle(comp, len(packed))
        raw_total += len(packed)
        blobs.append((sym, key, comp, is_ph))
        print(f"  {key:<16} raw {len(packed):>6}  rle {len(comp):>6}  "
              f"{len(packed) / len(comp):.2f}x{'  [placeholder]' if is_ph else ''}")

    total = sum(len(b) for _, _, b, _ in blobs)
    print(f"  {'TOTAL':<16} raw {raw_total:>6}  rle {total:>6}  "
          f"{raw_total / total:.2f}x   ({nph}/{len(blobs)} placeholders)")
    if args.preview_only:
        print(f"previews -> {args.preview} (no header written)")
        return

    with open(args.out, "w", newline="\n") as f:
        f.write(HEADER_TOP.format(w=ART_W, h=ART_H, w2=ART_W // 2,
                                  n=len(ENCOUNTERS), g=len(GLYPHS),
                                  raw=raw_total, total=total,
                                  ratio=raw_total / total, ph=nph))
        for sym, key, blob, is_ph in blobs:
            f.write(f"\n// {key} — {len(blob)} bytes RLE"
                    f"{'  (PLACEHOLDER)' if is_ph else ''}\n")
            f.write(c_array("ENEMY_ART_" + sym, blob) + "\n")
        f.write("\n// Indexed by adr::EncounterId; nullptr = no art.\n")
        f.write(f"static const uint8_t* const ENEMY_ART[{len(ENCOUNTERS)}]"
                " __attribute__((unused)) = {\n")
        for sym, _, _ in ENCOUNTERS:
            f.write(f"    ENEMY_ART_{sym},\n")
        f.write("};\n")
        f.write("\n// Setpiece fallback, looked up by Combat.enemyChara.\n")
        f.write("static const struct { char ch; const uint8_t* art; }"
                f" ENEMY_ART_GLYPH[{len(GLYPHS)}] __attribute__((unused)) = {{\n")
        for g, _ in GLYPHS:
            f.write(f"    {{ '{g}', ENEMY_ART_GLYPH_{g} }},\n")
        f.write("};\n")
    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes of source)")


HEADER_TOP = """\
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Combat portraits: {n} random encounters (indexed by adr::EncounterId) plus {g}
// per-glyph fallbacks for setpiece enemies, which carry no id of their own.
// MACHINE GENERATED by tools/gen_enemy_art.py — DO NOT EDIT BY HAND.
//
// {ph} of these are still PLACEHOLDERS (framed box + glyph): the source drawings
// live outside the repo at docs/enemy-art/<key>.png and the generator
// synthesises a stand-in for every one that is missing, so the layout and the
// decoder can ship and be flown before the art lands. Drop a PNG in and re-run.
//
// Include from exactly ONE .cpp (fight_modal.cpp) — it carries the whole blob.
//
// FORMAT — identical to event_art_data.h, decoded by the same art::blit():
//   {w}x{h} pixels of 4-bit grey, 0 = ink black .. 15 = paper white, packed two
//   per byte (HIGH nibble = the LEFT pixel). {w} is even so a row is exactly {w2}
//   bytes with no padding. That stream is then run-length encoded:
//     0x00 <len> <len bytes>   literal run, len = 1..255
//     <n>  <val>               n copies of val, n = 1..255
//   {raw} bytes raw -> {total} bytes encoded ({ratio:.2f}x).
#pragma once
#include <stdint.h>

// See fight_modal.cpp for the vertical budget these dimensions come out of.
constexpr int ENEMY_ART_W = {w};
constexpr int ENEMY_ART_H = {h};
"""

if __name__ == "__main__":
    main()
