// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// CJK/ASCII text renderer + tr() translation lookup. This is the SOLE includer
// of the two big generated tables (strings_zh.h, cjk_font12.h) so the flat
// string table and bitmap pool live in exactly one translation unit.
#include "cjk_text.h"
#include "cjk_font12.h"
#include "strings_zh.h"

#include <M5Unified.h>
#include <string.h>

// --- tr(): binary search the en_key-sorted official translation table -------
const char* tr(const char* en_key) {
    int lo = 0, hi = (int)ADR_STRINGS_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        int c = strcmp(en_key, ADR_STRINGS[mid].en_key);
        if (c == 0) return ADR_STRINGS[mid].zh;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return en_key;   // fall back to the English key
}

namespace {

// Baseline sits ASCENT px below the line-box top; ASCENT = max(-yoff) in the
// font (full-height CJK glyphs have yoff = -11). DEF_ADV advances an absent
// glyph so a missing char leaves a gap, never overlaps.
constexpr int ASCENT = 11;
constexpr int DEF_ADV = 6;

// Decode one UTF-8 codepoint at *p, advancing *p past it. Returns the cp, or
// -1 at the terminating NUL. Malformed lead/continuation bytes degrade to a
// single Latin-1 byte so a bad string can't run the pointer away.
int32_t nextCp(const char** p) {
    const unsigned char* s = (const unsigned char*)*p;
    if (*s == 0) return -1;
    int32_t cp; int n;
    if (*s < 0x80)            { cp = *s;        n = 1; }
    else if ((*s >> 5) == 0x6){ cp = *s & 0x1F; n = 2; }
    else if ((*s >> 4) == 0xE){ cp = *s & 0x0F; n = 3; }
    else if ((*s >> 3) == 0x1E){cp = *s & 0x07; n = 4; }
    else                      { cp = *s;        n = 1; }   // stray byte
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) { n = i; break; }        // truncated
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *p = (const char*)(s + n);
    return cp;
}

// Binary search the cp-sorted glyph table.
const CjkGlyph* find(uint32_t cp) {
    int lo = 0, hi = (int)cjkFont12Count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint16_t c = cjkFont12Glyphs[mid].cp;
        if (c == cp) return &cjkFont12Glyphs[mid];
        if (c < cp) lo = mid + 1; else hi = mid - 1;
    }
    return nullptr;
}

int advOf(uint32_t cp) {
    const CjkGlyph* g = find(cp);
    return g ? g->adv : DEF_ADV;
}

// A line may break AFTER a space or any wide (CJK ideograph / full-width mark)
// codepoint; a wide cp may also START a line. cp >= 0x2000 catches CJK
// punctuation (…—，。), full-width forms and the ideograph blocks; ASCII words
// stay intact between spaces.
inline bool isWide(uint32_t cp)  { return cp >= 0x2000; }
inline bool breakAfter(uint32_t cp) { return cp == 0x20 || isWide(cp); }

}  // namespace

int cjk::drawText(m5gfx::M5Canvas& c, int x, int y, const char* utf8,
                  int scale, uint16_t color) {
    int pen = x;
    int baseY = y + ASCENT * scale;
    const char* p = utf8;
    for (;;) {
        int32_t cp = nextCp(&p);
        if (cp < 0) break;
        const CjkGlyph* g = find((uint32_t)cp);
        if (!g) { pen += DEF_ADV * scale; continue; }
        const uint8_t* bm = cjkFont12Bitmaps + g->off;
        int bit = 0;
        for (int yy = 0; yy < g->h; yy++) {
            for (int xx = 0; xx < g->w; xx++) {
                if ((bm[bit >> 3] >> (7 - (bit & 7))) & 1) {
                    int px = pen + (g->xoff + xx) * scale;
                    int py = baseY + (g->yoff + yy) * scale;
                    if (scale == 1) c.drawPixel(px, py, color);
                    else            c.fillRect(px, py, scale, scale, color);
                }
                bit++;
            }
        }
        pen += g->adv * scale;
    }
    return pen;
}

int cjk::textWidth(const char* utf8, int scale) {
    int pen = 0;
    const char* p = utf8;
    for (;;) {
        int32_t cp = nextCp(&p);
        if (cp < 0) break;
        pen += advOf((uint32_t)cp) * scale;
    }
    return pen;
}

int cjk::drawWrapped(m5gfx::M5Canvas& c, int x, int y, int w, const char* utf8,
                     int scale, int line_h, uint16_t color) {
    if (line_h <= 0) line_h = 15 * scale;

    // Decode the whole string once: codepoint, advance, and byte offset of each
    // char's start (offs[n] = total length) so a wrapped line can be re-emitted
    // as a NUL-terminated slice of the original bytes.
    constexpr int MAX = 256;   // longest official zh line is < 80 cps
    static uint32_t cps[MAX];
    static int      advs[MAX];
    static int      offs[MAX + 1];
    int n = 0;
    const char* p = utf8;
    while (n < MAX) {
        const char* start = p;
        int32_t cp = nextCp(&p);
        if (cp < 0) break;
        cps[n] = (uint32_t)cp;
        advs[n] = advOf((uint32_t)cp) * scale;
        offs[n] = (int)(start - utf8);
        n++;
    }
    offs[n] = (int)(p - utf8);

    char buf[1024];
    auto flush = [&](int a, int b, int ly) {
        if (a >= b) return;
        int len = offs[b] - offs[a];
        if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
        memcpy(buf, utf8 + offs[a], len);
        buf[len] = 0;
        cjk::drawText(c, x, ly, buf, scale, color);
    };

    int ly = y;
    int lineStart = 0;   // first cp index of the current line
    int i = 0;
    while (i < n) {
        // Greedily fill [lineStart .. ) until the next cp would overflow w.
        int curW = 0;
        int lastBreak = -1;    // last index whose char permits a break AFTER it
        int j = lineStart;
        for (; j < n; j++) {
            // A wide cp may start a fresh line, so a break BEFORE it is also ok
            // when we've already got content — treated via breakAfter(prev).
            if (curW + advs[j] > w && j > lineStart) break;
            curW += advs[j];
            if (breakAfter(cps[j])) lastBreak = j;
        }
        int lineEnd;
        if (j >= n) {
            lineEnd = n;                       // last line, everything fits
        } else if (lastBreak >= lineStart) {
            lineEnd = lastBreak + 1;           // break just after the opportunity
        } else {
            lineEnd = j;                       // no break point: hard split
        }
        flush(lineStart, lineEnd, ly);
        ly += line_h;
        lineStart = lineEnd;
        i = lineEnd;
        // Skip a single leading space carried to the next line.
        if (lineStart < n && cps[lineStart] == 0x20) { lineStart++; i++; }
    }
    return ly;
}
