#!/usr/bin/env python3
"""TTF -> Adafruit/LovyanGFX GFXfont C header, rasterized to MATCH the daemon.

The firmware self-draws its client pages (clock header, pomodoro buttons/
countdown) and must render the SAME glyphs the daemon's Pillow pages do
(daemon/card_render_pixel.py). Both sides therefore share one rasterization
recipe, implemented here so a firmware font is byte-for-byte the panel image
the host would have pushed:

  * PIL ``ImageDraw.fontmode = "1"`` — no grayscale anti-aliasing, so every
    font pixel is a hard 1-bit block (the daemon sets the same on every draw).
  * Glyph metrics are the MEASURED ink bbox, never the TTF's declared metrics
    (Minecraftia's are unreliable — see minecraftia16.h's header note).
  * Baseline = the bottom ink row of '0' + 1 (the row just under the digit),
    so every digit sits on a shared baseline exactly as minecraftia16.h defines
    it. '0' is rendered for the baseline even when it isn't in the charset.
  * xAdvance = round(font.getlength(ch)) — the face's real advance, so
    multi-glyph strings (HH:MM, MM:SS, "min") space like the daemon's.
  * xOffset = measured left bearing, yOffset = ink_top - baseline.

GFXglyph row order matches LovyanGFX: {bitmapOffset, width, height, xAdvance,
xOffset, yOffset}. Bitmaps are 1bpp MSB-first, packed continuously across the
whole glyph (byte-aligned only at each glyph's end) — the Adafruit convention
LovyanGFX consumes.

Fresh mode writes a whole header for a charset. ``--extend BASE.h`` instead
PRESERVES an existing header's bitmap array + glyph rows verbatim and only
APPENDS the charset's not-yet-present glyphs (used to grow minecraftia16.h with
weekday letters without disturbing a single existing byte/metric — status_bar
renders the old glyphs unchanged). Every run also drops a proof PNG that paints
the WHOLE charset back out of the emitted glyph data (a round-trip check).

Usage:
  gen_fw_fonts.py --ttf F.ttf --px N --charset STR --name IDENT --out OUT.h
                  [--extend BASE.h] [--proof PROOF.png]
"""
from __future__ import annotations

import argparse
import os
import re

from PIL import Image, ImageDraw, ImageFont


def _canvas(px):
    # Generous margins so no ascender/descender clips; origin well inside.
    side = px * 8
    org = (px * 2, px * 3)
    return side, org


def rasterize(font, ch, px):
    """Render one glyph with hard 1-bit edges; return (ink_bbox, PIL image).
    ink_bbox is (l, t, r, b) with r/b EXCLUSIVE (PIL getbbox), or None if the
    glyph inks nothing (e.g. space)."""
    side, org = _canvas(px)
    im = Image.new("L", (side, side), 0)
    d = ImageDraw.Draw(im)
    d.fontmode = "1"
    d.text(org, ch, font=font, fill=255)
    return im.getbbox(), im, org


def measure_baseline(font, px):
    """Baseline row = the row just below '0's bottom ink (the shared digit
    baseline minecraftia16.h documents). Rendered regardless of charset."""
    bb, _, _ = rasterize(font, "0", px)
    if bb is None:
        # No '0' in the face — fall back to PIL's own baseline (ascent below
        # the "la" top). Keeps the tool usable for digit-less faces.
        _, org = _canvas(px)
        return org[1] + font.getmetrics()[0]
    return bb[3]        # bb[3] is exclusive bottom => first row below the ink


def pack_bits(im, bbox):
    """Pack the ink crop as 1bpp MSB-first, continuous across rows, padded to a
    whole byte at the glyph's end (Adafruit GFXfont layout)."""
    l, t, r, b = bbox
    bits = []
    px = im.load()
    for y in range(t, b):
        for x in range(l, r):
            bits.append(1 if px[x, y] >= 128 else 0)
    out = bytearray()
    for i in range(0, len(bits), 8):
        chunk = bits[i:i + 8]
        chunk += [0] * (8 - len(chunk))
        byte = 0
        for bit in chunk:
            byte = (byte << 1) | bit
        out.append(byte)
    return bytes(out)


def build_glyph(font, ch, px, baseline):
    """Return (bitmap_bytes, width, height, xadvance, xoffset, yoffset) for ch."""
    bb, im, org = rasterize(font, ch, px)
    xadv = round(font.getlength(ch))
    if bb is None:                       # empty (space): zero box, real advance
        return b"", 0, 0, xadv, 0, 0
    l, t, r, b = bb
    w, h = r - l, b - t
    xoff = l - org[0]
    yoff = t - baseline
    return pack_bits(im, bb), w, h, xadv, xoff, yoff


# ---- fresh header -----------------------------------------------------------

def _fmt_bytes(data, per_line=12, indent="    "):
    lines = []
    for i in range(0, len(data), per_line):
        row = ", ".join(f"0x{b:02X}" for b in data[i:i + per_line])
        lines.append(indent + row + ",")
    return "\n".join(lines)


def emit_fresh(name, ttf, px, charset, header_note):
    font = ImageFont.truetype(ttf, px)
    baseline = measure_baseline(font, px)
    cps = sorted(ord(c) for c in charset)
    first, last = cps[0], cps[-1]
    present = set(cps)

    bitmaps = bytearray()
    rows = []            # (cp, off, w, h, xadv, xoff, yoff, char)
    for cp in range(first, last + 1):
        if cp in present:
            data, w, h, xadv, xoff, yoff = build_glyph(font, chr(cp), px, baseline)
            off = len(bitmaps)
            bitmaps += data
            rows.append((cp, off, w, h, xadv, xoff, yoff, chr(cp)))
        else:
            # Gap inside the range: zero-size filler at the current offset,
            # default advance 4 (the minecraftia16.h convention).
            rows.append((cp, len(bitmaps), 0, 0, 4, 0, 0, None))

    return _render_header(name, px, bytes(bitmaps), rows, first, last,
                          header_note), bitmaps, rows, baseline


def _row_line(cp, off, w, h, xadv, xoff, yoff, ch):
    label = f"0x{cp:02X}"
    if ch is not None and 0x20 <= cp < 0x7F:
        label += f" '{ch}'"
    return (f"    {{ {off:5d}, {w:2d}, {h:2d}, {xadv:2d}, {xoff:2d}, {yoff:3d} }},"
            f"   // {label}")


def _render_header(name, px, bitmaps, rows, first, last, header_note):
    guard = name.upper() + "_H"
    body = []
    body.append(header_note.rstrip("\n"))
    body.append("#pragma once")
    body.append("#include <M5GFX.h>")
    body.append("")
    body.append(f"static const uint8_t {name}Bitmaps[] = {{")
    body.append(_fmt_bytes(bitmaps))
    body.append("};")
    body.append("")
    body.append("// { bitmapOffset, width, height, xAdvance, xOffset, yOffset }")
    body.append(f"static const lgfx::v1::GFXglyph {name}Glyphs[] = {{")
    for r in rows:
        body.append(_row_line(*r))
    body.append("};")
    body.append("")
    body.append(f"static const lgfx::v1::GFXfont {name} = {{")
    body.append(f"    (uint8_t*){name}Bitmaps,")
    body.append(f"    (lgfx::v1::GFXglyph*){name}Glyphs,")
    body.append(f"    0x{first:02X}, 0x{last:02X}, {px},")
    body.append("};")
    body.append("")
    return "\n".join(body)


# ---- proof PNG (round-trip from emitted glyph data) -------------------------

def emit_proof(path, name, px, bitmaps, rows, baseline_note, charset):
    scale = max(2, 48 // px + 1)
    cell_w = px * 3
    cell_h = px * 3
    per = 8
    glyphs = [r for r in rows if r[7] is not None and r[2] > 0]
    # Always include space/empty visually? Skip zero-size in the grid.
    n = len(glyphs)
    cols = min(per, n) or 1
    import math
    rowsN = math.ceil(n / cols)
    W = cols * cell_w * scale + 40
    H = rowsN * cell_h * scale + 60
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)
    for idx, (cp, off, w, h, xadv, xoff, yoff, ch) in enumerate(glyphs):
        gc = idx % cols
        gr = idx // cols
        # cell origin (baseline-relative). Place baseline at 2/3 down the cell.
        ox = 20 + gc * cell_w * scale
        oy = 40 + gr * cell_h * scale
        base_y = oy + int(cell_h * scale * 0.72)
        pen_x = ox + int(cell_w * scale * 0.30)
        # baseline guide
        d.line([ox, base_y, ox + cell_w * scale - 8, base_y], fill=(210, 210, 210))
        d.line([pen_x, oy, pen_x, oy + cell_h * scale - 8], fill=(230, 230, 230))
        # decode packed bits
        nbits = w * h
        glyph_bytes = bitmaps[off:off + (nbits + 7) // 8]
        bit = 0
        for yy in range(h):
            for xx in range(w):
                byte = glyph_bytes[bit >> 3]
                on = (byte >> (7 - (bit & 7))) & 1
                bit += 1
                if on:
                    px0 = pen_x + (xoff + xx) * scale
                    py0 = base_y + (yoff + yy) * scale
                    d.rectangle([px0, py0, px0 + scale - 1, py0 + scale - 1],
                                fill=(0, 0, 0))
        d.text((ox, oy - 14), f"{ch} {w}x{h}", fill=(0, 0, 160))
    d.text((10, H - 16), f"{name}  px={px}  {baseline_note}", fill=(0, 0, 0))
    img.save(path)
    return W, H


# ---- extend an existing header (preserve verbatim, append the new glyphs) ---

_ARR_RE = r"static const uint8_t {name}Bitmaps\[\] = \{{(.*?)\n\}};"
_GLY_RE = r"static const lgfx::v1::GFXglyph {name}Glyphs\[\] = \{{(.*?)\n\}};"
_FONT_RE = r"(static const lgfx::v1::GFXfont {name} = \{{.*?0x[0-9A-Fa-f]+, )0x([0-9A-Fa-f]+)(, \d+,)"


def _parse_base(text, name):
    m = re.search(_ARR_RE.format(name=re.escape(name)), text, re.S)
    if not m:
        raise SystemExit(f"could not find {name}Bitmaps[] in base header")
    arr_body = m.group(1)
    bytes_list = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", arr_body)]
    g = re.search(_GLY_RE.format(name=re.escape(name)), text, re.S)
    if not g:
        raise SystemExit(f"could not find {name}Glyphs[] in base header")
    gly_body = g.group(1)
    # parse each row's cp from its trailing comment 0xNN
    rows = {}
    for rm in re.finditer(
        r"\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\}\s*,"
        r"\s*//\s*0x([0-9A-Fa-f]+)", gly_body):
        off, w, h, xadv, xoff, yoff, cp = rm.groups()
        rows[int(cp, 16)] = (int(off), int(w), int(h), int(xadv), int(xoff), int(yoff))
    fm = re.search(_FONT_RE.format(name=re.escape(name)), text, re.S)
    if not fm:
        raise SystemExit(f"could not find {name} GFXfont struct in base header")
    last = int(fm.group(2), 16)
    return bytes_list, rows, last


def extend_header(base_path, name, ttf, px, charset, header_note):
    """Grow base_path's font with the charset's not-yet-real glyphs, preserving
    every existing byte and glyph row exactly; return the merged header text and
    (bitmaps, rows) for the proof."""
    text = open(base_path, encoding="utf-8").read()
    base_bytes, base_rows, base_last = _parse_base(text, name)
    baseline = measure_baseline(ImageFont.truetype(ttf, px), px)
    font = ImageFont.truetype(ttf, px)

    bitmaps = bytearray(base_bytes)
    # which charset cps need generating: not present, or present only as a
    # zero-size filler (w==h==0). Existing REAL glyphs are left untouched.
    want = sorted(set(ord(c) for c in charset))
    new_real = {}
    # Safety: re-generate charset cps that ARE real in base and assert identity.
    for cp in want:
        r = base_rows.get(cp)
        is_real = r is not None and (r[1] > 0 or r[2] > 0)
        data, w, h, xadv, xoff, yoff = build_glyph(font, chr(cp), px, baseline)
        if is_real:
            _off, bw, bh, bxadv, bxoff, byoff = r
            if (bw, bh, bxadv, bxoff, byoff) != (w, h, xadv, xoff, yoff):
                raise SystemExit(
                    f"REGEN MISMATCH for 0x{cp:02X} {chr(cp)!r}: base "
                    f"{(bw,bh,bxadv,bxoff,byoff)} != new {(w,h,xadv,xoff,yoff)}")
            continue
        off = len(bitmaps)
        bitmaps += data
        new_real[cp] = (off, w, h, xadv, xoff, yoff)

    new_last = max(base_last, max(want))
    # assemble dense rows first..new_last
    first = min(base_rows) if base_rows else min(want)
    rows = []
    cur_fill_off = len(base_bytes)      # fillers between base_last..new_last
    for cp in range(first, new_last + 1):
        if cp in new_real:
            off, w, h, xadv, xoff, yoff = new_real[cp]
            rows.append((cp, off, w, h, xadv, xoff, yoff, chr(cp)))
        elif cp in base_rows:
            off, w, h, xadv, xoff, yoff = base_rows[cp]
            ch = chr(cp) if (w or h) else None
            rows.append((cp, off, w, h, xadv, xoff, yoff, ch))
        else:
            rows.append((cp, len(bitmaps), 0, 0, 4, 0, 0, None))

    header = _render_header(name, px, bytes(bitmaps), rows, first, new_last,
                            header_note)
    return header, bytes(bitmaps), rows, baseline, new_real


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ttf", required=True)
    ap.add_argument("--px", type=int, required=True)
    ap.add_argument("--charset", required=True)
    ap.add_argument("--name", required=True, help="C identifier, e.g. VCR_OSD_60")
    ap.add_argument("--out", required=True)
    ap.add_argument("--extend", default=None, help="base .h to preserve + append into")
    ap.add_argument("--proof", default=None)
    ap.add_argument("--note", default=None, help="header comment block")
    args = ap.parse_args()

    note = args.note or f"// Generated by tools/gen_fw_fonts.py from {os.path.basename(args.ttf)}\n"

    if args.extend:
        header, bitmaps, rows, baseline, new_real = extend_header(
            args.extend, args.name, args.ttf, args.px, args.charset, note)
        print(f"extended {args.name}: +{len(new_real)} glyphs "
              f"({', '.join(chr(c) for c in sorted(new_real))})")
    else:
        header, bitmaps, rows, baseline = emit_fresh(
            args.name, args.ttf, args.px, args.charset, note)
        nglyph = sum(1 for r in rows if r[7] is not None and (r[2] or r[3]))
        print(f"fresh {args.name}: {len([r for r in rows if r[2]>0])} inked glyphs, "
              f"{len(bitmaps)} bitmap bytes")

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
    print(f"wrote {args.out} ({len(bitmaps)} bitmap bytes, baseline row {baseline})")

    if args.proof:
        W, H = emit_proof(args.proof, args.name, args.px, bitmaps, rows,
                          f"baseline={baseline}", args.charset)
        print(f"proof {args.proof} ({W}x{H})")


if __name__ == "__main__":
    main()
