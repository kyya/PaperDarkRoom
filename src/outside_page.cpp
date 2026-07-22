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

// ---- vertical budget (§9.4), all measured to clear the 32px status bar (< 928).
// v0.4.2 reflow: the standalone population row is GONE — its 人口/伐木者 line
// folded INTO the 工人 fieldset — and 工人 moved to the FIRST section (right under
// the tab header). The three content blocks are identical fieldset boxes (1px
// border + a legend embedded in the top border + inset content, drawFieldset
// below); the 野外 action row (伐木 | 查看陷阱 | 分工) stays sunk at the very
// BOTTOM. Layout top->bottom (box y0=top border, y1=bottom border):
//   header(0..72) · 工人 fieldset(box 96..232, legend "工人"@84: a 人口 X/Y line
//   @120, then a 3-col grid @148 of 伐木者 + each unlocked job "名 xN" — 1+6=7
//   cells -> 3 rows) · 建筑 fieldset(box 256..420, legend "建筑"@244, 5x2 cells) ·
//   库存 fieldset(box 444..804, legend "库存"@432, 12x3 cells) · action row
//   (836..916, 3 cells: 伐木 | 查看陷阱 | 分工).
// ----------------------------------------------------------------------------

// ---- shared fieldset geometry (all three boxes span the same x, PAD..540-PAD).
constexpr int FS_BOX_X0      = PAD;          // 24 — box left edge
constexpr int FS_BOX_X1      = 540 - PAD;    // 516 — box right edge
constexpr int FS_LEGEND_INSET = 8;           // left corner -> where the top border breaks
constexpr int FS_LEGEND_GAP   = 4;           // gap each side of the legend text
constexpr int FS_PAD_SIDE    = 12;           // box border -> column content inset
constexpr int FS_ROW_GAP     = 12;           // clearance below the legend glyph -> row 0
constexpr int FS_CONTENT_X0  = FS_BOX_X0 + FS_PAD_SIDE;   // 36
constexpr int FS_CONTENT_X1  = FS_BOX_X1 - FS_PAD_SIDE;   // 504
// The legend glyph straddles the top border (centered on box-y0, drawn at
// legendY = boxY0 - GLYPH/2), so it hangs GLYPH/2 = 12px BELOW the border to
// legendY+GLYPH — row 0 must clear THAT (both sit at content column-0's x=36),
// hence ROW_TOP = legendY + GLYPH + FS_ROW_GAP (= boxY0 + GLYPH/2 + FS_ROW_GAP).

// ---- 工人 fieldset (v0.4.2: now the FIRST section, and it absorbs the pop line).
// A 人口 X/Y line spans the top (col-0 x); below it a 3-col two-ends grid lists
// 伐木者 (idle gatherers — tr("gatherer"), naturally one more "job") in cell 0
// then every UNLOCKED job "名 xN" (incl. x0). 1 + <=6 jobs = <=7 cells -> 3 rows.
// Legend "工人" (both glyphs in the §8.3 closure via 炼钢工人 etc.). Shares the
// inventory box's 3-column geometry (INV_COLX / INV_COL_W below).
constexpr int WRK_LEGEND_Y  = 84;
constexpr int WRK_BOX_Y0    = WRK_LEGEND_Y + GLYPH / 2;             // 96
constexpr int WRK_ROWH      = 28;
constexpr int WRK_POP_Y     = WRK_LEGEND_Y + GLYPH + FS_ROW_GAP;    // 120 — 人口 line
constexpr int WRK_GRID_TOP  = WRK_POP_Y + WRK_ROWH;                 // 148 — grid row 0
constexpr int WRK_GRID_ROWS = 3;             // 1 gatherer + <=6 jobs = <=7 cells
constexpr int WRK_BOX_Y1    = WRK_GRID_TOP + WRK_GRID_ROWS * WRK_ROWH;   // 232

// ---- 建筑 fieldset: non-zero buildings as "名 数量", 2 columns. 10 P1 building
// types fit exactly in 5x2 = 10 cells (no "..." drop). Legend "建筑" (建/筑 in
// the §8.3 closure via 建造者/建筑).
constexpr int BLD_LEGEND_Y  = 244;
constexpr int BLD_BOX_Y0    = BLD_LEGEND_Y + GLYPH / 2;             // 256
constexpr int BLD_ROW_TOP   = BLD_LEGEND_Y + GLYPH + FS_ROW_GAP;    // 280
constexpr int BLD_ROWH      = 28;
constexpr int BLD_ROWS      = 5;
constexpr int BLD_BOX_Y1    = BLD_ROW_TOP + BLD_ROWS * BLD_ROWH;    // 420
constexpr int BLD_COL_GAP   = 12;
constexpr int BLD_COL_W     = (FS_CONTENT_X1 - FS_CONTENT_X0 - BLD_COL_GAP) / 2;  // 228
constexpr int BLD_COLX[2]   = { FS_CONTENT_X0, FS_CONTENT_X0 + BLD_COL_W + BLD_COL_GAP };  // {36,276}

// ---- 库存 fieldset: non-zero stores then non-zero items, 3-col two-ends grid.
// The box runs down to just above the bottom action row: 12 rows x 3 = 36 cells,
// past the 19 resources + P1 items ever non-zero, so the "…" tail-collapse is
// effectively unreachable. Legend tr("stores") == "库存".
constexpr int INV_LEGEND_Y  = 432;
constexpr int INV_BOX_Y0    = INV_LEGEND_Y + GLYPH / 2;             // 444
constexpr int INV_ROW_TOP   = INV_LEGEND_Y + GLYPH + FS_ROW_GAP;    // 468
constexpr int INV_ROWH      = 28;
constexpr int INV_ROWS      = 12;            // 468 + 12*28 = 804 (< action row @836)
constexpr int INV_BOX_Y1    = INV_ROW_TOP + INV_ROWS * INV_ROWH;    // 804
constexpr int INV_COLS      = 3;
constexpr int INV_COL_GAP   = 12;
// 468px content / 3 cols with 2 gutters: (468 - 2*12) / 3 = 148 exactly.
constexpr int INV_COL_W     = (FS_CONTENT_X1 - FS_CONTENT_X0
                                - (INV_COLS - 1) * INV_COL_GAP) / INV_COLS;   // 148
constexpr int INV_COLX[INV_COLS] = {
    FS_CONTENT_X0,                                        // 36
    FS_CONTENT_X0 + (INV_COL_W + INV_COL_GAP),            // 196
    FS_CONTENT_X0 + 2 * (INV_COL_W + INV_COL_GAP),        // 356
};
constexpr int INV_CELLS     = INV_ROWS * INV_COLS;       // 36 cells before overflow

// ---- 野外 action row (v0.4.1: sunk to the page bottom). 伐木 | 查看陷阱 | 分工,
// evenly spread over the 492px content: col width (492 - 2*12)/3 = 156, x =
// {24, 192, 360} (360 + 156 = 516 = right edge ✓). 36px labels: the widest,
// "查看陷阱" = 4x36 = 144px < 156 ✓. The band bottom hugs 916: ACT_TOP = 916 -
// 80 = 836, band 836..916 (< 928 status bar), 32px of air below the 库存 box.
constexpr int ACT_H       = 80;              // long-press band (§9.3: >=80px floor)
constexpr int ACT_TOP     = 916 - ACT_H;     // 836 — bottom-anchored
constexpr int ACT_COL_GAP = 12;
constexpr int ACT_COL_W   = (CONTENT_W - 2 * ACT_COL_GAP) / 3;    // 156
constexpr int ACT_COLX[3] = { PAD, PAD + ACT_COL_W + ACT_COL_GAP,
                              PAD + 2 * (ACT_COL_W + ACT_COL_GAP) };   // {24,192,360}
// Column split boundaries for the press x (onLocalAction). x < 192 -> 伐木,
// x < 360 -> 查看陷阱, else 分工.
constexpr int ACT_DIV0    = ACT_COLX[1];     // 192
constexpr int ACT_DIV1    = ACT_COLX[2];     // 360

// 36px labels (原作「框大字小」) for the action-row verbs, centered in their cells.
constexpr int BTN_SCALE = 3;                 // 12px grid x3 = 36px (verb label)
constexpr int BTN_GLYPH = 12 * BTN_SCALE;    // 36px line box

// The野外 action row is a single Region carrying this sentinel param — its three
// cells route to gatherWood / checkTraps / open-assign by the press column
// (ACT_DIV0/ACT_DIV1), so onLocalAction resolves the verb from x.
constexpr uint8_t PARAM_ACTIONS = 0xFE;

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

// The P1-assignable jobs currently unlocked: a job is offerable once its
// required building stands (lodge -> hunter/trapper, tannery -> tanner, ...).
// gatherer is derived (idle population) and never gets a row. Miners map to
// BLD_NONE (P2 mines), so they never appear. Returns the count. Shared by the
// read-only worker summary here and (identically) by AssignPage.
int buildJobs(uint8_t* out, int cap) {
    int n = 0;
    for (uint8_t j = J_HUNTER; j < JOB_COUNT && n < cap; j++) {
        uint8_t reqB = JOB_REQ_BLD[j];
        if (reqB != BLD_NONE && g_game.buildings[reqB] > 0) out[n++] = j;
    }
    return n;
}

// ---- drawing pieces --------------------------------------------------------

// The shared fieldset box: a 1px rect (x FS_BOX_X0..FS_BOX_X1, y y0..y1) with
// `legend` embedded in the top border line, HTML-<fieldset> style — the line
// runs from the left edge to just before the text, breaks for the text (plus a
// small gap each side), then resumes to the right edge; the other three sides
// are plain rect edges. The legend straddles the top border (drawn GLYPH/2 above
// it). Used by all three content blocks (建筑 / 工人 / 库存).
void drawFieldset(m5gfx::M5Canvas& c, int y0, int y1, const char* legend) {
    int legendW = cjk::textWidth(legend, SCALE);
    int lineEnd = FS_BOX_X0 + FS_LEGEND_INSET;                   // 32
    int textX   = lineEnd + FS_LEGEND_GAP;                       // 36
    int resumeX = textX + legendW + FS_LEGEND_GAP;

    c.drawFastHLine(FS_BOX_X0, y0, lineEnd - FS_BOX_X0, TFT_BLACK);
    if (resumeX < FS_BOX_X1)
        c.drawFastHLine(resumeX, y0, FS_BOX_X1 - resumeX, TFT_BLACK);
    cjk::drawText(c, textX, y0 - GLYPH / 2, legend, SCALE);

    c.drawFastVLine(FS_BOX_X0, y0, y1 - y0, TFT_BLACK);
    c.drawFastVLine(FS_BOX_X1, y0, y1 - y0, TFT_BLACK);
    c.drawFastHLine(FS_BOX_X0, y1, FS_BOX_X1 - FS_BOX_X0 + 1, TFT_BLACK);
}

// 建筑 fieldset: the box + every non-zero building as "名 数量", 2 columns x up
// to 5 rows (10 cells == BLD_COUNT, so the whole set shows; a defensive "..."
// tail guard remains but is unreachable in P1).
void drawBuildings(m5gfx::M5Canvas& c) {
    drawFieldset(c, BLD_BOX_Y0, BLD_BOX_Y1, "建筑");   // 建/筑 closure-safe

    const int cap = BLD_ROWS * 2;               // 10 cells
    int nz = 0;
    for (int b = 0; b < BLD_COUNT; b++)
        if (g_game.buildings[b] > 0) nz++;
    bool overflow = nz > cap;
    int limit = overflow ? cap - 1 : nz;
    int shown = 0;
    for (int b = 0; b < BLD_COUNT && shown < limit; b++) {
        if (g_game.buildings[b] == 0) continue;
        int col = shown % 2, row = shown / 2;
        char line[48];
        snprintf(line, sizeof(line), "%s %u",
                 tr(BLD_KEY[b]), (unsigned)g_game.buildings[b]);
        cjk::drawText(c, BLD_COLX[col], BLD_ROW_TOP + row * BLD_ROWH, line, SCALE);
        shown++;
    }
    if (overflow) {
        int col = (cap - 1) % 2, row = (cap - 1) / 2;
        cjk::drawText(c, BLD_COLX[col], BLD_ROW_TOP + row * BLD_ROWH, "...", SCALE);
    }
}

// One 工人 grid cell: name left-aligned at the column's left edge, "xN" count
// right-aligned at the column's right edge (two-ends, 库存 cell parity). Row-major
// over the shared 3-col grid, starting at WRK_GRID_TOP.
void drawWorkerCell(m5gfx::M5Canvas& c, int idx, const char* name, unsigned n) {
    int col = idx % INV_COLS, row = idx / INV_COLS;
    int x0 = INV_COLX[col];
    int y  = WRK_GRID_TOP + row * WRK_ROWH;
    cjk::drawText(c, x0, y, name, SCALE);

    char qty[12];
    snprintf(qty, sizeof(qty), "x%u", n);
    int qw = cjk::textWidth(qty, SCALE);
    cjk::drawText(c, x0 + INV_COL_W - qw, y, qty, SCALE);
}

// 工人 fieldset: the box + a 人口 X/Y line, then a 3-col two-ends grid listing
// 伐木者 (idle gatherers — the derived count, shown as one more "job" in cell 0)
// followed by every UNLOCKED job (buildJobs) "名 xN" (incl. x0). Read-only — no
// touch region, no ▲/▼; assignment lives on AssignPage.
void drawWorkerSummary(m5gfx::M5Canvas& c) {
    drawFieldset(c, WRK_BOX_Y0, WRK_BOX_Y1, "工人");   // 工/人 closure-safe

    // 人口 X/Y line (official "人口 " label + population / max), spanning the top.
    char pop[40];
    snprintf(pop, sizeof(pop), "%s%u/%u", tr("pop "),
             (unsigned)g_game.population, (unsigned)g_game.maxPopulation());
    cjk::drawText(c, FS_CONTENT_X0, WRK_POP_Y, pop, SCALE);

    // Grid cell 0 = 伐木者 (idle gatherers), then each unlocked job.
    drawWorkerCell(c, 0, tr("gatherer"), (unsigned)g_game.numGatherers());
    uint8_t jobs[JOB_COUNT];
    int n = buildJobs(jobs, (int)sizeof(jobs));
    for (int i = 0; i < n; i++)
        drawWorkerCell(c, i + 1, tr(JOB_KEY[jobs[i]]), (unsigned)g_game.workers[jobs[i]]);
}

// One inventory cell: name left-aligned at the column's left edge, quantity
// right-aligned at the column's right edge (two-ends alignment). Row-major:
// idx -> row idx/INV_COLS, column idx%INV_COLS.
void drawInvCell(m5gfx::M5Canvas& c, int idx, const char* name, long qty) {
    int col = idx % INV_COLS, row = idx / INV_COLS;
    int x0 = INV_COLX[col];
    int y  = INV_ROW_TOP + row * INV_ROWH;
    cjk::drawText(c, x0, y, name, SCALE);

    char qtyStr[8];
    fmtAmount((int32_t)qty, qtyStr, sizeof(qtyStr));   // v0.3.3: 1.2K/56K/1.2M
    int qw = cjk::textWidth(qtyStr, SCALE);
    cjk::drawText(c, x0 + INV_COL_W - qw, y, qtyStr, SCALE);
}

// 库存 fieldset: the box + every non-zero resource (whole units) followed by
// every non-zero crafted item, three columns, name-left/qty-right. Past the
// 36-cell grid the last cell collapses to "…" (the grid holds the whole
// resource+item set, so the ellipsis is a vestigial guard); an empty inventory
// shows "空".
void drawInventory(m5gfx::M5Canvas& c) {
    drawFieldset(c, INV_BOX_Y0, INV_BOX_Y1, tr("stores"));   // "库存"

    int nz = 0;
    for (int r = 0; r < RES_COUNT; r++)  if (g_game.whole((uint8_t)r) > 0) nz++;
    for (int i = 0; i < ITEM_COUNT; i++) if (g_game.items[i] > 0)          nz++;

    bool overflow = nz > INV_CELLS;
    int limit = overflow ? INV_CELLS - 1 : nz;   // reserve the last cell for "…"
    int shown = 0;
    for (int r = 0; r < RES_COUNT && shown < limit; r++) {
        long q = (long)g_game.whole((uint8_t)r);
        if (q > 0) drawInvCell(c, shown++, tr(RES_KEY[r]), q);
    }
    for (int i = 0; i < ITEM_COUNT && shown < limit; i++)
        if (g_game.items[i] > 0)
            drawInvCell(c, shown++, tr(ITEM_KEY[i]), (long)g_game.items[i]);

    if (overflow) {
        int col = (INV_CELLS - 1) % INV_COLS, row = (INV_CELLS - 1) / INV_COLS;
        cjk::drawText(c, INV_COLX[col], INV_ROW_TOP + row * INV_ROWH, "…", SCALE);
    } else if (shown == 0) {
        cjk::drawText(c, INV_COLX[0], INV_ROW_TOP, tr("none"), SCALE);   // "空"
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

// Paint the whole action row (3 cells). Left = 伐木 (gather wood): always offered
// here — the Room page gated it on outsideUnlocked, which is a precondition for
// this page drawing at all. Middle = 查看陷阱 (check traps): drawn only when a
// trap stands (buildings[B_TRAP] > 0); with none, the cell is left blank (无供给
// 整格不画, not a disabled frame), matching the Room供给 condition. Right = 分工:
// always drawn, opens AssignPage (no供给/cooldown gate). A live gather/traps
// cooldown (channels 1/2) renders its cell dashed with a draining bar.
void drawActionRow(m5gfx::M5Canvas& c, uint32_t now) {
    int gcool = g_game.cooldownLeft(1, now);                 // gather channel
    drawActionBand(c, ACT_COLX[0], ACT_TOP, tr("gather wood"),
                   gcool == 0, gcool, GATHER_DELAY_S);
    if (g_game.buildings[B_TRAP] > 0) {
        int tcool = g_game.cooldownLeft(2, now);             // traps channel
        drawActionBand(c, ACT_COLX[1], ACT_TOP, tr("check traps"),
                       tcool == 0, tcool, TRAPS_DELAY_S);
    }
    // 分工 — opens the worker-assignment page. Hardcoded literal like the old
    // "更多": 分/工 are in the §8.3 closure (分享 / 工人).
    drawActionBand(c, ACT_COLX[2], ACT_TOP, "分工", true, 0, 0);
}

// The action row's partial-refresh target (Room's buttonAreaRect parity): the
// bottom-anchored row band plus a 2px bleed. tick() repaints just this rect while
// a cooldown drains, instead of a full-page redraw.
pages::Rect actionRowRect() {
    return pages::Rect{ 0, ACT_TOP - 2, 540, ACT_H + 4 };
}

// Clear the action row rect and repaint it into `c` (for the partial-refresh
// path — the surrounding full-page pixels already sit in the canvas).
void repaintActionRow(m5gfx::M5Canvas& c, uint32_t now) {
    pages::Rect r = actionRowRect();
    c.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
    drawActionRow(c, now);
}
}  // namespace

// ================================ Page API =================================

const pages::Region* OutsidePage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Hidden until the forest opens: returning false makes showPageOrNext skip this
// ring slot, so the page is invisible (and untappable) until outsideUnlocked.
bool OutsidePage::draw(m5gfx::M5Canvas& c) {
    if (!g_game.outsideUnlocked) return false;
    c.fillSprite(TFT_WHITE);
    page_tabs::draw(c, 1);           // shared tab header, Outside active
    drawWorkerSummary(c);            // 工人 fieldset (人口 line + read-only grid)
    drawBuildings(c);                // 建筑 fieldset
    drawInventory(c);                // 库存 fieldset

    // 野外 action row — the page's only Region, bottom-anchored: one type=1 band
    // carrying PARAM_ACTIONS; onLocalAction resolves 伐木 / 查看陷阱 / 分工 from x.
    drawActionRow(c, epochNow());
    m_regions[0].y0 = (uint16_t)ACT_TOP;
    m_regions[0].y1 = (uint16_t)(ACT_TOP + ACT_H);
    m_regions[0].type  = 1;
    m_regions[0].param = PARAM_ACTIONS;
    m_regionCount = 1;
    return true;
}

// Long-press on the action row -> the press column picks the verb (x thirds at
// ACT_DIV0/ACT_DIV1): 伐木 gatherWood, 查看陷阱 checkTraps (only when a trap
// stands — else a blank cell, low beep), 分工 opens the worker-assignment page.
// A success high-beeps + persists + repaints; a rejected/blank press low-beeps.
void OutsidePage::onLocalAction(uint8_t param, int x, int y) {
    (void)y;
    if (param != PARAM_ACTIONS) { M5.Speaker.tone(600, 120); return; }

    // 分工 (right third): jump to AssignPage. open() flips its visibility gate;
    // showPage draws it (its draw() now returns true) and persists it as current.
    if (x >= ACT_DIV1) {
        assign_page::open();
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::ringIndexByName("assign"), false);
        return;
    }

    uint32_t now = epochNow();
    Result r;
    if (x < ACT_DIV0) {
        r = g_game.gatherWood(now);                   // 伐木 (left third)
    } else if (g_game.buildings[B_TRAP] > 0) {
        r = g_game.checkTraps(now);                   // 查看陷阱 (middle third)
    } else {
        M5.Speaker.tone(600, 120);                    // blank cell (no trap stands)
        return;
    }
    if (r == RC_OK) {
        M5.Speaker.tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
    } else {
        M5.Speaker.tone(600, 120);                    // cooldown / engine reject
    }
}

// Time axis (awake only). Settle the economy each second, then repaint on any
// change to a painted number/label (population, workers, buildings, inventory).
// tick 签名 keeps the worker mix so a change made on AssignPage (then paged back)
// still repaints the worker summary here. The bottom action-row cooldowns get the
// same partial-refresh path the Room page uses; everything else is a full redraw.
void OutsidePage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick = 0;
    static uint32_t s_lastSig  = 0;
    static bool     s_wasCooling = false;

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    uint32_t now = epochNow();
    g_game.settle(now);

    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix(g_game.population); mix((uint32_t)g_game.maxPopulation());
    mix(g_game.outsideUnlocked ? 1u : 0u);
    mix((uint32_t)(uint8_t)g_game.fire);       // Room tab title (shared header)
    for (int i = 0; i < JOB_COUNT; i++) mix(g_game.workers[i]);
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);
    // Inventory section: repaint when any shown store/item count changes.
    for (int i = 0; i < RES_COUNT; i++)  mix((uint32_t)g_game.whole((uint8_t)i));
    for (int i = 0; i < ITEM_COUNT; i++) mix((uint32_t)g_game.items[i]);

    // 野外 action-row cooldowns (gather ch1 / traps ch2) drain a progress bar,
    // but — unlike the content above — they are NOT in the content signature: the
    // wood/meat they yield is banked at press time, so nothing else changes while
    // they cool. Mirror the Room page: while either is live (or on the tick it
    // clears) repaint JUST the action row, QUALITY on the clearing tick to wipe
    // the bar ghost and restore the solid frame.
    bool cooling = g_game.cooldownLeft(1, now) > 0 ||
                   (g_game.buildings[B_TRAP] > 0 && g_game.cooldownLeft(2, now) > 0);

    if (sig != s_lastSig) {
        s_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);   // recomputes the page
        s_wasCooling = cooling;
        return;
    }

    if (cooling || s_wasCooling) {
        repaintActionRow(canvas, now);
        bool cleared = (!cooling && s_wasCooling);
        pager::partialRefresh(actionRowRect(),
                              cleared ? pages::RefreshMode::QUALITY
                                      : pages::RefreshMode::FAST);
    }
    s_wasCooling = cooling;
}
