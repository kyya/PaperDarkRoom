// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Header renderer — see page_tabs.h for the model. Titles are 36px
// (cjk::drawText scale=3), left-aligned from PAD, separated by a vertical rule;
// the active tab carries a 3px underline the width of its own text. Every string
// goes through tr() so the sparse 12px CJK face only ever sees the official
// translation (the §8.3 glyph-closure iron law). All nine title glyph sets
// (小黑屋/生火间/静谧森林/孤独小屋/小型村落/中型村落/大型村落/喧嚣小镇/贸易站) are
// in the strings_zh.h closure the font is generated from, so none render as tofu.
#include "page_tabs.h"
#include "cjk_text.h"
#include "pomo_page.h"          // PAD (shared layout authority)
#include "game_state.h"
#include <M5Unified.h>

// main.cpp owns the game model.
extern adr::GameState g_game;

using namespace adr;

namespace {
constexpr int TAB_SCALE = 3;                  // 12px grid x3 = 36px CJK title
constexpr int TAB_GLYPH = 12 * TAB_SCALE;     // 36px line box
constexpr int TAB_Y     = 16;                 // title top margin (ink 16..52)
constexpr int UL_Y      = TAB_Y + TAB_GLYPH + 6;   // underline top (58)
constexpr int UL_H      = 3;                  // underline thickness (58..61)
constexpr int DIV_GAP   = 20;                 // whitespace each side of the rule
constexpr int DIV_W     = 2;                  // vertical rule thickness

// Room title: the fire's lit/unlit state as an official room-name (the tab's own
// dimension — the page's state line still carries the literal fire intensity).
const char* roomTitle() {
    return tr(g_game.fire == FIRE_DEAD ? "A Dark Room" : "A Firelit Room");
}

// Outside title by hut count — the upstream outside.js getTitle thresholds.
const char* outsideTitle() {
    int huts = g_game.buildings[B_HUT];
    const char* key;
    if      (huts == 0)  key = "A Silent Forest";
    else if (huts == 1)  key = "A Lonely Hut";
    else if (huts <= 4)  key = "A Tiny Village";
    else if (huts <= 8)  key = "A Modest Village";
    else if (huts <= 14) key = "A Large Village";
    else                 key = "A Raucous Village";
    return tr(key);
}
}  // namespace

// Draw a vertical divider rule at divX; returns the x where the next tab title
// starts (past the rule and its right-side whitespace).
static int drawDivider(m5gfx::M5Canvas& c, int divX) {
    c.fillRect(divX, TAB_Y + 2, DIV_W, TAB_GLYPH - 4, TFT_BLACK);
    return divX + DIV_W + DIV_GAP;
}

void page_tabs::draw(m5gfx::M5Canvas& c, int activeTab) {
    // Tab 0 — Room (always present).
    const char* t0 = roomTitle();
    int x0 = PAD;
    int w0 = cjk::textWidth(t0, TAB_SCALE);
    cjk::drawText(c, x0, TAB_Y, t0, TAB_SCALE);
    if (activeTab == 0) c.fillRect(x0, UL_Y, w0, UL_H, TFT_BLACK);
    int penEnd = x0 + w0;               // right edge of the last drawn tab

    // Tab 1 — Outside, only once the forest is a reachable page.
    if (g_game.outsideUnlocked) {
        int x1 = drawDivider(c, penEnd + DIV_GAP);
        const char* t1 = outsideTitle();
        int w1 = cjk::textWidth(t1, TAB_SCALE);
        cjk::drawText(c, x1, TAB_Y, t1, TAB_SCALE);
        if (activeTab == 1) c.fillRect(x1, UL_Y, w1, UL_H, TFT_BLACK);
        penEnd = x1 + w1;
    }

    // Tab 2 — Trade, only once the trading post stands (Outside-tab precedent:
    // a tab appears with its page). The trading post is a forest-gated craft, so
    // Outside is always unlocked by the time this draws — the three tabs never
    // exceed the 492px content width (see page_tabs.h).
    if (g_game.buildings[B_TRADING_POST] > 0) {
        int x2 = drawDivider(c, penEnd + DIV_GAP);
        const char* t2 = tr("trading post");
        int w2 = cjk::textWidth(t2, TAB_SCALE);
        cjk::drawText(c, x2, TAB_Y, t2, TAB_SCALE);
        if (activeTab == 2) c.fillRect(x2, UL_Y, w2, UL_H, TFT_BLACK);
    }
}
