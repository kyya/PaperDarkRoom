// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Shared game-page header — the original "A Dark Room" location tabs, ported to
// the 540x960 panel (fw 0.2.2; a third tab lands in v0.3.3). Every game page
// (Room, Outside, Trade) paints the SAME header from this one implementation, so
// the tabs read as a single header no matter which page you are on:
//   [生火间] │ [小型村落] │ [贸易站]
// The active page's tab carries a 3px underline; the others do not. Tab text is
// dynamic and routed through tr() (strings_zh.h) so only the official
// Simplified-Chinese title ever reaches the sparse 12px CJK face (§8.3):
//   Room tab   — fire dead -> "A Dark Room" (小黑屋), lit -> "A Firelit Room"
//                (生火间).
//   Outside tab — by hut count, the upstream outside.js getTitle thresholds:
//                0 "A Silent Forest", 1 "A Lonely Hut", 2-4 "A Tiny Village",
//                5-8 "A Modest Village", 9-14 "A Large Village",
//                >=15 "A Raucous Village".
//   Trade tab  — the static "trading post" title (贸易站).
// A tab is drawn only once its page is reachable: Outside once the forest is
// unlocked (outsideUnlocked), Trade once the trading post stands
// (buildings[B_TRADING_POST] > 0) — before that a tab would point nowhere. Even
// all three at their widest (生火间 108 │ 喧嚣小镇 144 │ 贸易站 108, +2×42px
// dividers = 444px) clear the 492px content width (scratchpad/measure_labels).
// Tab switching needs no touch table: the pager's short-press page turn (left
// half = prev, right half = next; the ring wraps) steps between them. See
// page_tabs.cpp for the geometry.
#pragma once

namespace m5gfx { class M5Canvas; }

namespace page_tabs {

// Header band height: page content begins at CONTENT_TOP. The 36px titles sit in
// [16, 52], the active underline in [58, 61], leaving margin down to CONTENT_TOP.
constexpr int TAB_H       = 72;
constexpr int CONTENT_TOP = TAB_H;

// Paint the header into the top band of `c`. activeTab: 0 = Room, 1 = Outside,
// 2 = Trade. Reads g_game for the dynamic titles and the per-tab unlock gates.
void draw(m5gfx::M5Canvas& c, int activeTab);

}  // namespace page_tabs
