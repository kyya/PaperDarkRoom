// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Trade (trading-post) page — the third game page (v0.3.3). Trading-post buying
// used to share the Room page's flat build/craft/buy grid (v0.3.2); user
// feedback split it off so the Room stays build/craft-focused and every buy
// button can show WHAT it costs. Layout (540x960, all text via tr(), see
// trade_page.cpp) top to bottom: the shared three-tab header (生火间 │ 村落 │
// 贸易站) · the purchase log stream (24px, newest on top) · 毛皮/鳞片/牙齿
// bound to the top of the button group · bottom-anchored 80px BUY bands. A long-
// press buys: success high-beeps + saves + redraws; a cost failure low-beeps
// and immediately refreshes the log ("毛皮不够了"), same as Room. Visible
// goods past one screen paginate with "更多". The whole page draws nothing — draw() returns false so
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
    // Six 80px bands leave room for the log between the balance row and the
    // grid; a 7th+ visible good paginates (5 goods + a trailing "更多").
    static constexpr int MAX_BANDS = 6;
    mutable pages::Region m_regions[MAX_BANDS];
    mutable int           m_regionCount = 0;   // == visible band count
    mutable uint8_t       m_slotCodes[MAX_BANDS];   // Trade id, or A_MORE per band
    mutable int           m_slotCount = 0;
    int                   m_page = 0;      // which batch of goods is shown
    int                   m_btnTop = 0;    // live grid top the screen was drawn from
    uint32_t              m_lastSig = 0;   // tick()'s content baseline; onLocalAction
    uint32_t              m_lastLogSig = 0;
                                           // re-syncs it after its showPage so a buy
                                           // doesn't force a second full redraw next
                                           // tick (see trade_page.cpp)
};
