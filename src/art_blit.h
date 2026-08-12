// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Shared decoder for the RLE-over-4bpp art blobs (event_art_data.h,
// enemy_art_data.h). Lifted verbatim out of event_modal.cpp when the fight
// overlay became a second caller — the encoding is defined once by
// tools/gen_event_art.py, so the decoder that has to match it byte for byte
// lives in exactly one place too.
#pragma once
#include <stdint.h>
#include <M5GFX.h>

namespace art {

// Blit one RLE/4bpp plate into `c` with its top-left at (x, y).
//
// `rle` is the encoded stream, `w`/`h` its pixel dimensions; `w` MUST be even
// (two pixels per packed byte, no row padding). Passing nullptr is a no-op, so
// a missing plate just leaves the area untouched.
//
// `srcY0`/`rows` crop vertically: only source rows [srcY0, srcY0+rows) are
// written, landing at y..y+rows. Default (0 / -1) draws the whole plate. The
// fight overlay uses this to letterbox one stored plate into whatever height the
// attack grid left it — the stream is sequential, so skipped rows are still
// decoded, just not stored. Cropping never changes where a kept row lands
// horizontally, so a centred subject stays centred.
//
// The two layers are unpacked in ONE forward pass straight into the canvas — no
// intermediate buffer. An 8bpp scratch copy would be w*h bytes of PSRAM to
// allocate, memset and free on every draw just to memcpy it away again; there is
// no reason to touch the heap when the destination rows are already sitting
// there. The canvas must be grayscale_8bit (main.cpp), i.e. one byte of luma per
// pixel with a row stride of c.width(), so a nibble expands with a *17
// (15 -> 255 paper white, 0 -> ink black) and lands directly.
//
// The cursor is kept in PACKED bytes: `bx` counts byte-pairs across the row and
// wraps into the next canvas row at w/2, so a single run can span rows the way
// it does in the stream.
void blit(m5gfx::M5Canvas& c, const uint8_t* rle, int w, int h, int x, int y,
          int srcY0 = 0, int rows = -1);

}  // namespace art
