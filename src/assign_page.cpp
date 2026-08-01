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
#include "action_band.h"        // shared band frame + title baseline + 返回 band
#include "stepper.h"            // shared ±1 / ±10 stepper zone (PathPage parity)
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

// Worker-band stepper: the shared two-column ±1 / ±10 zone (see stepper.h — the
// geometry, the four hit zones and the ▲/▼ glyphs all live there,
// identical to PathPage's outfit rows). v0.14 grew it from one column to two,
// which moved the zone's left rule from 410 to 344 — the "xN" count re-anchors
// to stepper::dividerX() below, and the widest job name (军械工人/铁矿工人 at
// 144px @36px) still leaves 136px of clear space before it.
constexpr int LABEL_X    = 8;                        // label left pad in the band
constexpr int LABEL_GAP  = 8;                         // gap: count -> stepper divider
constexpr int STEP_DIV_X = stepper::dividerX(BAND_W); // 344 — zone's left rule

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

// The rect of the band at `top` — one description of where a band is, shared by
// every draw call here and by pressRect, so the drawn frame and the
// invert-flash rect cannot drift apart.
pages::Rect bandRect(int top) {
    return pages::Rect{ BAND_X, top, BAND_W, BAND_H };
}

// A job band is disabled when NEITHER stepper half can act: no worker in the job
// to pull back to idle (minus) AND no idle villager to add (plus). Same predicate
// the Outside page used before assignment moved here.
bool jobBandDisabled(uint8_t job) {
    return g_game.workers[job] == 0 && g_game.numGatherers() == 0;
}

// One full-width job band at `top`. The FRAME (solid double ring / 1px dashed)
// comes from the shared action_band, the 36px name's baseline from
// action_band::titleBoxY, and the whole right-hand ±1 / ±10 zone from stepper —
// what is left here is only this page's own arrangement of the LEFT side: a
// stepper row lays its name and count SIDE BY SIDE on one baseline rather than
// stacking a subtitle under a title, so it is a different layout, not a drifted
// copy of the standard band. The 36px job `name` sits at LABEL_X, and the 24px
// "xN" count is right-aligned just left of the stepper's divider rule (LABEL_GAP
// px clear), sharing the name's bottom baseline.
void drawJobBand(m5gfx::M5Canvas& c, int top, const char* name, const char* sub,
                 bool disabled) {
    pages::Rect band = bandRect(top);
    action_band::drawFrame(c, band, !disabled);

    int ny = action_band::titleBoxY(top, BAND_H);       // 36px name box top
    cjk::drawText(c, BAND_X + LABEL_X, ny, name, BTN_SCALE);
    int sw = cjk::textWidth(sub, SCALE);
    cjk::drawText(c, BAND_X + STEP_DIV_X - LABEL_GAP - sw, ny + BTN_GLYPH - GLYPH,
                  sub, SCALE);

    stepper::draw(c, band);
}

// The trailing 返回 band: the plain shared band, lone 36px label centered.
// tr("go home") == "返回" (real translation, not a bare literal). It carries no
// subtitle, so action_band centres the title — landing it on exactly the y
// action_band::titleBoxY gives the job bands' names above it.
void drawReturnBand(m5gfx::M5Canvas& c, int top) {
    action_band::draw(c, bandRect(top), tr("go home"), nullptr, true, 0, 0);
}

// Content signature — a hash of every live value that alters a painted number
// (population, worker mix, buildings — a new building unlocks a job band). tick()
// compares it each second to decide a full redraw; onLocalAction re-baselines it
// right after its own showPage so the same assignment no longer forces a SECOND
// full redraw on the next tick (see onLocalAction). Reads only g_game, never mutates.
uint32_t contentSig() {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix(g_game.population); mix((uint32_t)g_game.maxPopulation());
    for (int i = 0; i < JOB_COUNT; i++) mix(g_game.workers[i]);
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);
    return sig;
}
}  // namespace

// ================================ Page API =================================

const pages::Region* AssignPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Press feedback, mirroring onLocalAction's own decision exactly (both ignore
// x — see below) rather than the Page default's full y-band/full-panel-width
// flash, which read as "the whole row turned black" (the bug this fixes).
// 返回 band: onLocalAction fires on ANY (x,y) inside the band's y-range — it
// never reads x, and doesn't need to since the whole band is one action — so
// there is no x/y split to mirror. The flash rect is the band's own drawn
// frame (BAND_X/BAND_W/BAND_H — the exact rect drawReturnBand's two concentric
// drawRect calls paint), not a label-hugging sub-rect: the whole button box
// inverts, matching what the player actually sees as "the button".
// Job bands: v0.14 gave them FOUR zones (±1 / ±10 x up / down), so unlike the
// 返回 band they now read BOTH x and y — stepper::zoneRect decodes the press
// exactly as stepper::deltaFor does in onLocalAction, so the rect that inverts
// is always the button that fires. A press left of the stepper (the name area)
// still counts as ±1, as it always has, and flashes the ±1 column accordingly.
// Every press dispatches — assignWorker no-ops silently (still low-beeping) when
// the band is disabled or already at a limit, it is never skipped — so there is
// no "don't flash" case to mirror here.
pages::Rect AssignPage::pressRect(const pages::Region& rg, int x, int y) const {
    if ((int)rg.param == m_jobCount) {                     // 返回 band
        return bandRect(rg.y0);                            // drawReturnBand's own frame
    }
    return stepper::zoneRect(bandRect(rg.y0), x, y);
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
// 返回 (close + jump back to the village); a job band assigns/unassigns villagers,
// the press picking BOTH the size and the direction — stepper::deltaFor turns
// (x,y) into ±1 (fine column, or the name area) or ±10 (coarse column).
// GameState::assignWorker already TRUNCATES to what is available in either
// direction (min(delta, idle gatherers) adding, min(delta, workers) removing —
// game_state.cpp:554-563), which is exactly upstream's
// Math.min(available, btn.data) (outside.js:376), so ±10 with 3 idle villagers
// moves 3 rather than refusing. No engine change was needed for that.
// A real change high-beeps + persists + repaints; a genuine no-op (nothing idle
// to add, or already 0 — both magnitudes are equally dead in that direction)
// low-beeps, the same feedback the ±1-only stepper gave.
void AssignPage::onLocalAction(uint8_t param, int x, int y) {
    if ((int)param > m_jobCount) { M5.Speaker.tone(600, 120); return; }

    if ((int)param == m_jobCount) {                  // 返回 band
        assign_page::close();
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::ringIndexByName("outside"), false);
        return;                                      // navigates away — no tick to double up
    }

    uint8_t job = m_jobs[param];
    int before  = (int)g_game.workers[job];
    int bandTop = BAND_TOP + (int)param * (BAND_H + BAND_GAP);
    int delta   = stepper::deltaFor(bandRect(bandTop), x, y);
    g_game.assignWorker(job, delta);

    if ((int)g_game.workers[job] != before) {        // a villager actually moved
        M5.Speaker.tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
        // Re-baseline tick()'s content signature to the state we JUST drew, so this
        // same assignment no longer trips a SECOND full-page redraw next tick — only
        // genuine economy advancing in the following second still does. No extra
        // settle: draw() paints un-settled g_game and contentSig() must mirror it.
        m_lastSig = contentSig();
    } else {
        M5.Speaker.tone(600, 120);                   // no idle pop / already at 0
    }
}

// Time axis (awake only, and only while this page is current — which requires it
// to be open). Settle the economy each second, then repaint on any change to a
// painted number (population, workers, idle count, buildings — a new building
// unlocks a job band). A simplified Outside::tick: no cooldowns live here, so
// every change is a full redraw with no partial path. onLocalAction re-baselines
// m_lastSig after its own showPage, so an assignment no longer forces a second
// full redraw here.
void AssignPage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick = 0;

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    g_game.settle(epochNow());

    uint32_t sig = contentSig();
    if (sig != m_lastSig) {
        m_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);
    }
}
