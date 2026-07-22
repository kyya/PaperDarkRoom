// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Outside (village) page — the real Phase-1 Outside UI, driven by the
// game_state engine (src/game_state.*). Every piece of text routes through
// tr() (strings_zh.h) so only the official Simplified-Chinese translation ever
// reaches the sparse 12px CJK face — the §8.3 glyph-closure iron law. Layout
// follows the §9.4 vertical budget (24px CJK, >=80px long-press bands, paginate
// rather than compress). See outside_page.h for the region model.
#include "outside_page.h"
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
constexpr int MAX_ROWS  = 4;                 // matches OutsidePage::MAX_BANDS (rows)
constexpr int MAX_COLS  = 2;                 // two-column worker grid
constexpr int MAX_SLOTS = MAX_ROWS * MAX_COLS;   // 8 worker cells / page

// ---- vertical budget (§9.4), all measured to clear the 32px status bar. The
// shared tab header (page_tabs::TAB_H = 72px) owns the top band, so the info block
// reflows below it and the page ends with a merged-in inventory section (the
// standalone Inventory page was folded here, fw 0.2.2). fw 0.2.3 inserts a野外
// action row (伐木 / 查看陷阱, migrated off the Room page — they are upstream
// outside.js actions) between the building summary and the worker bands: one
// 80px band-row (ACT_H + 10px gap = 90px). Everything below shifts down 90px, so
// the inventory grid gives back exactly the 3 rows that 90px is worth
// (INV_ROWS 9 -> 6). Long-press bands stay 80px (§9.3's hard floor). Layout
// top->bottom:
//   header(0..72) · population(80) · buildings(120, <=4x2) · action row
//   (258, 1x80: 伐木 | 查看陷阱) · worker bands (348, <=4x80) ·
//   inventory (bordered box 720..916, legend "库存" embedded in the top
//   border at 708..732, up to 6x3 rows inside — v0.3.2: 2 cols -> 3 cols,
//   the resource set outgrew 12 cells (trading post unlocks 6 more goods)).
// ----------------------------------------------------------------------------
constexpr int POP_Y     = 80;                // population row (below the header)
constexpr int BLD_TOP   = 120;               // building summary top
constexpr int BLD_ROWH  = 32;                // per building row
constexpr int BLD_ROWS  = 4;                 // <=4 rows x 2 cols (8 cells)
constexpr int BLD_COLX[2] = { PAD, 288 };    // two-column x origins (Room parity)

// 野外 action row: 伐木 (left col) / 查看陷阱 (right col), same two-column
// geometry as the worker bands (COL_X0 / COL_W below). Buildings end at
// 120 + 4*32 = 248; the row sits at 258 (10px gap), ends 338, then a 10px gap
// to the first worker band.
constexpr int ACT_TOP   = 258;               // action row top (below buildings)
constexpr int ACT_H     = 80;                // long-press band (§9.3: >=80px floor)

constexpr int BAND_TOP  = 348;               // first worker row top (below action row)
constexpr int BAND_H    = 80;                // long-press band (§9.3: >=80px, hard floor)
constexpr int BAND_GAP  = 10;                // vertical gap between rows
// 4 worker rows end at 348 + 3*90 + 80 = 698. The inventory section (folded in
// from the old Inventory page) sits below as a bordered box, matching upstream
// A Dark Room's fieldset-style stores panel: a 1px rect (x 24..516) with the
// "库存" legend embedded in the top border line (the line breaks around the
// text, a few px of gap on each side — same idea as an HTML <fieldset>). Inside,
// non-zero stores (whole units) then non-zero items fill a THREE-column grid
// (v0.3.2: was 2 cols — the trading post's 6 new buyable resources routinely
// push the non-zero count past 12), each cell "name ... qty" with the name
// left-aligned and the quantity right-aligned (two ends of the column, not a
// single flush-left string). 6 rows x 3 = 18 cells fit before the bottom
// border at 916 (< 928 status bar); a 19th+ entry collapses the last cell to
// "…" (U+2026 is in the glyph closure). Column budget check (scratchpad/
// measure_inv3.cpp): the widest name ("energy cell"/"alien alloy" -> 4 CJK
// glyphs, 96px at scale=2) plus a worst-case abbreviated qty (v0.3.3 fmtAmount:
// "999K" = 50px / "1.2M" = 48px, and real stockpiles hit the thousands) is
// ~146px, inside the 148px column with a couple px to spare — every RES_KEY/
// ITEM_KEY name clears its abbreviated quantity with no overlap.
constexpr int INV_LABEL_Y     = 708;         // legend text top (glyph top)
constexpr int INV_BOX_X0      = PAD;         // 24 — box left edge
constexpr int INV_BOX_X1      = 540 - PAD;   // 516 — box right edge
constexpr int INV_BOX_Y0      = INV_LABEL_Y + GLYPH / 2;   // 720 — top border,
                                              // through the legend's vertical
                                              // center (fieldset/legend style)
constexpr int INV_BOX_Y1      = 916;         // bottom border (< 928 status bar)
constexpr int INV_LEGEND_INSET = 8;          // left corner -> where the top
                                              // border line breaks
constexpr int INV_LEGEND_GAP   = 4;          // gap on each side of the legend
                                              // text before the line resumes
constexpr int INV_PAD_SIDE    = 12;          // box border -> column content inset
// The legend glyph straddles the top border (centered on it, see INV_BOX_Y0
// above), so it extends GLYPH/2 = 12px BELOW the line itself, to y=732 —
// row 0 must clear THAT, not the border line, or it collides with "库存"
// (both sit at column-0's x=36). >=8px clearance below the glyph bottom:
constexpr int INV_LEGEND_BOTTOM = INV_LABEL_Y + GLYPH;      // 732
constexpr int INV_ROW_GAP       = 12;        // clearance below the legend glyph
constexpr int INV_ROW_TOP     = INV_LEGEND_BOTTOM + INV_ROW_GAP;  // 744 — first row top
constexpr int INV_ROWH        = 28;          // per inventory cell row height
constexpr int INV_ROWS        = 6;           // 744 + 6*28 = 912 (< INV_BOX_Y1 916)
constexpr int INV_COLS        = 3;           // v0.3.2: 2 -> 3 (trading post goods)
constexpr int INV_CELLS       = INV_ROWS * INV_COLS;   // 18 cells before overflow
constexpr int INV_COL_GAP     = 12;          // gutter between columns (v0.3.2: 16 -> 12
                                              // to make room for a 3rd column)
constexpr int INV_CONTENT_X0  = INV_BOX_X0 + INV_PAD_SIDE;   // 36
constexpr int INV_CONTENT_X1  = INV_BOX_X1 - INV_PAD_SIDE;   // 504
// 468px content / 3 cols with 2 gutters: (468 - 2*12) / 3 = 148 exactly
// (3*148 + 2*12 == 468, so the 3rd column's right edge lands exactly on
// INV_CONTENT_X1 — no leftover slack).
constexpr int INV_COL_W       = (INV_CONTENT_X1 - INV_CONTENT_X0
                                  - (INV_COLS - 1) * INV_COL_GAP) / INV_COLS; // 148
constexpr int INV_COLX[INV_COLS] = {
    INV_CONTENT_X0,                                       // 36
    INV_CONTENT_X0 + (INV_COL_W + INV_COL_GAP),            // 196
    INV_CONTENT_X0 + 2 * (INV_COL_W + INV_COL_GAP),        // 356
};
// Two 240px columns with a 12px gutter fill the 492px content width. Each worker
// band (v0.3.3) is a left label zone + a right vertical stepper: "名 xN" left-
// aligned in the label zone, and a stepper zone on the right with ▲ (increment,
// upper half) over ▼ (decrement, lower half), split by a horizontal rule, the
// zone itself set off by a vertical rule. The press x picks the column
// (COL_MID); the press y then picks the stepper half (upper = +1, lower = −1) —
// replacing v0.3.2's left−/right＋ x-split. The widest shown worker label
// ("炼钢工人 x80" = 144px @24px, scratchpad/measure_jobs) clears the 148px label
// zone, so the label and stepper never collide.
constexpr int COL_GAP   = 12;
constexpr int COL_W     = (CONTENT_W - COL_GAP) / 2;         // 240
constexpr int COL_X0[MAX_COLS] = { PAD, PAD + COL_W + COL_GAP };   // {24, 276}
constexpr int COL_MID   = 540 / 2;           // 270: x < MID => left column

// Worker-band stepper geometry (local x within a COL_W band). The label zone
// takes the left ~150px; a vertical rule then a 66px stepper zone (12px right
// inset) holds the two triangles. Triangles are drawn geometrically (fillTriangle)
// — ▲/▼ are not guaranteed in the sparse 12px face, so never render them as text.
constexpr int STEP_INSET = 12;                       // stepper zone right inset
constexpr int STEP_W     = 66;                       // stepper zone width
constexpr int STEP_X0    = COL_W - STEP_INSET - STEP_W;   // 162 — stepper zone left
constexpr int STEP_DIV_X = STEP_X0 - 4;              // 158 — vertical rule x
constexpr int LABEL_X    = 8;                        // label left pad in the band
constexpr int TRI_HALF_W = 11;                       // triangle half base
constexpr int TRI_HALF_H = 9;                        // triangle half height

// Action-row buttons (伐木/查看陷阱) use a 36px label (v0.3.3, 原作「框大字小」),
// unlike the worker bands which stay 24px (compound stepper controls, space is
// tight beside the ▲▼ zone).
constexpr int BTN_SCALE = 3;                 // 12px grid x3 = 36px (action label)
constexpr int BTN_GLYPH = 12 * BTN_SCALE;    // 36px line box

// "更多" pagination sentinel; real params are Job ids (J_HUNTER..J_ARMOURER).
constexpr uint8_t A_MORE = 0xFF;

// The野外 action row (伐木 / 查看陷阱) is a single Region carrying this sentinel
// param — distinct from the worker row indices (0..MAX_ROWS-1) that share the
// region table — so onLocalAction can tell the two row kinds apart (no page.h /
// pager change). Its two cells route to gatherWood/checkTraps by the press
// column, exactly as the Room page's A_GATHER/A_TRAPS did.
constexpr uint8_t PARAM_ACTIONS = 0xFE;

struct BandView {
    uint8_t code;                // Job id, or A_MORE
    char    label[48];           // "猎人 x3" for a job, "更多 (p/n)" for more
};

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
// gatherer is derived (idle population) and never gets a band. Miners map to
// BLD_NONE (P2 mines), so they never appear. Returns the count.
int buildJobs(uint8_t* out, int cap) {
    int n = 0;
    for (uint8_t j = J_HUNTER; j < JOB_COUNT && n < cap; j++) {
        uint8_t reqB = JOB_REQ_BLD[j];
        if (reqB != BLD_NONE && g_game.buildings[reqB] > 0) out[n++] = j;
    }
    return n;
}

// Compute the visible worker grid for `page`. Fills slotCodes[] (row-major:
// slot s -> row s/2, col s%2) + views[] (code + label) for painting, and
// regionsOut[] with ONE y-band per ROW (param = row index). Both columns of a
// row share a row band; the pager hit-tests y only, so onLocalAction resolves
// the column — and then the −/＋ half — from the press x. Batches of 7 real
// jobs + a trailing "更多" cell once the full list exceeds MAX_SLOTS (Room's
// 手法). *slotCountOut receives the filled cell count; returns the ROW count.
int layoutBands(pages::Region* regionsOut, uint8_t* slotCodes, BandView* views,
                int page, int* slotCountOut) {
    uint8_t all[JOB_COUNT];
    int total = buildJobs(all, (int)sizeof(all));

    int numPages, start, take;
    bool more;
    int pg = 0;
    if (total <= MAX_SLOTS) {
        numPages = 1; start = 0; take = total; more = false;
    } else {
        int perPage = MAX_SLOTS - 1;                 // 7 real + 1 "更多"
        numPages = (total + perPage - 1) / perPage;
        pg = ((page % numPages) + numPages) % numPages;
        start = pg * perPage;
        take = total - start; if (take > perPage) take = perPage;
        more = true;
    }

    int k = 0;
    for (int i = 0; i < take && k < MAX_SLOTS; i++) slotCodes[k++] = all[start + i];
    if (more && k < MAX_SLOTS) slotCodes[k++] = A_MORE;
    int slotCount = k;

    for (int s = 0; s < slotCount; s++) {
        views[s].code = slotCodes[s];
        if (slotCodes[s] == A_MORE)
            snprintf(views[s].label, sizeof(views[s].label), "更多 (%d/%d)",
                     (pg + 1 < numPages ? pg + 2 : 1), numPages);
        else
            snprintf(views[s].label, sizeof(views[s].label), "%s x%u",
                     tr(JOB_KEY[slotCodes[s]]),
                     (unsigned)g_game.workers[slotCodes[s]]);
    }

    int rows = (slotCount + MAX_COLS - 1) / MAX_COLS;
    for (int r = 0; r < rows; r++) {
        int top = BAND_TOP + r * (BAND_H + BAND_GAP);
        regionsOut[r].y0 = (uint16_t)top;
        regionsOut[r].y1 = (uint16_t)(top + BAND_H);
        regionsOut[r].type = 1;                      // firmware-local
        regionsOut[r].param = (uint8_t)r;            // row; onLocalAction adds col from x
    }
    *slotCountOut = slotCount;
    return rows;
}

// ---- drawing pieces --------------------------------------------------------

// Population row: official "人口 " label (tr key "pop ") + X/Y (Y = huts x4),
// then the derived idle gatherer count on the same 24px baseline.
void drawPopRow(m5gfx::M5Canvas& c) {
    char pop[40];
    snprintf(pop, sizeof(pop), "%s%u/%u", tr("pop "),
             (unsigned)g_game.population, (unsigned)g_game.maxPopulation());
    int x = cjk::drawText(c, PAD, POP_Y, pop, SCALE);

    char gath[40];
    snprintf(gath, sizeof(gath), "%s x%d", tr("gatherer"), g_game.numGatherers());
    cjk::drawText(c, x + GLYPH, POP_Y, gath, SCALE);   // gap then gatherers
}

// Non-zero buildings as "名 数量", 2 columns x up to 4 rows. Overflow past the
// 8 cells collapses to a trailing "..." (ASCII dots — … is not in the closure).
void drawBuildings(m5gfx::M5Canvas& c) {
    const int cap = BLD_ROWS * 2;               // 8 cells
    int nz = 0;
    for (int b = 0; b < BLD_COUNT; b++)
        if (g_game.buildings[b] > 0) nz++;
    bool overflow = nz > cap;
    int limit = overflow ? cap - 1 : nz;        // reserve the last cell for "..."
    int shown = 0;
    for (int b = 0; b < BLD_COUNT && shown < limit; b++) {
        if (g_game.buildings[b] == 0) continue;
        int col = shown % 2, row = shown / 2;
        char line[48];
        snprintf(line, sizeof(line), "%s %u",
                 tr(BLD_KEY[b]), (unsigned)g_game.buildings[b]);
        cjk::drawText(c, BLD_COLX[col], BLD_TOP + row * BLD_ROWH, line, SCALE);
        shown++;
    }
    if (overflow) {
        int col = (cap - 1) % 2, row = (cap - 1) / 2;
        cjk::drawText(c, BLD_COLX[col], BLD_TOP + row * BLD_ROWH, "...", SCALE);
    }
}

// One inventory cell: name left-aligned at the column's left edge, quantity
// right-aligned at the column's right edge (two-ends alignment, matching
// upstream's fieldset stores panel — not a single flush-left "name qty" run).
// Row-major: idx -> row idx/INV_COLS, column idx%INV_COLS (v0.3.2: 3 cols).
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

// The bordered inventory box itself: a 1px rect with the "库存" legend embedded
// in the top border line, fieldset/legend style — the line runs from the box's
// left edge to just before the text, breaks for the text (plus a small gap on
// each side), then resumes to the right edge. The three remaining sides are
// plain rect edges.
void drawInventoryBox(m5gfx::M5Canvas& c) {
    const char* legend = tr("stores");                          // "库存"
    int legendW  = cjk::textWidth(legend, SCALE);
    int lineEnd  = INV_BOX_X0 + INV_LEGEND_INSET;                // 32
    int textX    = lineEnd + INV_LEGEND_GAP;                     // 36
    int resumeX  = textX + legendW + INV_LEGEND_GAP;

    c.drawFastHLine(INV_BOX_X0, INV_BOX_Y0, lineEnd - INV_BOX_X0, TFT_BLACK);
    if (resumeX < INV_BOX_X1)
        c.drawFastHLine(resumeX, INV_BOX_Y0, INV_BOX_X1 - resumeX, TFT_BLACK);
    cjk::drawText(c, textX, INV_BOX_Y0 - GLYPH / 2, legend, SCALE);

    c.drawFastVLine(INV_BOX_X0, INV_BOX_Y0, INV_BOX_Y1 - INV_BOX_Y0, TFT_BLACK);
    c.drawFastVLine(INV_BOX_X1, INV_BOX_Y0, INV_BOX_Y1 - INV_BOX_Y0, TFT_BLACK);
    c.drawFastHLine(INV_BOX_X0, INV_BOX_Y1, INV_BOX_X1 - INV_BOX_X0 + 1, TFT_BLACK);
}

// The merged-in inventory section: the bordered box + legend, then every
// non-zero resource (whole units) followed by every non-zero crafted item,
// three columns (v0.3.2), name-left/qty-right. Past the 18-cell grid the last
// cell collapses to "…" (Phase 1's usual count fits, so the ellipsis is a rare
// tail guard); an empty inventory shows "空".
void drawInventory(m5gfx::M5Canvas& c) {
    drawInventoryBox(c);

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

// A job band is disabled when NEITHER stepper half can act: no worker in the
// job to pull back to idle (minus) AND no idle villager to add (plus).
// assignWorker() has no cost/prerequisite gate of its own — a band never
// appears until its building exists (buildJobs already filters that) — so
// "no assignable villagers" is the only one of the three general disable
// categories (资源不足/前置未满足/无可分配村民) that applies to this page's
// buttons. Reads GameState's public fields only; the engine is untouched.
bool jobBandDisabled(uint8_t job) {
    return g_game.workers[job] == 0 && g_game.numGatherers() == 0;
}

// One half-width worker band at column origin x0: enabled = 2px frame (two
// concentric rects); disabled (see jobBandDisabled) = a single 1px dashed outer
// frame, no inner ring — the label stays normal 24px either way. Left: the
// "名 xN" label, left-aligned in the label zone, vertically centered. Right: a
// vertical stepper — a vertical rule sets off the stepper zone, a horizontal
// rule splits it into an upper ▲ (increment) and lower ▼ (decrement) half, each
// triangle drawn with fillTriangle (~22px, no font dependency). Press behavior:
// a disabled band's press still resolves to an engine no-op (low beep), same as
// before; the ▲/▼ are hit by the press y-half, not the exact triangle pixels
// (see onLocalAction).
void drawJobBand(m5gfx::M5Canvas& c, int x0, int top, const char* label,
                  bool disabled) {
    if (disabled) {
        drawDashedRect(c, x0, top, COL_W, BAND_H);
    } else {
        c.drawRect(x0, top, COL_W, BAND_H, TFT_BLACK);
        c.drawRect(x0 + 1, top + 1, COL_W - 2, BAND_H - 2, TFT_BLACK);
    }

    // Label — left-aligned, vertically centered (24px).
    cjk::drawText(c, x0 + LABEL_X, top + (BAND_H - GLYPH) / 2 - 4, label, SCALE);

    // Stepper zone: vertical rule + horizontal split rule.
    int midY = top + BAND_H / 2;
    c.drawFastVLine(x0 + STEP_DIV_X, top + 10, BAND_H - 20, TFT_BLACK);
    c.drawFastHLine(x0 + STEP_X0, midY, STEP_W, TFT_BLACK);

    int cx   = x0 + STEP_X0 + STEP_W / 2;     // stepper column center
    int cyUp = top + BAND_H / 4;              // ▲ center (upper half)
    int cyDn = top + 3 * BAND_H / 4;          // ▼ center (lower half)
    // ▲ increment (apex up)
    c.fillTriangle(cx, cyUp - TRI_HALF_H,
                   cx - TRI_HALF_W, cyUp + TRI_HALF_H,
                   cx + TRI_HALF_W, cyUp + TRI_HALF_H, TFT_BLACK);
    // ▼ decrement (apex down)
    c.fillTriangle(cx, cyDn + TRI_HALF_H,
                   cx - TRI_HALF_W, cyDn - TRI_HALF_H,
                   cx + TRI_HALF_W, cyDn - TRI_HALF_H, TFT_BLACK);
}

// The trailing "更多" cell: half-width frame, centered label, no −/＋ (either
// half flips the page).
void drawMoreBand(m5gfx::M5Canvas& c, int x0, int top, const char* label) {
    c.drawRect(x0, top, COL_W, BAND_H, TFT_BLACK);
    c.drawRect(x0 + 1, top + 1, COL_W - 2, BAND_H - 2, TFT_BLACK);
    int lw = cjk::textWidth(label, SCALE);
    cjk::drawText(c, x0 + (COL_W - lw) / 2, top + (BAND_H - GLYPH) / 2 - 4,
                  label, SCALE);
}

// One half-width野外 action cell (伐木 / 查看陷阱) at column origin x0 — the
// same frame language the Room page uses (drawBand): enabled = solid double
// ring; unavailable/cooling = 1px dashed outer frame. A live cooldown draws a
// thin progress bar hugging the band's inner bottom edge (v0.3.3: reverted from
// v0.3.2's whole-button grey fill, which read as too heavy on the panel — Room
// drawBand parity): an 8px-tall outlined bar inset 12px, its inner fill width =
// the fraction of the cooldown remaining, draining left-anchored to 0. The 36px
// label centers in the band, clear above the bar. Geometry is identical to a
// worker band; only the stepper is absent — this is a single long-press verb.
void drawActionBand(m5gfx::M5Canvas& c, int x0, int top, const char* label,
                    bool enabled, int coolLeft, int coolTotal) {
    if (enabled) {
        c.drawRect(x0, top, COL_W, ACT_H, TFT_BLACK);
        c.drawRect(x0 + 1, top + 1, COL_W - 2, ACT_H - 2, TFT_BLACK);
    } else {
        drawDashedRect(c, x0, top, COL_W, ACT_H);
    }

    int lw = cjk::textWidth(label, BTN_SCALE);
    cjk::drawText(c, x0 + (COL_W - lw) / 2, top + (ACT_H - BTN_GLYPH) / 2 - 4,
                  label, BTN_SCALE);

    if (coolTotal > 0 && coolLeft > 0) {                      // draining cooldown
        int barX0 = x0 + 12, barX1 = x0 + COL_W - 12;
        int barY = top + ACT_H - 16, barH = 8;
        c.drawRect(barX0, barY, barX1 - barX0, barH, TFT_BLACK);
        int inner = barX1 - barX0 - 4;
        int fw = (int)((int64_t)inner * coolLeft / coolTotal);   // drains L-anchored
        if (fw > 0) c.fillRect(barX0 + 2, barY + 2, fw, barH - 4, TFT_BLACK);
    }
}

// Paint the whole action row. Left column = 伐木 (gather wood): always offered
// here — the Room page gated it on outsideUnlocked, which is a precondition for
// this page drawing at all. Right column = 查看陷阱 (check traps): drawn only
// when a trap stands (buildings[B_TRAP] > 0); with none, the cell is left blank
// (无供给整格不画, not a disabled frame), matching the Room供给 condition. A live
// gather/traps cooldown (channels 1/2) renders the cell dashed with a draining
// bar, same as the Room page.
void drawActionRow(m5gfx::M5Canvas& c, uint32_t now) {
    int gcool = g_game.cooldownLeft(1, now);                 // gather channel
    drawActionBand(c, COL_X0[0], ACT_TOP, tr("gather wood"),
                   gcool == 0, gcool, GATHER_DELAY_S);
    if (g_game.buildings[B_TRAP] > 0) {
        int tcool = g_game.cooldownLeft(2, now);             // traps channel
        drawActionBand(c, COL_X0[1], ACT_TOP, tr("check traps"),
                       tcool == 0, tcool, TRAPS_DELAY_S);
    }
}

// The action row's partial-refresh target (Room's buttonAreaRect parity): the
// row band plus a 2px bleed. tick() repaints just this rect while a cooldown
// drains, instead of a full-page redraw.
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

// Place slot s at row s/2, column s%2 (row-major reading order).
void paintBands(m5gfx::M5Canvas& c, const BandView* views, int slotCount) {
    for (int s = 0; s < slotCount; s++) {
        int row = s / MAX_COLS, col = s % MAX_COLS;
        int top = BAND_TOP + row * (BAND_H + BAND_GAP);
        int x0 = COL_X0[col];
        if (views[s].code == A_MORE) drawMoreBand(c, x0, top, views[s].label);
        else                         drawJobBand(c, x0, top, views[s].label,
                                                  jobBandDisabled(views[s].code));
    }
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
    drawPopRow(c);
    drawBuildings(c);

    // 野外 action row — region[0]: one type=1 band carrying PARAM_ACTIONS; the
    // press column resolves 伐木 vs 查看陷阱 in onLocalAction.
    drawActionRow(c, epochNow());
    m_regions[0].y0 = (uint16_t)ACT_TOP;
    m_regions[0].y1 = (uint16_t)(ACT_TOP + ACT_H);
    m_regions[0].type  = 1;
    m_regions[0].param = PARAM_ACTIONS;

    // Worker bands — region[1..]; layoutBands sets each param = worker row index.
    BandView views[MAX_SLOTS];
    int workerRows = layoutBands(m_regions + 1, m_slotCodes, views, m_page, &m_slotCount);
    m_regionCount = 1 + workerRows;
    paintBands(c, views, m_slotCount);
    drawInventory(c);                // merged-in stores/items section (lower band)
    return true;
}

// Long-press on a band -> assign/unassign a villager. x resolves the column;
// then, within the hit band, the press y picks the stepper half — upper half
// (▲) increments, lower half (▼) decrements (v0.3.3, replacing the old left−/
// right＋ x-split). Each half is a ~40px sub-target: below §9.3's 65px pointer-
// target guideline, but the direction is trivially reversible (added one too
// many? press ▼) and the whole band still clears the 80px long-press floor — an
// accepted trade for the clearer stepper look. Any real change high-beeps,
// persists, and repaints; a no-op (no idle population to add, or already 0) low
// beeps. "更多" flips to the next batch of jobs. save() lives here (the engine
// action does not persist itself — single write, no double-save).
void OutsidePage::onLocalAction(uint8_t param, int x, int y) {
    // 野外 action row (伐木 / 查看陷阱) — the migrated Room verbs. Same GameState
    // path and feedback as the Room page: the press column picks the verb, a
    // success high-beeps + persists + repaints, a rejected/blank press low-beeps.
    if (param == PARAM_ACTIONS) {
        uint32_t now = epochNow();
        int col = (x < COL_MID) ? 0 : 1;              // left 伐木, right 查看陷阱
        Result r;
        if (col == 0) {
            r = g_game.gatherWood(now);
        } else if (g_game.buildings[B_TRAP] > 0) {
            r = g_game.checkTraps(now);
        } else {
            M5.Speaker.tone(600, 120);                // blank cell (no trap stands)
            return;
        }
        if (r == RC_OK) {
            M5.Speaker.tone(1800, 80);
            g_game.save();
            pager::showPage(pager::currentRingIndex(), false);
        } else {
            M5.Speaker.tone(600, 120);                // cooldown / engine reject
        }
        return;
    }

    // param is the worker ROW index; the press x resolves the column (x < COL_MID
    // = left), the press y the stepper half (below). An empty cell (odd count's
    // trailing column) low-beeps and does nothing.
    int row  = param;
    int col  = (x < COL_MID) ? 0 : 1;
    int slot = row * MAX_COLS + col;
    if (slot < 0 || slot >= m_slotCount) { M5.Speaker.tone(600, 120); return; }
    uint8_t code = m_slotCodes[slot];

    if (code == A_MORE) {
        m_page++;
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::currentRingIndex(), false);
        return;
    }
    uint8_t job = code;
    if (job == J_GATHERER || job >= JOB_COUNT) { M5.Speaker.tone(600, 120); return; }

    int before = (int)g_game.workers[job];
    // Stepper direction from the press y-half against the hit band's midline:
    // upper half (▲) increments, lower half (▼) decrements.
    int bandTop = BAND_TOP + row * (BAND_H + BAND_GAP);
    int delta = (y < bandTop + BAND_H / 2) ? +1 : -1;
    g_game.assignWorker(job, delta);

    if ((int)g_game.workers[job] != before) {   // a villager actually moved
        M5.Speaker.tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
    } else {
        M5.Speaker.tone(600, 120);              // no idle pop / already at 0
    }
}

// Time axis (awake only). Settle the economy each second, then repaint on any
// change to a painted number/label (population, workers, buildings, worker
// disabled-frames — see jobBandDisabled). No clock header lives on this page
// anymore, so there is no per-minute chrome refresh to do; a quiet second is a
// pure no-op. Worker bands carry no cooldown, so — unlike Room — every change
// is a full redraw; there is no partial button-area path.
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
    // but — unlike the worker bands — they are NOT in the content signature: the
    // wood/meat they yield is banked at press time, so nothing else changes while
    // they cool. Mirror the Room page: while either is live (or on the tick it
    // clears) repaint JUST the action row, QUALITY on the clearing tick to wipe
    // the bar ghost and restore the solid frame.
    bool cooling = g_game.cooldownLeft(1, now) > 0 ||
                   (g_game.buildings[B_TRAP] > 0 && g_game.cooldownLeft(2, now) > 0);

    if (sig != s_lastSig) {
        s_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);   // recomputes bands
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
