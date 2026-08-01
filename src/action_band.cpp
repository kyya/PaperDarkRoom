// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "action_band.h"
#include "cjk_text.h"
#include <M5Unified.h>
#include <string.h>

namespace {

// The 1px dashed unavailable frame: runs of 4 lit pixels every 8, clipped at
// the far edge, on all four sides. Private on purpose — drawFrame() is the only
// way in, so a caller picks "solid or dashed" and never gets to hand-assemble a
// frame out of halves (which is how the seven copies this replaced got started).
// This is the drawFastLine formulation the assign/path pages used; it emits ~w/4
// line calls instead of w drawPixel calls and produces byte-identical output to
// the drawPixel formulation the other five copies used (verified: both light
// exactly the offsets with i % 8 < 4).
void drawDashedRect(m5gfx::M5Canvas& c, int x, int y, int w, int h) {
    int x1 = x + w - 1, y1 = y + h - 1;
    for (int px = x; px <= x1; px += 8) {
        int len = (x1 - px + 1 < 4) ? (x1 - px + 1) : 4;
        c.drawFastHLine(px, y,  len, TFT_BLACK);
        c.drawFastHLine(px, y1, len, TFT_BLACK);
    }
    for (int py = y; py <= y1; py += 8) {
        int len = (y1 - py + 1 < 4) ? (y1 - py + 1) : 4;
        c.drawFastVLine(x,  py, len, TFT_BLACK);
        c.drawFastVLine(x1, py, len, TFT_BLACK);
    }
}

// Count the lines cjk::drawWrapped would emit for `utf8` at width w — same
// greedy CJK/space wrap, no drawing (room_page.cpp's log stream keeps its own
// copy of this for an unrelated purpose — the log band's line budget — so this
// one stays local to the cost/yield subtitle it serves here).
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

void action_band::drawFrame(m5gfx::M5Canvas& c, const pages::Rect& r, bool enabled) {
    if (enabled) {
        c.drawRect(r.x, r.y, r.w, r.h, TFT_BLACK);
        c.drawRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, TFT_BLACK);
    } else {
        drawDashedRect(c, r.x, r.y, r.w, r.h);
    }
}

int action_band::titleBoxY(int top, int h) {
    return top + (h - TITLE_GLYPH) / 2 - INK_NUDGE;
}

void action_band::draw(m5gfx::M5Canvas& c, const pages::Rect& r,
                       const char* title, const char* subtitle, bool enabled,
                       int coolLeft, int coolTotal) {
    drawFrame(c, r, enabled);
    if (!title) title = "";

    bool hasSub = subtitle && subtitle[0];

    // Subtitle wrap width. Deliberately the FULL band width, not the SIDE_PAD-
    // inset one the title is measured against: three Room craft costs (贸易站
    // "-400 木头  -100 毛皮", 工坊, 钢剑) come out at exactly 240px — the width
    // of a Room/Outside cell to the pixel — so wrapping at the inset 228px would
    // break all three onto a second line for a 12px overhang, and a 2-line block
    // is the one layout this band height has no slack for (see the budget below).
    // Measured over every cost line the game can produce, nothing wraps at 240.
    int subLines = hasSub ? wrapLineCount(subtitle, r.w, SUB_SCALE) : 0;

    // ---- vertical budget -------------------------------------------------
    // Centre exactly what the band carries — the whole rule, see action_band.h
    // "ONE BAND, ONE CENTERING". The title always occupies a TITLE_GLYPH(36) box
    // even when it had to shrink to 24px (see the downgrade note in the header),
    // so the block height, and therefore the subtitle's y, never depends on the
    // title's actual scale.
    //
    //   block      = 36 + (hasSub ? 6 + lines*24 : 0)
    //   titleBox y = top + (h - block)/2 - INK_NUDGE
    //                (the no-subtitle case IS titleBoxY(), called below, so a
    //                 plain band and an assign/path stepper row agree exactly)
    //
    // Room/Outside (h=96), the tightest case:
    //   no subtitle : block 36 -> title box [29,65), cooldown bar [84,92),
    //                 inner frame ring at [94,96). 19px clear above the bar.
    //   1 line      : block 66 -> title box [14,50), subtitle [56,80),
    //                 cooldown bar [84,92). 4px clear above the bar, 2px below.
    //                 (The 15px step between this and the case above is the
    //                 accepted cost of per-button centring — do not "fix" it.)
    //   2 lines     : block 90 -> title box [2,38), subtitles [44,92). The box
    //                 abuts the top frame rows [0,2) but the title's INK starts a
    //                 grid pixel in, at 5, so 3px of white still reads above it
    //                 and 2px below the last subtitle. That it fits at all is
    //                 exactly why ROOM_BTN_H/ACT_H are 96 and not the §9.3 floor
    //                 of 80. No cooldown bar can collide — see action_band.h.
    // Trade (h=80) / the modals (h=84) only ever carry a 1-line subtitle and
    // never a cooldown, so they sit well inside the same arithmetic.
    int block  = TITLE_GLYPH + (hasSub ? SUBGAP + subLines * SUB_GLYPH : 0);
    int titleY = hasSub ? r.y + (r.h - block) / 2 - INK_NUDGE
                        : titleBoxY(r.y, r.h);

    // Narrow-cell guard: shrink THIS title to the subtitle scale rather than let
    // it run under the frame. Centred inside the reserved 36px slot so the band
    // geometry above is untouched.
    int tScale = TITLE_SCALE;
    int tw     = cjk::textWidth(title, TITLE_SCALE);
    int usable = r.w - 2 * FRAME - 2 * SIDE_PAD;
    if (tw > usable) { tScale = SUB_SCALE; tw = cjk::textWidth(title, SUB_SCALE); }
    int inkY = titleY + (TITLE_GLYPH - 12 * tScale) / 2;
    cjk::drawText(c, r.x + (r.w - tw) / 2, inkY, title, tScale);

    if (hasSub) {
        int subY = titleY + TITLE_GLYPH + SUBGAP;
        if (subLines <= 1) {
            int sw = cjk::textWidth(subtitle, SUB_SCALE);
            cjk::drawText(c, r.x + (r.w - sw) / 2, subY, subtitle, SUB_SCALE);
        } else {
            cjk::drawWrapped(c, r.x, subY, r.w, subtitle, SUB_SCALE, SUB_GLYPH);
        }
    }

    if (coolTotal > 0 && coolLeft > 0) {              // draining cooldown
        int barX0 = r.x + 12, barX1 = r.x + r.w - 12;
        // Anchored to the band's own bottom, not to the subtitle: with the
        // subtitle slot reserved across a grid, "just below the cost line" and
        // "hugging the band's bottom" are the SAME place, so the priced-vs-free
        // split the pre-v0.12 renderer needed collapses into one rule.
        int barY = r.y + r.h - BAR_GUTTER;
        c.drawRect(barX0, barY, barX1 - barX0, BAR_H, TFT_BLACK);
        int inner = barX1 - barX0 - 4;
        int fw = (int)((int64_t)inner * coolLeft / coolTotal);   // drains L-anchored
        if (fw > 0) c.fillRect(barX0 + 2, barY + 2, fw, BAR_H - 4, TFT_BLACK);
    }
}
