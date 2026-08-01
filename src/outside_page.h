// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Outside (village) page — the real Phase-1 Outside UI over the game_state
// engine. No clock header on this page (fw 0.11 retired it from all game
// pages); layout (540x960, all text via tr(), see outside_page.cpp) runs top to
// bottom as DYNAMICALLY-HEIGHTED fieldset boxes (v0.4.5 — a 2px border with the
// legend embedded in the top border, drawFieldset; each box's height follows its
// live content row count plus 6px of bottom padding, and the boxes flow down
// from a fixed top anchor with a
// 12px gap): 工人 (READ-ONLY worker summary — 人口 X/Y line + every unlocked job
// "名 xN" incl. x0, 3 cols) · 建筑 (non-zero buildings, 2 cols; HIDDEN when there
// are none) · 库存 (the merged-in inventory, fw 0.2.2, 3 cols; row count clamped to
// the space above the action area, tail-collapses to "…" on overflow). Sunk to the
// BOTTOM is the 野外 action AREA (v0.4.5): 240px-column, 96px rows anchored so
// the block's bottom hugs ~916. Since v0.14 the grid PACKS — the visible cells
// are laid row-major with no holes, in the order
//   伐木 · 科技树 · 查看陷阱 · 分工 · 漫漫尘途
// and a cell whose gate is shut simply is not there (the static-slot version left
// a visible hole where 查看陷阱 would go before the first trap was built, which
// is what prompted the change). So the grid is 1 row early on and grows to 3, and
// because it is bottom-anchored the fieldsets above get the unused space back.
// 伐木/查看陷阱 are upstream outside.js verbs migrated off the Room page (long-press
// to gatherWood/checkTraps, dashed + a draining bar while cooling; each also carries
// a cost/yield line, v0.10.1 — "+50 木头" / "-2 诱饵"). The other three cells are
// pure navigation into sub-pages: 科技树 -> TechPage (v0.14 moved this entry here
// from the Room grid at the user's request — NOTE that this puts the tech tree
// behind outsideUnlocked, where the Room entry was reachable from the first screen;
// every ladder it explains needs a workshop, far past the forest, so nothing becomes
// unreachable), 分工 -> AssignPage (v0.4.0, once a job is unlocked) and 漫漫尘途 ->
// PathPage (once a compass is held). Each ROW is a type=1 touch Region carrying its
// row index; the press column (x < 276 = left) completes the packed slot index.
// Worker assignment itself lives on AssignPage; this page has no ▲/▼ stepper bands.
// The page draws nothing until outsideUnlocked (available() is false, so the pager
// skips it in the ring). tick() settles the offline economy and repaints on change.
#pragma once
#include "page.h"

class OutsidePage : public pages::Page {
public:
    const char* name() const override { return "outside"; }
    bool draw(m5gfx::M5Canvas& canvas) override;
    bool available() const override;   // outsideUnlocked (see outside_page.cpp)
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;  // action row: x picks the verb column
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;  // exact verb cell
    void tick(uint32_t nowMs) override;
    // wantsAwake stays false: the economy accrues offline via settle() on wake.

private:
    // The 野外 action grid PACKS its visible cells (v0.14): 伐木 and 科技树 are
    // always there, 查看陷阱 / 分工 / 尘土之路 join as their gates open, and a
    // gated-off cell takes no slot — so the grid runs 2..5 cells over 1..3 rows
    // and is bottom-anchored, freeing the space above for the fieldsets. One
    // Region per ROW (param = row index); the press x picks the column and
    // row*2+col indexes the packed list, the same model RoomPage's grid uses.
    // The worker summary / buildings / inventory above are read-only. See
    // outside_page.cpp for the geometry and the dynamic vertical budget.
    static constexpr int MAX_ACT_SLOTS = 5;
    static constexpr int MAX_ACT_ROWS  = 3;
    mutable pages::Region m_regions[MAX_ACT_ROWS];
    mutable int           m_regionCount = 0;    // 1..3 (the packed row count)
    mutable uint8_t       m_slotCodes[MAX_ACT_SLOTS] = {0};  // cell code per slot
    mutable int           m_slotCount = 0;      // packed cells on screen
    mutable int           m_areaTop   = 0;      // grid's top edge this draw
    uint32_t              m_lastSig = 0;   // tick()'s content baseline; onLocalAction
                                           // re-syncs it after its showPage so a
                                           // press doesn't force a second full
                                           // redraw next tick (see outside_page.cpp)
};
