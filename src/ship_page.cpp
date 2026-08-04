// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Starship (破旧星舰) page — see ship_page.h for the unlock chain and the 3a/3b
// split. A thin renderer over the ship half of game_state.h: this file never
// mutates g_game except through reinforceHull / upgradeEngine / startLiftoff /
// liftOff, so every rule (the alloy price, the hull gate, the 120s cooldown) has
// exactly one home and the host smoke test can reach all of it.
//
// Layout (540x960, §9: 36px button labels / 24px body, >=80px long-press bands).
// A tab-less location page, so it draws its own title like the World page:
//   title(16, 36px) · rule(64) · 外壳/引擎 stat rows (36px) · the 外星合金 balance
//   (24px — the one currency both buttons spend) · three full-width 96px bands ·
//   a hint line that only appears while the hull is still 0.
// The bands stop well above the 928 status bar; a short page that leaves its
// lower half empty is the same shape the Trade page takes when few goods are
// offerable, so nothing new is being invented here.
#include "ship_page.h"
#include "action_band.h"        // the app-wide button band — the ONLY button frame
#include "cjk_text.h"
#include "pomo_page.h"          // PAD (shared layout authority)
#include "page_tabs.h"          // resetHitCache — a tab-less page clears the cache
#include "pager.h"
#include "game_state.h"
#include "event_engine.h"       // the one-shot 「准备好要离开了吗?」 confirmation
#include "events_data.h"        // EV_SHIP_LIFTOFF
#include "beeper.h"
#include "rtc_bm8563.h"
#include <M5Unified.h>
#include <stdio.h>
#include <time.h>

// main.cpp owns the game model.
extern adr::GameState g_game;

using namespace adr;

namespace {
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px body
constexpr int GLYPH     = 12 * SCALE;        // 24px line box
constexpr int TITLE_SCALE = 3;               // 12px grid x3 = 36px page title
constexpr int STAT_SCALE = 3;                // the stat rows ride the same 36px
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)

// ---- vertical budget (§9.4) ------------------------------------------------
constexpr int TITLE_Y   = 16;                // 36px title ink 16..52
constexpr int RULE_Y    = 64;                // 2px rule under the title
constexpr int STAT_Y0   = 84;                // 外壳: N
constexpr int STAT_Y1   = 132;               // 引擎: N
constexpr int ALLOY_Y   = 188;               // 外星合金 N (24px)
constexpr int BAND_TOP  = 236;               // first band top
// 96, not the 80px floor: this is the height action_band's own budget is written
// against for a band that carries BOTH a cost column and a cooldown bar (see the
// BAR_GUTTER/BAR_H note in action_band.h). Using one height for all three keeps
// the priced pair and the coolable liftoff band on the same rhythm.
constexpr int BAND_H    = 96;
constexpr int BAND_GAP  = 12;
constexpr int HINT_Y    = BAND_TOP + 3 * BAND_H + 2 * BAND_GAP + 20;   // 568

// Band identity == region param == the order they are drawn in.
enum : uint8_t { A_HULL = 0, A_ENGINE, A_LIFTOFF };

// Why the hull can't fly yet — shown ONLY while shipHull == 0, which is the one
// state where a solid-looking page has a dashed button and no visible reason.
// ship.js has no such line (a web page can hover a tooltip); a card cannot, and
// "the button that says 起飞 is greyed out" was exactly the kind of dead end the
// tech page was added to kill.
//   §8.3 closure-safe firmware literal: 船身/还/没有/加固/无法/起飞 all appear in
// official translations already in strings_zh.h, so this costs no new glyph.
const char* const HINT_NEED_HULL = "船身还没有加固，无法起飞";

// RTC -> Unix epoch, mirroring room_page/trade_page/world_page. The liftoff
// cooldown is measured on this same clock as every other cooldown and as
// settle(), so a deep sleep simply expires it.
uint32_t epochNow() {
    rtc::Date d; rtc::Time t;
    rtc::getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}

// The rect of the band at index `i` — the ONE description of where a button is,
// shared by the draw call and pressRect so the drawn frame and the invert-flash
// rect cannot drift apart.
pages::Rect bandRect(int i) {
    return pages::Rect{ PAD, BAND_TOP + i * (BAND_H + BAND_GAP), CONTENT_W, BAND_H };
}

// "-1 外星合金" — the same "-amount name" convention (and the same two-space join
// for multi-entry costs) trade_page/tech_page/event_modal already emit, so a ship
// price reads exactly like a trading-post price.
void alloyCost(int amount, char* out, size_t cap) {
    snprintf(out, cap, "-%d %s", amount, tr(RES_KEY[R_ALIEN_ALLOY]));
}

// One 36px key/value stat row: the label at the content's left edge, the number
// right-aligned to its right edge — the read-only twin of an action_band's
// left-title/right-cost split (变体 B), so the stats above the buttons and the
// prices inside them share one column grid.
void drawStat(m5gfx::M5Canvas& c, int y, const char* label, int value) {
    cjk::drawText(c, PAD, y, label, STAT_SCALE);
    char v[12];
    snprintf(v, sizeof(v), "%d", value);
    cjk::drawText(c, (540 - PAD) - cjk::textWidth(v, STAT_SCALE), y, v, STAT_SCALE);
}

// Content signature — every live value that changes a painted number, label or
// frame state. liftoffCooldownLeft() is IN it on purpose: while the button cools
// this ticks down once a second and drives the bar's drain, and while it is ready
// it is a constant 0 that costs nothing. That makes room_page's separate
// coolMask bookkeeping unnecessary here (this page has exactly one coolable
// band, so "the cooldown changed" and "the page changed" are the same event).
uint32_t contentSig(uint32_t now) {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix((uint32_t)g_game.shipHull);
    mix((uint32_t)g_game.shipThrusters);
    mix((uint32_t)g_game.whole(R_ALIEN_ALLOY));
    mix((uint32_t)g_game.liftoffCooldownLeft(now));
    return sig;
}
}  // namespace

// ================================ Page API =================================

const pages::Region* ShipPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Press feedback: the band's own drawn frame, not the Page default's full 540px
// y-band — the bands are PAD-inset, so the default would also flash the clear
// margin outside the frame. x/y are unused: one band is one action.
pages::Rect ShipPage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)x; (void)y;
    // param IS the band index (draw() writes it), so no y arithmetic is needed —
    // and a stale table cannot land the flash on a rect that isn't a band.
    if (rg.param >= BAND_COUNT) return pages::Rect{ 0, 0, 0, 0 };   // don't flash
    return bandRect(rg.param);
}

// Hidden until the wreck has been salvaged AND walked home from (see the header):
// returning false makes showPageOrNext skip this ring slot and drops its dot from
// the status bar, the same mechanism Outside/Trade use for their unlocks. This is
// the single source of that predicate — draw() consults it, so the page and the
// dot count can never disagree.
bool ShipPage::available() const { return g_game.shipUnlocked; }

bool ShipPage::draw(m5gfx::M5Canvas& c) {
    if (!available()) return false;
    uint32_t now = epochNow();
    c.fillSprite(TFT_WHITE);
    page_tabs::resetHitCache();      // tab-less page: drop any stale header hitbox

    cjk::drawText(c, PAD, TITLE_Y, tr("An Old Starship"), TITLE_SCALE);
    c.fillRect(PAD, RULE_Y, CONTENT_W, 2, TFT_BLACK);

    // 外壳 / 引擎. Both keys are upstream's own (ship.js:37,42) — note that the
    // official table renders "hull:" as 外壳: here while the button below it says
    // 加固船身, because upstream keys those two strings separately and translated
    // them differently (research-phase3.md §6.1). Faithful port; not a typo.
    drawStat(c, STAT_Y0, tr("hull:"),   g_game.shipHull);
    drawStat(c, STAT_Y1, tr("engine:"), g_game.shipThrusters);

    // The currency both buttons spend, so "why is 加固船身 dashed" is answerable
    // without leaving the page (trade_page's balance row, one resource wide).
    {
        char amt[8]; fmtAmount(g_game.whole(R_ALIEN_ALLOY), amt, sizeof(amt));
        char line[40];
        snprintf(line, sizeof(line), "%s %s", tr(RES_KEY[R_ALIEN_ALLOY]), amt);
        cjk::drawText(c, PAD, ALLOY_Y, line, SCALE);
    }

    // Each band prices itself off its OWN constant — they are both 1 alloy today
    // (ship.js), and one shared affordability test would quietly hide it if they
    // ever weren't.
    int32_t alloy = g_game.stores[R_ALIEN_ALLOY];
    char cost[40];
    alloyCost(ALLOY_PER_HULL, cost, sizeof(cost));
    action_band::draw(c, bandRect(A_HULL), tr("reinforce hull"), cost,
                      alloy >= (int32_t)ALLOY_PER_HULL * FP, 0, 0);
    alloyCost(ALLOY_PER_THRUSTER, cost, sizeof(cost));
    action_band::draw(c, bandRect(A_ENGINE), tr("upgrade engine"), cost,
                      alloy >= (int32_t)ALLOY_PER_THRUSTER * FP, 0, 0);

    // 点火起飞 is free (ship.js takes no cost and does not even look at the Path
    // bag) but carries the 120s cooldown, so it is the one band with a bar and no
    // cost column. Dashed while the hull is 0 OR while it is cooling — the same
    // "enabled == pressable right now" rule the Room grid uses for its own
    // cooling buttons.
    int coolLeft = g_game.liftoffCooldownLeft(now);
    action_band::draw(c, bandRect(A_LIFTOFF), tr("lift off"), nullptr,
                      g_game.shipHull > 0 && coolLeft == 0,
                      coolLeft, LIFTOFF_COOLDOWN_S);

    if (g_game.shipHull <= 0)
        cjk::drawText(c, PAD, HINT_Y, HINT_NEED_HULL, SCALE);

    for (int i = 0; i < BAND_COUNT; i++) {
        pages::Rect r = bandRect(i);
        m_regions[i].y0 = (uint16_t)r.y;
        m_regions[i].y1 = (uint16_t)(r.y + r.h);
        m_regions[i].type  = 1;                  // firmware-local
        m_regions[i].param = (uint8_t)i;         // full-width row -> band index
    }
    m_regionCount = BAND_COUNT;
    return true;
}

// Long-press on a band. Full-width single column, so param IS the band index and
// x/y are unused. Success high-beeps, persists and repaints (a stat moved, the
// alloy balance moved, a band's dashed state may have flipped); a refusal
// low-beeps — the engine already pushed 「外星合金不足」 to the log when that was
// the reason, and the band was already drawn dashed, so nothing on screen owes
// the player a repaint.
void ShipPage::onLocalAction(uint8_t param, int x, int y) {
    (void)x; (void)y;
    uint32_t now = epochNow();

    if (param == A_HULL || param == A_ENGINE) {
        Result r = param == A_HULL ? g_game.reinforceHull() : g_game.upgradeEngine();
        if (r != RC_OK) { beeper::tone(600, 120); return; }
        beeper::tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
        // Re-baseline tick()'s signature to the state we JUST drew so this same
        // press doesn't trip a SECOND full redraw a second later (room/trade do
        // the same right after their own showPage).
        m_lastSig = contentSig(now);
        return;
    }
    if (param != A_LIFTOFF) { beeper::tone(600, 120); return; }   // stale region table

    // ship.js checkLiftOff(): the press starts the cooldown FIRST (that is what
    // 「裹足徘徊」 later has to refund), then branches on whether the player has
    // ever been warned.
    if (g_game.startLiftoff(now) != RC_OK) { beeper::tone(600, 120); return; }

    if (!g_game.shipSeenWarning) {
        // Raise the one-shot confirmation. main.cpp's loop pops event_modal on its
        // next pass (events::active() && !event_modal::active()), and THAT repaint
        // is the frame the player sees — so no showPage here, which would only
        // flash the page under a modal about to cover it.
        if (events::startScripted(EV_SHIP_LIFTOFF, now)) {
            beeper::tone(1800, 80);
            g_game.save();     // the cooldown stamp is spent either way the player answers
            return;
        }
        // A random event won the screen between the loop's events::tick and this
        // press (pager only gates touch on event_MODAL::active(), which is still
        // false for that one pass). The press did nothing, so refund the cooldown
        // rather than charging 120 seconds for a swallowed tap.
        g_game.clearLiftoffCooldown();
        beeper::tone(600, 120);
        return;
    }

    // Raises spacePending; main.cpp's loop starts the Space level on its next
    // pass (space_page.h explains why it cannot start from inside this handler).
    // The showPage below is still worth doing — it acks the press with the
    // cooldown bar already draining, which is the frame the player sees for the
    // ~5 ms before the level takes the panel.
    g_game.liftOff();
    beeper::tone(1800, 80);
    g_game.save();
    pager::showPage(pager::currentRingIndex(), false);
    m_lastSig = contentSig(now);
}

// Time axis (awake only). Settle the offline economy each second — this is a
// village page and the workers keep working while it is up — then repaint on any
// change to a painted value. contentSig() carries the cooldown remainder, so the
// draining liftoff bar and an alloy delivery both come through the one comparison.
void ShipPage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick = 0;
    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    uint32_t now = epochNow();
    g_game.settle(now);

    uint32_t sig = contentSig(now);
    if (sig != m_lastSig) {
        m_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);
    }
}
