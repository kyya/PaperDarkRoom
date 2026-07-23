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
#include "assign_page.h"        // 分工 cell opens the worker-assignment page
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
// (independent of the flow) as two 80px rows: row 1 伐木 | 查看陷阱, row 2 分工 | —.
//
// Per-box height (box y0=top border, y1=bottom border): a box that shows N
// content rows spans boxY0..(rowTop + N*ROWH), where boxY0 = legendY + GLYPH/2 and
// rowTop = legendY + GLYPH + FS_ROW_GAP (the legend glyph straddles the top border,
// hanging GLYPH/2 below it, so row 0 clears legendY+GLYPH). The next box's
// legendY = prevBoxY1 + FS_BOX_GAP. Row counts:
//   工人: 1 pop line + ceil((1 gatherer + nJobs)/3) grid rows. nJobs<=6 (P1),
//         so <=1+3 = 4 rows.
//   建筑: ceil(nzBuildings/2) rows; HIDDEN ENTIRELY when nzBuildings==0 — a
//         freshly-unlocked village has no buildings, and an empty box reads as a
//         bug; the section appears with its first building (the same "unlocks into
//         view" feel the Outside tab/page itself has). nz<=10 -> <=5 rows.
//   库存: ceil(nzEntries/3) rows, min 1 (for the "空" empty state), CLAMPED to the
//         space left above the action area (see overflow protection).
//
// WORST-CASE BUDGET (everything maximal, to prove no collision):
//   工人 4 rows  -> box 96..232
//   建筑 10 buildings -> 5 rows -> legend@244, box 256..420
//   库存 starts rowTop=468. Space to the action area: INV_MAX_BOTTOM =
//        ACT_ROW1_TOP - 12 = 734, so floor((734-468)/28) = 9 rows = 27 cells.
//   Max 库存 entries = RES_COUNT(19) + ITEM_COUNT(14) = 33 > 27 -> the grid CLAMPS
//   to 9 rows and the last cell collapses to "…" (the pre-existing tail-collapse,
//   now driven by remaining-space rows instead of a fixed INV_ROWS constant).
//   库存 box then ends 468+9*28 = 720 < 734, clearing the action row-1 top (746)
//   by 26px. All region y's + the tick cooldown-refresh rect track the (static)
//   action area, so they need no dynamic recompute.
// ----------------------------------------------------------------------------

// ---- shared fieldset geometry (all three boxes span the same x, PAD..540-PAD).
constexpr int FS_BOX_X0      = PAD;          // 24 — box left edge
constexpr int FS_BOX_X1      = 540 - PAD;    // 516 — box right edge
constexpr int FS_LEGEND_INSET = 8;           // left corner -> where the top border breaks
constexpr int FS_LEGEND_GAP   = 4;           // gap each side of the legend text
constexpr int FS_PAD_SIDE    = 12;           // box border -> column content inset
constexpr int FS_ROW_GAP     = 12;           // clearance below the legend glyph -> row 0
constexpr int FS_BOX_GAP     = 12;           // vertical gap between stacked boxes
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

// ---- 野外 action AREA (v0.4.5): two 240px columns, two rows, bottom-anchored.
// Row 2 (分工) bottom hugs 916 (< 928 status bar); row 1 (伐木 | 查看陷阱) sits a
// 10px gap above it. Two columns: (492 - 12)/2 = 240px each, x = {24, 276} (276 +
// 240 = 516 = right edge ✓). 36px labels have room to spare — the widest,
// "查看陷阱" = 4x36 = 144px < 240 ✓.
constexpr int ACT_H        = 80;             // long-press band (§9.3: >=80px floor)
constexpr int ACT_ROW_GAP  = 10;
constexpr int ACT_ROW2_TOP = 916 - ACT_H;                       // 836 — 分工 row (bottom)
constexpr int ACT_ROW1_TOP = ACT_ROW2_TOP - ACT_ROW_GAP - ACT_H; // 746 — gather/traps row
constexpr int ACT_COL_GAP  = 12;
constexpr int ACT_COL_W    = (CONTENT_W - ACT_COL_GAP) / 2;      // 240
constexpr int ACT_COLX[2]  = { PAD, PAD + ACT_COL_W + ACT_COL_GAP };  // {24, 276}
constexpr int ACT_DIV      = ACT_COLX[1];    // 276 — press x < DIV -> left column

// Overflow ceiling for the 库存 box: it must stop this far above the action area.
constexpr int INV_MAX_BOTTOM = ACT_ROW1_TOP - FS_BOX_GAP;        // 734

// 36px labels (原作「框大字小」) for the action verbs, centered in their cells.
constexpr int BTN_SCALE = 3;                 // 12px grid x3 = 36px (verb label)
constexpr int BTN_GLYPH = 12 * BTN_SCALE;    // 36px line box

// One param per action ROW (the pager resolves the row from the press y = which
// Region band it hit; onLocalAction then resolves the column from x).
constexpr uint8_t PARAM_ROW1 = 0xFE;         // 伐木 | 查看陷阱
constexpr uint8_t PARAM_ROW2 = 0xFD;         // 分工 | —

int ceilDiv(int a, int b) { return (a + b - 1) / b; }

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
    L.wrkBoxY1   = L.wrkGridTop + wrkGridRows * ROWH;

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
        L.bldBoxY1  = L.bldRowTop + L.bldRows * ROWH;
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
    int availRows = (INV_MAX_BOTTOM - L.invRowTop) / ROWH;     // floor
    if (availRows < 1) availRows = 1;
    L.invRows  = wantRows < availRows ? wantRows : availRows;
    L.invBoxY1 = L.invRowTop + L.invRows * ROWH;
    return L;
}

// ---- drawing pieces --------------------------------------------------------

// The shared fieldset box: a 2px border (two concentric strokes — an outer edge
// plus an inner edge 1px inward, the SAME drawRect+drawRect language the enabled
// action buttons use in drawActionBand, so the box and the buttons read as one
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

// 1px dashed rect, 4px-on/4px-off on all four edges — the disabled-button
// frame (no inner concentric rect, unlike the enabled 2-ring frame below).
void drawDashedRect(m5gfx::M5Canvas& c, int x0, int y0, int w, int h) {
    int x1 = x0 + w - 1, y1 = y0 + h - 1;
    for (int x = x0; x <= x1; x += 8) {
        int len = (x1 - x + 1 < 4) ? (x1 - x + 1) : 4;
        c.drawFastHLine(x, y0, len, TFT_BLACK);
        c.drawFastHLine(x, y1, len, TFT_BLACK);
    }
    for (int y = y0; y <= y1; y += 8) {
        int len = (y1 - y + 1 < 4) ? (y1 - y + 1) : 4;
        c.drawFastVLine(x0, y, len, TFT_BLACK);
        c.drawFastVLine(x1, y, len, TFT_BLACK);
    }
}

// One action cell (伐木 / 查看陷阱 / 分工) at column origin x0, width ACT_COL_W —
// the same frame language the Room page uses (drawBand): enabled = solid double
// ring; unavailable/cooling = 1px dashed outer frame. A live cooldown draws a
// thin progress bar hugging the band's inner bottom edge (Room drawBand parity):
// an 8px-tall outlined bar inset 12px, its inner fill width = the fraction of the
// cooldown remaining, draining left-anchored to 0. The 36px label centers in the
// band, clear above the bar. 分工 (open-assign) has no cooldown — it always draws
// solid with no bar (coolTotal 0).
void drawActionBand(m5gfx::M5Canvas& c, int x0, int top, const char* label,
                    bool enabled, int coolLeft, int coolTotal) {
    if (enabled) {
        c.drawRect(x0, top, ACT_COL_W, ACT_H, TFT_BLACK);
        c.drawRect(x0 + 1, top + 1, ACT_COL_W - 2, ACT_H - 2, TFT_BLACK);
    } else {
        drawDashedRect(c, x0, top, ACT_COL_W, ACT_H);
    }

    int lw = cjk::textWidth(label, BTN_SCALE);
    cjk::drawText(c, x0 + (ACT_COL_W - lw) / 2, top + (ACT_H - BTN_GLYPH) / 2 - 4,
                  label, BTN_SCALE);

    if (coolTotal > 0 && coolLeft > 0) {                      // draining cooldown
        int barX0 = x0 + 12, barX1 = x0 + ACT_COL_W - 12;
        int barY = top + ACT_H - 16, barH = 8;
        c.drawRect(barX0, barY, barX1 - barX0, barH, TFT_BLACK);
        int inner = barX1 - barX0 - 4;
        int fw = (int)((int64_t)inner * coolLeft / coolTotal);   // drains L-anchored
        if (fw > 0) c.fillRect(barX0 + 2, barY + 2, fw, barH - 4, TFT_BLACK);
    }
}

// Paint the two-row action area (v0.4.5). ROW 1 left = 伐木 (gather wood): always
// offered here — the Room page gated it on outsideUnlocked, a precondition for
// this page drawing at all. ROW 1 right = 查看陷阱 (check traps): drawn only when
// a trap stands (buildings[B_TRAP] > 0); with none the cell is left blank (无供给
// 整格不画, not a disabled frame), matching the Room 供给 condition. ROW 2 left =
// 分工: opens AssignPage, but only once at least one job is unlocked — with none
// there is nothing to assign, so the cell is left blank by the SAME 无供给 rule
// (g_game.hasUnlockedJob(), the shared job filter). ROW 2 right is always blank.
// A live gather/traps cooldown (channels 1/2) renders its cell dashed + draining.
void drawActionArea(m5gfx::M5Canvas& c, uint32_t now) {
    int gcool = g_game.cooldownLeft(1, now);                 // gather channel
    drawActionBand(c, ACT_COLX[0], ACT_ROW1_TOP, tr("gather wood"),
                   gcool == 0, gcool, GATHER_DELAY_S);
    if (g_game.buildings[B_TRAP] > 0) {
        int tcool = g_game.cooldownLeft(2, now);             // traps channel
        drawActionBand(c, ACT_COLX[1], ACT_ROW1_TOP, tr("check traps"),
                       tcool == 0, tcool, TRAPS_DELAY_S);
    }
    // 分工 — hardcoded literal like the old "更多": 分/工 are in the §8.3 closure
    // (分享 / 工人). Blank until a job exists (parity with 查看陷阱's 无供给 blank).
    if (g_game.hasUnlockedJob())
        drawActionBand(c, ACT_COLX[0], ACT_ROW2_TOP, "分工", true, 0, 0);
}

// The cooldown partial-refresh target: only ROW 1 carries a draining bar (gather
// /traps), so the tick repaints just that row's band (+ a 2px bleed) instead of a
// full-page redraw — ROW 2 (分工) has no cooldown and never changes on a tick.
pages::Rect actionCoolRect() {
    return pages::Rect{ 0, ACT_ROW1_TOP - 2, 540, ACT_H + 4 };
}

// Repaint BOTH action rows into `c` for the partial-refresh path (the surrounding
// full-page pixels already sit in the canvas). Only ROW 1 is pushed (actionCoolRect)
// — redrawing ROW 2 identically is harmless and keeps one draw path.
void repaintActionArea(m5gfx::M5Canvas& c, uint32_t now) {
    pages::Rect r = actionCoolRect();
    c.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
    drawActionArea(c, now);
}

// Content signature — a hash of every live value that alters a painted number or
// label (population, worker mix, buildings, inventory, the shared Room tab title).
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

// Bounding rect (2px bleed) of the ROW-1 verb cells currently draining a bar:
// bit 0 = 伐木 (gather ch1, left), bit 1 = 查看陷阱 (traps ch2, right). The cooldown
// tick pushes just this — not the whole 540-wide row — so only the cell with a
// moving bar flips. Empty mask -> zero rect (the caller gates on mask).
pages::Rect coolingRect(uint8_t mask) {
    int x0 = 540, x1 = 0;
    for (int col = 0; col < 2; col++) {
        if (!(mask & (1u << col))) continue;
        if (ACT_COLX[col] < x0)             x0 = ACT_COLX[col];
        if (ACT_COLX[col] + ACT_COL_W > x1) x1 = ACT_COLX[col] + ACT_COL_W;
    }
    if (x1 <= x0) return pages::Rect{ 0, 0, 0, 0 };
    return pages::Rect{ x0 - 2, ACT_ROW1_TOP - 2, (x1 - x0) + 4, ACT_H + 4 };
}
}  // namespace

// ================================ Page API =================================

const pages::Region* OutsidePage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Press-flash target: each action ROW is two 240px columns, so flash only the
// column cell that carries a real button — mirroring drawActionArea's 无供给
// blanks (查看陷阱 only with a trap, 分工 only with a job, ROW 2 right always
// blank). A blank cell returns w=0 so an empty half never flashes black.
pages::Rect OutsidePage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)y;
    bool left = x < ACT_DIV;
    if (rg.param == PARAM_ROW1) {
        if (left) return pages::Rect{ ACT_COLX[0], rg.y0, ACT_COL_W, ACT_H }; // 伐木
        if (g_game.buildings[B_TRAP] > 0)
            return pages::Rect{ ACT_COLX[1], rg.y0, ACT_COL_W, ACT_H };       // 查看陷阱
    } else if (rg.param == PARAM_ROW2 && left && g_game.hasUnlockedJob()) {
        return pages::Rect{ ACT_COLX[0], rg.y0, ACT_COL_W, ACT_H };           // 分工
    }
    return pages::Rect{ 0, rg.y0, 0, 0 };                                     // blank cell
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

    Layout L = computeLayout();      // flowed fieldset y-geometry for this state
    drawWorkerSummary(c, L);         // 工人 fieldset (人口 line + read-only grid)
    if (L.bldShown) drawBuildings(c, L);   // 建筑 fieldset (hidden when empty)
    drawInventory(c, L);             // 库存 fieldset

    // 野外 action area — two bottom-anchored rows, each a type=1 Region: row 1
    // (伐木 | 查看陷阱, PARAM_ROW1) and row 2 (分工 | —, PARAM_ROW2). onLocalAction
    // resolves the column from x within the row the pager already picked by y.
    drawActionArea(c, epochNow());
    m_regions[0].y0 = (uint16_t)ACT_ROW1_TOP;
    m_regions[0].y1 = (uint16_t)(ACT_ROW1_TOP + ACT_H);
    m_regions[0].type  = 1;
    m_regions[0].param = PARAM_ROW1;
    m_regions[1].y0 = (uint16_t)ACT_ROW2_TOP;
    m_regions[1].y1 = (uint16_t)(ACT_ROW2_TOP + ACT_H);
    m_regions[1].type  = 1;
    m_regions[1].param = PARAM_ROW2;
    m_regionCount = 2;
    return true;
}

// Long-press on an action row -> param picks the row, the press x picks the column
// (x < ACT_DIV = left). ROW 1: 伐木 gatherWood | 查看陷阱 checkTraps (only when a
// trap stands — else blank, low beep). ROW 2: 分工 opens AssignPage (only when a
// job is unlocked — else blank, low beep) | blank right. A success high-beeps +
// persists + repaints; a rejected/blank press low-beeps.
void OutsidePage::onLocalAction(uint8_t param, int x, int y) {
    (void)y;

    if (param == PARAM_ROW2) {
        // 分工 (left cell): jump to AssignPage — but only if a job exists (the
        // entry gate the empty-assign fix hinges on). Right cell / no job = blank.
        if (x < ACT_DIV && g_game.hasUnlockedJob()) {
            assign_page::open();
            M5.Speaker.tone(1800, 80);
            pager::showPage(pager::ringIndexByName("assign"), false);
        } else {
            M5.Speaker.tone(600, 120);                // blank cell (no job / right)
        }
        return;
    }
    if (param != PARAM_ROW1) { M5.Speaker.tone(600, 120); return; }

    uint32_t now = epochNow();
    Result r;
    if (x < ACT_DIV) {
        r = g_game.gatherWood(now);                   // 伐木 (left cell)
    } else if (g_game.buildings[B_TRAP] > 0) {
        r = g_game.checkTraps(now);                   // 查看陷阱 (right cell)
    } else {
        M5.Speaker.tone(600, 120);                    // blank cell (no trap stands)
        return;
    }
    if (r == RC_OK) {
        M5.Speaker.tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
        // Re-baseline tick()'s content signature to the state we JUST drew (no extra
        // settle: draw() paints un-settled g_game, contentSig() must mirror it). So
        // this same 伐木/查看陷阱 no longer forces a SECOND full redraw next tick —
        // only genuine economy advancing in the following second still does. (The 分工
        // branch navigates away to AssignPage, so it has no such tick to double up.)
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
    static uint8_t  s_lastCoolMask = 0;   // cooling cells the previous tick pushed

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    uint32_t now = epochNow();
    g_game.settle(now);

    uint32_t sig = contentSig();

    // 野外 action-row cooldowns (gather ch1 left / traps ch2 right) drain a bar,
    // but — unlike the content above — they are NOT in the signature: the wood/meat
    // they yield is banked at press time, so nothing else changes while they cool.
    // bit 0 = 伐木 (left), bit 1 = 查看陷阱 (right, only when a trap stands).
    uint8_t coolMask = 0;
    if (g_game.cooldownLeft(1, now) > 0) coolMask |= 1u;
    if (g_game.buildings[B_TRAP] > 0 && g_game.cooldownLeft(2, now) > 0)
        coolMask |= 2u;

    if (sig != m_lastSig) {
        m_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);   // recomputes the page
        s_lastCoolMask = coolMask;
        return;
    }

    if (coolMask || s_lastCoolMask) {
        repaintActionArea(canvas, now);
        // Union of the cells cooling now and the ones that just cleared this tick
        // (were cooling last tick) — never the whole 540-wide row. FASTEST; the
        // ghost cleanup is deferred to sleep (see the function note).
        pager::partialRefresh(coolingRect((uint8_t)(coolMask | s_lastCoolMask)),
                              pages::RefreshMode::FASTEST);
    }
    s_lastCoolMask = coolMask;
}
