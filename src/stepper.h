// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// The ±1 / ±10 stepper zone that rides the right-hand end of a quantity band —
// AssignPage's job rows (villagers into a job) and PathPage's outfit rows (units
// into the bag). Both pages draw a 492x80 band whose LEFT side is their own
// side-by-side name + counts (that part differs, so it stays in the pages) and
// whose RIGHT side is this: an identical stepper, to the pixel. It lives here so
// there is ONE of it — the same reason action_band.h exists, and the reason this
// module appeared the moment the stepper grew from 2 zones to 4 rather than
// being copy-pasted into both pages a second time.
//
// WHY ±10 EXISTS (v0.14, user: "一个一个调节太费事"): upstream A Dark Room has
// had it all along and the port had dropped it. outside.js:356-359 and
// path.js:260-263 give every row FOUR buttons — upBtn/dnBtn carrying data 1 and
// upManyBtn/dnManyBtn carrying data 10 — and the handler spends
// Math.min(available, btn.data) (outside.js:376), so a ±10 tap with only 3
// available moves 3 rather than refusing. TRUNCATION, not all-or-nothing. Both
// callers reproduce that; see each page's adjust path.
//
// ---- LAYOUT (all x are LOCAL to the band's left edge; the band is `w` wide) --
// Two 66px columns hug the band's right edge behind a 12px inset, so the zone
// occupies the last 144px:
//
//        |<-- name + counts (the page's own) -->|  ±1  |  ±10 |
//        ^dividerX(w)                     oneX0(w)^  manyX0(w)^
//
// The COARSE (±10) column sits OUTERMOST, hard against the band's right edge
// where a thumb lands most easily, with the fine ±1 column inboard of it. Each
// column splits at the band's vertical midpoint: upper half increments, lower
// half decrements, matching the ▲/▼ drawn in it. The two columns draw the
// IDENTICAL triangle — position is the only thing distinguishing coarse from
// fine (用户裁定: a doubled ▲▲ glyph was tried on-device and rejected).
//
// Adding the second column pushed dividerX from 410 to 344 — the pages'
// right-aligned counts re-anchor to it and must be re-measured when a name or a
// sub-value grows (the tightest today is PathPage's 能量元件 + "负重 0.2" +
// "x999", which still leaves 28px of clear space).
//
// ---- HIT MODEL --------------------------------------------------------------
// deltaFor()/zoneRect() decode a press into one of four zones. A press to the
// LEFT of the ±10 column — including the whole name area — counts as ±1, which
// is exactly what pressing anywhere in the row did before this module existed;
// keeping that means the change adds a capability without taking one away.
#pragma once
#include "page.h"          // pages::Rect

namespace m5gfx { class M5Canvas; }

namespace stepper {

// Upstream's upManyBtn/dnManyBtn step (outside.js/path.js `data: 10`).
constexpr int MANY = 10;

constexpr int COL_W = 66;    // one stepper column
constexpr int INSET = 12;    // band right edge -> the ±10 column

// Column origins + the rule that fences the whole zone off from the band's
// text, all local to the band's left edge. Callers right-align their own
// content to dividerX(w).
constexpr int manyX0(int w)   { return w - INSET - COL_W; }   // 414 @ w=492
constexpr int oneX0(int w)    { return manyX0(w) - COL_W; }   // 348
constexpr int dividerX(int w) { return oneX0(w) - 4; }        // 344

// Paint the zone (two rules, one split rule, and four ▲/▼ glyphs) into `band`.
// Both columns use the SAME triangle and are told apart by position alone.
// Triangles are drawn geometrically (fillTriangle) because ▲/▼ are not in the
// sparse 12px face and must never be rendered as text.
//
// NOT the Trade page's ×10 column: that one is up-only (a good cannot be sold
// back), carries an icon plus a multiplier rather than a glyph, and needs a
// wider column to seat them — so it lays out its own geometry in trade_page.cpp
// and shares only MANY, the step size itself.
void draw(m5gfx::M5Canvas& c, const pages::Rect& band);

// The SIGNED number of units a press at (x,y) inside `band` is asking for:
// ±MANY in the outer column, ±1 anywhere left of it; the y-half picks the sign.
// The caller passes this straight to its own truncating adjust routine — this
// module never decides how much is actually available.
int deltaFor(const pages::Rect& band, int x, int y);

// The half-column rect that press lands in, for the invert-flash. A name-area
// press reports the ±1 column, because that is the button that will fire.
pages::Rect zoneRect(const pages::Rect& band, int x, int y);

}  // namespace stepper
