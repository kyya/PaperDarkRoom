// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// See fb_codec.h.
#include "fb_codec.h"

namespace fb {

// The packed byte at index `i` of a w/2-per-row 4bpp stream, read straight out
// of the 8-bit source (no intermediate packed buffer — the frame is 518KB and
// the packed copy would be another 259KB for nothing).
static inline uint8_t packedAt(const uint8_t* gray8, int w, size_t i) {
    const size_t rowBytes = (size_t)(w / 2);
    const uint8_t* src = gray8 + (i / rowBytes) * (size_t)w + (i % rowBytes) * 2;
    return (uint8_t)((src[0] & 0xF0) | (src[1] >> 4));
}

size_t encode(const uint8_t* gray8, int w, int h, uint8_t* out, size_t cap) {
    if (!gray8 || !out || w <= 0 || h <= 0 || (w & 1)) return 0;
    const size_t packed = (size_t)(w / 2) * h;

    size_t o = 0;
    size_t litStart = 0;         // index in `out` of the open literal's length
    int    litLen   = -1;        // -1 = no literal packet open
    size_t i = 0;
    while (i < packed) {
        const uint8_t b = packedAt(gray8, w, i);
        size_t j = i + 1;
        int run = 1;
        while (j < packed && run < 255 && packedAt(gray8, w, j) == b) { run++; j++; }

        if (run >= 3) {          // a run pays for its 2-byte packet
            litLen = -1;         // close any open literal
            if (o + 2 > cap) return 0;
            out[o++] = (uint8_t)run;
            out[o++] = b;
            i = j;
            continue;
        }
        // Otherwise append to a literal packet, opening (or rolling over) one.
        if (litLen < 0 || litLen == 255) {
            if (o + 2 > cap) return 0;
            out[o++] = 0;
            litStart = o;
            out[o++] = 0;
            litLen = 0;
        }
        if (o + 1 > cap) return 0;
        out[o++] = b;
        out[litStart] = (uint8_t)(++litLen);
        i++;
    }
    return o;
}

}  // namespace fb
