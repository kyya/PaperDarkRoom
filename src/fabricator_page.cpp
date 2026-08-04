// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Fabricator (嗡鸣的制造机) page — see fabricator_page.h for the unlock chain and
// the panel's two groups. A thin renderer over the fabricator half of
// game_state.h: this file never mutates g_game except through fabricate(), so
// every rule (the blueprint gate, the alloy price, the per-upgrade cap) has
// exactly one home and the host smoke test can reach all of it.
//
// Layout (540x960, §9: 36px button labels / 24px body, >=80px long-press bands).
// A tab-less location page, so it draws its own title like the Ship/World pages:
//   title(16, 36px) · rule(64) · 外星合金 balance (24px) · [蓝图 group, only when
//   at least one is redeemed] · 制造: legend · full-width 80px bands · 更多 pager.
// Only the balance row and the title are at fixed y — everything below the
// blueprints group reflows, because that group is one line with a couple of
// blueprints and two with all five. That is also why pressRect reads the band's
// y off the region rather than recomputing it (the Trade page's trick).
#include "fabricator_page.h"
#include "action_band.h"        // the app-wide button band — the ONLY button frame
#include "cjk_text.h"
#include "pomo_page.h"          // PAD (shared layout authority)
#include "page_tabs.h"          // resetHitCache — a tab-less page clears the cache
#include "pager.h"
#include "game_state.h"
#include "beeper.h"
#include "rtc_bm8563.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// main.cpp owns the game model.
extern adr::GameState g_game;

using namespace adr;

namespace {
constexpr int SCALE      = 2;                 // 12px grid x2 = 24px body
constexpr int GLYPH      = 12 * SCALE;        // 24px line box
constexpr int TITLE_SCALE = 3;                // 12px grid x3 = 36px page title
constexpr int CONTENT_W  = 540 - 2 * PAD;     // 492px usable (§9.2)

// ---- vertical budget (§9.4) ------------------------------------------------
constexpr int TITLE_Y    = 16;                // 36px title ink 16..52
constexpr int RULE_Y     = 64;                // 2px rule under the title
constexpr int ALLOY_Y    = 84;                // 外星合金 N (24px)
constexpr int GROUP_GAP  = 16;                // space above a group legend
constexpr int LIST_GAP   = 6;                 // legend -> its list/bands
constexpr int BAND_H     = 80;                // long-press band (§9.3 floor)
constexpr int BAND_GAP   = 12;
// The lowest pixel a band may occupy. Worst case (all five blueprints redeemed,
// so the list wraps to two lines): 蓝图 legend 124, list 154..214, 制造: legend 230,
// first band 260, seventh band ends 892 — and the eighth would end 984, which is
// exactly the case the 更多 pager exists for.
constexpr int BAND_BOTTOM_LIMIT = 912;

// "更多" pagination sentinel; real slot codes are Fab ids (0..FAB_COUNT-1).
constexpr uint8_t A_MORE = 0xFF;

// RTC -> Unix epoch, mirroring ship_page/trade_page/room_page.
uint32_t epochNow() {
    rtc::Date d; rtc::Time t;
    rtc::getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}

// The rect of a band whose top is `top` — the ONE description of where a button
// is, shared by the draw call and pressRect so the drawn frame and the
// invert-flash rect cannot drift apart.
pages::Rect bandRect(int top) {
    return pages::Rect{ PAD, top, CONTENT_W, BAND_H };
}

// Is row `id` offerable at all? fabricator.js:213-215 canFabricate — no blueprint,
// no button (NOT a dashed one). A capped row that is already owned KEEPS its band
// and goes dashed, which is what upstream's Button.setDisabled actually does.
bool fabOfferable(uint8_t id) { return g_game.canFabricate(id); }

// Can row `id` be fabricated right now? The cap and the alloy price — the same two
// tests fabricate() enforces, so a dashed band and a refused press always agree.
bool fabEnabled(uint8_t id) {
    const Fabricatable& f = FABRICATE[id];
    if (f.maximum >= 0 && g_game.fabricatedCount(id) >= f.maximum) return false;
    return g_game.stores[R_ALIEN_ALLOY] >= (int32_t)f.alloyCost * FP;
}

// The offerable rows for the current blueprint set, in FABRICATE order.
int buildRows(uint8_t* out, int cap) {
    int n = 0;
    for (uint8_t id = 0; id < FAB_COUNT && n < cap; id++)
        if (fabOfferable(id)) out[n++] = id;
    return n;
}

// "针剂 (x5)" — fabricator.js appends the batch size to the button text for the
// one row that makes more than one at a time. ASCII '(' / 'x' / ')' are baked into
// the same 12px face as the CJK (cjk_text.h), so this needs no new glyph.
void fabLabel(uint8_t id, char* out, size_t cap) {
    const Fabricatable& f = FABRICATE[id];
    if (f.quantity > 1) snprintf(out, cap, "%s (x%d)", tr(f.key), (int)f.quantity);
    else                snprintf(out, cap, "%s", tr(f.key));
}

// "-1 外星合金" — the same "-amount name" convention every other priced band in
// the firmware emits (trade_page/ship_page/tech_page/event_modal). Every row costs
// alien alloy and nothing else, so this is always a single entry.
void fabCost(uint8_t id, char* out, size_t cap) {
    snprintf(out, cap, "-%d %s", (int)FABRICATE[id].alloyCost,
             tr(RES_KEY[R_ALIEN_ALLOY]));
}

// The blueprints group's list line: the PRODUCT names of every redeemed blueprint,
// joined by the same two spaces every cost run uses. Upstream lists the product
// name, not "<product> blueprint" (fabricator.js:113-118), and FABRICATE is the
// single place that maps a blueprint bit to its product — so no second table.
// Returns false when nothing is redeemed (the whole group is then skipped).
bool blueprintLine(char* out, size_t cap) {
    out[0] = 0;
    size_t used = 0;
    for (uint8_t id = 0; id < FAB_COUNT; id++) {
        int8_t bp = FABRICATE[id].blueprintBit;
        if (bp == BP_NONE || !g_game.hasBlueprint((uint8_t)bp)) continue;
        int wrote = snprintf(out + used, cap - used, "%s%s", used ? "  " : "",
                             tr(FABRICATE[id].key));
        if (wrote < 0) break;
        used += (size_t)wrote;
        if (used >= cap) { used = cap - 1; break; }
    }
    return used > 0;
}

// Content signature — every live value that changes a painted number, label or
// frame state: the alloy balance (the one price), the redeemed blueprint set
// (which bands exist), each row's owned count (which capped bands are dashed) and
// the current pagination page.
uint32_t contentSig(int page) {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix((uint32_t)g_game.whole(R_ALIEN_ALLOY));
    mix((uint32_t)g_game.blueprints);
    for (uint8_t id = 0; id < FAB_COUNT; id++)
        mix((uint32_t)g_game.fabricatedCount(id));
    mix((uint32_t)page);
    return sig;
}
}  // namespace

// ================================ Page API =================================

const pages::Region* FabricatorPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Press feedback: the band's own drawn frame, not the Page default's full 540px
// y-band — the bands are PAD-inset, so the default would also flash the clear
// margin outside the frame. The band's top comes off the region itself because
// this page's first band's y depends on the blueprints group above it.
pages::Rect FabricatorPage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)x; (void)y;
    return bandRect(rg.y0);
}

// Hidden until the Executioner's prologue has been cleared AND walked home from
// (see the header): returning false makes showPageOrNext skip this ring slot and
// drops its dot from the status bar, the same mechanism Outside/Trade/Ship use.
// This is the single source of that predicate — draw() consults it, so the page
// and the dot count can never disagree.
bool FabricatorPage::available() const { return g_game.execEntered; }

bool FabricatorPage::draw(m5gfx::M5Canvas& c) {
    if (!available()) return false;
    c.fillSprite(TFT_WHITE);
    page_tabs::resetHitCache();      // tab-less page: drop any stale header hitbox

    cjk::drawText(c, PAD, TITLE_Y, tr("A Whirring Fabricator"), TITLE_SCALE);
    c.fillRect(PAD, RULE_Y, CONTENT_W, 2, TFT_BLACK);

    // The currency EVERY band spends, so "why is 制造 dashed" is answerable
    // without leaving the page (the ship page's balance row, one resource wide).
    {
        char amt[8]; fmtAmount(g_game.whole(R_ALIEN_ALLOY), amt, sizeof(amt));
        char line[40];
        snprintf(line, sizeof(line), "%s %s", tr(RES_KEY[R_ALIEN_ALLOY]), amt);
        cjk::drawText(c, PAD, ALLOY_Y, line, SCALE);
    }

    int y = ALLOY_Y + GLYPH;

    // ---- blueprints group (skipped entirely when none is redeemed) ----------
    char bps[160];
    if (blueprintLine(bps, sizeof bps)) {
        y += GROUP_GAP;
        cjk::drawText(c, PAD, y, tr("blueprints"), SCALE);
        y += GLYPH + LIST_GAP;
        y = cjk::drawWrapped(c, PAD, y, CONTENT_W, bps, SCALE);
    }

    // ---- fabricate group ----------------------------------------------------
    y += GROUP_GAP;
    cjk::drawText(c, PAD, y, tr("fabricate:"), SCALE);
    y += GLYPH + LIST_GAP;

    // Paginate the offerable rows into what still clears the status bar. The
    // window is measured, not assumed, because the blueprints group above moves
    // the first band's top by up to ~90px.
    uint8_t all[FAB_COUNT];
    int total = buildRows(all, (int)sizeof(all));
    int fit = 0;
    while (fit < MAX_BANDS &&
           y + (fit + 1) * (BAND_H + BAND_GAP) - BAND_GAP <= BAND_BOTTOM_LIMIT)
        fit++;

    int numPages = 1, start = 0, take = total, pg = 0;
    bool more = false;
    // fit >= 2 is what makes a pager band worth drawing at all: with room for one
    // band, spending it on 更多 would page between empty screens. The measured
    // budget above never actually gets that tight (7 with the blueprints block,
    // 8 without), so this is the guard that keeps a future translation growing
    // that block from turning the page into a dead end rather than a live one.
    if (total > fit && fit >= 2) {
        int perPage = fit - 1;                    // (fit-1) real rows + 更多
        numPages = (total + perPage - 1) / perPage;
        pg = ((m_page % numPages) + numPages) % numPages;
        start = pg * perPage;
        take = total - start; if (take > perPage) take = perPage;
        more = true;
    }

    int k = 0;
    for (int i = 0; i < take && k < fit; i++) m_slotCodes[k++] = all[start + i];
    if (more && k < fit) m_slotCodes[k++] = A_MORE;
    m_slotCount = k;

    // Lay out and draw in ONE pass — unlike the Trade page, nothing here needs the
    // whole band list before the first stroke, so a per-row pair of buffers does.
    for (int s = 0; s < m_slotCount; s++) {
        char label[48], cost[32];
        bool hasCost, enabled;
        if (m_slotCodes[s] == A_MORE) {
            snprintf(label, sizeof(label), "更多 (%d/%d)",
                     (pg + 1 < numPages ? pg + 2 : 1), numPages);
            hasCost = false;
            enabled = true;
        } else {
            uint8_t id = m_slotCodes[s];
            fabLabel(id, label, sizeof(label));
            fabCost(id, cost, sizeof(cost));
            hasCost = true;
            enabled = fabEnabled(id);
        }
        int top = y + s * (BAND_H + BAND_GAP);
        action_band::draw(c, bandRect(top), label, hasCost ? cost : nullptr,
                          enabled, 0, 0);
        m_regions[s].y0 = (uint16_t)top;
        m_regions[s].y1 = (uint16_t)(top + BAND_H);
        m_regions[s].type  = 1;                   // firmware-local
        m_regions[s].param = (uint8_t)s;          // full-width row -> slot index
    }
    m_regionCount = m_slotCount;
    return true;
}

// Long-press on a band -> fabricate the row. Full-width single column, so param is
// the slot index directly (x/y unused). Success: high beep, persist, full redraw
// (the alloy balance moved, a capped band may have just gone dashed). A refusal
// low-beeps — the engine already pushed 「外星合金不足」 to the log when that was
// the reason, and the band was already drawn dashed, so nothing on screen owes the
// player a repaint. "更多" flips to the next batch.
void FabricatorPage::onLocalAction(uint8_t param, int x, int y) {
    (void)x; (void)y;
    int slot = param;
    if (slot < 0 || slot >= m_slotCount) { beeper::tone(600, 120); return; }
    uint8_t code = m_slotCodes[slot];

    if (code == A_MORE) {
        m_page++;
        beeper::tone(1800, 80);
        pager::showPage(pager::currentRingIndex(), false);
        m_lastSig = contentSig(m_page);
        return;
    }
    if (code >= FAB_COUNT) { beeper::tone(600, 120); return; }   // stale region table

    if (g_game.fabricate(code) != RC_OK) { beeper::tone(600, 120); return; }
    beeper::tone(1800, 80);
    g_game.save();
    pager::showPage(pager::currentRingIndex(), false);
    // Re-baseline tick()'s signature to the state we JUST drew so this same press
    // doesn't trip a SECOND full redraw a second later (room/trade/ship all do the
    // same right after their own showPage).
    m_lastSig = contentSig(m_page);
}

// Time axis (awake only). Settle the offline economy each second — this is a
// village page and the workers keep working while it is up — then repaint on any
// change to a painted value (an alloy delivery, a blueprint redeemed by a trip
// that ended while this page was up, a band crossing its cap).
void FabricatorPage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick = 0;
    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    g_game.settle(epochNow());

    uint32_t sig = contentSig(m_page);
    if (sig != m_lastSig) {
        m_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);
    }
}
