// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Assign (worker-assignment) page — see assign_page.h for the role/visibility
// model. Every piece of text routes through tr() (strings_zh.h) so only the
// official Simplified-Chinese translation reaches the sparse 12px CJK face — the
// §8.3 glyph-closure iron law. The one literal used here, "更多", is the same
// closure-safe (already-baked) literal trade_page.cpp/room_page.cpp use for
// their own pager band, not a bare ad-hoc string. Layout obeys §9 (36px verb
// labels / 24px body, >=80px long-press bands).
#include "assign_page.h"
#include "action_band.h"        // shared band frame + title baseline
#include "stepper.h"            // shared ±1 / ±10 stepper zone (PathPage parity)
#include "cjk_text.h"
#include "page_layout.h"        // PAD (shared layout authority)
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
//   full-width band per visible job/更多 slot (120 + n*90). NO trailing 返回
//   band (v0.10.3 — see assign_page.h): the job-band list runs all the way to
//   MAX_BANDS.
// P1 shipped with <=6 assignable jobs, comfortably one page even WITH a 返回
// band (the 6th job band ended at 120 + 5*90 + 80 = 650, 返回 at 660 ended
// 740 < 928). P2 raised the real ceiling to 9 assignable jobs (miners/
// steelworker/armourer); v0.10.2 handled that by paginating behind a 返回 band
// (MAX_BANDS=8 + 返回 = 9 total, 920 < 928); v0.10.3 dropped the 返回 band
// instead (user: "这页没有别的按钮,为什么不能放完整的工人调整"), which buys
// back exactly the one slot needed to fit ALL 9 real jobs with zero pagination:
// MAX_BANDS=9 job bands alone end at 120 + 8*90 + 80 = 920 < 928; a 10th slot
// (MAX_BANDS=10) would end at 120 + 9*90 + 80 = 1010, well past the status bar.
// layoutBands() below still mirrors trade_page.cpp's split exactly — perPage =
// MAX_BANDS-1 (8) real jobs + a trailing "更多" band — but with today's real
// max of 9 (JOB_COUNT-1) never exceeding MAX_BANDS=9, that branch is a sleep
// guard against a future job add, not something that fires today.
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

// "更多" pagination sentinel; real slot codes are Job ids (0..JOB_COUNT-1) —
// the Outside/Trade/Room pager 手法 (trade_page.cpp's A_MORE), ported here now
// that P2 pushed the unlocked-job list past one page (see assign_page.h).
constexpr uint8_t A_MORE = 0xFF;

// The "更多 (n/N)" pager band: same plain shared band as 返回 (centred lone
// label, no subtitle, no stepper) — a whole-band press, not a stepper zone
// (see pressRect/onLocalAction).
void drawMoreBand(m5gfx::M5Canvas& c, int top, const char* label) {
    action_band::draw(c, bandRect(top), label, nullptr, true, 0, 0);
}

// Compute the visible job/更多 slot list for `page`: fills slotCodes[] (a Job
// id, or A_MORE) and regionsOut[] (one full-width y-band per slot, param =
// slot index), and — when a "更多" slot is placed — the "更多 (n/N)" label
// into moreLabel. This is trade_page.cpp's layoutBands split verbatim: query
// the FULL unlocked list into a JOB_COUNT-sized LOCAL scratch first (so the
// query itself can never truncate — game_state.cpp's unlockedJobs() never
// returns more than JOB_COUNT-1 real jobs), THEN slice out one page's worth
// (maxBands-1 real jobs + a trailing "更多") once the full list exceeds
// maxBands. Returns the slot count == the region count (no trailing 返回
// band — see assign_page.h).
int layoutBands(pages::Region* regionsOut, uint8_t* slotCodes, int page,
                int maxBands, int* slotCountOut, char* moreLabel,
                size_t moreLabelCap) {
    uint8_t all[JOB_COUNT];
    int total = g_game.unlockedJobs(all, (int)sizeof(all));

    int numPages, start, take;
    bool more;
    int pg = 0;
    if (total <= maxBands) {
        numPages = 1; start = 0; take = total; more = false;
    } else {
        int perPage = maxBands - 1;                  // 7 real + 1 "更多"
        numPages = (total + perPage - 1) / perPage;
        pg = ((page % numPages) + numPages) % numPages;
        start = pg * perPage;
        take = total - start; if (take > perPage) take = perPage;
        more = true;
    }

    int k = 0;
    for (int i = 0; i < take && k < maxBands; i++) slotCodes[k++] = all[start + i];
    if (more && k < maxBands) {
        slotCodes[k++] = A_MORE;
        if (moreLabel) snprintf(moreLabel, moreLabelCap, "更多 (%d/%d)",
                                (pg + 1 < numPages ? pg + 2 : 1), numPages);
    }
    int slotCount = k;

    for (int s = 0; s < slotCount; s++) {
        int top = BAND_TOP + s * (BAND_H + BAND_GAP);
        regionsOut[s].y0 = (uint16_t)top;
        regionsOut[s].y1 = (uint16_t)(top + BAND_H);
        regionsOut[s].type  = 1;                     // firmware-local
        regionsOut[s].param = (uint8_t)s;            // slot index
    }
    *slotCountOut = slotCount;
    return slotCount;
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
// x for a 更多 band — see below) rather than the Page default's full y-band/
// full-panel-width flash, which read as "the whole row turned black" (the bug
// this fixes).
// 更多 band: onLocalAction fires on ANY (x,y) inside the band's y-range — it
// never reads x, and doesn't need to since the whole band is one action — so
// there is no x/y split to mirror. The flash rect is the band's own drawn
// frame (BAND_X/BAND_W/BAND_H — the exact rect drawMoreBand's two concentric
// drawRect calls paint), not a label-hugging sub-rect: the whole button box
// inverts, matching what the player actually sees as "the button".
// Job bands: v0.14 gave them FOUR zones (±1 / ±10 x up / down), so unlike the
// 更多 band they read BOTH x and y — stepper::zoneRect decodes the press
// exactly as stepper::deltaFor does in onLocalAction, so the rect that inverts
// is always the button that fires. A press left of the stepper (the name area)
// still counts as ±1, as it always has, and flashes the ±1 column accordingly.
// Every press dispatches — assignWorker no-ops silently (still low-beeping) when
// the band is disabled or already at a limit, it is never skipped — so there is
// no "don't flash" case to mirror here.
pages::Rect AssignPage::pressRect(const pages::Region& rg, int x, int y) const {
    if (m_slotCodes[rg.param] == A_MORE) {                 // 更多 band
        return bandRect(rg.y0);                            // drawMoreBand's own frame
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

    char moreLabel[24] = {0};
    layoutBands(m_regions, m_slotCodes, m_page, MAX_BANDS, &m_slotCount,
                moreLabel, sizeof(moreLabel));
    for (int i = 0; i < m_slotCount; i++) {
        int top = BAND_TOP + i * (BAND_H + BAND_GAP);
        if (m_slotCodes[i] == A_MORE) {
            drawMoreBand(c, top, moreLabel);
        } else {
            uint8_t job = m_slotCodes[i];
            char sub[12];
            snprintf(sub, sizeof(sub), "x%u", (unsigned)g_game.workers[job]);
            drawJobBand(c, top, tr(JOB_KEY[job]), sub, jobBandDisabled(job));
        }
    }
    // No trailing 返回 band (v0.10.3 — see assign_page.h): the region table ends
    // right where the job/更多 bands do.
    m_regionCount = m_slotCount;
    return true;
}

// Long-press on a band. param is the band index (0..m_slotCount-1 — there is no
// trailing 返回 band, v0.10.3): a 更多 band advances m_page (the Outside/Trade/
// Room pager 手法); a job band assigns/unassigns villagers, the press picking
// BOTH the size and the direction — stepper::deltaFor turns (x,y) into ±1 (fine
// column, or the name area) or ±10 (coarse column). GameState::assignWorker
// already TRUNCATES to what is available in either direction (min(delta, idle
// gatherers) adding, min(delta, workers) removing — game_state.cpp:554-563),
// which is exactly upstream's Math.min(available, btn.data) (outside.js:376),
// so ±10 with 3 idle villagers moves 3 rather than refusing. No engine change
// was needed for that.
// A real change high-beeps + persists + repaints; a genuine no-op (nothing idle
// to add, or already 0 — both magnitudes are equally dead in that direction)
// low-beeps, the same feedback the ±1-only stepper gave. Leaving the page is NOT
// this method's job any more — the tab header and the pager's page-turn
// fallback are the only exits (both close this page's latch — see pager.cpp's
// closeSubPageLatches).
void AssignPage::onLocalAction(uint8_t param, int x, int y) {
    if ((int)param >= m_slotCount) { M5.Speaker.tone(600, 120); return; }

    if (m_slotCodes[param] == A_MORE) {              // 更多 band
        m_page++;
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::currentRingIndex(), false);
        return;
    }

    uint8_t job = m_slotCodes[param];
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
