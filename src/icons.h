// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Blitter for the 1bpp UI glyphs in icons_data.h (rasterised from Lucide by
// tools/gen_icons.py). Separate from art_blit's 4bpp/RLE plates because these
// are a different thing: tiny, opaque, single-colour marks that sit inside a
// button, not greyscale pictures — 1 bit per pixel keeps a 34x34 icon at 170
// bytes and lets the caller pick the ink so a disabled band can grey its glyph.
#pragma once
#include <stdint.h>
#include <M5GFX.h>

namespace icons {

// Draw a 1bpp glyph with its TOP-LEFT at (x, y). `stride` is bytes per row,
// MSB = leftmost pixel; set bits are painted in `ink`, clear bits are left
// alone so the glyph composites onto whatever is already there.
void draw(m5gfx::M5Canvas& c, const uint8_t* bits, int w, int h, int stride,
          int x, int y, uint16_t ink);

// Same, centred on (cx, cy) — what a button column almost always wants.
void drawCentred(m5gfx::M5Canvas& c, const uint8_t* bits, int w, int h,
                 int stride, int cx, int cy, uint16_t ink);

}  // namespace icons
