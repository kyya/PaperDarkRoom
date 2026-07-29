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
// 补给 / 覆满 / 水桶). Section headings 武器/护甲 and the 返回 label are real
// tr() values, not literals. Layout obeys §9 (24px body, >=80px long-press band).
#include "tech_page.h"
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
void open()  { s_active = true;  }
void close() { s_active = false; }
bool isOpen() { return s_active; }
}  // namespace tech_page

namespace {
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px CJK
constexpr int GLYPH     = 12 * SCALE;        // 24px line box
constexpr int BTN_SCALE = 3;                 // 12px grid x3 = 36px (返回 label)
constexpr int BTN_GLYPH = 12 * BTN_SCALE;    // 36px line box
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)

// ---- vertical budget (§9.4), clearing the 32px status bar (< 928). Top ->
// bottom: tab header(0..72) · four sections from SEC_TOP, each a HEAD_H heading
// block plus one ROWH row per entry, sections SEC_GAP apart · the footnote just
// above the band · the 返回 band. The four lines carry 5+3+3+3 = 14 rows, so the
// content ends at 84 + 4*(32+12) + 14*30 = 680 — one screen, no 更多 pagination
// (and the entry set is a fixed table, so that can never grow at runtime).
constexpr int SEC_TOP   = 84;                // first section heading top
constexpr int HEAD_H    = GLYPH + 8;         // 32 — heading line + clearance
constexpr int ROWH      = 30;                // one entry row
constexpr int SEC_GAP   = 12;                // gap below a section's last row
constexpr int BAND_H    = 80;                // long-press band (§9.3: >=80px floor)
constexpr int BAND_TOP  = 916 - BAND_H;      // 836 — bottom-anchored 返回 band
constexpr int BAND_X    = PAD;               // full-width single column
constexpr int BAND_W    = CONTENT_W;         // 492
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

// Held already? Every entry on these four lines is an ITEM craftable (craft id
// >= CRAFT_BLD_COUNT), so its count slot is items[], and one >0 test covers both
// kinds: the repeatable weapons (CT_WEAPON, maximum -1 — "held" means at least
// one in stock) and the one-shot upgrades (CT_UPGRADE, maximum 1 — count >= 1 is
// the same test). 拳头 is always held.
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
    if (g_game.items[slot] > 0) return true;
    return g_world.ex.active && g_world.ex.outfitItem[slot] > 0;
}

// An entry's price, e.g. "-500 木头  -100 铁" — the same "-amount name"
// convention (and two-space join) the Room's cost sub-rows, trade_page and the
// modals already use, so this reads as the price the Room will actually charge.
// No woodIncrPerN surcharge folding is needed here (unlike room_page's own cost
// line): every entry on these four lines is a tool/upgrade/weapon, and all of
// those carry woodIncrPerN == 0 — only the trap/hut BUILDINGS scale with count.
// Empty for 拳头 (no CRAFT row, nothing to pay).
void costLine(uint8_t id, char* out, size_t cap) {
    out[0] = 0;
    if (id == CRAFT_NONE) return;
    const Craftable& c = CRAFT[id];
    size_t used = 0;
    for (int i = 0; i < 3 && c.cost[i].res != RA_END; i++) {
        int wrote = snprintf(out + used, cap - used, "%s-%ld %s",
                             used ? "  " : "", (long)c.cost[i].amt,
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
// the content's right edge. Even the widest pair (双肩包 at 72px and 篷车's
// "-1000 木头  -200 铁  -100 钢" at 336px) leaves ~58px of clear space between
// them, so the two runs never need a wrap or an ellipsis (scratchpad/measure).
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

// The trailing 返回 band: full-width frame, lone 36px label centered — the exact
// shape (and the exact tr("go home") == "返回" label) the AssignPage return band
// uses, so leaving a sub-page always looks and behaves the same.
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

const pages::Region* TechPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Press feedback: the band's own drawn frame (the rect drawReturnBand's two
// concentric drawRect calls paint), not the default full-panel y-band — the
// whole button box inverts, matching what the player sees as "the button".
// onLocalAction reads neither x nor y (the whole band is one action), so there
// is no sub-cell split to mirror.
pages::Rect TechPage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)x; (void)y;
    return pages::Rect{ BAND_X, rg.y0, BAND_W, BAND_H };
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
    page_tabs::draw(c, 0);           // shared tab header, 生火间 lit (Room's sub-page)

    int y = SEC_TOP;
    for (int i = 0; i < LINE_COUNT; i++) {
        drawHeading(c, y, lineTitle(i));
        y += HEAD_H;
        for (int k = 0; k < LINES[i].n; k++) {
            drawEntry(c, y, LINES[i].ids[k]);
            y += ROWH;
        }
        y += SEC_GAP;
    }
    cjk::drawWrapped(c, PAD, NOTE_Y, CONTENT_W, NOTE, SCALE, GLYPH);

    drawReturnBand(c, BAND_TOP);
    m_regions[0].y0 = (uint16_t)BAND_TOP;
    m_regions[0].y1 = (uint16_t)(BAND_TOP + BAND_H);
    m_regions[0].type  = 1;                 // firmware-local
    m_regions[0].param = 0;                 // the sole band: 返回
    m_regionCount = 1;
    return true;
}

// Long-press on the 返回 band: close the latch (re-hiding this ring slot) and
// jump back to the Room, mirroring the AssignPage return. The page has no other
// action — a param that is not the band's can only be a stale region table, so
// it low-beeps.
void TechPage::onLocalAction(uint8_t param, int x, int y) {
    (void)x; (void)y;
    if (param != 0) { M5.Speaker.tone(600, 120); return; }
    tech_page::close();
    M5.Speaker.tone(1800, 80);
    pager::showPage(pager::ringIndexByName("room"), false);
}
