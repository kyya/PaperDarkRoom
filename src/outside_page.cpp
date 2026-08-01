// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Outside (village) page — the real Phase-1 Outside UI, driven by the
// game_state engine (src/game_state.*). Every piece of text routes through
// tr() (strings_zh.h) so only the official Simplified-Chinese translation ever
// reaches the sparse 12px CJK face — the §8.3 glyph-closure iron law. (The
// hardcoded literals "分工", "建筑", "工人" and "..." reuse glyphs already IN
// that closure — 分/工 via 分享/工人, 建/筑 via 建造者/建筑, 人 via 工人 — the
// same way the old "更多" band did; "库存" comes from tr("stores").) Layout
// follows the §9.4 vertical budget (24px CJK, >=80px long-press bands). See
// outside_page.h for the region model.
#include "outside_page.h"
#include "action_band.h"        // shared band renderer (v0.10.1, room+outside)
#include "assign_page.h"        // 分工 cell opens the worker-assignment page
#include "path_page.h"          // 尘土之路 cell opens the Path (背包整备) page
#include "tech_page.h"          // 科技树 cell opens the tech-tree sub-page (v0.14)
#include "cjk_text.h"
#include "pomo_page.h"          // PAD (shared layout authority)
#include "page_tabs.h"          // shared tab header (生火间 │ 村落 │ 贸易站)
#include "pager.h"
#include "game_state.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// main.cpp owns the game model and the full-screen sprite.
extern adr::GameState g_game;
extern M5Canvas canvas;

using namespace adr;

namespace {
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px CJK
constexpr int GLYPH     = 12 * SCALE;        // 24px line box
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)

// ---- DYNAMIC vertical layout + worst-case budget (v0.4.5) --------------------
// The three fieldset boxes (工人 / 建筑 / 库存) no longer sit at hardcoded y's;
// each box's height follows its live content row count and the boxes FLOW down
// from a fixed top anchor (WRK_LEGEND_Y=84, under the tab header) with a uniform
// 12px gap. Only the y axis + row counts are dynamic — the x geometry (box edges,
// column widths/positions) stays constant. The 野外 action AREA is bottom-anchored
// (independent of the flow) as THREE 96px rows: 伐木 | 科技树 · 查看陷阱 | 分工 ·
// 尘土之路 | — (v0.14).
//
// Per-box height (box y0=top border, y1=bottom border): a box that shows N
// content rows spans boxY0..(rowTop + N*ROWH + FS_PAD_BOTTOM), where boxY0 =
// legendY + GLYPH/2 and rowTop = legendY + GLYPH + FS_ROW_GAP (the legend glyph
// straddles the top border, hanging GLYPH/2 below it, so row 0 clears
// legendY+GLYPH). FS_PAD_BOTTOM is the 6px bottom inner padding added
// 2026-08-01 — see the constant for why. The next box's legendY = prevBoxY1 +
// FS_BOX_GAP, so the padding pushes every box below it down too. Row counts:
//   工人: 1 pop line + ceil((1 gatherer + nJobs)/3) grid rows. Every one of the
//         9 non-gatherer jobs has a real unlock building (JOB_REQ_BLD), and
//         Phase 2's World mines supply the last three, so nJobs<=9 -> <=1+4 = 5
//         rows. (This used to read "nJobs<=6 (P1)"; the mines outdated it.)
//   建筑: ceil(nzBuildings/2) rows; HIDDEN ENTIRELY when nzBuildings==0 — a
//         freshly-unlocked village has no buildings, and an empty box reads as a
//         bug; the section appears with its first building (the same "unlocks into
//         view" feel the Outside tab/page itself has). BLD_COUNT is 13 (10
//         craftable + 3 World mines) -> <=7 rows.
//   库存: ceil(nzEntries/3) rows, min 1 (for the "空" empty state), CLAMPED to the
//         space left above the action area (see overflow protection).
//
// WORST-CASE BUDGET — the action grid's height is DYNAMIC since v0.14, so the
// fieldset ceiling moves with it. The grid is bottom-anchored at 916 and packs
// 2..5 visible cells into 1..3 rows, giving three possible ceilings:
//     rows  areaTop  INV_MAX_BOTTOM (= areaTop - FS_BOX_GAP)
//       1     820          808          伐木 · 科技树
//       2     714          702          + 查看陷阱 and/or 分工
//       3     608          596          + 漫漫尘途 (everything unlocked)
// Taking the tallest possible fieldset stack (工人 1 pop line + 4 grid rows ->
// 96..266; 建筑 13 buildings -> 7 rows -> 290..516; 库存 rowTop 564) against each:
//       1 row  -> floor((808-564-6)/28) = 8 库存 rows (24 cells)
//       2 rows -> floor((702-564-6)/28) = 4 库存 rows (12 cells)
//       3 rows -> floor((596-564-6)/28) = 0 -> the `availRows < 1 -> 1` floor
//                 gives 1 row (3 cells); the box then ends at 598, overshooting
//                 INV_MAX_BOTTOM by 2px but still clearing the grid by 10px.
// So the third action row costs the inventory 7 rows at the very worst — and it
// only costs that once the compass is bought, by which point the player is deep
// enough that 库存 has long been in tail-collapse anyway. Early on the grid is a
// single row and 库存 gets the space back.
// Max 库存 entries = RES_COUNT(19) + ITEM_COUNT(14) = 33, so any clamp below 11
// rows tail-collapses the last cell to "…" (the pre-existing mechanism, now
// driven by remaining-space rows instead of a fixed INV_ROWS constant).
// FS_MIN_ROWS=2 is deliberately NOT honoured in the 3-row worst case: the space
// clamp outranks the min-height floor, which is the pre-existing precedence and
// the only way to guarantee no overlap.
// Verified exhaustively over every reachable combination of the three gates
// (trap / job / compass) crossed with nJobs 0..9 and nzBuildings 0..13, skipping
// states the gates make impossible: NO collision in any of them, and the minimum
// clearance between the 库存 box and the grid is 278px at 1 row, 88px at 2 rows
// and 10px at 3 rows. All region y's and the cooldown-refresh rects are derived
// from the same packing, so nothing needs a separate recompute.
// ----------------------------------------------------------------------------

// ---- shared fieldset geometry (all three boxes span the same x, PAD..540-PAD).
constexpr int FS_BOX_X0      = PAD;          // 24 — box left edge
constexpr int FS_BOX_X1      = 540 - PAD;    // 516 — box right edge
constexpr int FS_LEGEND_INSET = 8;           // left corner -> where the top border breaks
constexpr int FS_LEGEND_GAP   = 4;           // gap each side of the legend text
constexpr int FS_PAD_SIDE    = 12;           // box border -> column content inset
constexpr int FS_ROW_GAP     = 12;           // clearance below the legend glyph -> row 0
constexpr int FS_BOX_GAP     = 12;           // vertical gap between stacked boxes
// Bottom inner padding (user, 2026-08-01: "fieldset 组件没有 bottom padding!").
// A box used to end exactly at rowTop + rows*ROWH, and a 28px row slot carries
// only ~4px of slack under its 24px glyph box, so the LAST row's text sat 4px
// off the bottom border while the top (FS_ROW_GAP 12) and sides (FS_PAD_SIDE 12)
// were generously padded — the box read as bottom-heavy. 6 more px puts the
// visual gap at ~10px, close enough to the 12px used everywhere else without
// spending a whole extra row of vertical budget (see the worst case above).
constexpr int FS_PAD_BOTTOM  = 6;
constexpr int FS_CONTENT_X0  = FS_BOX_X0 + FS_PAD_SIDE;   // 36
constexpr int FS_CONTENT_X1  = FS_BOX_X1 - FS_PAD_SIDE;   // 504
constexpr int ROWH           = 28;           // one content row (shared by all boxes)
// Minimum content rows a SHOWN fieldset reserves, so a sparse box (库存 "空", a
// single-building 建筑) is not a cramped one-line sliver and the layout stops
// jumping around as sections fill in during early unlocks. 2 rows = 56px of
// content band — enough to read as a deliberate box, not a label strip. Content
// below this still tops-aligns; the box just carries bottom whitespace. Does NOT
// resurrect a hidden box (零建筑 stays hidden — see computeLayout). The 工人 box
// already clears this floor intrinsically (its 人口 line + >=1 grid row = 2 rows).
constexpr int FS_MIN_ROWS    = 2;

// Fixed top anchor: the 工人 fieldset's legend baseline, just under the tab header.
constexpr int WRK_LEGEND_Y  = 84;

// ---- 建筑 fieldset x geometry: 2 columns ("名 数量").
constexpr int BLD_COL_GAP   = 12;
constexpr int BLD_COL_W     = (FS_CONTENT_X1 - FS_CONTENT_X0 - BLD_COL_GAP) / 2;  // 228
constexpr int BLD_COLX[2]   = { FS_CONTENT_X0, FS_CONTENT_X0 + BLD_COL_W + BLD_COL_GAP };  // {36,276}

// ---- 库存 / 工人 grid x geometry: 3 columns, two-ends (name-left / qty-right).
constexpr int INV_COLS      = 3;
constexpr int INV_COL_GAP   = 12;
constexpr int INV_COL_W     = (FS_CONTENT_X1 - FS_CONTENT_X0
                                - (INV_COLS - 1) * INV_COL_GAP) / INV_COLS;   // 148
constexpr int INV_COLX[INV_COLS] = {
    FS_CONTENT_X0,                                        // 36
    FS_CONTENT_X0 + (INV_COL_W + INV_COL_GAP),            // 196
    FS_CONTENT_X0 + 2 * (INV_COL_W + INV_COL_GAP),        // 356
};

// ---- 野外 action AREA (v0.4.5): two 240px columns, THREE rows (v0.14),
// bottom-anchored. The bottom row hugs 916 (< 928 status bar) and each row above
// sits a 10px gap higher. Two columns: (492 - 12)/2 = 240px each, x = {24, 276}
// (276 + 240 = 516 = right edge ✓). Reading order is row-major:
//   ROW 1: 伐木          | 科技树
//   ROW 2: 查看陷阱      | 分工
//   ROW 3: 尘土之路      | —
// v0.14 inserted 科技树 directly after 伐木 (user: "科技树的按钮请你挪到小镇里面
// 伐木的后面一个按钮"), pushing everything下 one slot and adding the third row.
// The cells keep FIXED positions with conditional blanks — the same model the
// page has always used — rather than packing the visible ones tight. Packing was
// considered (it would let an early village with no trap/job/compass use fewer
// rows and hand the space back to 库存) but rejected: it cannot help the WORST
// case, which is what the vertical budget below has to survive — once everything
// is unlocked all five cells exist and three rows are needed regardless — and it
// would turn a static y-table into a dynamic one that pressRect, onLocalAction
// and the cooldown rects would all have to re-derive.
// v0.10.1 ("第二页的按钮加消耗/收获"): the two verbs carry a cost/yield line
// ("+50 木头" / "-2 诱饵") — ACT_H grew 80 -> 96 for it. Under 变体 B (v0.14) that
// line sits in the band's RIGHT column beside the title rather than under it, so
// the height is no longer driven by a stacked block; 96 stays because it is what
// lets a 3-entry cost column clear the cooldown bar (see action_band.cpp). The
// widest label here, 查看陷阱 / 漫漫尘途 at 4x36 = 144px, does NOT fit beside a
// cost line in a 240px cell — 查看陷阱 is one of the four titles the narrow-cell
// guard shrinks to 24px; 漫漫尘途 carries no cost, so it stays 36px and centred.
constexpr int ACT_H        = 96;             // long-press band (§9.3: >=80px floor)
constexpr int ACT_ROW_GAP  = 10;
constexpr int ACT_PITCH    = ACT_H + ACT_ROW_GAP;                // 106
constexpr int ACT_BOTTOM   = 916;            // area's fixed bottom (status bar 928)
constexpr int ACT_COLS     = 2;
constexpr int ACT_COL_GAP  = 12;
constexpr int ACT_COL_W    = (CONTENT_W - ACT_COL_GAP) / 2;      // 240
constexpr int ACT_COLX[ACT_COLS] = { PAD, PAD + ACT_COL_W + ACT_COL_GAP };  // {24, 276}
constexpr int ACT_DIV      = ACT_COLX[1];    // 276 — press x < DIV -> left column

// The five action cells, in fixed reading order. A cell that is gated off does
// NOT hold its slot — the ones after it pack up (v0.14, user: the static table
// left a visible HOLE where 查看陷阱 would be before the first trap is built).
enum : uint8_t { AC_GATHER = 0, AC_TECH, AC_TRAPS, AC_ASSIGN, AC_PATH, AC_MAX };

struct CellView {
    uint8_t code;
    const char* label;
    char        cost[24];
    bool        hasCost;
    bool        enabled;
    int         coolLeft, coolTotal;
};

// Which cells exist right now, packed. 伐木 and 科技树 are never gated, so the
// count runs 2..5 and the grid is 1..3 rows.
int buildCells(uint8_t* out) {
    int n = 0;
    out[n++] = AC_GATHER;                                    // always offered
    out[n++] = AC_TECH;                                      // never gated
    if (g_game.buildings[B_TRAP] > 0)  out[n++] = AC_TRAPS;  // needs a trap
    if (g_game.hasUnlockedJob())       out[n++] = AC_ASSIGN; // needs a job
    if (g_game.whole(R_COMPASS) > 0)   out[n++] = AC_PATH;   // needs a compass
    return n;
}

int ceilDiv(int a, int b) { return (a + b - 1) / b; }

// Rows the packed grid needs (1..3), and where its top edge lands. The area is
// BOTTOM-anchored at ACT_BOTTOM, so shedding a row moves the whole block DOWN
// and hands the space back to the fieldsets above (same family as the modals'
// bottom-anchored button columns).
int actionRows() {
    uint8_t tmp[AC_MAX];
    return ceilDiv(buildCells(tmp), ACT_COLS);
}
int actionAreaTop(int rows) {
    return ACT_BOTTOM - rows * ACT_H - (rows - 1) * ACT_ROW_GAP;
}
int actionAreaTop() { return actionAreaTop(actionRows()); }

// Overflow ceiling for the 库存 box: it must stop this far above the action area.
// DYNAMIC since v0.14 — it rises as the grid sheds rows (see the budget above):
//   3 rows -> areaTop 608 -> 596     2 rows -> 714 -> 702     1 row -> 820 -> 808
int invMaxBottom() { return actionAreaTop() - FS_BOX_GAP; }

// The rect of packed slot `s` (row-major: row s/2, column s%2) for a grid whose
// top edge is `areaTop` — the ONE description of where a 野外 button is, shared
// by the draw call, pressRect's invert-flash and the cooldown rect, so all three
// track the same dynamic layout.
pages::Rect actCellRect(int slot, int areaTop) {
    return pages::Rect{ ACT_COLX[slot % ACT_COLS],
                        areaTop + (slot / ACT_COLS) * ACT_PITCH,
                        ACT_COL_W, ACT_H };
}

// 伐木's current wood yield — "+50 木头" with a cart, else "+10 木头" (room.js
// gatherWood's own amount, game_state.cpp:336). Always exactly one resource.
void gatherYieldLine(char* out, size_t cap) {
    int amt = g_game.buildings[B_CART] > 0 ? 50 : 10;
    snprintf(out, cap, "+%d %s", amt, tr(RES_KEY[R_WOOD]));
}

// 查看陷阱's current bait consumption — "-2 诱饵", mirroring game_state.cpp
// checkTraps' own `baited = min(numBait, numTraps)` (the extra-roll bait THIS
// press will actually spend). False (no subtitle) when no bait is held: the
// press is genuinely free this time, same "free action = no subtitle" rule the
// Room page's craft costs use.
bool trapsCostLine(char* out, size_t cap) {
    int numTraps = g_game.buildings[B_TRAP];
    int numBait  = g_game.whole(R_BAIT); if (numBait < 0) numBait = 0;
    int baited   = numBait < numTraps ? numBait : numTraps;
    if (baited <= 0) { out[0] = 0; return false; }
    snprintf(out, cap, "-%d %s", baited, tr(RES_KEY[R_BAIT]));
    return true;
}

// RTC -> Unix epoch, mirroring room_page/main.cpp's epochNow (only differences
// matter to settle(), so the mktime timezone is irrelevant if consistent).
uint32_t epochNow() {
    m5::rtc_date_t d; m5::rtc_time_t t;
    M5.Rtc.getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}

// The flowed y-geometry of the three fieldset boxes for the CURRENT game state.
// Recomputed each draw() (deterministic from live counts, so cheap); the action
// area is static and not part of this.
struct Layout {
    int  wrkBoxY0, wrkBoxY1, wrkPopY, wrkGridTop;
    bool bldShown;
    int  bldBoxY0, bldBoxY1, bldRowTop, bldRows;
    int  invBoxY0, invBoxY1, invRowTop, invRows;   // invRows = space-clamped cap
};

Layout computeLayout() {
    Layout L{};

    // 工人: 1 pop line + a 3-col grid of (1 gatherer + nJobs) cells.
    uint8_t jobsTmp[JOB_COUNT];
    int nJobs = g_game.unlockedJobs(jobsTmp, (int)sizeof(jobsTmp));
    int wrkGridRows = ceilDiv(1 + nJobs, INV_COLS);
    L.wrkBoxY0   = WRK_LEGEND_Y + GLYPH / 2;                    // 96
    L.wrkPopY    = WRK_LEGEND_Y + GLYPH + FS_ROW_GAP;           // 120
    L.wrkGridTop = L.wrkPopY + ROWH;                            // 148
    L.wrkBoxY1   = L.wrkGridTop + wrkGridRows * ROWH + FS_PAD_BOTTOM;

    int prevBottom = L.wrkBoxY1;

    // 建筑: hidden when empty (see the budget note); else ceil(nz/2) rows.
    int nzB = 0;
    for (int b = 0; b < BLD_COUNT; b++) if (g_game.buildings[b] > 0) nzB++;
    L.bldShown = nzB > 0;
    if (L.bldShown) {
        L.bldRows  = ceilDiv(nzB, 2);                          // nz<=10 -> <=5 rows
        if (L.bldRows < FS_MIN_ROWS) L.bldRows = FS_MIN_ROWS;  // min-height floor
        int legendY = prevBottom + FS_BOX_GAP;
        L.bldBoxY0  = legendY + GLYPH / 2;
        L.bldRowTop = legendY + GLYPH + FS_ROW_GAP;
        L.bldBoxY1  = L.bldRowTop + L.bldRows * ROWH + FS_PAD_BOTTOM;
        prevBottom  = L.bldBoxY1;
    }

    // 库存: ceil(nz/3) rows (min 1 for "空"), clamped to the space above the
    // action area — past the cap the last cell collapses to "…".
    int nzI = 0;
    for (int r = 0; r < RES_COUNT; r++)  if (g_game.whole((uint8_t)r) > 0) nzI++;
    for (int i = 0; i < ITEM_COUNT; i++) if (g_game.items[i] > 0)          nzI++;
    int legendY = prevBottom + FS_BOX_GAP;
    L.invBoxY0  = legendY + GLYPH / 2;
    L.invRowTop = legendY + GLYPH + FS_ROW_GAP;
    int wantRows = nzI > 0 ? ceilDiv(nzI, INV_COLS) : 1;       // >=1: the "空" row
    if (wantRows < FS_MIN_ROWS) wantRows = FS_MIN_ROWS;        // min-height floor
    // The bottom padding lives INSIDE the ceiling, so subtract it before
    // dividing — otherwise a full 库存 would push its padded bottom border past
    // INV_MAX_BOTTOM and into the action area's clearance.
    int availRows = (invMaxBottom() - L.invRowTop - FS_PAD_BOTTOM) / ROWH;  // floor
    if (availRows < 1) availRows = 1;
    L.invRows  = wantRows < availRows ? wantRows : availRows;
    L.invBoxY1 = L.invRowTop + L.invRows * ROWH + FS_PAD_BOTTOM;
    return L;
}

// ---- drawing pieces --------------------------------------------------------

// The shared fieldset box: a 2px border (two concentric strokes — an outer edge
// plus an inner edge 1px inward, the SAME drawRect+drawRect language an enabled
// button frame uses; re-verified against action_band::draw after v0.12 made that
// the app's one button renderer, so the box and the buttons still read as one
// weight) with `legend` embedded in the TOP border, HTML-<fieldset> style — BOTH
// top rows run from the left edge to just before the text, break for the text
// (plus a small gap each side), then resume to the right edge; the other three
// sides are solid 2px edges. The legend straddles the top border (drawn GLYPH/2
// above it). The break is applied to BOTH top rows so the thicker border still
// opens cleanly around the legend. No padding change was needed for the extra
// stroke: content sits FS_PAD_SIDE=12px in from the box edge (the inner stroke at
// +1px leaves ~11px) and each row carries a 4px bottom gap within its 28px slot,
// both comfortably clearing 1 extra pixel. Used by all three blocks (建筑/工人/库存).
void drawFieldset(m5gfx::M5Canvas& c, int y0, int y1, const char* legend) {
    int legendW = cjk::textWidth(legend, SCALE);
    int lineEnd = FS_BOX_X0 + FS_LEGEND_INSET;                   // 32
    int textX   = lineEnd + FS_LEGEND_GAP;                       // 36
    int resumeX = textX + legendW + FS_LEGEND_GAP;

    // Top border — 2px (rows y0 and y0+1), each broken at the legend gap.
    for (int dy = 0; dy <= 1; dy++) {
        c.drawFastHLine(FS_BOX_X0, y0 + dy, lineEnd - FS_BOX_X0, TFT_BLACK);
        if (resumeX < FS_BOX_X1)
            c.drawFastHLine(resumeX, y0 + dy, FS_BOX_X1 - resumeX, TFT_BLACK);
    }
    cjk::drawText(c, textX, y0 - GLYPH / 2, legend, SCALE);

    int h = y1 - y0;
    // Left / right borders — 2px (cols X0,X0+1 and X1-1,X1).
    c.drawFastVLine(FS_BOX_X0,     y0, h, TFT_BLACK);
    c.drawFastVLine(FS_BOX_X0 + 1, y0, h, TFT_BLACK);
    c.drawFastVLine(FS_BOX_X1 - 1, y0, h, TFT_BLACK);
    c.drawFastVLine(FS_BOX_X1,     y0, h, TFT_BLACK);
    // Bottom border — 2px (rows y1-1 and y1), full width.
    c.drawFastHLine(FS_BOX_X0, y1 - 1, FS_BOX_X1 - FS_BOX_X0 + 1, TFT_BLACK);
    c.drawFastHLine(FS_BOX_X0, y1,     FS_BOX_X1 - FS_BOX_X0 + 1, TFT_BLACK);
}

// 建筑 fieldset: the box + every non-zero building as "名 数量", 2 columns. The
// box height already grew to fit every non-zero building (ceil(nz/2) rows), so
// the whole set always shows — no tail-collapse. Never called when nz==0 (the
// section is hidden then; see computeLayout / draw()).
void drawBuildings(m5gfx::M5Canvas& c, const Layout& L) {
    drawFieldset(c, L.bldBoxY0, L.bldBoxY1, "建筑");   // 建/筑 closure-safe

    int shown = 0;
    for (int b = 0; b < BLD_COUNT; b++) {
        if (g_game.buildings[b] == 0) continue;
        int col = shown % 2, row = shown / 2;
        char line[48];
        snprintf(line, sizeof(line), "%s %u",
                 tr(BLD_KEY[b]), (unsigned)g_game.buildings[b]);
        cjk::drawText(c, BLD_COLX[col], L.bldRowTop + row * ROWH, line, SCALE);
        shown++;
    }
}

// One 工人 grid cell: name left-aligned at the column's left edge, "xN" count
// right-aligned at the column's right edge (two-ends, 库存 cell parity). Row-major
// over the shared 3-col grid, starting at gridTop.
void drawWorkerCell(m5gfx::M5Canvas& c, int gridTop, int idx, const char* name, unsigned n) {
    int col = idx % INV_COLS, row = idx / INV_COLS;
    int x0 = INV_COLX[col];
    int y  = gridTop + row * ROWH;
    cjk::drawText(c, x0, y, name, SCALE);

    char qty[12];
    snprintf(qty, sizeof(qty), "x%u", n);
    int qw = cjk::textWidth(qty, SCALE);
    cjk::drawText(c, x0 + INV_COL_W - qw, y, qty, SCALE);
}

// 工人 fieldset: the box + a 人口 X/Y line, then a 3-col two-ends grid listing
// 伐木者 (idle gatherers — the derived count, shown as one more "job" in cell 0)
// followed by every UNLOCKED job (GameState::unlockedJobs) "名 xN" (incl. x0).
// Read-only — no touch region, no ▲/▼; assignment lives on AssignPage.
void drawWorkerSummary(m5gfx::M5Canvas& c, const Layout& L) {
    drawFieldset(c, L.wrkBoxY0, L.wrkBoxY1, "工人");   // 工/人 closure-safe

    // 人口 X/Y line (official "人口 " label + population / max), spanning the top.
    char pop[40];
    snprintf(pop, sizeof(pop), "%s%u/%u", tr("pop "),
             (unsigned)g_game.population, (unsigned)g_game.maxPopulation());
    cjk::drawText(c, FS_CONTENT_X0, L.wrkPopY, pop, SCALE);

    // Grid cell 0 = 伐木者 (idle gatherers), then each unlocked job.
    drawWorkerCell(c, L.wrkGridTop, 0, tr("gatherer"), (unsigned)g_game.numGatherers());
    uint8_t jobs[JOB_COUNT];
    int n = g_game.unlockedJobs(jobs, (int)sizeof(jobs));
    for (int i = 0; i < n; i++)
        drawWorkerCell(c, L.wrkGridTop, i + 1, tr(JOB_KEY[jobs[i]]),
                       (unsigned)g_game.workers[jobs[i]]);
}

// One inventory cell: name left-aligned at the column's left edge, quantity
// right-aligned at the column's right edge (two-ends alignment). Row-major:
// idx -> row idx/INV_COLS, column idx%INV_COLS, from rowTop.
void drawInvCell(m5gfx::M5Canvas& c, int rowTop, int idx, const char* name, long qty) {
    int col = idx % INV_COLS, row = idx / INV_COLS;
    int x0 = INV_COLX[col];
    int y  = rowTop + row * ROWH;
    cjk::drawText(c, x0, y, name, SCALE);

    char qtyStr[8];
    fmtAmount((int32_t)qty, qtyStr, sizeof(qtyStr));   // v0.3.3: 1.2K/56K/1.2M
    int qw = cjk::textWidth(qtyStr, SCALE);
    cjk::drawText(c, x0 + INV_COL_W - qw, y, qtyStr, SCALE);
}

// 库存 fieldset: the box + every non-zero resource (whole units) followed by
// every non-zero crafted item, three columns, name-left/qty-right. The box holds
// L.invRows*3 cells (space-clamped by computeLayout); past that the last cell
// collapses to "…". An empty inventory shows "空".
void drawInventory(m5gfx::M5Canvas& c, const Layout& L) {
    drawFieldset(c, L.invBoxY0, L.invBoxY1, tr("stores"));   // "库存"

    int cells = L.invRows * INV_COLS;
    int nz = 0;
    for (int r = 0; r < RES_COUNT; r++)  if (g_game.whole((uint8_t)r) > 0) nz++;
    for (int i = 0; i < ITEM_COUNT; i++) if (g_game.items[i] > 0)          nz++;

    bool overflow = nz > cells;
    int limit = overflow ? cells - 1 : nz;   // reserve the last cell for "…"
    int shown = 0;
    for (int r = 0; r < RES_COUNT && shown < limit; r++) {
        long q = (long)g_game.whole((uint8_t)r);
        if (q > 0) drawInvCell(c, L.invRowTop, shown++, tr(RES_KEY[r]), q);
    }
    for (int i = 0; i < ITEM_COUNT && shown < limit; i++)
        if (g_game.items[i] > 0)
            drawInvCell(c, L.invRowTop, shown++, tr(ITEM_KEY[i]), (long)g_game.items[i]);

    if (overflow) {
        int col = (cells - 1) % INV_COLS, row = (cells - 1) / INV_COLS;
        cjk::drawText(c, INV_COLX[col], L.invRowTop + row * ROWH, "…", SCALE);
    } else if (shown == 0) {
        cjk::drawText(c, INV_COLX[0], L.invRowTop, tr("none"), SCALE);   // "空"
    }
}

// Fill the packed cell list + one CellView per cell, and the region table (one
// y-band per ROW — the pager hit-tests y only, so onLocalAction resolves the
// COLUMN from the press x, exactly the model RoomPage's two-column grid uses).
// Returns the ROW count; *slotCountOut gets the cell count and *areaTopOut the
// grid's top edge for this state.
int layoutCells(pages::Region* regionsOut, uint8_t* slotCodes, CellView* views,
                uint32_t now, int* slotCountOut, int* areaTopOut) {
    int n = buildCells(slotCodes);
    int rows = ceilDiv(n, ACT_COLS);
    int areaTop = actionAreaTop(rows);

    for (int i = 0; i < n; i++) {
        CellView& v = views[i];
        v.code = slotCodes[i];
        v.cost[0] = 0; v.hasCost = false;
        v.enabled = true; v.coolLeft = 0; v.coolTotal = 0;
        switch (v.code) {
            case AC_GATHER:
                // 伐木 — always offered here; the Room page gated it on
                // outsideUnlocked, a precondition for this page drawing at all.
                v.label = tr("gather wood");
                gatherYieldLine(v.cost, sizeof v.cost);
                v.hasCost   = true;
                v.coolTotal = GATHER_DELAY_S;
                v.coolLeft  = g_game.cooldownLeft(1, now);   // gather channel
                v.enabled   = v.coolLeft == 0;
                break;
            case AC_TECH:
                // 科技树 — v0.14 moved this entry here from the Room grid. 科/技
                // ride the gen_cjk_font.py FIRMWARE_LITERAL_CHARS registry and 树
                // is in the §8.3 closure, so the literal is glyph-safe wherever it
                // lives (the registry is explicit, not scanned out of sources).
                // Pure navigation and NEVER gated — the ladders it explains read
                // the same from the first fire onward. It does now sit behind
                // outsideUnlocked, since the whole page is: see outside_page.h.
                v.label = "科技树";
                break;
            case AC_TRAPS: {
                // 查看陷阱 — only listed once a trap stands; before that the cell
                // does not exist at all and the cells after it pack up.
                v.label = tr("check traps");
                v.hasCost   = trapsCostLine(v.cost, sizeof v.cost);
                v.coolTotal = TRAPS_DELAY_S;
                v.coolLeft  = g_game.cooldownLeft(2, now);   // traps channel
                v.enabled   = v.coolLeft == 0;
                break;
            }
            case AC_ASSIGN:
                // 分工 — hardcoded literal like the old "更多": 分/工 are in the
                // §8.3 closure (分享 / 工人). Pure navigation, no cost ever.
                v.label = "分工";
                break;
            default:
                // 尘土之路 — tr("A Dusty Path") == 漫漫尘途 (official name, glyphs
                // in the closure). Pure navigation, no cost ever.
                v.label = tr("A Dusty Path");
                break;
        }
    }

    for (int r = 0; r < rows; r++) {
        regionsOut[r].y0    = (uint16_t)(areaTop + r * ACT_PITCH);
        regionsOut[r].y1    = (uint16_t)(areaTop + r * ACT_PITCH + ACT_H);
        regionsOut[r].type  = 1;                 // firmware-local
        regionsOut[r].param = (uint8_t)r;        // ROW; onLocalAction adds col from x
    }
    *slotCountOut = n;
    *areaTopOut   = areaTop;
    return rows;
}

// Paint the packed action grid: cell i at row i/2, column i%2. A live
// gather/traps cooldown renders its cell dashed + draining.
void drawActionArea(m5gfx::M5Canvas& c, const CellView* views, int n, int areaTop) {
    for (int i = 0; i < n; i++)
        action_band::draw(c, actCellRect(i, areaTop), views[i].label,
                          views[i].hasCost ? views[i].cost : nullptr,
                          views[i].enabled, views[i].coolLeft, views[i].coolTotal);
}

// The whole action area (+2px bleed) — what the partial-refresh path CLEARS
// before redrawing. It must span every row: a cooldown can now live in any cell
// (伐木 is always slot 0, but 查看陷阱's slot moves with the packing), and the
// area's own top edge moves as rows are gained or lost.
pages::Rect actionAreaRect(int areaTop) {
    return pages::Rect{ 0, areaTop - 2, 540, (ACT_BOTTOM - areaTop) + 4 };
}

// Bounding rect (2px bleed) of the packed cells currently draining a bar (bit i
// = slot i). Slot-indexed rather than hardcoded to a row, so it follows the
// packing wherever 查看陷阱 lands. Empty mask -> zero rect (the caller gates).
pages::Rect coolingRect(uint16_t mask, int areaTop) {
    int x0 = 540, y0 = 960, x1 = 0, y1 = 0;
    for (int i = 0; i < AC_MAX; i++) {
        if (!(mask & (1u << i))) continue;
        pages::Rect r = actCellRect(i, areaTop);
        if (r.x < x0)         x0 = r.x;
        if (r.x + r.w > x1)   x1 = r.x + r.w;
        if (r.y < y0)         y0 = r.y;
        if (r.y + r.h > y1)   y1 = r.y + r.h;
    }
    if (x1 <= x0) return pages::Rect{ 0, 0, 0, 0 };
    return pages::Rect{ x0 - 2, y0 - 2, (x1 - x0) + 4, (y1 - y0) + 4 };
}

// Content signature — a hash of every live value that alters a painted number or
// label (population, worker mix, buildings, inventory, the shared Room tab title).
// It ALSO has to cover everything that changes which action cells exist, because
// the grid packs and its row count feeds the fieldset budget: a gate flip must
// force the full redraw or a freshly-built trap would not grow its button until
// something else happened to change. All three gates are already in here —
// 查看陷阱 reads buildings[B_TRAP] and 分工 reads hasUnlockedJob(), both covered
// by the buildings[] loop; 尘土之路 reads whole(R_COMPASS), covered by the
// resource loop. 伐木/科技树 are never gated. Nothing more to add, but do not
// remove those loops.
// tick() compares it each second to decide a full redraw; onLocalAction re-baselines
// it right after its own showPage so the same action's state change doesn't force a
// SECOND full redraw next tick (see onLocalAction). Reads only g_game, never mutates.
uint32_t contentSig() {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix(g_game.population); mix((uint32_t)g_game.maxPopulation());
    mix(g_game.outsideUnlocked ? 1u : 0u);
    mix((uint32_t)(uint8_t)g_game.fire);       // Room tab title (shared header)
    for (int i = 0; i < JOB_COUNT; i++) mix(g_game.workers[i]);
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);
    for (int i = 0; i < RES_COUNT; i++)  mix((uint32_t)g_game.whole((uint8_t)i));
    for (int i = 0; i < ITEM_COUNT; i++) mix((uint32_t)g_game.items[i]);
    return sig;
}

}  // namespace

// ================================ Page API =================================

const pages::Region* OutsidePage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Press-flash target: param is the ROW, the press x picks the COLUMN, and the
// two together give the packed slot index — the SAME slot onLocalAction resolves
// and the same actCellRect the grid was drawn from. A slot past the packed cells
// (the odd trailing half when the cell count is odd) returns w=0, so the empty
// half never flashes black. There is no per-cell gating left to mirror here:
// gated-off cells are absent from the packing entirely rather than drawn blank.
pages::Rect OutsidePage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)y;
    int slot = (int)rg.param * ACT_COLS + (x < ACT_DIV ? 0 : 1);
    if (slot < 0 || slot >= m_slotCount) return pages::Rect{ 0, rg.y0, 0, 0 };
    return actCellRect(slot, m_areaTop);
}

// Hidden until the forest opens: returning false makes showPageOrNext skip this
// ring slot, so the page is invisible (and untappable) until outsideUnlocked.
// The hide condition lives in available() — draw() and the status bar's page-dot
// count share that one predicate (no drift between "skipped" and "no dot").
bool OutsidePage::available() const { return g_game.outsideUnlocked; }

bool OutsidePage::draw(m5gfx::M5Canvas& c) {
    if (!available()) return false;
    c.fillSprite(TFT_WHITE);
    page_tabs::draw(c, 1);           // shared tab header, Outside active

    // The action grid is laid out FIRST: its row count decides where the area's
    // top edge lands, and computeLayout()'s 库存 ceiling (invMaxBottom) reads
    // that same packing, so the fieldsets above take back whatever the grid does
    // not use.
    CellView views[AC_MAX];
    m_regionCount = layoutCells(m_regions, m_slotCodes, views, epochNow(),
                                &m_slotCount, &m_areaTop);

    Layout L = computeLayout();      // flowed fieldset y-geometry for this state
    drawWorkerSummary(c, L);         // 工人 fieldset (人口 line + read-only grid)
    if (L.bldShown) drawBuildings(c, L);   // 建筑 fieldset (hidden when empty)
    drawInventory(c, L);             // 库存 fieldset

    drawActionArea(c, views, m_slotCount, m_areaTop);
    return true;
}

// Long-press on an action row -> param picks the row, the press x picks the
// column; together they index the packed cell list, so a press always resolves
// to whatever cell is actually drawn there. 伐木/查看陷阱 run their engine verb;
// 科技树/分工/尘土之路 latch their sub-page and jump to it by name. A success
// high-beeps + persists + repaints; a rejected press or an empty trailing half
// low-beeps.
void OutsidePage::onLocalAction(uint8_t param, int x, int y) {
    (void)y;
    int slot = (int)param * ACT_COLS + (x < ACT_DIV ? 0 : 1);
    if (slot < 0 || slot >= m_slotCount) { M5.Speaker.tone(600, 120); return; }
    uint8_t code = m_slotCodes[slot];

    // ---- the three navigation cells. Each latches its sub-page visible and then
    // jumps to that ring slot by name. They navigate away, so none of them
    // re-baselines the tick signature. Their gates already decided whether the
    // cell exists at all, so reaching one here means it is live.
    if (code == AC_TECH || code == AC_ASSIGN || code == AC_PATH) {
        const char* ring;
        if (code == AC_TECH)        { tech_page::open();   ring = "tech";   }
        else if (code == AC_ASSIGN) { assign_page::open(); ring = "assign"; }
        else                        { path_page::open();   ring = "path";   }
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::ringIndexByName(ring), false);
        return;
    }

    uint32_t now = epochNow();
    Result r = (code == AC_GATHER) ? g_game.gatherWood(now) : g_game.checkTraps(now);
    if (r == RC_OK) {
        M5.Speaker.tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
        // Re-baseline tick()'s content signature to the state we JUST drew (no extra
        // settle: draw() paints un-settled g_game, contentSig() must mirror it). So
        // this same 伐木/查看陷阱 no longer forces a SECOND full redraw next tick —
        // only genuine economy advancing in the following second still does. (The
        // navigation branches above leave for another page, so they have no such
        // tick to double up.)
        m_lastSig = contentSig();
    } else {
        M5.Speaker.tone(600, 120);                    // cooldown / engine reject
    }
}

// Time axis (awake only). Settle the economy each second, then repaint on any
// change to a painted number/label (population, workers, buildings, inventory).
// tick 签名 keeps the worker mix so a change made on AssignPage (then paged back)
// still repaints the worker summary here. onLocalAction re-baselines m_lastSig after
// its own showPage, so a 伐木/查看陷阱 press no longer forces a second full redraw
// here. The bottom action-row cooldowns drain a bar: paint both rows into the canvas
// but push ONLY the cooling cell(s) (coolingRect), FASTEST — never QUALITY, whose
// full-row grayscale flash is the "big black block" the user reported; that ghost is
// cleaned at sleep by pager::payGhostDebtIfDue instead. Mirrors the Room page.
void OutsidePage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick     = 0;
    static uint16_t s_lastCoolMask = 0;   // cooling slots the previous tick pushed

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    uint32_t now = epochNow();
    g_game.settle(now);

    uint32_t sig = contentSig();

    // 野外 cooldowns drain a bar, but — unlike the content above — they are NOT in
    // the signature: the wood/meat they yield is banked at press time, so nothing
    // else changes while they cool. The mask is over PACKED SLOT indices now, not
    // fixed rows, so it is built from the same layoutCells() result the painter
    // uses — 查看陷阱's slot moves depending on which cells exist.
    CellView views[AC_MAX];
    m_regionCount = layoutCells(m_regions, m_slotCodes, views, now,
                                &m_slotCount, &m_areaTop);
    uint16_t coolMask = 0;
    for (int i = 0; i < m_slotCount; i++)
        if (views[i].coolTotal > 0 && views[i].coolLeft > 0)
            coolMask |= (uint16_t)(1u << i);

    if (sig != m_lastSig) {
        m_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);   // recomputes the page
        s_lastCoolMask = coolMask;
        return;
    }

    if (coolMask || s_lastCoolMask) {
        // Clear the whole (dynamic) area and repaint every cell into the canvas...
        pages::Rect area = actionAreaRect(m_areaTop);
        canvas.fillRect(area.x, area.y, area.w, area.h, TFT_WHITE);
        drawActionArea(canvas, views, m_slotCount, m_areaTop);
        // ...but PUSH only the union of the cells cooling now and the ones that
        // just cleared this tick. FASTEST; the ghost cleanup is deferred to sleep
        // (see the function note). A row-count change cannot sneak through here:
        // it can only come from a gate flip, which moves contentSig and takes the
        // full-redraw branch above.
        pager::partialRefresh(coolingRect((uint16_t)(coolMask | s_lastCoolMask), m_areaTop),
                              pages::RefreshMode::FASTEST);
    }
    s_lastCoolMask = coolMask;
}
