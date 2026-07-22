// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Outside (village) page — the real Phase-1 Outside UI over the game_state
// engine. No clock header on this page (fw 0.11 retired it from all game
// pages); layout (540x960, all text via tr(), see outside_page.cpp) runs top to
// bottom: the shared tab header · a population row (人口 X/Y + idle gatherers) ·
// THREE stacked fieldset boxes (v0.4.1 — a 1px border with the legend embedded
// in the top border, drawFieldset): 建筑 (non-zero buildings, 2 cols) · 工人
// (READ-ONLY worker summary, every unlocked job "名 xN" incl. x0, 3 cols) · 库存
// (the merged-in inventory, fw 0.2.2, 13x3 cells — the full resource set shows
// without the "…" collapse) · and, sunk to the BOTTOM of the page (v0.4.1), the
// 野外 action row (伐木 | 查看陷阱 | 分工, a 3-cell 80px band-row hugging ~916):
// the first two are upstream outside.js verbs migrated off the Room page
// (long-press to gatherWood/checkTraps, dashed + a draining bar while cooling,
// 查看陷阱 drawn only when a trap stands), the third opens the standalone
// worker-assignment page (AssignPage, v0.4.0). The action row is the page's ONLY
// touch Region (type=1, param=PARAM_ACTIONS): onLocalAction maps the press column
// (x thirds at 192/360) to gatherWood / checkTraps / open-assign. Worker
// assignment itself lives on AssignPage; this page has no ▲/▼ stepper bands. The
// page draws nothing until outsideUnlocked (draw() returns false so the pager
// skips it in the ring). tick() settles the offline economy and repaints on
// change.
#pragma once
#include "page.h"

class OutsidePage : public pages::Page {
public:
    const char* name() const override { return "outside"; }
    bool draw(m5gfx::M5Canvas& canvas) override;
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;  // action row: x picks the verb column
    void tick(uint32_t nowMs) override;
    // wantsAwake stays false: the economy accrues offline via settle() on wake.

private:
    // The page's single touch Region is the 野外 action row (伐木 | 查看陷阱 |
    // 分工, param = PARAM_ACTIONS); the worker summary and inventory below are
    // read-only. onLocalAction resolves the pressed column from x (see
    // outside_page.cpp).
    mutable pages::Region m_regions[1];
    mutable int           m_regionCount = 0;   // 1 (the action row)
};
