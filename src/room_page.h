// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Room (fire) page — the real Phase-1 Room UI over the game_state engine.
// Layout (540x960, all text via tr(), see room_page.cpp):
//   clock header (page_header, 0..112) · fire/temp state line · resource summary
//   (non-zero stores, 2 cols) · log stream (newest on top, wrapped) · action
//   bands (92px long-press regions, state-driven, paged with a "more" band when
//   they overflow 4). Every band is a type=1 Region -> onLocalAction maps it to
//   the engine action API. Buttons carry a cooldown drain bar + disabled stipple
//   while cooling. tick() settles the offline economy and repaints on change.
#pragma once
#include "page.h"
#include "game_data.h"   // CRAFT_COUNT — sizes the visible-action scratch

class RoomPage : public pages::Page {
public:
    const char* name() const override { return "room"; }
    bool draw(m5gfx::M5Canvas& canvas) override;
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x) override;   // x unused (no ±split)
    void tick(uint32_t nowMs) override;
    // wantsAwake stays false: the economy accrues offline via settle() on wake,
    // so there is nothing to keep the panel powered for between interactions.

private:
    static constexpr int MAX_BANDS = 4;    // 4 x 92px bands fit the button budget
    // Current on-screen bands (recomputed by draw()/tick from live game state).
    // type=1, param = an action code (see room_page.cpp) — regions() returns
    // exactly these, so the pager hit-tests only the buttons actually drawn.
    mutable pages::Region m_regions[MAX_BANDS];
    mutable int           m_regionCount = 0;
    int                   m_page = 0;      // which batch of actions is shown
};
