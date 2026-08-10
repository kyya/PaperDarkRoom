// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Host test for the FBGet framebuffer encoder (src/fb_codec.cpp).
//
// What it actually guards: the encoder and tools/fbget.py's decoder are two
// independent implementations of one wire format, and nothing about a corrupt
// screenshot says WHICH side is wrong. So the decoder below is a transcription
// of the Python one, and every case asserts decode(encode(x)) == pack(x) against
// a packer written separately from the encoder's own packing.
//
// Build (clang++ is the host toolchain on this box):
//   clang++ -std=c++17 -I src tools/fb_codec_test.cpp src/fb_codec.cpp \
//           -o fb_codec_test.exe
#include "fb_codec.h"
#include <cstdio>
#include <vector>

static int pass = 0, fail = 0;
#define CHECK(c, m) do { if (c) { pass++; } else { fail++; printf("  [FAIL] %s\n", m); } } while (0)

// Mirror of tools/fbget.py unrle().
static std::vector<uint8_t> unrle(const uint8_t* d, size_t n, size_t expect) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < n && out.size() < expect) {
        if (d[i] == 0) {                       // literal packet
            size_t k = d[i + 1];
            for (size_t j = 0; j < k; j++) out.push_back(d[i + 2 + j]);
            i += 2 + k;
        } else {                               // run packet
            size_t r = d[i];
            for (size_t j = 0; j < r; j++) out.push_back(d[i + 1]);
            i += 2;
        }
    }
    return out;
}

// What the packed stream should be, derived independently of the encoder.
static std::vector<uint8_t> refPack(const std::vector<uint8_t>& g, int w, int h) {
    std::vector<uint8_t> p;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x += 2)
            p.push_back((uint8_t)((g[y * w + x] & 0xF0) | (g[y * w + x + 1] >> 4)));
    return p;
}

static void roundTrip(const char* name, std::vector<uint8_t> g, int w, int h) {
    std::vector<uint8_t> out(fb::encodedCap(w, h));
    size_t n = fb::encode(g.data(), w, h, out.data(), out.size());
    CHECK(n > 0, name);
    auto want = refPack(g, w, h);
    auto got  = unrle(out.data(), n, want.size());
    CHECK(got.size() == want.size(), "decoded length matches packed length");
    CHECK(got == want, "decode(encode(x)) == packed(x)");
    printf("     %-22s raw %6zu -> rle %6zu (%.1fx)\n", name, want.size(), n,
           n ? (double)want.size() / n : 0.0);
}

int main() {
    const int W = 540, H = 960;                 // the panel's real geometry
    printf("== FBGet encoder round-trips against the host decoder ==\n");
    roundTrip("all white", std::vector<uint8_t>(W * H, 0xFF), W, H);
    roundTrip("all black", std::vector<uint8_t>(W * H, 0x00), W, H);
    // Worst case: no two adjacent packed bytes equal, so every byte is a
    // literal. This is what caught encodedCap() under-counting the 2-byte packet
    // headers — a dithered plate really does land here and would have failed the
    // grab with FBERR.
    {
        std::vector<uint8_t> g(W * H);
        for (size_t i = 0; i < g.size(); i++) g[i] = (uint8_t)((i * 37) & 0xFF);
        roundTrip("noise (all literals)", g, W, H);
    }
    {   // Something shaped like a real page: flats, bars, a mid grey.
        std::vector<uint8_t> g(W * H, 0xFF);
        for (int y = 100; y < 200; y++) for (int x = 0; x < W; x++) g[y * W + x] = 0x00;
        for (int y = 300; y < 340; y++) for (int x = 24; x < 516; x++) g[y * W + x] = 0x88;
        roundTrip("ui-ish bands", g, W, H);
    }

    printf("== guards ==\n");
    {
        std::vector<uint8_t> g(16 * 4, 0xFF);
        std::vector<uint8_t> o(4);
        CHECK(fb::encode(g.data(), 16, 4, o.data(), 1) == 0, "short cap -> 0");
        CHECK(fb::encode(nullptr, 16, 4, o.data(), o.size()) == 0, "null src -> 0");
        // Odd width would need row padding the format does not have.
        CHECK(fb::encode(g.data(), 15, 4, o.data(), o.size()) == 0, "odd width -> 0");
    }

    printf("\n==== %d passed, %d failed ====\n", pass, fail);
    return fail ? 1 : 0;
}
