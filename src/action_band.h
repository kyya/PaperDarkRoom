// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// THE long-press button band — the single renderer behind EVERY framed button
// in the firmware. Room/Outside's 240px action grids, the Trade page's buy
// bands, the event/setpiece modal choice bands, the fight modal's attack grid
// and victory band, and the 返回/更多/出发 bands on tech/assign/path all come
// through action_band::draw. Nothing else may draw a button frame.
//
// WHY ONE MODULE (v0.12, "带 subtitle 的按钮样式统一成贸易站的按钮组件"):
// before this, eight files each carried their own near-copy of the same band —
// the frames agreed, but the label scale (24px vs 36px), the label/subtitle gap
// (4 vs 6), the vertical centering (bottom-anchored vs centered), and even the
// dashed-frame helper (7 verbatim copies across two drawing primitives) had all
// drifted apart page by page. The Trade page's buy band was the one everybody
// agreed looked right, so ITS geometry is now the contract below and every
// other site was migrated onto it. A new button MUST call this; adding a ninth
// private copy is how the drift started.
//
// ---- THE VISUAL CONTRACT ---------------------------------------------------
// A band is a rectangle carrying a 36px title, optionally one or more 24px
// cost/yield lines, and optionally a draining cooldown bar along the bottom:
//
//   enabled  -> two concentric 2px strokes (drawRect at the edge + 1px inward)
//   disabled -> a single 1px dashed frame, 4px on / 4px off
//
// ---- LEFT TITLE / RIGHT COSTS — "变体 B" (2026-08-01, chosen at the device) --
// A band WITH cost lines splits into two columns:
//
//   +--------------------------------------------------------+
//   |  狩猎小屋                                   -200 木头   |
//   |                                              -10 毛皮   |
//   +--------------------------------------------------------+
//
//   * the 36px title is LEFT-aligned, EDGE_PAD in from the band's left edge
//   * EVERY cost/yield entry gets its OWN 24px line, RIGHT-aligned EDGE_PAD in
//     from the right edge — no more "-200 木头  -10 毛皮" run together on one
//   * that right column as a whole is VERTICALLY CENTRED in the band
//   * the title's y is ALWAYS titleBoxY(top, h)
//
// A band with NO cost lines centres its title HORIZONTALLY (the user's explicit
// instruction) at that same y — so every subtitle-less button in the firmware
// (返回 / 更多 / 出发 / 分工 / 科技树 / the whole fight grid) looks exactly as it
// did before this change. Only priced bands moved.
//
// WHY THIS LAYOUT WON — one title y, everywhere. Every earlier layout centred a
// title+subtitle BLOCK, which made a title's y depend on what sat under it, and
// that produced three separate reported bugs in three months:
//   * v0.10.0: a coolable band shrank its block, so 添柴 sat higher than every
//     other priced button ("添柴和火把按钮文字高度不一致").
//   * v0.12: reserving a subtitle slot fixed that but parked lone titles 15px
//     high, which read as broken ("如果没有 subtitle 的内容 你就居中那个 title").
//   * v0.13: dropping the reservation fixed THAT but reintroduced a 15px step
//     across mixed rows, and made Outside's 查看陷阱 jump 15px every time bait
//     ran out and its cost line vanished.
// Splitting the axes ends the whole family: the title owns the left column and a
// fixed y, the costs own the right column and grow DOWNWARD from a centred
// block. A cost line appearing or disappearing can no longer move a title, and
// titleBoxY() is now the single y for standard bands, subtitle-less bands and
// assign/path's stepper-row names alike. Do not reintroduce a stacked block.
//
// ---- THE OPTICAL NUDGE (this replaced a magic "-4") ------------------------
// Eight call sites used to centre a lone title with `top + (h - GLYPH)/2 - 4`.
// The 4 was never derived and did not scale, so a 36px band and a 24px band got
// the same nudge. The real number falls out of the font: a full-height CJK
// glyph's ink occupies rows [1,12) of the 12px grid cell (cjk_font12.h gives
// yoff=-10, h=11 against cjk_text.cpp's ASCENT=11), i.e. the ink is one grid
// pixel SHORTER than its line box and hugs the box's BOTTOM. Centering the BOX
// therefore leaves the ink half a grid pixel low; lifting the box by half a
// grid pixel — inkNudge(scale) = scale/2 — re-centres the ink. The title column
// and the cost column each apply it at their own scale (both land on 1px today).
//
// ---- NARROW-CELL AUTO-DOWNGRADE -------------------------------------------
// Two columns in a 240px grid cell is a tight budget, so draw() measures the
// real thing: the 36px title, plus MID_GAP, plus the WIDEST cost line, against
// the usable width (w - 2*EDGE_PAD = 224px in a 240px cell). If that overflows,
// THAT ONE title falls back to the 24px scale. Unlike the pre-变体-B guard —
// which measured the title alone and therefore never fired — this one genuinely
// triggers on the narrow Room/Outside grid; see the measured list in
// action_band.cpp. The title's BOX stays 36px tall either way, and a shrunk
// title centres its ink inside that box, so a downgrade never moves the title's
// y or the cost column.
#pragma once
#include "page.h"          // pages::Rect — the app's one button-rect type

namespace m5gfx { class M5Canvas; }

namespace action_band {

// ---- the contract, as numbers ---------------------------------------------
constexpr int TITLE_SCALE = 3;                    // 12px grid x3
constexpr int TITLE_GLYPH = 12 * TITLE_SCALE;     // 36px title line box
constexpr int SUB_SCALE   = 2;                    // 12px grid x2
constexpr int SUB_GLYPH   = 12 * SUB_SCALE;       // 24px subtitle line box
constexpr int FRAME       = 2;                    // enabled double-ring thickness
// Left/right inner padding: how far the left-aligned title and the right-aligned
// cost lines sit in from the band's own edges. 8 is not arbitrary — it is
// assign_page/path_page's LABEL_X, the inset their stepper rows have always used
// for a left-aligned 36px name. Matching it means a Room cell's title, a Trade
// band's title and an AssignPage job name all start on the SAME x offset within
// their band, so the whole app reads as one grid rather than three.
constexpr int EDGE_PAD    = 8;
// Minimum clear space between the title column and the cost column. Deliberately
// the SAME 8, so a band's three horizontal gaps — left edge, middle, right edge
// — are one repeated unit rather than three tuned numbers. It is also exactly
// the value that makes the downgrade guard fire on the four titles that really
// need it and no others: at 12 the marginal 3-glyph names (贸易站/制革屋/熏肉房/
// 双肩包, each 108px against a 108px cost) overflow 224 by 4px and shrink for no
// visible benefit, while at 8 they fit at 224 exactly. See the measured list in
// action_band.cpp.
constexpr int MID_GAP     = EDGE_PAD;
// Ink-vs-box optical correction at a given scale (see THE OPTICAL NUDGE).
constexpr int inkNudge(int scale) { return scale / 2; }
constexpr int INK_NUDGE   = inkNudge(TITLE_SCALE);   // 1 — the title column's

constexpr int MAX_SUB_LINES = 4;                  // cost tables are cost[3]; the
                                                  // 4th slot absorbs any tail
constexpr int BAR_GUTTER  = 12;                   // band bottom -> cooldown bar top
constexpr int BAR_H       = 8;                    // cooldown bar height

// Draw one band into `r`.
//   title          — 36px. LEFT-aligned when `subtitle` is present, otherwise
//                    horizontally CENTRED. Its y is titleBoxY(r.y, r.h) either
//                    way. Auto-shrunk to 24px only when the title + MID_GAP +
//                    the widest cost line will not fit (see the header).
//   subtitle       — the page's cost/yield string, entries joined by TWO spaces
//                    ("-200 木头  -10 毛皮") exactly as every costLine already
//                    emits it. draw() SPLITS it on that separator and stacks the
//                    entries as separate right-aligned 24px lines, so no caller
//                    had to change its signature to get the new layout.
//                    nullptr or "" = no cost column at all.
//   enabled        — solid double frame vs the 1px dashed unavailable frame
//   coolLeft/Total — both > 0 draws a left-anchored draining bar in the band's
//                    bottom gutter. The cost column is centred, so with the most
//                    entries any table can produce (3) it ends exactly at the
//                    bar's top edge in a 96px band — see the budget in draw().
void draw(m5gfx::M5Canvas& c, const pages::Rect& r, const char* title,
          const char* subtitle, bool enabled, int coolLeft, int coolTotal);

// Just the frame — solid double ring when `enabled`, the 1px dashed
// unavailable frame when not. Exported for assign_page/path_page: their
// worker/outfit rows put a 36px name and a 24px count SIDE BY SIDE on one
// baseline (plus a ▲/▼ stepper) instead of stacking title over subtitle, so
// they lay out their own content — but the box around it must be the same box
// every other button draws, and used to be seven separate copies of it. This is
// the ONLY way to get either frame from outside; the dashed-rectangle routine
// itself is private to action_band.cpp, because "solid or dashed" is the whole
// decision a caller ever needs to make and exposing the halves separately is
// how the copies got started.
void drawFrame(m5gfx::M5Canvas& c, const pages::Rect& r, bool enabled);

// The y a lone 36px title's LINE BOX takes in a band of height `h` starting at
// `top` — centred, with INK_NUDGE applied. This is the SAME expression draw()
// uses for a subtitle-less band (draw() calls it), so an assign/path stepper row
// — whose 36px name sits beside its counts rather than above them, and which is
// therefore subtitle-less in the layout sense — lands its name on exactly the y
// a plain 返回/更多/出发 band lands its title. Exported so those two pages align
// to that one expression instead of re-deriving it with their own fudge factor,
// which is how the old "-4" spread to eight files.
int titleBoxY(int top, int h);

}  // namespace action_band
