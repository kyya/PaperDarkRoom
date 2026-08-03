// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Starship (破旧星舰) page — the Phase-3a payoff of the W landmark, ported from
// upstream script/ship.js. Reached by paging, like Trade: it is NOT a village
// sub-page (no page_tabs header, no 返回 band) but a location of its own, so it
// draws its own 36px title exactly as the World page does.
//
// HOW IT UNLOCKS (world.js goHome:965-968, research-phase3.md §1.1). Three
// steps, and the third is the interesting one:
//   1. the map carries exactly one W at Manhattan 28 (world_data.h LANDMARKS);
//   2. stepping on it runs the `ship` setpiece, whose SPE_CLEAR_SHIP sets
//      ex.clearedShip and draws the road (already in the tree since Phase 2.4);
//   3. WALKING HOME ALIVE is what actually opens this page — clearedShip lives
//      on the Expedition, which die() throws away wholesale. Salvage the wreck
//      and then starve on the way back and the trip was for nothing.
// goHome calls GameState::unlockShip(), so the persistent flag it sets
// (g_game.shipUnlocked) is the ONE predicate available() reads.
//
// WHAT IS HERE (ship.js:36-45): two stats — 外壳 (hull) and 引擎 (engine) — over
// three action_band buttons. 加固船身 and 升级引擎 each cost 1 外星合金 and add a
// point; 点火起飞 carries the 120-second cooldown. hull starts at 0 (ship.js
// BASE_HULL), which is why 点火起飞 opens as a dashed/disabled band and one
// reinforcement is what arms it.
//
// WHAT IS NOT HERE YET: the flight. Space is Phase 3b; GameState::liftOff() is a
// stub that logs and returns. The whole gate around it — the hull check, the
// 120s cooldown, the one-shot 「准备好要离开了吗?」 confirmation and its 「裹足
// 徘徊」 cooldown refund — is already the real upstream behaviour, so 3b replaces
// that one function body and this page does not change.
//
// Every string is an official upstream translation via tr() (strings_zh.h), so
// the §8.3 glyph closure needs no new codepoints; the two firmware-local hint
// literals are spelled entirely out of characters that closure already carries.
#pragma once
#include "page.h"

class ShipPage : public pages::Page {
public:
    const char* name() const override { return "ship"; }
    bool draw(m5gfx::M5Canvas& canvas) override;   // false until the ship is found
    bool available() const override;               // == g_game.shipUnlocked
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;  // full-width: x/y unused
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;
    void tick(uint32_t nowMs) override;            // economy + the draining cooldown bar
    // wantsAwake stays false: the liftoff cooldown is epoch-based, so it keeps
    // running through a deep sleep and needs no keep-awake to expire.

private:
    // Three fixed full-width bands (reinforce / upgrade / lift off) — one band,
    // one row, one region, so param IS the band index.
    static constexpr int BAND_COUNT = 3;
    mutable pages::Region m_regions[BAND_COUNT];
    mutable int           m_regionCount = 0;
    uint32_t              m_lastSig = 0;   // tick()'s content baseline; onLocalAction
                                           // re-syncs it after its own showPage so an
                                           // action doesn't force a second full redraw
};
