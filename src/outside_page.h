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
// BOTTOM is the 野外 action AREA (v0.4.5): two 240px-column, 96px rows anchored so
// row 2's bottom hugs ~916 — ROW 1: 伐木 | 查看陷阱, ROW 2: 分工 | —. 伐木/查看陷阱
// are upstream outside.js verbs migrated off the Room page (long-press to
// gatherWood/checkTraps, dashed + a draining bar while cooling, 查看陷阱 drawn only
// when a trap stands; each also carries a cost/yield subtitle under its title,
// v0.10.1 — "+50 木头" / "-2 诱饵"); 分工 opens the standalone worker-assignment
// page (AssignPage, v0.4.0) and is drawn/hit-tested only once at least one job
// is unlocked (else the cell is blank, the same 无供给 rule 查看陷阱 uses). Each
// row is a type=1 touch Region (PARAM_ROW1 / PARAM_ROW2): onLocalAction maps
// the press column (x < 276 = left) to the verb. Worker assignment itself
// lives on AssignPage; this page has no ▲/▼ stepper bands. The page draws
// nothing until outsideUnlocked (available() is false, so the pager skips it
// in the ring). tick() settles the offline economy and repaints on change.
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
    // The 野外 action area is two bottom-anchored rows, each its own Region
    // (row 1 伐木 | 查看陷阱, row 2 分工 | —), distinguished by param; the press
    // x picks the column within a row. The worker summary / buildings /
    // inventory above are read-only. See outside_page.cpp for the geometry.
    mutable pages::Region m_regions[2];
    mutable int           m_regionCount = 0;   // 2 (the two action rows)
    uint32_t              m_lastSig = 0;   // tick()'s content baseline; onLocalAction
                                           // re-syncs it after its showPage so a
                                           // press doesn't force a second full
                                           // redraw next tick (see outside_page.cpp)
};
