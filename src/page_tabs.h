// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Shared two-tab header for the game pages — the original "A Dark Room" location
// tabs, ported to the 540x960 panel (fw 0.2.2). Both game pages (Room, Outside)
// paint the SAME header from this one implementation, so the two tabs read as a
// single header no matter which page you are on:
//   [生火间] │ [小型村落]
// The active page's tab carries a 3px underline; the other does not. Tab text is
// dynamic and routed through tr() (strings_zh.h) so only the official
// Simplified-Chinese title ever reaches the sparse 12px CJK face (§8.3):
//   Room tab   — fire dead -> "A Dark Room" (小黑屋), lit -> "A Firelit Room"
//                (生火间).
//   Outside tab — by hut count, the upstream outside.js getTitle thresholds:
//                0 "A Silent Forest", 1 "A Lonely Hut", 2-4 "A Tiny Village",
//                5-8 "A Modest Village", 9-14 "A Large Village",
//                >=15 "A Raucous Village".
// The Outside tab is only drawn once the forest is unlocked (outsideUnlocked) —
// before that the ring has a single reachable page, so a second tab would point
// nowhere. Tab switching needs no touch table: the pager's short-press page turn
// (left half = prev, right half = next; a two-page ring wraps either way) turns
// one tab into the other. See page_tabs.cpp for the geometry.
#pragma once

namespace m5gfx { class M5Canvas; }

namespace page_tabs {

// Header band height: page content begins at CONTENT_TOP. The 36px titles sit in
// [16, 52], the active underline in [58, 61], leaving margin down to CONTENT_TOP.
constexpr int TAB_H       = 72;
constexpr int CONTENT_TOP = TAB_H;

// Paint the two-tab header into the top band of `c`. activeTab: 0 = Room,
// 1 = Outside. Reads g_game for the dynamic titles and the Outside-unlock gate.
void draw(m5gfx::M5Canvas& c, int activeTab);

}  // namespace page_tabs
