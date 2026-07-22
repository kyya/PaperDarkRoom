// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Outside (village) page — the real Phase-1 Outside UI over the game_state
// engine. No clock header on this page (fw 0.11 retired it from all game
// pages); layout (540x960, all text via tr(), see outside_page.cpp) runs
// top to bottom on its own budget: population row (人口 X/Y + idle gatherers)
// · building summary (non-zero buildings, 2 cols) · 野外 action row (伐木 |
// 查看陷阱, an 80px band-row migrated off the Room page — upstream outside.js
// actions; each cell long-presses to gatherWood/checkTraps, dashed + a draining
// bar while cooling, 查看陷阱 drawn only when a trap stands) · worker assignment
// bands, two per row (one 80px long-press band per UNLOCKED job; a left label
// zone + a right vertical ▲/▼ stepper (v0.3.3); paged with a "更多" band when
// they overflow 8). A worker band whose stepper is a total no-op (no worker to
// remove AND no idle villager to add) draws a dashed single-ring frame instead
// of the normal solid double ring — see jobBandDisabled() in outside_page.cpp.
// Every band is a type=1 Region -> onLocalAction(param, x, y): the action row
// (param=PARAM_ACTIONS) maps the press column to gatherWood/checkTraps, while a
// worker row (param=row) resolves the column from x and the ▲/▼ half from y to
// assignWorker(job, ±1). The page draws nothing until outsideUnlocked
// (draw() returns false so the pager skips it in the ring). tick() settles
// the offline economy and repaints on change.
#pragma once
#include "page.h"

class OutsidePage : public pages::Page {
public:
    const char* name() const override { return "outside"; }
    bool draw(m5gfx::M5Canvas& canvas) override;
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;  // param=row, x picks col, y picks ▲/▼
    void tick(uint32_t nowMs) override;
    // wantsAwake stays false: the economy accrues offline via settle() on wake.

private:
    static constexpr int MAX_BANDS = 4;    // 4 rows x 80px fit the budget
    static constexpr int MAX_SLOTS = 8;    // 4 rows x 2 columns of worker cells
    // Current on-screen ROW bands (recomputed by draw()/tick from live game
    // state). regions()[0] is the fixed野外 action row (伐木 / 查看陷阱, param =
    // PARAM_ACTIONS); regions()[1..] are the worker rows (type=1, param = row
    // index). onLocalAction tells them apart by param, then resolves the column
    // (and, for a worker band, the −/＋ half) from the press x. m_slotCodes holds
    // the row-major Job id (or A_MORE) per WORKER grid cell (the action row's
    // codes are implicit in the column). +1 sizes the array for the action row.
    mutable pages::Region m_regions[MAX_BANDS + 1];
    mutable int           m_regionCount = 0;   // == action row + visible worker rows
    mutable uint8_t       m_slotCodes[MAX_SLOTS];
    mutable int           m_slotCount = 0;     // filled worker cells this page
    int                   m_page = 0;      // which batch of job bands is shown
};
