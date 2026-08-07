// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Room (fire) page — the real Phase-1 Room UI over the game_state engine.
// Layout (540x960, all text via tr(), see room_page.cpp): NO clock header — the
// page reflows from a small top margin:
//   fire/temp state line · log stream (newest on top, wrapped, 24px) ·
//   two-column action bands (96px long-press regions, state-driven, paged with
//   a "更多" band when they overflow the grid). The grid is BOTTOM-anchored: it
//   hangs up from a fixed bottom edge by however many rows the current page
//   fills, so the buttons never float mid-screen, and the log band takes all the
//   space left above them (9 lines at a full 5-row grid, 20+ early on). The
//   resource/inventory summary
//   moved to its own Inventory page. Every ROW is a type=1 Region -> the press x
//   resolves which of the row's two columns was hit; onLocalAction maps it to
//   the engine action API. A band's frame shows availability: solid rings when
//   the action can fire, a 1px dashed frame when its condition is not met (cost /
//   cooldown / room too cold). A priced action also carries a small cost
//   subtitle under its label (v0.10.0), rendered by the shared action_band
//   helper (v0.10.1) so its baseline matches every other band. Cooling bands
//   also drain a small progress bar. tick() settles the offline economy and
//   repaints on change.
#pragma once
#include "page.h"
#include "game_data.h"   // CRAFT_COUNT — sizes the visible-action scratch

class RoomPage : public pages::Page {
public:
    const char* name() const override { return "room"; }
    bool draw(m5gfx::M5Canvas& canvas) override;
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;   // x picks the column; y unused
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;  // exact grid cell
    void tick(uint32_t nowMs) override;
    // wantsAwake stays false: the economy accrues offline via settle() on wake,
    // so there is nothing to keep the panel powered for between interactions.

private:
    static constexpr int MAX_BANDS = 5;    // 5 rows x 96px fit the button budget
    static constexpr int MAX_SLOTS = 10;   // 5 rows x 2 columns of action cells
    // Current on-screen ROW bands (recomputed by draw()/tick from live game
    // state). type=1, param = row index — regions() returns exactly these, so
    // the pager hit-tests only the rows actually drawn; onLocalAction resolves
    // the column from the press x. m_slotCodes holds the row-major action code
    // per grid cell (the column the row band cannot carry).
    mutable pages::Region m_regions[MAX_BANDS];
    mutable int           m_regionCount = 0;   // == visible row count
    mutable uint8_t       m_slotCodes[MAX_SLOTS];
    mutable int           m_slotCount = 0;     // filled action cells this page
    int                   m_page = 0;      // which batch of actions is shown
    int                   m_btnTop = 366;  // live top of the bottom-anchored grid,
                                           // and so the log band's floor. Written
                                           // ONLY on the full-redraw path (draw(),
                                           // via layoutBands); every partial push
                                           // reads it so it can never place a cell
                                           // or clear a strip against a layout the
                                           // panel is not showing. Seeded with the
                                           // full 5-row top for the pre-first-draw
                                           // window (see room_page.cpp).
    uint32_t              m_lastSig = 0;   // tick()'s content baseline; onLocalAction
                                           // re-syncs it after its showPage so a
                                           // press doesn't force a second full
                                           // redraw next tick (see room_page.cpp)
    uint32_t              m_lastLogSig = 0;   // the LOG's own baseline, tracked apart
                                           // from m_lastSig so a new event line
                                           // repaints just the log strip instead of
                                           // the whole page (see room_page.cpp tick)
};
