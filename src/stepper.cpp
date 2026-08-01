// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "stepper.h"
#include <M5Unified.h>

namespace {

// The ▲/▼ glyph — the roomy v0.3.3 triangle, and the ONLY one. Both columns draw
// it identically and are told apart by POSITION alone (outer = coarse, inner =
// fine): 用户裁定,「复用之前的 +1 按钮就行」.
constexpr int TRI_HALF_W = 11;               // 22px base
constexpr int TRI_HALF_H = 9;                // 18px tall

// One triangle centred at (cx, cy): apex up (increment) or apex down.
void tri(m5gfx::M5Canvas& c, int cx, int cy, bool up) {
    if (up) c.fillTriangle(cx, cy - TRI_HALF_H,
                           cx - TRI_HALF_W, cy + TRI_HALF_H,
                           cx + TRI_HALF_W, cy + TRI_HALF_H, TFT_BLACK);
    else    c.fillTriangle(cx, cy + TRI_HALF_H,
                           cx - TRI_HALF_W, cy - TRI_HALF_H,
                           cx + TRI_HALF_W, cy - TRI_HALF_H, TFT_BLACK);
}

}  // namespace

void stepper::draw(m5gfx::M5Canvas& c, const pages::Rect& b) {
    int oneX  = b.x + oneX0(b.w);
    int manyX = b.x + manyX0(b.w);
    int midY  = b.y + b.h / 2;

    // Vertical rules: one fencing the zone off from the band's text, one
    // splitting fine from coarse. Both inset 10px top and bottom like the
    // original single rule, so they read as dividers rather than a grid.
    c.drawFastVLine(b.x + dividerX(b.w), b.y + 10, b.h - 20, TFT_BLACK);
    c.drawFastVLine(manyX - 4,           b.y + 10, b.h - 20, TFT_BLACK);
    // One horizontal split across BOTH columns — the increment/decrement line
    // is the same line for fine and coarse, which is what makes the four zones
    // read as one control rather than two unrelated steppers.
    c.drawFastHLine(oneX, midY, 2 * COL_W, TFT_BLACK);

    int cyUp  = b.y + b.h / 4;
    int cyDn  = b.y + 3 * b.h / 4;
    int cxOne = oneX  + COL_W / 2;
    int cxMany= manyX + COL_W / 2;

    tri(c, cxOne,  cyUp, true);     // ▲ +1
    tri(c, cxOne,  cyDn, false);    // ▼ −1
    tri(c, cxMany, cyUp, true);     // ▲ +10 — same glyph, coarse by position
    tri(c, cxMany, cyDn, false);    // ▼ −10
}

int stepper::deltaFor(const pages::Rect& b, int x, int y) {
    int mag = (x >= b.x + manyX0(b.w)) ? MANY : 1;
    return (y < b.y + b.h / 2) ? mag : -mag;
}

pages::Rect stepper::zoneRect(const pages::Rect& b, int x, int y) {
    bool many  = x >= b.x + manyX0(b.w);
    int  colX0 = b.x + (many ? manyX0(b.w) : oneX0(b.w));
    int  half  = b.h / 2;
    if (y < b.y + half) return pages::Rect{ colX0, b.y,        COL_W, half };
    return                     pages::Rect{ colX0, b.y + half, COL_W, b.h - half };
}
