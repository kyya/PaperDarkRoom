// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// See icons.h.
#include "icons.h"

namespace icons {

void draw(m5gfx::M5Canvas& c, const uint8_t* bits, int w, int h, int stride,
          int x, int y, uint16_t ink) {
    if (!bits || w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++) {
        const uint8_t* p = bits + (size_t)row * stride;
        int col = 0;
        while (col < w) {
            // Skip clear pixels a byte at a time where possible — most of an
            // icon's bounding box is empty.
            if ((col & 7) == 0 && col + 8 <= w && p[col >> 3] == 0) {
                col += 8;
                continue;
            }
            if (p[col >> 3] & (0x80 >> (col & 7)))
                c.drawPixel(x + col, y + row, ink);
            col++;
        }
    }
}

void drawCentred(m5gfx::M5Canvas& c, const uint8_t* bits, int w, int h,
                 int stride, int cx, int cy, uint16_t ink) {
    draw(c, bits, w, h, stride, cx - w / 2, cy - h / 2, ink);
}

}  // namespace icons
