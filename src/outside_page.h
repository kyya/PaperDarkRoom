// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Outside (village) page — the real Phase-1 Outside UI over the game_state
// engine. Layout (540x960, all text via tr(), see outside_page.cpp):
//   clock header (page_header, 0..112) · population row (人口 X/Y + idle
//   gatherers) · building summary (non-zero buildings, 2 cols) · worker
//   assignment bands (one 92px long-press band per UNLOCKED job, each split
//   into a −/＋ pair; paged with a "更多" band when they overflow 4).
// Every band is a type=1 Region -> onLocalAction(param=job, x) maps the press
// to assignWorker(job, x<mid ? -1 : +1). The page draws nothing until
// outsideUnlocked (draw() returns false so the pager skips it in the ring).
// tick() settles the offline economy and repaints on change.
#pragma once
#include "page.h"

class OutsidePage : public pages::Page {
public:
    const char* name() const override { return "outside"; }
    bool draw(m5gfx::M5Canvas& canvas) override;
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x) override;  // param=job, x picks −/＋
    void tick(uint32_t nowMs) override;
    // wantsAwake stays false: the economy accrues offline via settle() on wake.

private:
    static constexpr int MAX_BANDS = 4;    // 4 x 92px bands fit the budget
    // Current on-screen bands (recomputed by draw()/tick from live game state).
    // type=1, param = a Job id (or A_MORE) — regions() returns exactly these so
    // the pager hit-tests only the worker bands actually drawn.
    mutable pages::Region m_regions[MAX_BANDS];
    mutable int           m_regionCount = 0;
    int                   m_page = 0;      // which batch of job bands is shown
};
