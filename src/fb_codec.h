// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Framebuffer encoder for the FBGet screen grab (ble_link::fbSend). Pure logic,
// no Arduino/BLE, so it host-compiles into the smoke test — the wire format has
// to match tools/ble_fbget.py's decoder byte for byte, and that is worth proving
// on the host rather than by squinting at a PNG.
//
// The format is the SAME 4bpp + RLE the art blobs use (tools/gen_event_art.py
// writes it, art::blit reads it), which is why the host decoder was already
// written before this existed:
//   pack   two pixels per byte, HIGH nibble = the LEFT (even x) pixel
//   encode 0x00 <n> <n bytes>   literal run, n = 1..255
//          <n>  <val>           n copies of val, n = 1..255
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace fb {

// Worst-case encoded size for a w*h frame: every packed byte lands in a literal
// packet, and each such packet carries TWO header bytes (0x00 + length) per 255
// payload bytes. Underestimating this is not academic — a high-frequency frame
// (dithered art, noise) really does hit the all-literal path, and a short buffer
// makes encode() bail and the grab fail.
constexpr size_t encodedCap(int w, int h) {
    return (size_t)(w / 2) * h + 2 * (((size_t)(w / 2) * h) / 255 + 1) + 16;
}

// Encode w*h bytes of 8-bit luma (row stride == w) into `out`. `w` must be even.
// Returns the encoded length, or 0 if the arguments are bad or `cap` is short —
// callers treat 0 as "do not transmit".
size_t encode(const uint8_t* gray8, int w, int h, uint8_t* out, size_t cap);

}  // namespace fb
