// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Assign (worker-assignment) page — see assign_page.h for the role/visibility
// model. Every piece of text routes through tr() (strings_zh.h) so only the
// official Simplified-Chinese translation reaches the sparse 12px CJK face — the
// §8.3 glyph-closure iron law. The two literals used here are closure-safe:
// 「分工」(title) reuses 分 (分享) + 工 (工人/工具), and「返回」is the exact
// tr("go home") value, not a bare literal. Layout obeys §9 (36px verb labels /
// 24px body, >=80px long-press bands).
#include "assign_page.h"
#include "cjk_text.h"
#include "pomo_page.h"          // PAD (shared layout authority)
#include "page_tabs.h"          // shared tab header (生火间 │ 村落 │ 贸易站)
#include "pager.h"
#include "game_state.h"
#include <M5Unified.h>
#include <stdio.h>
#include <time.h>

// main.cpp owns the game model.
extern adr::GameState g_game;

using namespace adr;

namespace assign_page {
// Visibility latch — the page is drawable (and thus a reachable ring slot) only
// between open() and close(). Cleared on boot; the Outside 分工 cell sets it.
static bool s_active = false;
void open()  { s_active = true;  }
void close() { s_active = false; }
bool isOpen() { return s_active; }
}  // namespace assign_page

namespace {
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px CJK
constexpr int GLYPH     = 12 * SCALE;        // 24px line box
constexpr int BTN_SCALE = 3;                 // 12px grid x3 = 36px (title / name)
constexpr int BTN_GLYPH = 12 * BTN_SCALE;    // 36px line box
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)

// ---- vertical budget (§9.4), clearing the 32px status bar (< 928). v0.4.1
// swapped the self-drawn 36px title for the SHARED tab header (page_tabs, 村落
// tab lit — this is the village's sub-page), so the top matches room/outside/
// trade. Top -> bottom:
//   tab header(0..72) · info row 人口 X/Y + 伐木者 xN (80, 24px) · one 80px
//   full-width band per unlocked job (120 + n*90) · 返回 band. 6 jobs max (P1):
//   the 6th band ends at 120 + 5*90 + 80 = 650, the 返回 band at 120 + 6*90 =
//   660 ends 740 < 928. One page — a P2 job add is what would force pagination
//   (a "更多" band, the Outside/Trade 手法).
constexpr int INFO_Y    = 80;                // info row (below the tab header)
constexpr int BAND_TOP  = 120;               // first job band top (below info)
constexpr int BAND_H    = 80;                // long-press band (§9.3: >=80px floor)
constexpr int BAND_GAP  = 10;
constexpr int BAND_X    = PAD;               // full-width single column
constexpr int BAND_W    = CONTENT_W;         // 492

// Worker-band stepper geometry (local x within the full-width band). The band is
// wide (492px), so the stepper zone stays the roomy v0.3.3 size (66px zone, 22px
// triangles) — no need for the Outside page's narrowed variant. A vertical rule
// sets off the stepper zone; a horizontal rule splits it into an upper ▲
// (increment) over a lower ▼ (decrement). Triangles are drawn geometrically
// (fillTriangle) — ▲/▼ are not in the sparse face, so never render them as text.
constexpr int STEP_INSET = 12;                       // stepper zone right inset
constexpr int STEP_W     = 66;                       // stepper zone width
constexpr int STEP_X0    = BAND_W - STEP_INSET - STEP_W;  // 414 — stepper zone left
constexpr int STEP_DIV_X = STEP_X0 - 4;              // 410 — vertical rule x
constexpr int LABEL_X    = 8;                        // label left pad in the band
constexpr int LABEL_GAP  = 8;                         // gap: count -> stepper divider
constexpr int TRI_HALF_W = 11;                       // triangle half base (22px wide)
constexpr int TRI_HALF_H = 9;                        // triangle half height

// RTC -> Unix epoch, mirroring outside_page/main.cpp's epochNow.
uint32_t epochNow() {
    m5::rtc_date_t d; m5::rtc_time_t t;
    M5.Rtc.getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}


// ---- drawing pieces --------------------------------------------------------

// Info row (below the shared tab header): the Outside page's population line
// (official "人口 " + X/Y + the derived idle 伐木者 count) so the player sees how
// many villagers are free to assign.
void drawInfoRow(m5gfx::M5Canvas& c) {
    char pop[40];
    snprintf(pop, sizeof(pop), "%s%u/%u", tr("pop "),
             (unsigned)g_game.population, (unsigned)g_game.maxPopulation());
    int x = cjk::drawText(c, PAD, INFO_Y, pop, SCALE);

    char gath[40];
    snprintf(gath, sizeof(gath), "%s x%d", tr("gatherer"), g_game.numGatherers());
    cjk::drawText(c, x + GLYPH, INFO_Y, gath, SCALE);
}

// 1px dashed rect, 4px-on/4px-off on all four edges — the disabled-button frame
// (matches the Outside/Room drawDashedRect exactly).
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

// A job band is disabled when NEITHER stepper half can act: no worker in the job
// to pull back to idle (minus) AND no idle villager to add (plus). Same predicate
// the Outside page used before assignment moved here.
bool jobBandDisabled(uint8_t job) {
    return g_game.workers[job] == 0 && g_game.numGatherers() == 0;
}

// One full-width job band at `top`: enabled = 2px frame (two concentric rects);
// disabled = a single 1px dashed outer frame. Left: the 36px job `name` at
// LABEL_X, and the 24px "xN" count right-aligned just left of the stepper divider
// (LABEL_GAP px clear), sharing the name's bottom baseline. Right: the ▲/▼
// stepper — a vertical rule, a horizontal split rule, an upper ▲ (increment) over
// a lower ▼ (decrement), each a fillTriangle. The ▲/▼ are hit by the press
// y-half, not the exact triangle pixels (see onLocalAction).
void drawJobBand(m5gfx::M5Canvas& c, int top, const char* name, const char* sub,
                 bool disabled) {
    if (disabled) {
        drawDashedRect(c, BAND_X, top, BAND_W, BAND_H);
    } else {
        c.drawRect(BAND_X, top, BAND_W, BAND_H, TFT_BLACK);
        c.drawRect(BAND_X + 1, top + 1, BAND_W - 2, BAND_H - 2, TFT_BLACK);
    }

    int ny = top + (BAND_H - BTN_GLYPH) / 2 - 4;        // 36px name box top
    cjk::drawText(c, BAND_X + LABEL_X, ny, name, BTN_SCALE);
    int sw = cjk::textWidth(sub, SCALE);
    cjk::drawText(c, BAND_X + STEP_DIV_X - LABEL_GAP - sw, ny + BTN_GLYPH - GLYPH,
                  sub, SCALE);

    // Stepper zone: vertical rule + horizontal split rule.
    int midY = top + BAND_H / 2;
    c.drawFastVLine(BAND_X + STEP_DIV_X, top + 10, BAND_H - 20, TFT_BLACK);
    c.drawFastHLine(BAND_X + STEP_X0, midY, STEP_W, TFT_BLACK);

    int cx   = BAND_X + STEP_X0 + STEP_W / 2;  // stepper column center
    int cyUp = top + BAND_H / 4;               // ▲ center (upper half)
    int cyDn = top + 3 * BAND_H / 4;           // ▼ center (lower half)
    c.fillTriangle(cx, cyUp - TRI_HALF_H,      // ▲ increment (apex up)
                   cx - TRI_HALF_W, cyUp + TRI_HALF_H,
                   cx + TRI_HALF_W, cyUp + TRI_HALF_H, TFT_BLACK);
    c.fillTriangle(cx, cyDn + TRI_HALF_H,      // ▼ decrement (apex down)
                   cx - TRI_HALF_W, cyDn - TRI_HALF_H,
                   cx + TRI_HALF_W, cyDn - TRI_HALF_H, TFT_BLACK);
}

// The trailing 返回 band: full-width frame, lone 36px label centered. tr("go
// home") == "返回" (real translation, not a bare literal).
void drawReturnBand(m5gfx::M5Canvas& c, int top) {
    c.drawRect(BAND_X, top, BAND_W, BAND_H, TFT_BLACK);
    c.drawRect(BAND_X + 1, top + 1, BAND_W - 2, BAND_H - 2, TFT_BLACK);
    const char* label = tr("go home");                  // "返回"
    int lw = cjk::textWidth(label, BTN_SCALE);
    cjk::drawText(c, BAND_X + (BAND_W - lw) / 2, top + (BAND_H - BTN_GLYPH) / 2 - 4,
                  label, BTN_SCALE);
}
}  // namespace

// ================================ Page API =================================

const pages::Region* AssignPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Drawable only while open (and the forest is unlocked): returning false makes
// showPageOrNext skip this ring slot, so the page is invisible + untappable
// unless the Outside 分工 cell opened it — the same skip mechanism the un-unlocked
// Outside/Trade pages use. available() holds that predicate once, shared by
// draw() and the status bar's page-dot count so the two can't disagree.
// hasUnlockedJob() is folded in for consistency with the Outside 分工 entry gate:
// that gate already blocks opening this page when no job exists, so this is
// defensive (a job building can't be lost in P1) — but it keeps ONE predicate
// deciding "is there anything to assign", so an empty assign page can never show.
bool AssignPage::available() const {
    return assign_page::isOpen() && g_game.outsideUnlocked && g_game.hasUnlockedJob();
}

bool AssignPage::draw(m5gfx::M5Canvas& c) {
    if (!available()) return false;
    c.fillSprite(TFT_WHITE);
    page_tabs::draw(c, 1);           // shared tab header, 村落 lit (this is its sub-page)
    drawInfoRow(c);

    m_jobCount = g_game.unlockedJobs(m_jobs, MAX_JOBS);
    for (int i = 0; i < m_jobCount; i++) {
        int top = BAND_TOP + i * (BAND_H + BAND_GAP);
        char sub[12];
        snprintf(sub, sizeof(sub), "x%u", (unsigned)g_game.workers[m_jobs[i]]);
        drawJobBand(c, top, tr(JOB_KEY[m_jobs[i]]), sub, jobBandDisabled(m_jobs[i]));
        m_regions[i].y0 = (uint16_t)top;
        m_regions[i].y1 = (uint16_t)(top + BAND_H);
        m_regions[i].type  = 1;                 // firmware-local
        m_regions[i].param = (uint8_t)i;        // band index
    }

    // 返回 band right after the last job band (index == m_jobCount).
    int retTop = BAND_TOP + m_jobCount * (BAND_H + BAND_GAP);
    drawReturnBand(c, retTop);
    m_regions[m_jobCount].y0 = (uint16_t)retTop;
    m_regions[m_jobCount].y1 = (uint16_t)(retTop + BAND_H);
    m_regions[m_jobCount].type  = 1;
    m_regions[m_jobCount].param = (uint8_t)m_jobCount;
    m_regionCount = m_jobCount + 1;
    return true;
}

// Long-press on a band. param is the band index: the last band (== m_jobCount) is
// 返回 (close + jump back to the village); a job band assigns/unassigns one
// villager, the press y-half picking the direction (upper ▲ = +1, lower ▼ = −1).
// A real change high-beeps + persists + repaints; a no-op (no idle villager to
// add, or already 0) low-beeps.
void AssignPage::onLocalAction(uint8_t param, int x, int y) {
    (void)x;
    if ((int)param > m_jobCount) { M5.Speaker.tone(600, 120); return; }

    if ((int)param == m_jobCount) {                  // 返回 band
        assign_page::close();
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::ringIndexByName("outside"), false);
        return;
    }

    uint8_t job = m_jobs[param];
    int before  = (int)g_game.workers[job];
    int bandTop = BAND_TOP + (int)param * (BAND_H + BAND_GAP);
    int delta   = (y < bandTop + BAND_H / 2) ? +1 : -1;   // upper ▲ / lower ▼
    g_game.assignWorker(job, delta);

    if ((int)g_game.workers[job] != before) {        // a villager actually moved
        M5.Speaker.tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
    } else {
        M5.Speaker.tone(600, 120);                   // no idle pop / already at 0
    }
}

// Time axis (awake only, and only while this page is current — which requires it
// to be open). Settle the economy each second, then repaint on any change to a
// painted number (population, workers, idle count, buildings — a new building
// unlocks a job band). A simplified Outside::tick: no cooldowns live here, so
// every change is a full redraw with no partial path.
void AssignPage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick = 0;
    static uint32_t s_lastSig  = 0;

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    g_game.settle(epochNow());

    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix(g_game.population); mix((uint32_t)g_game.maxPopulation());
    for (int i = 0; i < JOB_COUNT; i++) mix(g_game.workers[i]);
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);

    if (sig != s_lastSig) {
        s_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);
    }
}
