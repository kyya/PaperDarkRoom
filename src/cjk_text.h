// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// CJK + ASCII text rendering for A Dark Room. UTF-8 in; every codepoint is
// blitted from ONE sparse 12px bitmap face (cjk_font12.h, generated from
// Fusion Pixel over the strings_zh.h glyph closure). Baking ASCII into the same
// face means a mixed row ("wood 木头 x 42") shares a single baseline with no
// cross-font drift — the research.md §8.4 recommended P1 path.
#pragma once
#include <stdint.h>

namespace m5gfx { class M5Canvas; }

namespace cjk {

// Draw utf8 with its line-box TOP-LEFT at (x, y). scale multiplies the 12px
// grid (scale=2 -> 24px CJK). color is RGB565 (0 = black). Returns the pen x
// after the last glyph (for chaining runs on one baseline).
int drawText(m5gfx::M5Canvas& canvas, int x, int y, const char* utf8,
             int scale = 1, uint16_t color = 0);

// Pixel width of utf8 at scale, no drawing (centering / wrap measurement).
int textWidth(const char* utf8, int scale = 1);

// Word-wrap utf8 into width w, line-box top-left at (x, y). CJK breaks between
// any two ideographs/marks; ASCII words break only at spaces. line_h is the
// per-line vertical advance in px (0 -> 15*scale). Returns the y below the last
// line (top of where the next block would go).
int drawWrapped(m5gfx::M5Canvas& canvas, int x, int y, int w, const char* utf8,
                int scale = 1, int line_h = 0, uint16_t color = 0);

}  // namespace cjk

// Official A Dark Room translation lookup (strings_zh.h). Binary search over
// the en_key-sorted table; returns the UTF-8 zh string, or en_key itself when
// absent (so a missing translation degrades to English, never a crash).
const char* tr(const char* en_key);
