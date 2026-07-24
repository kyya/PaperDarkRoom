// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Shared long-press action-band renderer for the Room/Outside two-column
// (240px) action grids (room_page.cpp's craft/build grid, outside_page.cpp's
// 伐木/查看陷阱/分工/尘土之路 row) — v0.10.1 ("按钮排版统一"). Both pages
// draw the same shape: a framed cell whose label optionally carries a small
// cost/yield subtitle and, while cooling, a draining progress bar. Lives in
// its own module (rather than room_page.cpp, this logic's original home)
// because outside_page.cpp needs the identical thing — see room_page.cpp for
// the full baseline-consistency rationale.
//
// THE RULE (fixes "添柴/火把主标题高度不一致" + "第一页按钮没对齐"): every
// band's label draws at the SAME fixed y offset from its own top — labelY(h)
// — regardless of whether it carries a subtitle, is coolable, or currently
// draws a cooldown bar. A subtitle (when present) instead bottom-anchors at a
// fixed offset from the band's BOTTOM edge (SUB_GUTTER, always reserved on a
// priced band so a cooldown bar never fights the text). Only a rare 2-line
// subtitle is allowed to push the label ABOVE its usual baseline to make
// room — the label never moves down, and a subtitle's own bottom edge never
// moves at all. See action_band.cpp draw() for the derivation.
#pragma once

namespace m5gfx { class M5Canvas; }

namespace action_band {

// v0.10.1 ("主标题偏大" feedback): both the label and the subtitle render at
// 24px (12px grid x2) — shrunk from the app-wide 36px verb-label convention
// (see room_page.cpp's header note for why Room/Outside's STACKED label-over-
// subtitle layout needs its own smaller scale where assign/path/trade's
// side-by-side name+count doesn't).
constexpr int SCALE      = 2;
constexpr int GLYPH      = 12 * SCALE;        // 24px line box (subtitle)
constexpr int BTN_SCALE  = 2;
constexpr int BTN_GLYPH  = 12 * BTN_SCALE;    // 24px line box (label)
constexpr int SUBGAP     = 4;                 // label -> subtitle line gap
constexpr int SUB_GUTTER = 16;                // bottom strip a PRICED band
                                              // always reserves (whether or
                                              // not it's ALSO coolable) for
                                              // its cost/yield line + a
                                              // cooldown bar below it

// The fixed label baseline (y offset from a band's own top) for the common
// case — no subtitle, or a 1-line subtitle. Every band of height `h` uses
// this SAME offset; see action_band.cpp draw() for how a 2-line subtitle is
// the one case allowed to move the label off it.
int labelY(int h);

// Draw one band at (x0,top), sized w x h. `enabled` picks the solid
// double-ring frame vs the 1px dashed "unavailable" frame; `hasCost` gates
// the subtitle (drawn from `cost`, up to a 2-line wrap); a live cooldown
// (coolTotal > 0 && coolLeft > 0) draws a thin draining bar, hanging just
// below the (fixed) subtitle baseline on a priced band or hugging the band's
// own bottom edge on a free one.
void draw(m5gfx::M5Canvas& c, int x0, int top, int w, int h,
          const char* label, const char* cost, bool hasCost, bool enabled,
          int coolLeft, int coolTotal);

}  // namespace action_band
