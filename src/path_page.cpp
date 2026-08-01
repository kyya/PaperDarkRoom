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
#include "action_band.h"        // shared band frame + title baseline + wide bands
#include "stepper.h"            // shared ±1 / ±10 stepper zone (AssignPage parity)
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
// also flags s_reset so the next draw() re-seeds the outfit from savedOutfit.
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

// Stepper: the shared two-column ±1 / ±10 zone (see stepper.h — AssignPage's job
// rows draw the identical control). v0.14 grew it from one column to two, moving
// the zone's left rule from 410 to 344, so BOTH right-aligned sub-values below
// re-anchor to stepper::dividerX(). Re-measured after the shift: the tightest
// row is 能量元件 (144px name @36px) with "负重 0.2" (92px) + "x999" (48px),
// which still leaves 28px of clear space between the name and the weight.
constexpr int LABEL_X    = 8;                            // name left pad
constexpr int LABEL_GAP  = 8;                            // count -> divider gap
constexpr int SUB_GAP    = 16;                           // weight -> count gap
constexpr int STEP_DIV_X = stepper::dividerX(BAND_W);    // 344 — zone's left rule

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

// Seconds left on the post-death embark lockout (World.DEATH_COOLDOWN_S, §1.5/
// §3.4). Epoch-based so a deep sleep past the window expires it; a clock at/
// behind the death epoch (incl. a 0 no-RTC read) reports 0 — fail-open, never a
// permanent lockout. Mirrors WorldState::embark's own guard so the two agree.
int deathCooldownLeft() {
    if (!g_game.deathAt) return 0;
    uint32_t now = epochNow();
    if (now < g_game.deathAt) return 0;            // clock behind death / no RTC -> fail open
    uint32_t el = now - g_game.deathAt;
    return el < (uint32_t)DEATH_COOLDOWN_S ? (int)((uint32_t)DEATH_COOLDOWN_S - el) : 0;
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

// The rect of the band at `top` — one description of where a band is, shared by
// every draw call here and by pressRect, so the drawn frame and the
// invert-flash rect cannot drift apart.
pages::Rect bandRect(int top) {
    return pages::Rect{ BAND_X, top, BAND_W, BAND_H };
}

// One item band: the 36px name, then (right-aligned before the stepper divider) a
// 24px 负重 W and a 24px carried "xN", then the shared ±1 / ±10 stepper. Disabled
// (dashed) only when the row can neither add nor remove — the cured-meat-with-
// no-meat case. The frame and the name's baseline come from the shared
// action_band and the stepper from stepper.h; what stays here is only this
// page's own LEFT side, which puts a name and its two sub-values SIDE BY SIDE on
// one baseline instead of stacking a subtitle under a title — a genuinely
// different layout, not a drifted copy of the standard band.
void drawItemBand(m5gfx::M5Canvas& c, int top, const char* name, int carried,
                  int weightCentiVal, bool disabled) {
    action_band::drawFrame(c, bandRect(top), !disabled);

    int ny   = action_band::titleBoxY(top, BAND_H);     // 36px name box top
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

    stepper::draw(c, bandRect(top));
}

// A full-width single-action band (更多 / 出发 / 返回) — the plain shared band:
// enabled = 2px double frame, disabled = 1px dashed frame, a lone 36px label
// centered — none of the three ever carries a subtitle, so the title simply
// centres in the band. 出发's countdown ("出发 118s") is baked into the label by
// the caller rather than passed as a subtitle: it is the same action being
// counted down, not a price, and routing it through the cost column would push it
// to the band's right edge in a small 24px face instead of reading as part of the
// verb.
void drawWideBand(m5gfx::M5Canvas& c, int top, const char* label, bool enabled) {
    action_band::draw(c, bandRect(top), label, nullptr, enabled, 0, 0);
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

// path.js increaseSupply / decreaseSupply, ONE unit: add clamps to the village
// stock AND the free bag space; remove clamps at 0. Returns whether it moved.
bool PathPage::adjustOutfitOne(int i, int step) {
    const Carry& c = CARRY[i];
    int cur = carriedOf(i);
    if (step > 0) {
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

// The stepper's request: `delta` is the SIGNED unit count the pressed zone asks
// for (±1 from the fine column, ±stepper::MANY from the coarse one). The move is
// TRUNCATED to whatever is actually available, matching upstream's
// Math.min(available, btn.data) (path.js:260-263 arms the ±10 buttons,
// outside.js:376 does the truncating) — a coarse ▲ tap with 3 in stock packs 3, it
// does not refuse.
//
// Implemented by repeating the single-unit step rather than computing a count up
// front, because a row has TWO independent limits (village stock and remaining
// bag weight) and only the per-unit path re-checks both as the bag fills: asking
// for 10 rifles with 8 owned but room for 4 must stop at 4, and freeCenti()
// changes with every unit added. That reuses the existing clamps verbatim
// instead of duplicating them into a second min() that could drift. 10 iterations
// of an O(CARRY_N) weight sum is nothing on a button press.
// Returns whether ANY unit moved, so the caller's beep/repaint logic is unchanged.
bool PathPage::adjustOutfit(int i, int delta) {
    int want = delta < 0 ? -delta : delta;
    int step = delta < 0 ? -1 : +1;
    int moved = 0;
    for (int k = 0; k < want; k++) {
        if (!adjustOutfitOne(i, step)) break;   // hit stock / weight / zero floor
        moved++;
    }
    return moved > 0;
}

// Seed the selection from the persistent remembered outfit (g_game.savedOutfit,
// written by world_state goHome's leaveItAtHome nicety) so a returning wanderer
// re-embarks with last trip's loadout instead of re-packing from zero. Two clamps
// in CARRY display order (cured meat + ammo first, weapons last — the survival-
// critical rows win a shrunk bag):
//   1. STOCK: min(remembered, current village stock) — stores may have been spent
//      since goHome (miners eat cured meat, crafting spends bullets).
//   2. CAPACITY: floor(freeSpace/weight) — capacity rarely shrinks (bag upgrades
//      aren't consumed), but the stock clamp above means a row may still not fit;
//      freeCenti() drains as earlier rows fill, so each row takes what's left.
// Old saves / a fresh game leave savedOutfit zeroed, so this seeds nothing — the
// pre-0.9 "empty on open" behaviour. (Death deliberately KEEPS the memory — see
// game_state.h savedOutfit — so a returning-from-death trip still pre-fills.)
void PathPage::prefillOutfit() {
    memset(m_outfitRes, 0, sizeof m_outfitRes);
    memset(m_outfitItem, 0, sizeof m_outfitItem);
    for (int i = 0; i < CARRY_N; i++) {
        const Carry& c = CARRY[i];
        int want = c.isItem ? (int)g_game.savedOutfitItem[c.idx]
                            : (int)g_game.savedOutfitRes[c.idx];
        if (want <= 0) continue;
        int stock = ownedOf(i);
        if (want > stock) want = stock;                    // (1) stock clamp
        int wc = weightCenti(c.key);
        if (wc > 0) { int fits = freeCenti() / wc; if (want > fits) want = fits; }  // (2)
        if (want <= 0) continue;
        if (c.isItem) m_outfitItem[c.idx] = (int16_t)want;
        else          m_outfitRes[c.idx]  = (int16_t)want;
    }
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
    if (path_page::s_reset) {                 // fresh open -> seed from savedOutfit
        prefillOutfit();
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

    // 出发 band — dashed-disabled until a cured meat is packed (the sole gate,
    // §1.5) AND the post-death lockout has elapsed (§3.4). While cooling, the label
    // carries the remaining seconds ("出发 118s").
    int cdLeft = deathCooldownLeft();
    bool canEmbark = carriedOf(CARRY_CURED_MEAT) > 0 && cdLeft == 0;
    char embarkLbl[32];
    if (cdLeft > 0) snprintf(embarkLbl, sizeof embarkLbl, "%s %ds", tr("embark"), cdLeft);
    else            snprintf(embarkLbl, sizeof embarkLbl, "%s", tr("embark"));
    drawWideBand(c, EMBARK_TOP, embarkLbl, canEmbark);
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
    if (rg.param == PARAM_PAGER || rg.param == PARAM_EMBARK ||
        rg.param == PARAM_RETURN)
        return bandRect(rg.y0);                 // the exact rect drawWideBand framed

    return stepper::zoneRect(bandRect(rg.y0), x, y);
}

// Long-press on a band. 返回 closes the latch + jumps to Outside; 更多 advances the
// page; 出发 embarks (gate-checked); an item band packs/unpacks units, the press
// picking BOTH size and direction via stepper::deltaFor — ±1 from the fine column
// (or the name area) and ±10 from the coarse one, truncated to stock/bag space by
// adjustOutfit. A real change high-beeps + repaints; a total no-op low-beeps
// (both magnitudes are equally dead in a direction that cannot move at all).
void PathPage::onLocalAction(uint8_t param, int x, int y) {
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
    int delta    = stepper::deltaFor(bandRect(bandTop), x, y);
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
    if (carriedOf(CARRY_CURED_MEAT) <= 0 || deathCooldownLeft() > 0) {
        M5.Speaker.tone(600, 120);               // no meat packed, or still cooling (§3.4)
        return;
    }
    if (!g_world.generated) {                    // first embark: make + save a map
        uint32_t seed = epochNow();
        if (!seed) seed = g_game.rng ? g_game.rng : 0x9e3779b9u;
        g_world.ensureGenerated(seed);
    }
    if (!g_world.embark(g_game, m_outfitRes, m_outfitItem, g_game.nextRand(),
                        epochNow())) {
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
    mix((uint32_t)deathCooldownLeft());   // ticks the embark countdown while cooling
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
