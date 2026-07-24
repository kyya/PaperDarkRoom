// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Path (出发准备 / 背包整备) page — see path_page.h for the role/visibility model
// and layout budget. Every piece of text routes through tr() (strings_zh.h) so
// only the official Simplified-Chinese translation reaches the sparse 12px CJK
// face — the §8.3 glyph-closure iron law. The two hardcoded literals are
// closure-safe: 「更多」 reuses the Outside page's old pager band (更/多 already in
// the closure), and every other string is a real tr() value (「返回」== tr("go
// home"), the item names, 出发, 护甲, 水, 负重, the armour tiers). Layout obeys §9
// (36px verb labels / 24px body, ≥80px long-press bands, 12px grid).
#include "path_page.h"
#include "cjk_text.h"
#include "pomo_page.h"          // PAD (shared layout authority)
#include "page_tabs.h"          // shared tab header (村落 lit — village sub-page)
#include "pager.h"
#include "game_state.h"
#include "world_state.h"        // WorldState::embark + bag/water caps + weightCenti
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// main.cpp owns both models.
extern adr::GameState  g_game;
extern adr::WorldState g_world;

using namespace adr;

namespace path_page {
// Visibility latch — the page is drawable (a reachable ring slot) only between
// open() and close(). Cleared on boot; the Outside 尘土之路 cell sets it. open()
// also flags s_reset so the next draw() clears the RAM-only outfit selection.
static bool s_active = false;
static bool s_reset  = false;
void open()  { s_active = true; s_reset = true; }
void close() { s_active = false; }
bool isOpen() { return s_active; }
}  // namespace path_page

namespace {
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px CJK body
constexpr int GLYPH     = 12 * SCALE;        // 24px line box
constexpr int BTN_SCALE = 3;                 // 12px grid x3 = 36px (name / verb)
constexpr int BTN_GLYPH = 12 * BTN_SCALE;    // 36px line box
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)

// ---- vertical budget (§9.4), clearing the 32px status bar (< 928). Top down:
//   tab header(0..72) · capacity row 背包剩余空间 free/cap (84) · a window of ≤6
//   item bands (120 + i*90, each 80px §9.3) with a trailing 更多 band on overflow
//   · read-only 护甲/水 row (712) · 出发 band (746) · 返回 band (836, ends 916).
// The item window (120..650 worst case) clears the 护甲/水 row (712) by 62px.
constexpr int CAP_Y      = 84;               // capacity readout (below tab header)
constexpr int BAND_TOP   = 120;              // first item band top
constexpr int BAND_H     = 80;               // long-press band (§9.3: >=80 floor)
constexpr int BAND_GAP   = 10;
constexpr int BAND_PITCH = BAND_H + BAND_GAP;    // 90
constexpr int BAND_X     = PAD;              // full-width single column
constexpr int BAND_W     = CONTENT_W;        // 492
constexpr int AWROW_Y    = 712;              // 护甲/水 read-only row
constexpr int EMBARK_TOP = 746;              // 出发 band
constexpr int RETURN_TOP = 836;              // 返回 band (bottom 916)

// Stepper geometry (AssignPage parity — the 492px band keeps the roomy v0.3.3
// stepper zone). A vertical rule sets off a 66px zone split by a horizontal rule
// into an upper ▲ (increment) over a lower ▼ (decrement); both are fillTriangle
// (▲/▼ are not in the sparse face, so never text). The press y-half, not the
// exact triangle pixels, picks the direction (see onLocalAction).
constexpr int STEP_INSET = 12;
constexpr int STEP_W     = 66;
constexpr int STEP_X0    = BAND_W - STEP_INSET - STEP_W;  // 414
constexpr int STEP_DIV_X = STEP_X0 - 4;                   // 410 — vertical rule x
constexpr int LABEL_X    = 8;                            // name left pad
constexpr int LABEL_GAP  = 8;                            // count -> divider gap
constexpr int SUB_GAP    = 16;                           // weight -> count gap
constexpr int TRI_HALF_W = 11;
constexpr int TRI_HALF_H = 9;

// The carryable rows: path.js carryable ∪ the Room.Craftables tools/weapons, in
// §1.3 order. `key` is the tr() key AND the world_data WEIGHTS lookup AND the
// store identity. Each maps onto a P1 store — a Res slot (whole units via
// g_game.whole) or an Item slot (g_game.items). Index 0 (cured meat) is the
// embark gate: it is always shown, even at zero.
struct Carry { const char* key; bool isItem; uint8_t idx; };
const Carry CARRY[] = {
    { "cured meat",  false, R_CURED_MEAT },
    { "bullets",     false, R_BULLETS },
    { "grenade",     true,  I_GRENADE },
    { "bolas",       true,  I_BOLAS },
    { "laser rifle", true,  I_LASER_RIFLE },
    { "energy cell", false, R_ENERGY_CELL },
    { "bayonet",     true,  I_BAYONET },
    { "charm",       false, R_CHARM },
    { "alien alloy", false, R_ALIEN_ALLOY },
    { "medicine",    false, R_MEDICINE },
    { "torch",       true,  I_TORCH },
    { "bone spear",  true,  I_BONE_SPEAR },
    { "iron sword",  true,  I_IRON_SWORD },
    { "steel sword", true,  I_STEEL_SWORD },
    { "rifle",       true,  I_RIFLE },
};
constexpr int CARRY_N          = (int)(sizeof(CARRY) / sizeof(CARRY[0]));
constexpr int CARRY_CURED_MEAT = 0;      // always-shown embark-gate row

int ceilDiv(int a, int b) { return (a + b - 1) / b; }

// RTC -> Unix epoch, mirroring assign_page/main.cpp's epochNow.
uint32_t epochNow() {
    m5::rtc_date_t d; m5::rtc_time_t t;
    M5.Rtc.getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}

// centi-unit weight -> compact decimal ("10", "19.9", "0.1", "0.5"). Weights are
// multiples of 0.1 (world_data WEIGHTS), so at most one significant decimal, but
// the 2-decimal branch is kept exact for safety.
void fmtWeight(int centi, char* out, size_t cap) {
    if (centi < 0) centi = 0;
    int whole = centi / 100, frac = centi % 100;
    if (frac == 0)          snprintf(out, cap, "%d", whole);
    else if (frac % 10 == 0) snprintf(out, cap, "%d.%d", whole, frac / 10);
    else                    snprintf(out, cap, "%d.%02d", whole, frac);
}

// Splice two args into a "...{0}...{1}..." template (the game's own placeholder
// form — see room_page fmt1). Used for tr("free {0}/{1}").
void fmt2(char* out, size_t cap, const char* tmpl, const char* a0, const char* a1) {
    char tmp[96];
    const char* h0 = strstr(tmpl, "{0}");
    if (!h0) { snprintf(out, cap, "%s", tmpl); return; }
    snprintf(tmp, sizeof tmp, "%.*s%s%s", (int)(h0 - tmpl), tmpl, a0, h0 + 3);
    const char* h1 = strstr(tmp, "{1}");
    if (!h1) { snprintf(out, cap, "%s", tmp); return; }
    snprintf(out, cap, "%.*s%s%s", (int)(h1 - tmp), tmp, a1, h1 + 3);
}

// 1px dashed rect, 4px-on/4px-off — the disabled-band frame (Outside/AssignPage
// drawDashedRect parity).
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

// The ▲/▼ stepper zone at band `top` (AssignPage drawJobBand parity).
void drawStepper(m5gfx::M5Canvas& c, int top) {
    int midY = top + BAND_H / 2;
    c.drawFastVLine(BAND_X + STEP_DIV_X, top + 10, BAND_H - 20, TFT_BLACK);
    c.drawFastHLine(BAND_X + STEP_X0, midY, STEP_W, TFT_BLACK);
    int cx   = BAND_X + STEP_X0 + STEP_W / 2;
    int cyUp = top + BAND_H / 4;
    int cyDn = top + 3 * BAND_H / 4;
    c.fillTriangle(cx, cyUp - TRI_HALF_H,               // ▲ increment (apex up)
                   cx - TRI_HALF_W, cyUp + TRI_HALF_H,
                   cx + TRI_HALF_W, cyUp + TRI_HALF_H, TFT_BLACK);
    c.fillTriangle(cx, cyDn + TRI_HALF_H,               // ▼ decrement (apex down)
                   cx - TRI_HALF_W, cyDn - TRI_HALF_H,
                   cx + TRI_HALF_W, cyDn - TRI_HALF_H, TFT_BLACK);
}

// One item band: the 36px name, then (right-aligned before the stepper divider) a
// 24px 负重 W and a 24px carried "xN", then the ▲/▼ stepper. Disabled (dashed) only
// when the row can neither add nor remove — the cured-meat-with-no-meat case.
void drawItemBand(m5gfx::M5Canvas& c, int top, const char* name, int carried,
                  int weightCentiVal, bool disabled) {
    if (disabled) {
        drawDashedRect(c, BAND_X, top, BAND_W, BAND_H);
    } else {
        c.drawRect(BAND_X, top, BAND_W, BAND_H, TFT_BLACK);
        c.drawRect(BAND_X + 1, top + 1, BAND_W - 2, BAND_H - 2, TFT_BLACK);
    }

    int ny   = top + (BAND_H - BTN_GLYPH) / 2 - 4;      // 36px name box top
    int subY = ny + BTN_GLYPH - GLYPH;                  // 24px sub bottom-aligned
    cjk::drawText(c, BAND_X + LABEL_X, ny, name, BTN_SCALE);

    char cnt[12];
    snprintf(cnt, sizeof cnt, "x%d", carried);
    int cw   = cjk::textWidth(cnt, SCALE);
    int cntX = BAND_X + STEP_DIV_X - LABEL_GAP - cw;
    cjk::drawText(c, cntX, subY, cnt, SCALE);

    char wv[8], ws[24];
    fmtWeight(weightCentiVal, wv, sizeof wv);
    snprintf(ws, sizeof ws, "%s %s", tr("weight"), wv);        // "负重 5"
    int ww = cjk::textWidth(ws, SCALE);
    cjk::drawText(c, cntX - SUB_GAP - ww, subY, ws, SCALE);

    drawStepper(c, top);
}

// A full-width single-action band (更多 / 出发 / 返回): enabled = 2px double frame,
// disabled = 1px dashed frame; a lone 36px label centered.
void drawWideBand(m5gfx::M5Canvas& c, int top, const char* label, bool enabled) {
    if (enabled) {
        c.drawRect(BAND_X, top, BAND_W, BAND_H, TFT_BLACK);
        c.drawRect(BAND_X + 1, top + 1, BAND_W - 2, BAND_H - 2, TFT_BLACK);
    } else {
        drawDashedRect(c, BAND_X, top, BAND_W, BAND_H);
    }
    int lw = cjk::textWidth(label, BTN_SCALE);
    cjk::drawText(c, BAND_X + (BAND_W - lw) / 2, top + (BAND_H - BTN_GLYPH) / 2 - 4,
                  label, BTN_SCALE);
}

// The armour tier word (world.js getMaxHealth ladder), highest owned wins. kinetic
// is P3 (not craftable in P1), so the top P1 tier is steel.
const char* armourWord() {
    if (g_game.items[I_S_ARMOUR] > 0) return "steel";
    if (g_game.items[I_I_ARMOUR] > 0) return "iron";
    if (g_game.items[I_L_ARMOUR] > 0) return "leather";
    return "none";
}
}  // namespace

// ---- outfit / capacity helpers -------------------------------------------

int PathPage::ownedOf(int i) const {
    const Carry& c = CARRY[i];
    return c.isItem ? (int)g_game.items[c.idx] : (int)g_game.whole(c.idx);
}
int PathPage::carriedOf(int i) const {
    const Carry& c = CARRY[i];
    return c.isItem ? (int)m_outfitItem[c.idx] : (int)m_outfitRes[c.idx];
}
int PathPage::freeCenti() const {
    int used = 0;
    for (int i = 0; i < CARRY_N; i++)
        used += carriedOf(i) * weightCenti(CARRY[i].key);
    return WorldState::bagCapacityCenti(g_game) - used;
}

// path.js increaseSupply / decreaseSupply, ±1: add clamps to the village stock
// AND the free bag space; remove clamps at 0. Returns whether anything changed.
bool PathPage::adjustOutfit(int i, int delta) {
    const Carry& c = CARRY[i];
    int cur = carriedOf(i);
    if (delta > 0) {
        if (cur >= ownedOf(i)) return false;              // can't carry more than owned
        if (freeCenti() < weightCenti(c.key)) return false;   // no room
        cur++;
    } else {
        if (cur <= 0) return false;
        cur--;
    }
    if (c.isItem) m_outfitItem[c.idx] = (int16_t)cur;
    else          m_outfitRes[c.idx]  = (int16_t)cur;
    return true;
}

// The carryable rows to show: owned > 0 OR already carried > 0; cured meat always
// (the embark gate must stay visible even at zero). Writes CARRY indices to `out`.
int PathPage::buildOutfitList(uint8_t* out) const {
    int n = 0;
    for (int i = 0; i < CARRY_N; i++)
        if (i == CARRY_CURED_MEAT || ownedOf(i) > 0 || carriedOf(i) > 0)
            out[n++] = (uint8_t)i;
    return n;
}

// ================================ Page API =================================

const pages::Region* PathPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Gated on the latch AND holding a compass (§1.6: the compass, bought at the
// trading post, is what discovers the Path at all). One predicate shared by draw()
// and the status bar's page-dot count so the two can't disagree — an un-opened or
// compass-less Path is a skipped ring slot, exactly like the closed AssignPage.
bool PathPage::available() const {
    return path_page::isOpen() && g_game.whole(R_COMPASS) > 0;
}

bool PathPage::draw(m5gfx::M5Canvas& c) {
    if (!available()) return false;
    if (path_page::s_reset) {                 // fresh open -> clear the selection
        memset(m_outfitRes, 0, sizeof m_outfitRes);
        memset(m_outfitItem, 0, sizeof m_outfitItem);
        m_page = 0;
        path_page::s_reset = false;
    }
    c.fillSprite(TFT_WHITE);
    page_tabs::draw(c, 1);        // shared tab header, 村落 lit (village sub-page)

    // Capacity row: 背包剩余空间 free/capacity (both centi -> compact decimal).
    char fv[8], cv[8], cap[64];
    fmtWeight(freeCenti(), fv, sizeof fv);
    fmtWeight(WorldState::bagCapacityCenti(g_game), cv, sizeof cv);
    fmt2(cap, sizeof cap, tr("free {0}/{1}"), fv, cv);
    cjk::drawText(c, PAD, CAP_Y, cap, SCALE);

    // Paginate the carryable rows into the ≤6-band window. Overflow reserves the
    // last slot for a 更多 band (5 rows/page); otherwise every row shows.
    uint8_t qual[CARRY_N];
    int nQ = buildOutfitList(qual);
    m_hasPager = nQ > WIN_SLOTS;
    int perPage = m_hasPager ? (WIN_SLOTS - 1) : nQ;
    if (perPage < 1) perPage = 1;
    m_pageCount = m_hasPager ? ceilDiv(nQ, perPage) : 1;
    if (m_page >= m_pageCount || m_page < 0) m_page = 0;   // owned set can shrink
    int start = m_page * perPage;
    m_slotCount = nQ - start;
    if (m_slotCount > perPage) m_slotCount = perPage;

    m_regionCount = 0;
    for (int s = 0; s < m_slotCount; s++) {
        int carryIdx = qual[start + s];
        int top = BAND_TOP + s * BAND_PITCH;
        int owned = ownedOf(carryIdx), carried = carriedOf(carryIdx);
        drawItemBand(c, top, tr(CARRY[carryIdx].key), carried,
                     weightCenti(CARRY[carryIdx].key),
                     owned == 0 && carried == 0);        // dashed = nothing to pack
        m_slotCarry[s] = (uint8_t)carryIdx;
        m_regions[m_regionCount].y0    = (uint16_t)top;
        m_regions[m_regionCount].y1    = (uint16_t)(top + BAND_H);
        m_regions[m_regionCount].type  = 1;
        m_regions[m_regionCount].param = (uint8_t)s;
        m_regionCount++;
    }
    if (m_hasPager) {
        int top = BAND_TOP + m_slotCount * BAND_PITCH;   // 更多 after the last row
        drawWideBand(c, top, "更多", true);              // closure-safe (Outside 手法)
        m_regions[m_regionCount].y0    = (uint16_t)top;
        m_regions[m_regionCount].y1    = (uint16_t)(top + BAND_H);
        m_regions[m_regionCount].type  = 1;
        m_regions[m_regionCount].param = PARAM_PAGER;
        m_regionCount++;
    }

    // Read-only 护甲 tier + 水 capacity row (armour -> max HP, water upgrade -> max
    // water; both applied at embark, neither occupies the bag).
    char arm[32];
    snprintf(arm, sizeof arm, "%s %s", tr("armour"), tr(armourWord()));
    cjk::drawText(c, PAD, AWROW_Y, arm, SCALE);
    char wat[32];
    snprintf(wat, sizeof wat, "%s %d", tr("water"), WorldState::maxWater(g_game));
    int wtW = cjk::textWidth(wat, SCALE);
    cjk::drawText(c, 540 - PAD - wtW, AWROW_Y, wat, SCALE);

    // 出发 band — dashed-disabled until a cured meat is packed (the sole gate, §1.5).
    bool canEmbark = carriedOf(CARRY_CURED_MEAT) > 0;
    drawWideBand(c, EMBARK_TOP, tr("embark"), canEmbark);
    m_regions[m_regionCount].y0    = (uint16_t)EMBARK_TOP;
    m_regions[m_regionCount].y1    = (uint16_t)(EMBARK_TOP + BAND_H);
    m_regions[m_regionCount].type  = 1;
    m_regions[m_regionCount].param = PARAM_EMBARK;
    m_regionCount++;

    // 返回 band — tr("go home") == "返回" (real translation, not a bare literal).
    drawWideBand(c, RETURN_TOP, tr("go home"), true);
    m_regions[m_regionCount].y0    = (uint16_t)RETURN_TOP;
    m_regions[m_regionCount].y1    = (uint16_t)(RETURN_TOP + BAND_H);
    m_regions[m_regionCount].type  = 1;
    m_regions[m_regionCount].param = PARAM_RETURN;
    m_regionCount++;
    return true;
}

// Press-flash target, mirroring onLocalAction's own decision (like AssignPage):
// the three wide bands flash their whole drawn frame; an item band flashes just the
// ▲/▼ stepper half the y picks, so the flash reads as "this stepper half fired"
// rather than blacking out the row.
pages::Rect PathPage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)x;
    if (rg.param == PARAM_PAGER || rg.param == PARAM_EMBARK ||
        rg.param == PARAM_RETURN)
        return pages::Rect{ BAND_X, rg.y0, BAND_W, BAND_H };

    int stepX0 = BAND_X + STEP_DIV_X + 1;
    int stepW  = BAND_W - STEP_DIV_X - 1;
    int half   = BAND_H / 2;
    if (y < rg.y0 + half) return pages::Rect{ stepX0, rg.y0, stepW, half };
    return pages::Rect{ stepX0, rg.y0 + half, stepW, BAND_H - half };
}

// Long-press on a band. 返回 closes the latch + jumps to Outside; 更多 advances the
// page; 出发 embarks (gate-checked); an item band packs/unpacks one unit, the press
// y-half picking direction (upper ▲ = +1, lower ▼ = −1). A real change high-beeps +
// repaints; a no-op low-beeps.
void PathPage::onLocalAction(uint8_t param, int x, int y) {
    (void)x;
    if (param == PARAM_RETURN) {
        path_page::close();
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::ringIndexByName("outside"), false);
        return;                                  // navigates away — no tick to double up
    }
    if (param == PARAM_PAGER) {
        if (m_pageCount > 1) {
            m_page = (m_page + 1) % m_pageCount;
            M5.Speaker.tone(1800, 80);
            pager::showPage(pager::currentRingIndex(), false);
            m_lastSig = contentSig();
        } else {
            M5.Speaker.tone(600, 120);
        }
        return;
    }
    if (param == PARAM_EMBARK) { doEmbark(); return; }

    if ((int)param >= m_slotCount) { M5.Speaker.tone(600, 120); return; }   // stale
    int carryIdx = m_slotCarry[param];
    int bandTop  = BAND_TOP + (int)param * BAND_PITCH;
    int delta    = (y < bandTop + BAND_H / 2) ? +1 : -1;   // upper ▲ / lower ▼
    if (adjustOutfit(carryIdx, delta)) {
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::currentRingIndex(), false);
        m_lastSig = contentSig();                // AssignPage re-baseline (no double redraw)
    } else {
        M5.Speaker.tone(600, 120);               // at 0, at stock, or bag full
    }
}

// path.js embark: deduct the outfit from the stores, fill hp/water from equipment,
// snapshot the map into a volatile trek, write trek.bin — then jump to the World
// page. The committed map is generated lazily on the first-ever embark (2.2 wires
// the boot restore that normally loads it).
void PathPage::doEmbark() {
    if (carriedOf(CARRY_CURED_MEAT) <= 0) {      // the one hard gate (§1.5)
        M5.Speaker.tone(600, 120);
        return;
    }
    if (!g_world.generated) {                    // first embark: make + save a map
        uint32_t seed = epochNow();
        if (!seed) seed = g_game.rng ? g_game.rng : 0x9e3779b9u;
        g_world.ensureGenerated(seed);
    }
    if (!g_world.embark(g_game, m_outfitRes, m_outfitItem, g_game.nextRand())) {
        M5.Speaker.tone(600, 120);               // defensive: gate + generated ensured above
        return;
    }
    g_game.save();                               // persist the deducted stores
    memset(m_outfitRes, 0, sizeof m_outfitRes);  // selection consumed into the trek
    memset(m_outfitItem, 0, sizeof m_outfitItem);
    m_page = 0;

    // TEMPORARY (milestone 2.1): the World page (2.2) is not registered yet. When
    // it lands, this jump navigates there; until then, low-beep and stay so the
    // interrupted expedition (now in trek.bin) is picked up by 2.2's boot restore.
    // This is the ONE allowed placeholder behaviour — 2.2 takes over the jump.
    int world = pager::ringIndexByName("world");
    if (world >= 0) {
        path_page::close();
        M5.Speaker.tone(1800, 80);
        pager::showPage(world, false);
    } else {
        M5.Speaker.tone(600, 120);
        pager::showPage(pager::currentRingIndex(), false);   // repaint post-embark state
        m_lastSig = contentSig();
    }
}

// Content signature — the packing-relevant live state (village stock of every
// carryable + the bag/water/armour/compass upgrades) plus the RAM-only selection
// and page. tick() repaints on any change; onLocalAction re-baselines it after its
// own showPage so a pack no longer forces a SECOND full redraw next tick.
uint32_t PathPage::contentSig() const {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    for (int i = 0; i < RES_COUNT; i++)  mix((uint32_t)g_game.whole((uint8_t)i));
    for (int i = 0; i < ITEM_COUNT; i++) mix((uint32_t)g_game.items[i]);
    for (int i = 0; i < RES_COUNT; i++)  mix((uint32_t)(uint16_t)m_outfitRes[i]);
    for (int i = 0; i < ITEM_COUNT; i++) mix((uint32_t)(uint16_t)m_outfitItem[i]);
    mix((uint32_t)m_page);
    return sig;
}

// Time axis (awake + current). Settle the offline economy each second (cured meat
// can accrue while you pack), then repaint on any change to a painted value.
// onLocalAction re-baselines m_lastSig after its showPage, so a pack/pager press no
// longer forces a second full redraw here. No cooldowns live on this page.
void PathPage::tick(uint32_t nowMs) {
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
