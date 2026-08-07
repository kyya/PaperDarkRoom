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
//   enabled  -> a 2px black ROUNDED ring: two concentric drawRoundRect strokes,
//               the inner one inset 1px and at CORNER_R-1 so the arcs nest
//   disabled -> the SAME rounded outline as a single 1px stroke in light grey,
//               with the title and cost text dropped to a darker grey
//
// v0.16 replaced the square double-drawRect / black dashed-square pair with the
// above. The dashes were the only "off" signal a disabled band had, so they had
// to shout; carrying the state in TONE instead frees the shape to be the same in
// both states, which is what makes a mixed Room grid stop looking like two
// different widgets. Dashes could not have survived the corners anyway — a
// 4-on/4-off run around an arc reads as damage, not as a dashed line.
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
// the usable width (w - 2*EDGE_PAD = 208px in a 240px cell). If that overflows,
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
// Corner radius of the OUTER frame stroke; the inner stroke uses CORNER_R-1 (see
// drawFrame). 8 against the shortest band the app draws (80px, the fight grid and
// Trade) is a tenth of the height — enough to read as intentionally rounded at
// arm's length on a 540px-wide panel, and still far from the pill a radius near
// h/2 would give. The 36px title's line box is centred, so no radius in this
// range can clip a glyph: even the widest arc only eats the corner 8px, and the
// title starts EDGE_PAD=16 in with 22px of vertical slack above it.
constexpr int CORNER_R    = 8;
// Left/right inner padding: how far the left-aligned title and the right-aligned
// cost lines sit in from the band's own edges. 8 through v0.16, matching
// assign_page/path_page's LABEL_X so a Room cell's title, a Trade band's title and
// an AssignPage job name all started on the same x offset within their band. On
// device that read as CRAMPED — the rounded frame pulls the corner in ~8px of its
// own, so a title at 8 sits right against the arc ("标题贴框太近"). 12 was tried
// and reported STILL too tight; this is 16, the top of the range the panel can
// carry (see MID_GAP for what pays for it). assign/path keep LABEL_X 8: their rows
// carry a name and a count side by side with a stepper on the right, a budget with
// nothing spare, and they are their own column of bands rather than neighbours of
// a Room cell — so the divergence never shows up in one field of view.
constexpr int EDGE_PAD    = 16;
// Minimum clear space between the title column and the cost column. It used to be
// pinned to EDGE_PAD so a band's three horizontal gaps — left edge, middle, right
// edge — were one repeated unit. That unit is DELIBERATELY BROKEN now, because the
// 240px cell makes the two numbers a straight trade: the binding case in the whole
// game is 狩猎小屋 at 24px (96) beside -200 木头 (108), so
//     2*EDGE_PAD + MID_GAP <= 240 - 204 = 36
// and every pixel of middle gap is a pixel the two edges cannot have. Measured
// over every combo the game can produce, the ceiling runs
//     MID_GAP 12 -> EDGE_PAD 12    MID_GAP 8 -> 14    MID_GAP 4 -> 16    0 -> 18
// 4 is the right place to spend it because MID_GAP is NOT a drawn gap — it is only
// the narrow-cell guard's threshold, and the ACTUAL middle gap of a band is
// usable - title - widest cost. At 16/4 exactly ONE band in the game (狩猎小屋)
// is tight enough to see 4px there; every other priced cell lands on 16px and most
// on 28px or more. So lowering MID_GAP costs one button a few pixels of internal
// air, while raising EDGE_PAD buys BOTH edges of EVERY band in the firmware. See
// the measured list in action_band.cpp.
constexpr int MID_GAP     = 4;
// Ink-vs-box optical correction at a given scale (see THE OPTICAL NUDGE).
constexpr int inkNudge(int scale) { return scale / 2; }
constexpr int INK_NUDGE   = inkNudge(TITLE_SCALE);   // 1 — the title column's

// ---- THE GREY LADDER -------------------------------------------------------
// Four tones carry the whole enabled/disabled + title/cost hierarchy, so the
// SHAPE of a band can stay identical in every state:
//
//   title, enabled           TFT_BLACK   panel grey  0   the band's subject
//   cost column, enabled     0x2104      panel grey  2   present, subordinate
//   title + cost, disabled   0x4208      panel grey  4   readable, clearly off
//   frame, disabled          0x8410      panel grey  8   the faintest thing drawn
//
// Explicit RGB565 rather than TFT_DARKGREY / TFT_LIGHTGREY for the reason
// room_page's LOG_FADE already documents: the canvas is grayscale_8bit, so a
// colour collapses to its luma, and TFT_LIGHTGREY's ~210 is far too faint to
// carry a glyph. 0x4208 (~luma 66) and 0x8410 (~luma 132) are literally the two
// LOG_FADE tiers — reusing them keeps the app on one palette instead of two —
// and 0x2104 (~luma 33) extends the same every-two-levels spacing down toward
// black. Two of the panel's 16 levels is the smallest gap that still separates
// at these stroke weights; one level (17/255) reads as a print artefact.
//
// A disabled band therefore holds grey 4 ink inside a grey 8 frame: the ink is
// the darker of the two on purpose, because the text is what a player still has
// to read (WHICH resource is short, and by how much) while the frame only has to
// say "there is a button here".
//
// KNOWN TRADEOFF — the same bet LOG_FADE made: how grey ink and a grey 1px
// stroke GHOST under lut_fast is unmeasured, and Room/Outside push their button
// grids FAST on every cooldown tick. If disabled bands smear on device the revert
// is mechanical — set these back to TFT_BLACK (the frame and both ink tiers) and
// the bands lose only the hierarchy, never a pixel of layout.
constexpr uint16_t COST_INK       = 0x2104;
constexpr uint16_t DISABLED_INK   = 0x4208;
constexpr uint16_t DISABLED_FRAME = 0x8410;
// The cooldown track is the same "faint structural line" role as a disabled
// frame, so it deliberately shares that tone rather than introducing a fifth.
constexpr uint16_t BAR_TRACK      = DISABLED_FRAME;

constexpr int MAX_SUB_LINES = 4;                  // cost tables are cost[3]; the
                                                  // 4th slot absorbs any tail
constexpr int BAR_GUTTER  = 12;                   // band bottom -> cooldown bar top
// Cooldown bar height. 8 -> 6 with the rounding pass: the bar is a progress hint
// under a 36px title, not a second title, and 6px is the thinnest a 1px track
// plus a 4px fill can be while still reading as two elements. Note the bar hangs
// from its TOP (r.y + h - BAR_GUTTER), so this number never moves the bar's top
// edge and therefore never touches the 3-line-cost clearance derived in draw().
constexpr int BAR_H       = 6;
// Track / fill corner radii. Both are the natural pill radius for their own
// height (track 6px -> 3, fill 4px -> 2), which also satisfies the inner-arc rule
// drawFrame follows: an inset stroke drops its radius by the inset.
constexpr int BAR_R       = BAR_H / 2;            // 3 — track, a full pill
constexpr int BAR_FILL_R  = BAR_R - 1;            // 2 — fill, inset 1px all round

// ---- THE QUANTIZED COOLDOWN BAR -------------------------------------------
// The bar does NOT drain pixel-by-pixel; its fill snaps to one of BAR_LEVELS
// steps. The point is the E-INK REFRESH BUDGET, not the look: a continuous bar
// changes width on almost every one-second tick, so Room/Outside used to push a
// partial refresh every single second an action cooled, and each of those pushes
// leaves ghosting behind. Quantizing means the drawn width only moves
// BAR_LEVELS times over a whole cooldown, and the pages push ONLY on the ticks
// where the level actually changed (see their tick()) — for the 60s traps
// cooldown that is 16 pushes instead of 60. 16 is fine visually: the shortest
// bar is 208px wide inside a Room cell (240 - 2*EDGE_PAD), so one step is still
// ~13px of travel.
// The pages must quantize with barLevel() too, or they would push on ticks the
// renderer draws identically; that shared use is why this lives in the header.
constexpr int BAR_LEVELS  = 16;
// Fill level of a draining bar, 0..BAR_LEVELS. Rounded UP so a cooldown with any
// time left keeps at least one visible step — the bar reaching empty must mean
// "done", never "nearly done".
constexpr int barLevel(int left, int total) {
    return (left > 0 && total > 0)
        ? (int)(((int64_t)left * BAR_LEVELS + total - 1) / total) : 0;
}

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
//   enabled        — picks BOTH the frame weight and the ink tier: true is the
//                    2px black rounded ring with a black title over a COST_INK
//                    cost column, false is the 1px DISABLED_FRAME rounded ring
//                    with every glyph in DISABLED_INK. Same geometry either way
//                    — only tone changes (see THE GREY LADDER).
//   coolLeft/Total — both > 0 draws a left-anchored draining bar in the band's
//                    bottom gutter, quantized to BAR_LEVELS steps (see above):
//                    a 1px BAR_TRACK pill with a black rounded fill inside.
//                    The cost column is centred, so with the most
//                    entries any table can produce (3) it ends exactly at the
//                    bar's top edge in a 96px band — see the budget in draw().
void draw(m5gfx::M5Canvas& c, const pages::Rect& r, const char* title,
          const char* subtitle, bool enabled, int coolLeft, int coolTotal);

// Just the frame — the 2px black rounded ring when `enabled`, the 1px
// DISABLED_FRAME rounded ring when not. Exported for assign_page/path_page:
// their worker/outfit rows put a 36px name and a 24px count SIDE BY SIDE on one
// baseline (plus a ▲/▼ stepper) instead of stacking title over subtitle, so
// they lay out their own content — but the box around it must be the same box
// every other button draws, and used to be seven separate copies of it. This is
// the ONLY way to get either frame from outside; the radii and the grey are
// applied in here, because "enabled or not" is the whole decision a caller ever
// needs to make and exposing the halves separately is how the copies got
// started. NOTE for those two pages: drawFrame only paints the BOX. A disabled
// row still draws its own name/count in black, so it is currently a grey frame
// around black text — deliberate scope limit, not an oversight; moving that ink
// to DISABLED_INK is a change to their layout code, not to this one.
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
