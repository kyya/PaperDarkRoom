// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Fabricator (嗡鸣的制造机) page — the Phase-3c payoff of the X landmark, ported
// from upstream script/fabricator.js. A location page of its own, exactly like
// the starship page next to it in the ring: no page_tabs header, no 返回 band, it
// draws its own 36px title.
//
// HOW IT UNLOCKS (world.js goHome:969-973, research-phase3.md §4.1). The same
// three-step shape the starship page documents, with the Executioner's prologue
// in place of the wreck:
//   1. the map carries exactly one X at Manhattan 28 (world_data.h LANDMARKS);
//   2. clearing `executioner-intro` scene 7 sets ex.clearedExec (SPE_MARK_EXEC);
//   3. WALKING HOME ALIVE is what opens this page — clearedExec rides the
//      Expedition, which die() throws away whole.
// goHome calls GameState::unlockFabricator(), which latches g_game.execEntered
// (this port's `features.location.fabricator`) and pushes builder's one-shot
// notice. execEntered is therefore the ONE predicate available() reads.
//
// WHAT IS HERE (fabricator.js:93-130). Upstream's two panel groups, in order:
//   * `blueprints` — a read-only list of the blueprints redeemed so far. Drawn
//     only when there is at least one, so a freshly opened Fabricator (prologue
//     cleared, no wing beaten yet) is just the fabricate list.
//   * `fabricate:` — one action_band per row of game_data.h's FABRICATE table.
// A row whose blueprint is not redeemed HAS NO BAND AT ALL (fabricator.js's
// canFabricate gate refuses to create the button), which is why the blueprints
// list matters: it is the only place a redeemed blueprint is legible as itself
// rather than as "a band that appeared". A capped upgrade keeps its band and goes
// dashed at 1 — that IS upstream's real behaviour, since its `maximum` guard is
// dead code and only the button-disable fires (research-phase3.md §4.2).
//
// The bands paginate the Trade page's way (n real rows + a trailing 更多) rather
// than compressing, because the blueprints block above them is variable height:
// with every blueprint redeemed the list needs a second line and the eight rows
// no longer clear the status bar.
#pragma once
#include "page.h"
#include "game_data.h"    // FAB_COUNT — the slot table is sized off it

class FabricatorPage : public pages::Page {
public:
    const char* name() const override { return "fabricator"; }
    bool draw(m5gfx::M5Canvas& canvas) override;   // false until the device is home
    bool available() const override;               // == g_game.execEntered
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;  // full-width: x/y unused
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;
    void tick(uint32_t nowMs) override;            // village economy + repaint-on-change
    // wantsAwake stays false: fabricating has no cooldown and no timer.

private:
    // Every row could be offerable at once, so the tables are sized to the whole
    // FABRICATE list. How many actually fit is MEASURED per draw against the
    // status bar (the blueprints block above them is 0, 1 or 2 lines tall) and the
    // 更多 band pages the rest — so this bound is deliberately never the limit.
    static constexpr int MAX_BANDS = adr::FAB_COUNT;
    mutable pages::Region m_regions[MAX_BANDS];
    mutable uint8_t       m_slotCodes[MAX_BANDS];  // Fab id, or A_MORE
    mutable int           m_slotCount  = 0;
    mutable int           m_regionCount = 0;
    int                   m_page = 0;              // pagination page (0-based)
    uint32_t              m_lastSig = 0;           // tick()'s content baseline
};
