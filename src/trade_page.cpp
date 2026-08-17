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
#include "action_band.h"        // the app-wide button band (this page's own style)
#include "cjk_text.h"
#include "page_layout.h"        // PAD (shared layout authority)
#include "page_tabs.h"          // shared three-tab header (生火间 │ 村落 │ 贸易站)
#include "stepper.h"            // the shared ×10 column (up-only here)
#include "icons.h"              // icons::drawCentred — 1bpp Lucide glyphs
#include "icons_data.h"         // ICON_CART_* — this is its ONE includer
#include "pager.h"
#include "game_state.h"
#include "world_state.h"        // WorldState::ensureGenerated + compassFromVillage (§1.6)
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// main.cpp owns both the game model and the full-screen sprite.
extern adr::GameState  g_game;
extern adr::WorldState g_world;
extern M5Canvas canvas;

using namespace adr;

namespace {
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px (balance row)
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)
// The band's own type scale (36px label over a 24px cost sub-row) is no longer
// declared here: it IS action_band's contract now (v0.12), and re-declaring it
// locally is exactly how the eight sites drifted apart in the first place.

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
                                             // (36 + 6 + 24 = 66 <= 80 — the
                                             // label+cost block clears an 80px
                                             // band with 7px top/bottom margin;
                                             // action_band.cpp owns the full
                                             // derivation now.)

// "更多" pagination sentinel; real slot codes are Trade ids (0..TRADE_COUNT-1).
constexpr uint8_t A_MORE = 0xFF;

// ---- the ×10 column -------------------------------------------------------
// Trade lays out its own coarse column instead of borrowing stepper's 66px one.
// That width is sized for a lone ▲ glyph; this column carries a 24px cart AND a
// 36px "x10" (64px of content), which left nothing for padding — the divider
// ended up 5px from the icon and the whole group read as one cramped smudge.
// 96px seats the same content with 16px of air on both sides of the rule.
//
// The room was always there: a band's cost prints ONE ENTRY PER LINE (splitCost),
// so the widest single line is "-1500 毛皮" at 120px, not the 360px a one-line
// rendering of 外星合金's full cost would need. Title + cost + this column still
// clears action_band's budget with room over.
//
// stepper::MANY is still the shared step size — the number 10 is one decision,
// wherever it is spent.
constexpr int STEP_COL_W = 96;
constexpr int STEP_INSET = 12;                 // column -> band right edge
constexpr int STEP_PAD   = 16;                 // air each side of the rule
constexpr int stepColX(int w) { return w - STEP_INSET - STEP_COL_W; }
// The rule is drawn ON the hit boundary, so what looks like the ×10 zone is
// exactly what behaves like it.
constexpr int stepRuleX(int w) { return stepColX(w) - 4; }
// Structural, not content: a hairline that separates without competing with the
// cost figures beside it (用户: 分割线的灰度再不明显一点). Lighter than
// action_band's DISABLED_INK, which is for text that is still meant to be read.
constexpr uint16_t STEP_RULE_INK = 0x9CD3;

// The band's label is the BARE good name — 鳞片, not 购买鳞片 (v0.20).
//
// This is what upstream room.js does too (`text: _(g)`); the port had prefixed
// every label with 购买 because its flat v0.3.2 grid mixed build/craft/buy rows
// with no section legend, so a bare resource name would have read like a
// craftable. That reason is gone: this is a dedicated page whose every row is a
// purchase, the row now ends in a basket button that says so, and repeating
// 购买 on all ten rows spent 4 of the label's characters saying what the page
// already says once — pushing long names like 外星合金 into the 24px fallback.
void tradeLabel(uint8_t tradeId, char* out, size_t cap) {
    snprintf(out, cap, "%s", tr(RES_KEY[TRADE[tradeId].product]));
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

// Is trade good `id` offerable now? Delegates to the engine's buyOfferable
// (room.js buyUnlocked: trading post up + product resource SEEN, compass always
// offered, hidden once compass owned). v0.4.8 fix B4: the seen gate replaces the
// old "everything shows the moment the post stands" behaviour. Cost is NOT
// checked here — an unaffordable-but-offered buy band still shows, dashed.
bool tradeOfferable(uint8_t id) {
    return g_game.buyOfferable(id);
}

// Can good `id` be bought right now? buy() has no cooldown and no room-too-cold
// gate (room.js buy() checks neither — only build()/craft() do), so only cost
// gates here — an unaffordable good renders dashed.
bool buyEnabled(uint8_t id) {
    return id < TRADE_COUNT && g_game.maxBuyable(id) > 0;
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

// The rect of the BUY band at `top` — the ONE description of where a buy button
// is, shared by the draw call and pressRect so the drawn frame and the
// invert-flash rect cannot drift apart.
pages::Rect bandRect(int top) {
    return pages::Rect{ BUY_X, top, BUY_W, BUY_H };
}

// Slot index -> its band's top, the one place that stacking rule is written
// (layoutBands lays the regions out with the identical expression).
int bandTopForSlot(int slot) { return BAND_TOP + slot * (BUY_H + BUY_GAP); }

// One full-width BUY band, through the shared renderer. This page's 36px-label-
// over-24px-cost, block-centered band IS the app-wide button style (v0.12 pulled
// it into action_band and migrated every other site onto it — see action_band.h),
// so there is nothing Trade-specific left to draw here. A priced band centres
// its label+cost block; the costless "更多" pager centres its lone label, which
// on this page reads cleanly because it only ever sits alone at the very bottom
// of a single full-width column.

void drawBuyBand(m5gfx::M5Canvas& c, int top, const BandView& v) {
    // Every priced band carries the column, affordable or not — an unaffordable
    // good greys its cart the way the band greys everything else, instead of
    // losing the button entirely and leaving a hole where the other rows have
    // one. The band must also keep its cost text clear of that column: the cost
    // right-aligns to the band edge, so without the inset the two land on top of
    // each other (they did, and it shipped that way once).
    const bool step = v.hasCost;
    action_band::draw(c, bandRect(top), v.label, v.hasCost ? v.cost : nullptr,
                      v.enabled, 0, 0,
                      step ? STEP_INSET + STEP_COL_W + STEP_PAD : 0);
    // The ×10 column (v0.20). A good's cost is paid over and over — 1 iron wants
    // 50 scales, i.e. fifty separate presses at ×1 — so the same coarse step the
    // Assign/Path rows have had since v0.14 belongs here too. Not drawn on 更多
    // (no quantity to step) or on a band already unaffordable at ×1.
    if (!step) return;
    pages::Rect band = bandRect(top);
    // Cart over its multiplier. The cart alone says "buy", which the whole page
    // already says — what this column actually does is buy TEN, and that number
    // has to be on the button or the player has no way to know pressing here
    // differs from pressing the row. The cart is lucide/shopping-cart rasterised
    // at 34px by tools/gen_icons.py; hand-drawing it was tried and rejected
    // (用户: "太丑了") — this page is line art, so the mark has to carry the same
    // considered stroke weight as everything else on it.
    c.drawFastVLine(band.x + stepRuleX(band.w), band.y + 14, band.h - 28,
                    STEP_RULE_INK);
    // Cart and multiplier side by side, both vertically centred, the pair centred
    // in the column — 24 + 4 + 36 = 64 of content in 96, so 16px clears the rule
    // on the left and the band's edge on the right.
    const int colX = band.x + stepColX(band.w);
    const int cy   = band.y + band.h / 2;
    const uint16_t ink = v.enabled ? TFT_BLACK : action_band::DISABLED_INK;
    const char* mult = "x10";
    const int mw   = cjk::textWidth(mult, 2);
    const int x0   = colX + (STEP_COL_W - (ICON_CART_W + 4 + mw)) / 2;
    icons::draw(c, ICON_CART_BITS, ICON_CART_W, ICON_CART_H, ICON_CART_STRIDE,
                x0, cy - ICON_CART_H / 2, ink);
    cjk::drawText(c, x0 + ICON_CART_W + 4, cy - 12, mult, 2, ink);
}

// Content signature — a hash of every live value that alters a painted number or
// label (whole resource units, buildings, the shared Room/Outside tab titles, the
// seen-mask that gates a buy band). tick() compares it each second to decide a full
// redraw; onLocalAction re-baselines it right after its own showPage so the same
// buy no longer forces a SECOND full redraw on the next tick (see onLocalAction).
// Reads only g_game, never mutates.
uint32_t contentSig() {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    for (int i = 0; i < RES_COUNT; i++) mix((uint32_t)g_game.whole((uint8_t)i));
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);
    mix((uint32_t)(uint8_t)g_game.fire);                 // Room tab title
    mix(g_game.outsideUnlocked ? 1u : 0u);               // Outside tab gate
    mix(g_game.seen);                                    // B4: a newly-seen resource adds its buy band
    return sig;
}

}  // namespace

// ================================ Page API =================================

const pages::Region* TradePage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Press feedback: narrow the Page default's full 540px-wide flash (page.h) to
// the band's own drawn frame — the SAME bandRect() drawBuyBand framed. Every
// band here, real good or "更多", shares that rect; BUY_X is PAD-inset (24px
// each side, not full-bleed), so the untouched default would also flash the
// white margin outside the drawn frame. x/y are unused: onLocalAction (and
// every band's drawn frame) doesn't split on them.
pages::Rect TradePage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)y;
    pages::Rect band = bandRect(rg.y0);
    // A band carrying the ×10 column flashes only the part that was pressed, so
    // the feedback says WHICH quantity is being bought. Bands with no column
    // (更多, or a good unaffordable even at ×1) keep flashing whole — the same
    // predicate drawBuyBand used, so the flash can never disagree with the paint.
    int slot = rg.param;
    if (slot < 0 || slot >= m_slotCount) return band;
    // 更多 has no column and flashes whole; every good has one, affordable or
    // not, so the flash tells the player which quantity they hit even when the
    // press is about to be refused for cost.
    if (m_slotCodes[slot] >= TRADE_COUNT) return band;
    const int cx = band.x + stepColX(band.w);
    if (x >= cx) return pages::Rect{ cx, band.y, STEP_COL_W, band.h };
    return pages::Rect{ band.x, band.y, stepColX(band.w) - 4, band.h };
}

// Hidden until the trading post stands: returning false makes showPageOrNext
// skip this ring slot (same mechanism the Outside page uses for outsideUnlocked
// — verified page-kind-agnostic: showPage -> pageAt resolves client pages too,
// draw()==false -> showPage returns false -> showPageOrNext steps past it).
// available() is the single source of that hide predicate — draw() and the
// status bar's page-dot count both read it, so they never drift.
bool TradePage::available() const { return g_game.buildings[B_TRADING_POST] > 0; }

bool TradePage::draw(m5gfx::M5Canvas& c) {
    if (!available()) return false;
    c.fillSprite(TFT_WHITE);
    page_tabs::draw(c, 2);           // three-tab header, Trade active
    drawBalance(c);
    if (m_lastPurchased > 0) {
        char label[48], notice[80];
        tradeLabel(m_lastPurchaseCode, label, sizeof(label));
        snprintf(notice, sizeof(notice), "%s%s x%d", tr("purchased:"), label,
                 m_lastPurchased);
        cjk::drawText(c, PAD, BAL_Y + 24, notice, 1);
    }
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
    (void)y;
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

    // x decides the quantity: the ×10 column buys ten, anywhere left of it (the
    // whole label + cost area, i.e. exactly where the band was pressed before
    // this column existed) buys one. buy() truncates, so a ×10 with only 3
    // affordable buys 3 and still reports RC_OK.
    const pages::Rect band = bandRect(bandTopForSlot(slot));
    const int qty = (x >= band.x + stepColX(band.w)) ? stepper::MANY : 1;
    BuyResult r = g_game.buy(code, qty);
    if (r.status == RC_OK) {
        // room.js: `if(stores.compass && !pathDiscovery){ pathDiscovery = true;
        // Path.openPath() }` — buying the compass (capped at 1, so this is always
        // the FIRST buy) is what "discovers" Path; openPath pushes the one-time
        // "the compass points <dir>" notice (§1.6). The direction needs the
        // COMMITTED map, which is otherwise lazily generated at the first embark
        // (path_page::doEmbark) — a compass is always bought before any embark
        // (Path itself is gated on holding one), so generate it here too, with
        // the same seed strategy, so the notice has a real direction to report.
        if (code == T_COMPASS) {
            if (!g_world.generated) {
                uint32_t seed = epochNow();
                if (!seed) seed = g_game.rng ? g_game.rng : 0x9e3779b9u;
                g_world.ensureGenerated(seed);
            }
            char key[40];
            if (g_world.compassFromVillage(key, sizeof key)) g_game.pushLog(key);
        }
        m_lastPurchased = r.purchased;
        m_lastPurchaseCode = code;
        M5.Speaker.tone(1800, 80);
        if (!g_game.save()) {
            Serial.println("[trade] save failed after purchase");
            M5.Speaker.tone(600, 120);
            return;
        }
        pager::showPage(pager::currentRingIndex(), false);
        // Re-baseline tick()'s content signature to the state we JUST drew, so this
        // same buy no longer trips a SECOND full-page redraw next tick — only genuine
        // economy advancing in the following second still does. No extra settle:
        // draw() paints un-settled g_game and contentSig() must mirror exactly that.
        // (buyOfferable is const, so draw() has no craftUnlocked-style side effect to
        // capture; after showPage stays the canonical spot for parity with Room.)
        m_lastSig = contentSig();
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
// button-area path; every change is a full redraw. onLocalAction re-baselines
// m_lastSig after its own showPage, so a buy no longer forces a second full redraw
// here. Mirrors the room/outside tick cadence.
void TradePage::tick(uint32_t nowMs) {
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
