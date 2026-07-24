// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "action_band.h"
#include "cjk_text.h"
#include <M5Unified.h>
#include <string.h>

namespace {

// 1px dashed rectangle, 4px on / 4px off — the unavailable-band frame (Room/
// Outside/Assign/Path/Trade all draw this identically; kept local here since
// only the two callers of action_band::draw need it).
void drawDashedRect(m5gfx::M5Canvas& c, int x, int y, int w, int h) {
    const int on = 4, per = 8;
    int xr = x + w - 1, yb = y + h - 1;
    for (int i = 0; i < w; i++)
        if (i % per < on) { c.drawPixel(x + i, y, TFT_BLACK);
                            c.drawPixel(x + i, yb, TFT_BLACK); }
    for (int i = 0; i < h; i++)
        if (i % per < on) { c.drawPixel(x, y + i, TFT_BLACK);
                            c.drawPixel(xr, y + i, TFT_BLACK); }
}

// Count the lines cjk::drawWrapped would emit for `utf8` at width w — same
// greedy CJK/space wrap, no drawing (room_page.cpp's log stream keeps its own
// copy of this for an unrelated purpose — the log band's line budget — so
// this one stays local to the cost/yield subtitle it serves here).
int wrapLineCount(const char* utf8, int w, int scale) {
    constexpr int MAX = 96;
    static uint32_t cps[MAX];
    static int      advs[MAX];
    int n = 0;
    const char* p = utf8;
    while (n < MAX) {
        const unsigned char* s = (const unsigned char*)p;
        if (*s == 0) break;
        int32_t cp; int len;
        if (*s < 0x80)              { cp = *s;        len = 1; }
        else if ((*s >> 5) == 0x6)  { cp = *s & 0x1F; len = 2; }
        else if ((*s >> 4) == 0xE)  { cp = *s & 0x0F; len = 3; }
        else if ((*s >> 3) == 0x1E) { cp = *s & 0x07; len = 4; }
        else                        { cp = *s;        len = 1; }
        for (int i = 1; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80) { len = i; break; }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        char one[5]; memcpy(one, s, (size_t)len); one[len] = 0;
        cps[n]  = (uint32_t)cp;
        advs[n] = cjk::textWidth(one, scale);
        p += len;
        n++;
    }
    if (n == 0) return 1;
    auto breakAfter = [](uint32_t cp) { return cp == 0x20 || cp >= 0x2000; };
    int lines = 0, lineStart = 0;
    while (lineStart < n) {
        int curW = 0, lastBreak = -1, j = lineStart;
        for (; j < n; j++) {
            if (curW + advs[j] > w && j > lineStart) break;
            curW += advs[j];
            if (breakAfter(cps[j])) lastBreak = j;
        }
        int lineEnd = (j >= n) ? n
                    : (lastBreak >= lineStart) ? lastBreak + 1 : j;
        lines++;
        lineStart = lineEnd;
        if (lineStart < n && cps[lineStart] == 0x20) lineStart++;
    }
    return lines < 1 ? 1 : lines;
}

}  // namespace

int action_band::labelY(int h) {
    // Derived from the SAME bottom-anchor equation draw() uses for a 1-line
    // subtitle (costBottom - block1), so a free-label band and a priced
    // 1-line-subtitle band always agree on where the label sits — see the
    // header note.
    constexpr int block1 = BTN_GLYPH + SUBGAP + GLYPH;
    return h - SUB_GUTTER - block1;
}

void action_band::draw(m5gfx::M5Canvas& c, int x0, int top, int w, int h,
                       const char* label, const char* cost, bool hasCost,
                       bool enabled, int coolLeft, int coolTotal) {
    if (enabled) {
        c.drawRect(x0, top, w, h, TFT_BLACK);
        c.drawRect(x0 + 1, top + 1, w - 2, h - 2, TFT_BLACK);
    } else {
        drawDashedRect(c, x0, top, w, h);
    }

    int costBottom = top + h - SUB_GUTTER;   // fixed for EVERY priced band,
                                             // whether or not it's coolable
    if (hasCost) {
        int lines = wrapLineCount(cost, w, SCALE);   // 1, or 2 for Room's
                                                      // priciest multi-
                                                      // resource crafts
        int block = BTN_GLYPH + SUBGAP + lines * GLYPH;
        int ly = costBottom - block;    // == top + labelY(h) when lines<=1
        int lw = cjk::textWidth(label, BTN_SCALE);
        cjk::drawText(c, x0 + (w - lw) / 2, ly, label, BTN_SCALE);
        int costY = ly + BTN_GLYPH + SUBGAP;
        if (lines <= 1) {
            int cw = cjk::textWidth(cost, SCALE);
            cjk::drawText(c, x0 + (w - cw) / 2, costY, cost, SCALE);
        } else {
            cjk::drawWrapped(c, x0, costY, w, cost, SCALE, GLYPH);
        }
    } else {
        int ly = top + labelY(h);
        int lw = cjk::textWidth(label, BTN_SCALE);
        cjk::drawText(c, x0 + (w - lw) / 2, ly, label, BTN_SCALE);
    }

    if (coolTotal > 0 && coolLeft > 0) {                      // draining cooldown
        int barX0 = x0 + 12, barX1 = x0 + w - 12;
        // A priced band hangs its bar just below the fixed subtitle bottom
        // (costBottom); a free coolable band hugs the band's own bottom edge.
        int barY = hasCost ? costBottom + 4 : top + h - 16;
        int barH = 8;
        c.drawRect(barX0, barY, barX1 - barX0, barH, TFT_BLACK);
        int inner = barX1 - barX0 - 4;
        int fw = (int)((int64_t)inner * coolLeft / coolTotal);   // drains L-anchored
        if (fw > 0) c.fillRect(barX0 + 2, barY + 2, fw, barH - 4, TFT_BLACK);
    }
}
