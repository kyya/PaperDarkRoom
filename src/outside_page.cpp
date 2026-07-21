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
#include "page_header.h"
#include "pomo_page.h"          // PAD, HDR_DIV_Y (shared layout authority)
#include "pager.h"
#include "game_state.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// main.cpp owns both the game model and the full-screen sprite.
extern adr::GameState g_game;
extern M5Canvas canvas;

using namespace adr;

namespace {
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px CJK
constexpr int GLYPH     = 12 * SCALE;        // 24px line box
constexpr int MAX_BANDS = 4;                 // matches OutsidePage::MAX_BANDS

// ---- vertical budget (§9.4), all measured to clear the 32px status bar -----
constexpr int POP_Y     = 120;               // population row (below the rule)
constexpr int BLD_TOP   = 172;               // building summary top
constexpr int BLD_ROWH  = 28;                // per building row
constexpr int BLD_ROWS  = 4;                 // <=4 rows x 2 cols (8 cells)
constexpr int BLD_COLX[2] = { PAD, 288 };    // two-column x origins (Room parity)

constexpr int BAND_TOP  = 300;               // first worker band top
constexpr int BAND_H    = 92;                // long-press band (§9.3: >=80px)
constexpr int BAND_GAP  = 12;
constexpr int BAND_X0   = PAD;               // 24
constexpr int BAND_X1   = 540 - PAD;         // 516
constexpr int BAND_MID  = 270;               // −/＋ split x (= center of band)

// Big geometric −/＋ glyphs, drawn as bars (no font dependency, unambiguous
// affordance). Minus sits in the left (decrement) half by the divider; plus is
// centered in the right (increment) half.
constexpr int SYM_LEN   = 44;                // bar long axis
constexpr int SYM_TH    = 10;                // bar thickness
constexpr int MINUS_CX  = 235;               // left of the divider
constexpr int PLUS_CX   = (BAND_MID + BAND_X1) / 2;   // 393, right-half center

// "更多" pagination sentinel; real params are Job ids (J_HUNTER..J_ARMOURER).
constexpr uint8_t A_MORE = 0xFF;

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

// Compute the visible bands for `page`: fills regionsOut[] (y-geometry + job
// param) and views[] (code + label). Batches of 3 real jobs + a trailing "更多"
// band once the full list exceeds MAX_BANDS (Room's手法). Returns the count.
int layoutBands(pages::Region* regionsOut, BandView* views, int page) {
    uint8_t all[JOB_COUNT];
    int total = buildJobs(all, (int)sizeof(all));

    int numPages, start, take;
    bool more;
    int pg = 0;
    if (total <= MAX_BANDS) {
        numPages = 1; start = 0; take = total; more = false;
    } else {
        int perPage = MAX_BANDS - 1;                 // 3 real + 1 "更多"
        numPages = (total + perPage - 1) / perPage;
        pg = ((page % numPages) + numPages) % numPages;
        start = pg * perPage;
        take = total - start; if (take > perPage) take = perPage;
        more = true;
    }

    int k = 0;
    for (int i = 0; i < take && k < MAX_BANDS; i++) views[k++].code = all[start + i];
    if (more && k < MAX_BANDS) views[k++].code = A_MORE;

    for (int i = 0; i < k; i++) {
        int top = BAND_TOP + i * (BAND_H + BAND_GAP);
        regionsOut[i].y0 = (uint16_t)top;
        regionsOut[i].y1 = (uint16_t)(top + BAND_H);
        regionsOut[i].type = 1;                      // firmware-local
        regionsOut[i].param = views[i].code;
        if (views[i].code == A_MORE)
            snprintf(views[i].label, sizeof(views[i].label), "更多 (%d/%d)",
                     (pg + 1 < numPages ? pg + 2 : 1), numPages);
        else
            snprintf(views[i].label, sizeof(views[i].label), "%s x%u",
                     tr(JOB_KEY[views[i].code]),
                     (unsigned)g_game.workers[views[i].code]);
    }
    return k;
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

// One worker band: 2px frame, left label "名 xN", a vertical divider at the
// −/＋ split, a big minus bar in the left half and a big plus in the right half.
void drawJobBand(m5gfx::M5Canvas& c, int top, const char* label) {
    c.drawRect(BAND_X0, top, BAND_X1 - BAND_X0, BAND_H, TFT_BLACK);
    c.drawRect(BAND_X0 + 1, top + 1, BAND_X1 - BAND_X0 - 2, BAND_H - 2, TFT_BLACK);

    int cy = top + BAND_H / 2;
    c.fillRect(BAND_MID - 1, top + 12, 2, BAND_H - 24, TFT_BLACK);   // divider

    cjk::drawText(c, BAND_X0 + 8, top + (BAND_H - GLYPH) / 2 - 4, label, SCALE);

    // minus (left/decrement half)
    c.fillRect(MINUS_CX - SYM_LEN / 2, cy - SYM_TH / 2, SYM_LEN, SYM_TH, TFT_BLACK);
    // plus (right/increment half)
    c.fillRect(PLUS_CX - SYM_LEN / 2, cy - SYM_TH / 2, SYM_LEN, SYM_TH, TFT_BLACK);
    c.fillRect(PLUS_CX - SYM_TH / 2, cy - SYM_LEN / 2, SYM_TH, SYM_LEN, TFT_BLACK);
}

// The trailing "更多" band: framed, centered label, no −/＋ (it flips the page).
void drawMoreBand(m5gfx::M5Canvas& c, int top, const char* label) {
    c.drawRect(BAND_X0, top, BAND_X1 - BAND_X0, BAND_H, TFT_BLACK);
    c.drawRect(BAND_X0 + 1, top + 1, BAND_X1 - BAND_X0 - 2, BAND_H - 2, TFT_BLACK);
    int lw = cjk::textWidth(label, SCALE);
    cjk::drawText(c, (540 - lw) / 2, top + (BAND_H - GLYPH) / 2 - 4, label, SCALE);
}

void paintBands(m5gfx::M5Canvas& c, const BandView* views, int count) {
    for (int i = 0; i < count; i++) {
        int top = BAND_TOP + i * (BAND_H + BAND_GAP);
        if (views[i].code == A_MORE) drawMoreBand(c, top, views[i].label);
        else                         drawJobBand(c, top, views[i].label);
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
    page_header::draw(c);             // clock header + dashed rule (every page)
    drawPopRow(c);
    drawBuildings(c);
    BandView views[MAX_BANDS];
    m_regionCount = layoutBands(m_regions, views, m_page);
    paintBands(c, views, m_regionCount);
    return true;
}

// Long-press on a band -> assign/unassign a villager. x resolves the half: left
// of the divider decrements, right increments. Any real change high-beeps,
// persists, and repaints; a no-op (no idle population to add, or already 0) low
// beeps. "更多" flips to the next batch of jobs. save() lives here (the engine
// action does not persist itself — single write, no double-save).
void OutsidePage::onLocalAction(uint8_t param, int x) {
    if (param == A_MORE) {
        m_page++;
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::currentRingIndex(), false);
        return;
    }
    uint8_t job = param;
    if (job == J_GATHERER || job >= JOB_COUNT) { M5.Speaker.tone(600, 120); return; }

    int before = (int)g_game.workers[job];
    int delta = (x < BAND_MID) ? -1 : +1;
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
// change to a painted number/label (population, workers, buildings); otherwise a
// wall-minute rollover refreshes the header clock (QUALITY, clears ghosting).
// Worker bands carry no cooldown, so — unlike Room — every change is a full
// redraw; there is no partial button-area path. Mirrors the Room cadence.
void OutsidePage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick = 0;
    static uint32_t s_lastSig  = 0;
    static int      s_lastMin  = -1;

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    uint32_t now = epochNow();
    g_game.settle(now);

    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix(g_game.population); mix((uint32_t)g_game.maxPopulation());
    mix(g_game.outsideUnlocked ? 1u : 0u);
    for (int i = 0; i < JOB_COUNT; i++) mix(g_game.workers[i]);
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);

    m5::rtc_time_t tm; M5.Rtc.getTime(&tm);
    bool minuteRolled = (tm.minutes != s_lastMin);
    s_lastMin = tm.minutes;

    if (sig != s_lastSig) {
        s_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);   // recomputes bands
        return;
    }

    if (minuteRolled) {
        page_header::draw(canvas);
        pager::partialRefresh(pages::Rect{ 0, 0, 540, HDR_DIV_Y + 4 },
                              pages::RefreshMode::QUALITY);
    }
}
