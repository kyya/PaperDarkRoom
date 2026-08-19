// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// See art_blit.h. Format lives in the generated headers' banner and in
// tools/gen_event_art.py, which is the only thing that writes this encoding.
#include "art_blit.h"

namespace art {

void blit(m5gfx::M5Canvas& c, const uint8_t* rle, int w, int h, int x, int y,
          int srcY0, int rows) {
    if (!rle || w <= 0 || h <= 0) return;
    if (rows < 0) rows = h;
    if (srcY0 < 0) srcY0 = 0;
    if (srcY0 >= h || rows <= 0) return;
    const int srcEnd = (srcY0 + rows < h) ? srcY0 + rows : h;   // exclusive
    uint8_t* buf = (uint8_t*)c.getBuffer();
    if (!buf) return;

    const int stride  = c.width();
    const int rowPack = w / 2;                  // packed bytes per row
    const uint8_t* p = rle;
    int bx = 0, by = 0;
    while (by < srcEnd) {
        uint8_t head = *p++;
        const uint8_t* lit = nullptr;
        uint8_t val = 0;
        int n;
        if (head == 0) { n = *p++; lit = p; p += n; }   // literal packet
        else           { n = head; val = *p++; }        // run packet
        for (int i = 0; i < n; i++) {
            // Rows before the crop window are decoded (the stream is sequential)
            // but not stored; `row` is only resolved for the kept ones.
            if (by >= srcY0) {
                uint8_t* row = buf + (size_t)(y + by - srcY0) * stride + x;
                uint8_t b = lit ? lit[i] : val;
                row[bx * 2]     = (uint8_t)((b >> 4) * 17);
                row[bx * 2 + 1] = (uint8_t)((b & 0x0F) * 17);
            }
            if (++bx == rowPack) {
                bx = 0;
                if (++by == srcEnd) return;             // last kept row consumed
            }
        }
    }
}

}  // namespace art
