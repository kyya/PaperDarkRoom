// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Path (出发准备 / 背包整备) page — Phase 2 milestone 2.1. The upstream
// "A Dusty Path" panel: pack an outfit out of the village stores, then embark
// into the World (world_state.h). Reached from the Outside page's 尘土之路 cell
// and gated on holding a compass (room.js pathDiscovery: buying a compass at the
// trading post is what unlocks the Path at all — §1.6). Built on the AssignPage
// sub-page model: an s_active latch gates draw()/available() (an un-opened Path
// is a skipped ring slot, exactly like the closed AssignPage), it reuses the
// SHARED tab header (page_tabs, 村落 lit — a village sub-page, not a 4th tab),
// and returns via a 「返回」band. Every string routes through tr() (strings_zh.h)
// so only the official Simplified-Chinese translation reaches the sparse 12px CJK
// face (the §8.3 glyph-closure iron law).
//
// Layout (540x960, all text via tr()):
//   tab header(0..72) · capacity row 背包剩余空间 free/cap (84) · a paginated
//   window of ≤6 carryable item bands (120 + i*90; each an 80px name + carried
//   "xN" + per-unit 负重 with the shared two-column ▲/▼ stepper —
//   increaseSupply/decreaseSupply ±1 and ±10, truncated to stock/bag space)
//   · a 更多 band when the owned carryables overflow one page · a read-only
//   护甲/水 row (712) · the 出发 band (746, dashed-disabled until cured meat ≥ 1,
//   the sole embark gate) · a 「返回」band (836). On open the selection is pre-filled
//   from the persistent remembered outfit (game_state savedOutfit, written by
//   world_state goHome) clamped to stock/capacity; embark() deducts it from the
//   stores, fills hp/water from equipment, and writes trek.bin. KEEPS its 返回
//   band (unlike AssignPage, v0.10.3): the 80px it would free sits BELOW the
//   item window, already spent on the fixed 护甲/水 row + 出发 band — reclaiming
//   it doesn't buy back "fit one more item band" the way it does on AssignPage,
//   which has nothing else on the page. See assign_page.h for that page's math.
//
// Carryables (path.js carryable ∪ the Room.Craftables tools/weapons, §1.3 order)
// map onto the P1 stores: some are Res (cured meat / bullets / energy cell /
// charm / alien alloy / medicine), some are Item (grenade / bolas / laser rifle /
// bayonet / torch / bone spear / iron sword / steel sword / rifle). armour and
// water are NOT bag rows — they surface read-only (armour tier -> max HP, water
// upgrade -> max water, both applied at embark). A row shows only when owned > 0
// OR already carried > 0; cured meat is always shown (it is the embark gate).
#pragma once
#include "page.h"
#include "game_data.h"   // RES_COUNT / ITEM_COUNT for the outfit arrays

namespace path_page {
// Open/close the Path page (flip the visibility latch). The caller pairs each
// with a pager::showPage to navigate (open -> this page's ring index; close ->
// back to the Outside page). open() also clears the RAM-only outfit selection.
void open();
void close();
bool isOpen();
}  // namespace path_page

class PathPage : public pages::Page {
public:
    const char* name() const override { return "path"; }
    bool draw(m5gfx::M5Canvas& canvas) override;   // false unless open + compass held
    bool available() const override;               // open + compass held (see .cpp)
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;
    void tick(uint32_t nowMs) override;
    // wantsAwake stays false: the offline economy accrues via settle() on wake.

private:
    // ≤6 bands in the middle window (item bands, plus a 更多 band when the owned
    // carryables overflow one page) + the 出发 and 返回 bands = 8 regions max.
    static constexpr int WIN_SLOTS   = 6;                 // item-band window height
    static constexpr int MAX_REGIONS = WIN_SLOTS + 2;     // + 出发 + 返回

    // Band params. Item slots carry their 0..WIN_SLOTS-1 slot index; the three
    // fixed bands carry sentinels (kept above any slot index).
    static constexpr uint8_t PARAM_PAGER  = 0xFC;   // 更多 (advance the page)
    static constexpr uint8_t PARAM_EMBARK = 0xFD;   // 出发
    static constexpr uint8_t PARAM_RETURN = 0xFE;   // 返回

    // RAM working copy of the outfit selection (upstream Path.outfit), indexed like
    // the stores. On open() it is PRE-FILLED from the persistent g_game.savedOutfit
    // (the loadout goHome remembered), clamped to current stock + capacity; the
    // player then tweaks it, and embark() consumes it. See prefillOutfit().
    int16_t m_outfitRes[adr::RES_COUNT]  = {0};
    int16_t m_outfitItem[adr::ITEM_COUNT] = {0};

    int m_page = 0;   // pagination page (0-based) when carryables overflow

    // ---- outfit / capacity helpers (read g_game + the selection above) ----
    int  ownedOf(int carryIdx) const;         // village stock of carryable row
    int  carriedOf(int carryIdx) const;       // units already in the outfit
    int  freeCenti() const;                    // capacity − carried weight (centi)
    bool adjustOutfitOne(int carryIdx, int step);  // ONE unit, space/stock clamped
    bool adjustOutfit(int carryIdx, int delta);    // ±1 / ±10, TRUNCATED to what
                                                   // fits (upstream Math.min —
                                                   // see path_page.cpp)
    void prefillOutfit();                          // seed selection from savedOutfit
    int  buildOutfitList(uint8_t* out) const;  // rows to show (owned/carried > 0)
    uint32_t contentSig() const;               // tick() repaint trigger
    void doEmbark();                           // embark() + jump to World (2.2 seam)

    // Rebuilt each draw(): the region table + the slot->carryable-row mapping for
    // the item bands currently on screen (m_slotCount excludes the 更多 band).
    mutable pages::Region m_regions[MAX_REGIONS];
    mutable int           m_regionCount = 0;
    mutable uint8_t       m_slotCarry[WIN_SLOTS] = {0};
    mutable int           m_slotCount = 0;
    mutable bool          m_hasPager  = false;
    mutable int           m_pageCount = 1;

    uint32_t m_lastSig = 0;   // tick()'s content baseline; onLocalAction re-syncs
                              // it after its showPage (AssignPage parity)
};
