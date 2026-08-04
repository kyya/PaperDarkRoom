// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Tech-tree (科技树) page — see tech_page.h for the why and the layout budget.
// Every item name comes from tr(CRAFT[id].key) and every price from that same
// CRAFT row, so only the official Simplified-Chinese translation reaches the
// sparse 12px CJK face — the §8.3 glyph-closure iron law. The handful of
// firmware-only literals here reuse glyphs already IN that closure, exactly the
// way the Outside page's 分工/建筑/工人 do: 拳头 (拳击手 / 木头), 水容器 (水桶 /
// 毁容 / 武器), 携带 (携带更多物资) and the footnote's 占领的据点会变成前哨站，
// 可补满水 (占住 / 首领 / 据点 / 供给点 / 会 / 变得 / 完成 / 前哨 / 站着 / 可疑 /
// 补给 / 覆满 / 水桶), plus the two paging labels 建筑 (建筑物 / 建造者, the same
// route the Outside page's 建筑 legend takes) and 装备 (装备精良). Section
// headings 武器/护甲 and the 返回 label are real tr() values, not literals. All
// ten building names on page 1 and every resource in their prices are tr()
// values too, so that page adds nothing to the closure beyond 建筑 itself.
// Layout obeys §9 (24px body, >=80px long-press band).
#include "tech_page.h"
#include "action_band.h"        // the app-wide button band (the 返回 band)
#include "cjk_text.h"
#include "pomo_page.h"          // PAD (shared layout authority)
#include "page_tabs.h"          // shared tab header (生火间 │ 村落 │ 贸易站)
#include "pager.h"
#include "game_state.h"
#include "world_state.h"        // g_world.ex.outfitItem — the packed-for-a-trek copy
#include <M5Unified.h>
#include <stdio.h>

// main.cpp owns both models.
extern adr::GameState  g_game;
extern adr::WorldState g_world;

using namespace adr;

namespace tech_page {
// Visibility latch — the page is drawable (and thus a reachable ring slot) only
// between open() and close(). Cleared on boot; the Room 科技树 cell sets it.
static bool s_active = false;
// Which of the two static pages is showing: 0 = 装备 (the four equipment
// ladders), 1 = 建筑 (the village build order). File-static rather than a
// TechPage member because open() — a free function — is what rewinds it, so
// every entry through the Outside 科技树 cell lands on 装备 no matter which page
// the player left on.
static uint8_t s_page = 0;
void open()  { s_active = true; s_page = 0; }
void close() { s_active = false; }
bool isOpen() { return s_active; }
}  // namespace tech_page

namespace {
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px CJK
constexpr int GLYPH     = 12 * SCALE;        // 24px line box
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)
// The 返回 label's 36px scale comes from action_band's contract (v0.12), not a
// local constant — the entry rows below are read-only text, not buttons, and
// keep their own 24px SCALE.

// ---- vertical budget (§9.4), clearing the 32px status bar (< 928). Top ->
// bottom: tab header(0..72) · sections from SEC_TOP, each a HEAD_H heading
// block plus one ROWH row per entry, sections SEC_GAP apart · on page 0 the
// footnote just above the band · the band row. Page 0's four lines carry
// 5+3+3+3 = 14 rows and end at 84 + 4*(32+12) + 14*30 = 680; page 1's single
// 建筑 section carries 10 rows and ends at 84 + 32 + 10*30 = 416. Both clear the
// 836 band by a wide margin, so neither needs 更多 pagination (and both entry
// sets are fixed tables, so neither can grow at runtime).
constexpr int SEC_TOP   = 84;                // first section heading top
constexpr int HEAD_H    = GLYPH + 8;         // 32 — heading line + clearance
constexpr int ROWH      = 30;                // one entry row
constexpr int SEC_GAP   = 12;                // gap below a section's last row
constexpr int BAND_H    = 80;                // long-press band (§9.3: >=80px floor)
constexpr int BAND_TOP  = 916 - BAND_H;      // 836 — bottom-anchored band row
// Two side-by-side bands in the SAME geometry the Room/Outside action grids use
// (240px columns, 12px gutter, filling the 492px content width) — the paging
// band on the left, 返回 on the right. Both labels are lone 36px titles (72px
// wide), so each centres comfortably in its 240px box.
constexpr int BAND_GAP  = 12;
constexpr int BAND_W    = (CONTENT_W - BAND_GAP) / 2;              // 240
constexpr int BAND_X0[2] = { PAD, PAD + BAND_W + BAND_GAP };       // {24, 276}
constexpr int BAND_MID  = 540 / 2;           // 270: x < MID => the paging band
constexpr int NOTE_Y    = BAND_TOP - 20 - GLYPH;   // 792 — footnote line top

// Owned marker: a 16px box at the row's left, FILLED when the entry is held and
// hollow when it is not. Drawn geometrically (fillRect/drawRect) rather than as
// a ✓/● glyph for the same reason assign_page draws its ▲/▼ with fillTriangle —
// those marks are not in the sparse face, so they must never be drawn as text.
constexpr int MARK      = 16;
constexpr int MARK_GAP  = 10;                // marker -> name gap
constexpr int NAME_X    = PAD + MARK + MARK_GAP;   // 50

// A growth line's entries, weakest first, as CRAFT ids — the ONE cost/name
// source (game_data.h). CRAFT_NONE marks 拳头: the unarmed baseline every game
// starts with, which upstream models as a World weapon (combat_data.h
// WEAPONS[0] "fists") rather than a craftable, so it has no price and is always
// held. It opens the weapon line because "拳头 is where you start" is exactly
// the context that makes 骨枪 read as a first rung rather than a ceiling.
constexpr uint8_t CRAFT_NONE = 0xFF;
const uint8_t WEAPON_IDS[] = { CRAFT_NONE, C_BONE_SPEAR, C_IRON_SWORD,
                               C_STEEL_SWORD, C_RIFLE };
const uint8_t ARMOUR_IDS[] = { C_L_ARMOUR, C_I_ARMOUR, C_S_ARMOUR };
const uint8_t WATER_IDS[]  = { C_WATERSKIN, C_CASK, C_WATER_TANK };
const uint8_t CARRY_IDS[]  = { C_RUCKSACK, C_WAGON, C_CONVOY };

struct Line { const uint8_t* ids; int n; };
const Line LINES[] = {
    { WEAPON_IDS, (int)(sizeof(WEAPON_IDS) / sizeof(WEAPON_IDS[0])) },
    { ARMOUR_IDS, (int)(sizeof(ARMOUR_IDS) / sizeof(ARMOUR_IDS[0])) },
    { WATER_IDS,  (int)(sizeof(WATER_IDS)  / sizeof(WATER_IDS[0]))  },
    { CARRY_IDS,  (int)(sizeof(CARRY_IDS)  / sizeof(CARRY_IDS[0]))  },
};
constexpr int LINE_COUNT = (int)(sizeof(LINES) / sizeof(LINES[0]));

// Page 1's one line: the village build order, in CRAFT-table order — which IS
// the upstream unlock order (room.js reveals a building once the previous one's
// cost is nearly affordable), so reading it top to bottom is reading the actual
// progression from 陷阱 to 军械坊. Craft ids 0..9 are exactly the craftable
// buildings (craftIsBuilding / CRAFT_BLD_COUNT); the non-craftable World mines
// that extend the Bld enum are deliberately absent — you find those, you don't
// build them.
const uint8_t BUILDING_IDS[] = { C_TRAP, C_CART, C_HUT, C_LODGE, C_TRADING_POST,
                                 C_TANNERY, C_SMOKEHOUSE, C_WORKSHOP,
                                 C_STEELWORKS, C_ARMOURY };
constexpr int BUILDING_COUNT = (int)(sizeof(BUILDING_IDS) / sizeof(BUILDING_IDS[0]));
static_assert(BUILDING_COUNT == CRAFT_BLD_COUNT, "building ladder must list every craftable building");

// The footnote (§8.3 closure-safe literal, see the file header): occupied
// landmarks turn into outposts, and an outpost refills the canteen
// (setpieces_data.h SPE_FILL_WATER). This is the single most-missed survival
// trick behind the "渴死在路上" reports, which is why it rides along on the page
// that explains the water-container ladder. 384px wide at 24px — one line.
const char* const NOTE = "占领的据点会变成前哨站，可补满水";

// Section heading. 武器/护甲 have official upstream keys; 水容器 and 携带 never
// entered the translation table (upstream labels those rows differently), so
// they use closure-safe literals — the same sanctioned deviation as 分工.
const char* lineTitle(int i) {
    switch (i) {
        case 0:  return tr("weapons");   // 武器
        case 1:  return tr("armour");    // 护甲
        case 2:  return "水容器";
        default: return "携带";
    }
}

// An entry's display name: the official craftable name, or 拳头 for the unarmed
// baseline (no CRAFT row, and upstream's own "fists" key carries no zh value —
// tr() would fall back to the English key).
const char* entryName(uint8_t id) {
    return id == CRAFT_NONE ? "拳头" : tr(CRAFT[id].key);
}

// Held already? A building entry's count slot is buildings[] and an item's is
// items[] (craftSlot maps either), and one >0 test covers every kind: the
// repeatable weapons and 陷阱/小屋 (maximum -1 / 10 / 20 — "held" means at least
// one standing), and the one-shot upgrades and buildings (maximum 1 — count >= 1
// is the same test). 拳头 is always held.
//   The village stock alone is NOT the whole answer while a trek is live: embark
// MOVES the packed units out of items[] into ex.outfitItem[] (world_state.cpp),
// and the Room — hence this sub-page — stays reachable mid-expedition. Packing
// the last bone spear would otherwise hollow its marker while it is in hand, so
// the carried copy counts too. Only the weapon rows can actually take that path
// (path_page's CARRY table packs weapons/consumables; armour, water containers
// and the carry upgrades are never packed, so their items[] count never leaves
// the village), but the outfit slot is the same items[] index for every row, so
// one test covers the whole table. goHome banks the bag back and die() empties
// it along with ex.active, so both endings settle on the items[] test again.
bool held(uint8_t id) {
    if (id == CRAFT_NONE) return true;
    uint8_t slot = craftSlot(id);
    if (craftIsBuilding(id)) return g_game.buildings[slot] > 0;   // never packed
    if (g_game.items[slot] > 0) return true;
    return g_world.ex.active && g_world.ex.outfitItem[slot] > 0;
}

// An entry's price, e.g. "-500 木头  -100 铁" — the same "-amount name"
// convention (and two-space join) the Room's cost sub-rows, trade_page and the
// modals already use, so this reads as the price the Room will actually charge.
// The wood entry folds in the count-scaling surcharge exactly as room_page's own
// craftCostLine does (room.js woodIncrPerN), so the 陷阱/小屋 rows on page 1 show
// what the NEXT one costs rather than a base price the Room stopped charging
// after the first. The equipment ladders are unaffected — every tool/upgrade/
// weapon carries woodIncrPerN == 0, so the fold is a no-op there.
// Empty for 拳头 (no CRAFT row, nothing to pay).
void costLine(uint8_t id, char* out, size_t cap) {
    out[0] = 0;
    if (id == CRAFT_NONE) return;
    const Craftable& c = CRAFT[id];
    uint8_t slot = craftSlot(id);
    int count = craftIsBuilding(id) ? g_game.buildings[slot] : g_game.items[slot];
    size_t used = 0;
    for (int i = 0; i < 3 && c.cost[i].res != RA_END; i++) {
        int32_t amt = c.cost[i].amt;
        if (c.cost[i].res == R_WOOD) amt += (int32_t)c.woodIncrPerN * count;
        int wrote = snprintf(out + used, cap - used, "%s-%ld %s",
                             used ? "  " : "", (long)amt,
                             tr(RES_KEY[c.cost[i].res]));
        if (wrote < 0) break;
        used += (size_t)wrote;
        if (used >= cap) break;
    }
}

// ---- drawing pieces --------------------------------------------------------

// Section heading: the 24px title at the content's left edge, then a 1px rule
// running from just past the text to the right edge, centered on the glyph box —
// a lighter separator than the Outside page's fieldset frames, which would fight
// with 14 closely-stacked rows.
void drawHeading(m5gfx::M5Canvas& c, int y, const char* title) {
    cjk::drawText(c, PAD, y, title, SCALE);
    int x0 = PAD + cjk::textWidth(title, SCALE) + 12;
    c.drawFastHLine(x0, y + GLYPH / 2, (540 - PAD) - x0, TFT_BLACK);
}

// One entry row: the owned marker, the 24px name, and the cost right-aligned to
// the content's right edge. Even the widest pair (page 0: 双肩包 at 72px and
// 篷车's "-1000 木头  -200 铁  -100 钢" at 336px; page 1: 军械坊 at 72px and its
// "-3000 木头  -100 钢  -50 硫磺" at 348px — the widest cost anywhere is 工坊's
// 360px, but its name is only 48px) leaves >=46px of clear space between them,
// so the two runs never need a wrap or an ellipsis (scratchpad/measure).
void drawEntry(m5gfx::M5Canvas& c, int y, uint8_t id) {
    int my = y + (GLYPH - MARK) / 2;
    if (held(id)) c.fillRect(PAD, my, MARK, MARK, TFT_BLACK);
    else          c.drawRect(PAD, my, MARK, MARK, TFT_BLACK);

    cjk::drawText(c, NAME_X, y, entryName(id), SCALE);

    char cost[64];
    costLine(id, cost, sizeof(cost));
    if (cost[0])
        cjk::drawText(c, (540 - PAD) - cjk::textWidth(cost, SCALE), y, cost, SCALE);
}

// The rect of band column `col` at `top` — one description shared by the draw
// call and pressRect, so the drawn frame and the invert-flash rect can't drift.
pages::Rect bandRect(int col, int top) {
    return pages::Rect{ BAND_X0[col], top, BAND_W, BAND_H };
}

// The label on the paging band: the page you are NOT on, so the band names its
// destination the way every other navigation cell in the app does. Both are
// closure-safe literals (see the file header) — upstream has no key for either
// as a UI noun.
const char* flipLabel() {
    return tech_page::s_page == 0 ? "建筑" : "装备";
}

// The trailing band row, through the shared action_band renderer (v0.12) — the
// exact shape (and the exact tr("go home") == "返回" label) the AssignPage and
// PathPage return bands now also get from it, so leaving a sub-page always looks
// and behaves the same. Neither band carries a subtitle, so action_band centres
// each lone label.
void drawBands(m5gfx::M5Canvas& c, int top) {
    action_band::draw(c, bandRect(0, top), flipLabel(),    nullptr, true, 0, 0);
    action_band::draw(c, bandRect(1, top), tr("go home"),  nullptr, true, 0, 0);
}
}  // namespace

// ================================ Page API =================================

const pages::Region* TechPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Press feedback: the pressed band's own drawn frame (the rect drawBands' two
// concentric drawRect calls paint), not the default full-panel y-band — the
// whole button box inverts, matching what the player sees as "the button". The
// x split here MUST be the one onLocalAction uses, or the flash would land on
// the band the press did not run.
pages::Rect TechPage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)y;
    return bandRect(x < BAND_MID ? 0 : 1, rg.y0);
}

// Drawable only while open: returning false makes showPageOrNext skip this ring
// slot, so the page is invisible + untappable unless the Room 科技树 cell opened
// it — the same skip mechanism AssignPage/PathPage use. Nothing else gates it:
// the ladders are the same from the first fire onward, which is the whole point
// of the page (available() holds that one predicate for draw() and the status
// bar's page-dot count alike).
bool TechPage::available() const { return tech_page::isOpen(); }

bool TechPage::draw(m5gfx::M5Canvas& c) {
    if (!available()) return false;
    c.fillSprite(TFT_WHITE);
    page_tabs::draw(c, 1);           // shared tab header, 村落 lit — v0.14 moved this
                                     // page's entry cell to the Outside grid, so it
                                     // is the village's sub-page now (AssignPage /
                                     // PathPage parity), not the Room's.

    int y = SEC_TOP;
    if (tech_page::s_page == 0) {
        for (int i = 0; i < LINE_COUNT; i++) {
            drawHeading(c, y, lineTitle(i));
            y += HEAD_H;
            for (int k = 0; k < LINES[i].n; k++) {
                drawEntry(c, y, LINES[i].ids[k]);
                y += ROWH;
            }
            y += SEC_GAP;
        }
        // The water footnote belongs to the equipment page: it is the payoff of
        // the 水容器 ladder sitting right above it, and page 1 has no ladder it
        // would explain.
        cjk::drawWrapped(c, PAD, NOTE_Y, CONTENT_W, NOTE, SCALE, GLYPH);
    } else {
        drawHeading(c, y, "建筑");
        y += HEAD_H;
        for (int k = 0; k < BUILDING_COUNT; k++) {
            drawEntry(c, y, BUILDING_IDS[k]);
            y += ROWH;
        }
    }

    drawBands(c, BAND_TOP);
    m_regions[0].y0 = (uint16_t)BAND_TOP;
    m_regions[0].y1 = (uint16_t)(BAND_TOP + BAND_H);
    m_regions[0].type  = 1;                 // firmware-local
    m_regions[0].param = 0;                 // the sole band row; x picks which half
    m_regionCount = 1;
    return true;
}

// Long-press on the band row. The LEFT half flips between the 装备 and 建筑
// pages — a full-page repaint (pager::showPage on this same ring slot, the
// PathPage 更多 idiom), because every row on screen changes and there is no
// sub-rect worth pushing. The RIGHT half closes the latch (re-hiding this ring
// slot) and jumps back to the page that opened it — the Outside grid since v0.14
// moved the 科技树 entry cell there, exactly mirroring the AssignPage/PathPage
// return; returning to the Room instead would drop the player somewhere they
// never came from. Both are real navigation, so both high-beep. The page has no
// other region — a param that is not the band row's can only be a stale region
// table, so it low-beeps.
void TechPage::onLocalAction(uint8_t param, int x, int y) {
    (void)y;
    if (param != 0) { M5.Speaker.tone(600, 120); return; }
    if (x < BAND_MID) {
        tech_page::s_page ^= 1;
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::currentRingIndex(), false);
        return;
    }
    tech_page::close();
    M5.Speaker.tone(1800, 80);
    pager::showPage(pager::ringIndexByName("outside"), false);
}
