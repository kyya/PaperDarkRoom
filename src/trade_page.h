// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Trade (trading-post) page — the third game page (v0.3.3). Trading-post buying
// used to share the Room page's flat build/craft/buy grid (v0.3.2); user
// feedback split it off so the Room stays build/craft-focused and every buy
// button can show WHAT it costs. Layout (540x960, all text via tr(), see
// trade_page.cpp) top to bottom: the shared three-tab header (生火间 │ 村落 │
// 贸易站) · a balance row (fur/scales/teeth — trade's primary payment resources,
// abbreviated over 1000) · single full-width 80px BUY bands, each a two-line
// block: a 36px label (购买X) over a 24px cost sub-row ("-150 毛皮 -50 鳞片",
// every cost item). Affordable -> solid double ring; unaffordable -> the global
// 1px dashed frame (same availability cue the Room/Outside/event-modal buttons
// use). A long-press buys (engine GameState::buy): success high-beeps + saves +
// redraws (the balance row and each band's affordability shift), a cost failure
// low-beeps (the engine already pushed "not enough X" to the log, surfaced back
// on the Room page). Visible goods past one screen (about 8 bands) paginate with
// the shared "更多" band. The whole page draws nothing — draw() returns false so
// the pager's showPageOrNext skips this ring slot — until the trading post
// stands (buildings[B_TRADING_POST] > 0), exactly as the Outside page stays
// hidden until the forest unlocks. tick() settles the offline economy and
// repaints on change. See trade_page.cpp for the region model + geometry.
#pragma once
#include "page.h"
#include "game_data.h"   // TRADE_COUNT — sizes the visible-good scratch

class TradePage : public pages::Page {
public:
    const char* name() const override { return "trade"; }
    bool draw(m5gfx::M5Canvas& canvas) override;   // false until the post stands
    bool available() const override;               // trading post stands (see .cpp)
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;  // full-width: x/y unused
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;  // drawn frame, not the 540 default
    void tick(uint32_t nowMs) override;
    // wantsAwake stays false: the economy accrues offline via settle() on wake.

private:
    // Single full-width column, so one band == one row == one region. 8 bands
    // clear the status bar (see BAND_TOP/BUY_H in trade_page.cpp); a 9th+ visible
    // good paginates (7 goods + a trailing "更多").
    static constexpr int MAX_BANDS = 8;
    mutable pages::Region m_regions[MAX_BANDS];
    mutable int           m_regionCount = 0;   // == visible band count
    mutable uint8_t       m_slotCodes[MAX_BANDS];   // Trade id, or A_MORE per band
    mutable int           m_slotCount = 0;
    int                   m_page = 0;      // which batch of goods is shown
};
