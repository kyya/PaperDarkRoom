// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Trade (trading-post) page — the real v0.3.3 buy UI over the game_state engine
// (src/game_state.*). Every string routes through tr() (strings_zh.h) so only
// the official Simplified-Chinese translation reaches the sparse 12px CJK face
// (§8.3 glyph-closure iron law). Layout obeys §9 (36px button labels / 24px
// body, >=80px long-press bands, paginate rather than compress). See
// trade_page.h for the region model.
#include "trade_page.h"
#include "cjk_text.h"
#include "pomo_page.h"          // PAD (shared layout authority)
#include "page_tabs.h"          // shared three-tab header (生火间 │ 村落 │ 贸易站)
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
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px (balance / cost)
constexpr int BTN_SCALE = 3;                 // 12px grid x3 = 36px (buy label, v0.3.3)
constexpr int GLYPH     = 12 * SCALE;        // 24px line box
constexpr int BTN_GLYPH = 12 * BTN_SCALE;    // 36px line box
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)

// ---- vertical budget (§9.4). The three-tab header (page_tabs::TAB_H = 72px)
// owns the top band; the balance row and BUY bands reflow below it. Buy bands
// are a SINGLE full-width column (x 24..516) — the buy labels ("购买能量元件" =
// 216px @36px, measured scratchpad/measure_labels) plus a cost sub-row need the
// whole width, unlike the Room/Outside two-column grids. Top -> bottom:
//   header(0..72) · balance row(84, 24px fur/scales/teeth) · BUY bands
//   (128 + n*(80+12), <=8 rows, last ends 852 < 928 status bar).
// ----------------------------------------------------------------------------
constexpr int BAL_Y     = page_tabs::CONTENT_TOP + 12;   // 84 — balance row top
// Three evenly-spread columns for the payment resources (毛皮/鳞片/牙齿), each
// "名 N" — name 48px + a compact (abbreviated) amount clears the 168px pitch.
constexpr int BAL_COLX[3] = { PAD, PAD + 168, PAD + 336 };   // {24, 192, 360}

constexpr int BAND_TOP  = 128;               // first BUY band top (below balance)
constexpr int BUY_H     = 80;                // long-press band (§9.3: >=80px, floor)
constexpr int BUY_GAP   = 12;                // vertical gap between bands
constexpr int BUY_X     = PAD;               // full-width single column
constexpr int BUY_W     = CONTENT_W;         // 492
constexpr int SUBGAP    = 6;                 // 36px label -> 24px cost sub-row gap.
                                             // 36 + 6 + 24 = 66 <= 80 (7px top/
                                             // bottom margin) — the two-line block
                                             // fits the band with room to spare.

// "更多" pagination sentinel; real slot codes are Trade ids (0..TRADE_COUNT-1).
constexpr uint8_t A_MORE = 0xFF;

// Trading-post buy labels (moved here from room_page.cpp v0.3.2). Upstream
// room.js's OWN persistent buy button shows the bare good name (`text: _(g)`),
// but this page's flat grid has no "build:/craft:/buy:" section legend, so a
// bare resource name would read like a craftable — the label composes tr("buy:")
// (stripped of its trailing ASCII ':') with the resource name for the 6 goods
// with no matching event "buy X" key, and reuses the exact event string for the
// 4 that do (scales/teeth/medicine/compass). Order matches the Trade enum.
static const char* const TRADE_BUY_KEY[TRADE_COUNT] = {
    "buy scales",   // T_SCALES
    "buy teeth",    // T_TEETH
    nullptr,        // T_IRON        — compose "buy:" + "iron"
    nullptr,        // T_COAL        — compose "buy:" + "coal"
    nullptr,        // T_STEEL       — compose "buy:" + "steel"
    "buy medicine", // T_MEDICINE
    nullptr,        // T_BULLETS     — compose "buy:" + "bullets"
    nullptr,        // T_ENERGY_CELL — compose "buy:" + "energy cell"
    nullptr,        // T_ALIEN_ALLOY — compose "buy:" + "alien alloy"
    "buy compass",  // T_COMPASS
};

void tradeLabel(uint8_t tradeId, char* out, size_t cap) {
    const char* direct = TRADE_BUY_KEY[tradeId];
    if (direct) { snprintf(out, cap, "%s", tr(direct)); return; }
    char prefix[16];
    snprintf(prefix, sizeof(prefix), "%s", tr("buy:"));      // "购买:"
    size_t plen = strlen(prefix);
    if (plen > 0 && prefix[plen - 1] == ':') prefix[plen - 1] = '\0';  // "购买"
    snprintf(out, cap, "%s%s", prefix, tr(RES_KEY[TRADE[tradeId].product]));
}

struct BandView {
    uint8_t code;                // Trade id, or A_MORE
    char    label[48];
    char    cost[64];            // "-150 毛皮  -50 鳞片" (empty for "更多")
    bool    hasCost;
    bool    enabled;             // false -> render unavailable (dashed)
};

// RTC -> Unix epoch, mirroring room_page/main.cpp's epochNow.
uint32_t epochNow() {
    m5::rtc_date_t d; m5::rtc_time_t t;
    M5.Rtc.getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}

// Is trade good `id` offerable now (under maximum)? Mirrors room_page's old
// tradeOfferable exactly: cost is NOT checked here (an unaffordable buy button
// still shows, dashed), only whether it could EVER be bought again right now
// (compass caps at 1 -> disappears once owned; room.js goodsMax parity).
bool tradeOfferable(uint8_t id) {
    const TradeGood& g = TRADE[id];
    int have = g_game.whole(g.product); if (have < 0) have = 0;
    if (g.maximum >= 0 && have >= g.maximum) return false;
    return true;
}

// Can good `id` be bought right now? buy() has no cooldown and no room-too-cold
// gate (room.js buy() checks neither — only build()/craft() do), so only cost
// gates here — an unaffordable good renders dashed.
bool buyEnabled(uint8_t id) {
    const TradeGood& g = TRADE[id];
    for (int i = 0; i < 3 && g.cost[i].res != RA_END; i++)
        if (g_game.stores[g.cost[i].res] < (int32_t)g.cost[i].amt * FP) return false;
    return true;
}

// The offerable trade goods for the current state (all under-maximum goods once
// the post stands). Returns the count.
int buildGoods(uint8_t* out, int cap) {
    int n = 0;
    for (uint8_t id = 0; id < TRADE_COUNT && n < cap; id++)
        if (tradeOfferable(id)) out[n++] = id;
    return n;
}

// A good's cost sub-line ("-150 毛皮  -50 鳞片"), joining entries with two
// spaces — the exact form event_modal::costLine uses. Full numbers (costs are
// small, fixed, and clearer un-abbreviated; the balance/inventory abbreviation
// is for volatile stockpiles, not fixed prices). Returns false (empty) if free.
bool costLine(uint8_t id, char* out, size_t cap) {
    out[0] = 0;
    const TradeGood& g = TRADE[id];
    if (g.cost[0].res == RA_END) return false;
    size_t used = 0;
    for (int i = 0; i < 3 && g.cost[i].res != RA_END; i++) {
        const char* rz = tr(RES_KEY[g.cost[i].res]);
        int wrote = snprintf(out + used, cap - used, "%s-%ld %s",
                             used ? "  " : "", (long)g.cost[i].amt, rz);
        if (wrote < 0) break;
        used += (size_t)wrote;
        if (used >= cap) { used = cap - 1; break; }
    }
    return used > 0;
}

// Compute the visible band list for `page`: fills slotCodes[] + views[] and
// regionsOut[] (one full-width y-band per slot, param = slot index). Batches of
// 7 real goods + a trailing "更多" once the offerable list exceeds MAX_BANDS
// (Room/Outside pagination 手法). Returns the band count (== region count).
int layoutBands(pages::Region* regionsOut, uint8_t* slotCodes, BandView* views,
                int page, int maxBands, int* slotCountOut) {
    uint8_t all[TRADE_COUNT];
    int total = buildGoods(all, (int)sizeof(all));

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
    if (more && k < maxBands) slotCodes[k++] = A_MORE;
    int slotCount = k;

    for (int s = 0; s < slotCount; s++) {
        views[s].code = slotCodes[s];
        if (slotCodes[s] == A_MORE) {
            snprintf(views[s].label, sizeof(views[s].label), "更多 (%d/%d)",
                     (pg + 1 < numPages ? pg + 2 : 1), numPages);
            views[s].cost[0] = 0;
            views[s].hasCost = false;
            views[s].enabled = true;
        } else {
            uint8_t id = slotCodes[s];
            tradeLabel(id, views[s].label, sizeof(views[s].label));
            views[s].hasCost = costLine(id, views[s].cost, sizeof(views[s].cost));
            views[s].enabled = buyEnabled(id);
        }
        int top = BAND_TOP + s * (BUY_H + BUY_GAP);
        regionsOut[s].y0 = (uint16_t)top;
        regionsOut[s].y1 = (uint16_t)(top + BUY_H);
        regionsOut[s].type  = 1;                     // firmware-local
        regionsOut[s].param = (uint8_t)s;            // slot index (full-width row)
    }
    *slotCountOut = slotCount;
    return slotCount;
}

// ---- drawing pieces --------------------------------------------------------

// Balance row: the three payment resources (毛皮/鳞片/牙齿), "名 N", N compact
// (fmtAmount) so a five-figure fur pile can't overrun the column.
void drawBalance(m5gfx::M5Canvas& c) {
    const uint8_t res[3] = { R_FUR, R_SCALES, R_TEETH };
    for (int i = 0; i < 3; i++) {
        char amt[8]; fmtAmount((int32_t)g_game.whole(res[i]), amt, sizeof(amt));
        char line[32];
        snprintf(line, sizeof(line), "%s %s", tr(RES_KEY[res[i]]), amt);
        cjk::drawText(c, BAL_COLX[i], BAL_Y, line, SCALE);
    }
}

// 1px dashed rectangle, 4px on / 4px off — the global unavailable-button frame
// (matches room_page/outside_page/event_modal drawDashedRect exactly).
void drawDashedRect(m5gfx::M5Canvas& c, int x, int y, int w, int h) {
    const int on = 4, per = 8;
    int xr = x + w - 1, yb = y + h - 1;
    for (int i = 0; i < w; i++)
        if (i % per < on) { c.drawPixel(x + i, y, TFT_BLACK);
                            c.drawPixel(x + i, yb, TFT_BLACK); }
    for (int i = 0; i < h; i++)
        if (i % per < on) { c.drawPixel(x, y + i, TFT_BLACK);
                            c.drawPixel(xr, y + i, TFT_BLACK); }
}

// One full-width BUY band. Available -> two solid rings (2px); unavailable (cost
// not met) -> a 1px dashed frame. The 36px label sits over the 24px cost
// sub-row, the two-line block vertically centered (event_modal parity, scaled up
// to a 36px label). A "更多" band has no cost row: its 36px label centers alone.
void drawBuyBand(m5gfx::M5Canvas& c, int top, const BandView& v) {
    if (v.enabled) {
        c.drawRect(BUY_X, top, BUY_W, BUY_H, TFT_BLACK);
        c.drawRect(BUY_X + 1, top + 1, BUY_W - 2, BUY_H - 2, TFT_BLACK);
    } else {
        drawDashedRect(c, BUY_X, top, BUY_W, BUY_H);
    }

    if (v.hasCost) {
        int block = BTN_GLYPH + SUBGAP + GLYPH;          // 36 + 6 + 24 = 66
        int ly = top + (BUY_H - block) / 2;              // top + 7
        int lw = cjk::textWidth(v.label, BTN_SCALE);
        cjk::drawText(c, BUY_X + (BUY_W - lw) / 2, ly, v.label, BTN_SCALE);
        int cw = cjk::textWidth(v.cost, SCALE);
        cjk::drawText(c, BUY_X + (BUY_W - cw) / 2, ly + BTN_GLYPH + SUBGAP,
                      v.cost, SCALE);
    } else {
        int lw = cjk::textWidth(v.label, BTN_SCALE);
        cjk::drawText(c, BUY_X + (BUY_W - lw) / 2,
                      top + (BUY_H - BTN_GLYPH) / 2 - 4, v.label, BTN_SCALE);
    }
}

}  // namespace

// ================================ Page API =================================

const pages::Region* TradePage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Hidden until the trading post stands: returning false makes showPageOrNext
// skip this ring slot (same mechanism the Outside page uses for outsideUnlocked
// — verified page-kind-agnostic: showPage -> pageAt resolves client pages too,
// draw()==false -> showPage returns false -> showPageOrNext steps past it).
bool TradePage::draw(m5gfx::M5Canvas& c) {
    if (g_game.buildings[B_TRADING_POST] == 0) return false;
    c.fillSprite(TFT_WHITE);
    page_tabs::draw(c, 2);           // three-tab header, Trade active
    drawBalance(c);
    BandView views[MAX_BANDS];
    m_regionCount = layoutBands(m_regions, m_slotCodes, views, m_page, MAX_BANDS,
                                &m_slotCount);
    for (int s = 0; s < m_slotCount; s++)
        drawBuyBand(c, BAND_TOP + s * (BUY_H + BUY_GAP), views[s]);
    return true;
}

// Long-press on a band -> buy the good. Full-width single column, so param is
// the slot index directly (x/y unused). Success: high beep, persist, full
// redraw (the balance row and each band's affordability shift). Cost failure:
// low beep — the engine already pushed "not enough X" to the log (surfaced on
// the Room page), and this band was already drawn dashed, so no repaint is
// needed here. "更多" flips to the next batch. save() lives here (buy() does not
// persist itself — single write, no double-save).
void TradePage::onLocalAction(uint8_t param, int x, int y) {
    (void)x; (void)y;
    int slot = param;
    if (slot < 0 || slot >= m_slotCount) { M5.Speaker.tone(600, 120); return; }
    uint8_t code = m_slotCodes[slot];

    if (code == A_MORE) {
        m_page++;
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::currentRingIndex(), false);
        return;
    }
    if (code >= TRADE_COUNT) { M5.Speaker.tone(600, 120); return; }

    Result r = g_game.buy(code);
    if (r == RC_OK) {
        M5.Speaker.tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
    } else {
        // RC_ERR_COST (engine pushed "not enough X") / RC_ERR_MAX / RC_ERR_LOCKED
        // — low beep; nothing on this page changed, so no repaint.
        M5.Speaker.tone(600, 120);
    }
}

// Time axis (awake only). Settle the economy each second, then repaint on any
// change to a painted number/label: the balance row (fur/scales/teeth), each
// band's affordable/dashed state and presence (a compass leaving the list at
// max, a good becoming affordable), plus the shared header's Room/Outside tab
// titles. Buys carry no cooldown, so — unlike Room/Outside — there is no partial
// button-area path; every change is a full redraw. Mirrors the room/outside
// tick cadence.
void TradePage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick = 0;
    static uint32_t s_lastSig  = 0;

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    g_game.settle(epochNow());

    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    for (int i = 0; i < RES_COUNT; i++) mix((uint32_t)g_game.whole((uint8_t)i));
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);
    mix((uint32_t)(uint8_t)g_game.fire);                 // Room tab title
    mix(g_game.outsideUnlocked ? 1u : 0u);               // Outside tab gate

    if (sig != s_lastSig) {
        s_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);
    }
}
