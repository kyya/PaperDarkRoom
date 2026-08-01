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
// A band is a rectangle carrying a 36px title, optionally a 24px subtitle under
// it (a cost/yield line — "-500 木头  -50 毛皮"), and optionally a draining
// cooldown bar along the bottom:
//
//   enabled  -> two concentric 2px strokes (drawRect at the edge + 1px inward)
//   disabled -> a single 1px dashed frame, 4px on / 4px off
//
//   title  : 36px = the 12px CJK grid x TITLE_SCALE(3) — the app-wide verb scale
//   subtitle: 24px = the same grid x SUB_SCALE(2), SUBGAP(6) below the title
//   the title+subtitle block is VERTICALLY CENTERED in the band
//
// ---- ONE BAND, ONE CENTERING (2026-08-01, decided at the device) -----------
// A band centres exactly what it carries, and nothing else:
//   subtitle present -> centre the 66px title+subtitle block
//   no subtitle      -> centre the lone 36px title
// There is no third mode. Every band in the firmware — grid cell or standalone
// — obeys those two lines, and draw() takes no flag to opt out of them.
//
// This DELIBERATELY overturns the v0.10.1 "reserve the subtitle's slot anyway"
// rule, which had every cell in a grid lay out as though it were priced so that
// a whole grid's titles shared one y. The user compared both on the physical
// panel and chose per-button optical centring — 「如果没有 subtitle 的内容 你就
// 居中那个 title!」 A lone title parked 15px above its band's optical centre,
// holding a line open for text that does not exist, reads as a broken button;
// that is worse than the mismatch the reservation was buying away.
//
// KNOWN COSTS — accepted on purpose. Do NOT "fix" these:
//   1. A MIXED grid steps. In Room's 96px cells a priced title's box lands at
//      +14 and a free one's at +29, so 添柴 (priced) sits 15px above 科技树
//      (free) in the same row. This is literally the v0.10.1 complaint
//      ("添柴/火把按钮文字高度不一致") coming back, by choice.
//   2. A title MOVES when its subtitle comes and goes. Outside's 查看陷阱 drops
//      its "-N 诱饵" line when no bait is held, so its title steps 15px down and
//      back as bait runs out and is restocked.
// Both were weighed against the alternative in front of the user. Reintroducing
// a reservation mode is a fresh product decision, not a bug fix.
//
// ---- THE OPTICAL NUDGE (this replaced a magic "-4") ------------------------
// Eight call sites used to centre a lone title with `top + (h - GLYPH)/2 - 4`.
// The 4 was never derived and did not scale, so a 36px band and a 24px band got
// the same nudge. The real number falls out of the font: a full-height CJK
// glyph's ink occupies rows [1,12) of the 12px grid cell (cjk_font12.h gives
// yoff=-10, h=11 against cjk_text.cpp's ASCENT=11), i.e. the ink is one grid
// pixel SHORTER than its line box and hugs the box's BOTTOM. Centering the BOX
// therefore leaves the ink half a grid pixel low; lifting the box by half a
// grid pixel — INK_NUDGE, TITLE_SCALE/2 = 1px at 36px — re-centres the ink.
// Every band now uses that one derived value, so the old -4 sites all shift
// ~3px DOWN together and the old no-nudge sites (Trade's priced bands) shift
// 1px up. Consistency across the app is the point; neither shift is a bug fix.
//
// ---- NARROW-CELL AUTO-DOWNGRADE -------------------------------------------
// A 240px grid cell can only show so much 36px text. draw() measures the title
// (cjk::textWidth) against the cell's usable width — w minus the 2px frame and
// SIDE_PAD(4) on each side, so 228px in a 240px cell — and falls back to the
// 24px scale for that ONE title if it would overflow. It is a guard, not a
// layout mode: measured over every label the game can currently produce, the
// widest 36px title in a 240px cell is Room's "更多 (n/n)" pager at 180px (216px
// if the batch count ever reached double digits) and the widest real name is
// 狩猎小屋 / 查看陷阱 / 漫漫尘途 at 144px, so NOTHING downgrades today. The
// title's box stays 36px tall either way (see draw()) — a shrunk title centres
// INSIDE that box — so a downgrade never moves the subtitle or the block; it
// only shrinks the ink.
#pragma once
#include "page.h"          // pages::Rect — the app's one button-rect type

namespace m5gfx { class M5Canvas; }

namespace action_band {

// ---- the contract, as numbers ---------------------------------------------
constexpr int TITLE_SCALE = 3;                    // 12px grid x3
constexpr int TITLE_GLYPH = 12 * TITLE_SCALE;     // 36px title line box
constexpr int SUB_SCALE   = 2;                    // 12px grid x2
constexpr int SUB_GLYPH   = 12 * SUB_SCALE;       // 24px subtitle line box
constexpr int SUBGAP      = 6;                    // title box -> subtitle box
constexpr int FRAME       = 2;                    // enabled double-ring thickness
constexpr int SIDE_PAD    = 4;                    // min clear space each side
constexpr int INK_NUDGE   = TITLE_SCALE / 2;      // 1 — see the header note
constexpr int BAR_GUTTER  = 12;                   // band bottom -> cooldown bar top
constexpr int BAR_H       = 8;                    // cooldown bar height

// Draw one band into `r`.
//   title          — 36px, centered; auto-shrunk to 24px only if it overflows
//   subtitle       — 24px cost/yield line under it; nullptr or "" = none, and
//                    then the title centres ALONE (see ONE BAND, ONE CENTERING).
//                    Wraps to a 2nd line if it must (Room's priciest
//                    multi-resource crafts are the only candidates); see the
//                    derivation in action_band.cpp draw() for why 2 lines still
//                    fit a 96px band
//   enabled        — solid double frame vs the 1px dashed unavailable frame
//   coolLeft/Total — both > 0 draws a left-anchored draining bar in the band's
//                    bottom gutter. Mutually exclusive with a 2-LINE subtitle by
//                    construction: the only coolable actions (生火/添柴/伐木/
//                    查看陷阱) all price in exactly one resource, and no
//                    craftable — the only 2-line candidate — has a cooldown.
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
