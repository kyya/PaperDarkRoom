// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "action_band.h"
#include "cjk_text.h"
#include <M5Unified.h>
#include <stdio.h>
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

// Split a page's cost/yield string into its individual entries. Every costLine
// in the firmware (room/outside/trade/event/setpiece) builds the same shape —
// "-200 木头  -10 毛皮", entries joined by TWO spaces — so splitting on that
// separator here gives 变体 B its one-entry-per-line right column WITHOUT
// touching nine call-site signatures to pass an array instead. The final slot
// keeps whatever is left verbatim, so nothing is ever dropped even if a future
// table exceeds MAX_SUB_LINES entries.
int splitCost(const char* s, char out[][32], int maxN) {
    int n = 0;
    while (n < maxN - 1) {
        const char* sep = strstr(s, "  ");
        if (!sep) break;
        int len = (int)(sep - s);
        if (len > 31) len = 31;
        memcpy(out[n], s, (size_t)len);
        out[n][len] = 0;
        n++;
        s = sep + 2;
        while (*s == ' ') s++;             // tolerate a 3+ space join
    }
    snprintf(out[n], 32, "%s", s);
    return n + 1;
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
    char subLine[MAX_SUB_LINES][32];
    int  subN = hasSub ? splitCost(subtitle, subLine, MAX_SUB_LINES) : 0;

    // Widest cost entry — what the title actually has to share the row with.
    int subW = 0;
    for (int i = 0; i < subN; i++) {
        int w = cjk::textWidth(subLine[i], SUB_SCALE);
        if (w > subW) subW = w;
    }

    // ---- horizontal budget: title | gap | costs, inside the padded width ----
    // The guard measures the REAL competition (title + MID_GAP + widest entry),
    // not the title alone, so unlike its pre-变体-B ancestor it genuinely fires
    // on the 240px Room/Outside cells. Measured over every label+cost the game
    // can produce (240px cells -> 224px usable), exactly FOUR titles overflow
    // and drop to 24px, and all four then fit:
    //   狩猎小屋  144 + 8 + 108 = 260 > 224   -> 24px: 96 + 8 + 108 = 212
    //   炼钢坊    108 + 8 + 120 = 236 > 224   -> 24px: 72 + 8 + 120 = 200
    //   军械坊    108 + 8 + 120 = 236 > 224   -> 24px: 72 + 8 + 120 = 200
    //   查看陷阱  144 + 8 +  96 = 248 > 224   -> 24px: 96 + 8 +  96 = 200
    // Nothing on the 492px bands (Trade / event / setpiece) comes close: the
    // widest there is 购买外星合金 at 216 + 8 + 120 = 344 against 476 usable.
    int tScale = TITLE_SCALE;
    int tw     = cjk::textWidth(title, TITLE_SCALE);
    int usable = r.w - 2 * EDGE_PAD;
    int need   = hasSub ? tw + MID_GAP + subW : tw;
    if (need > usable) { tScale = SUB_SCALE; tw = cjk::textWidth(title, SUB_SCALE); }

    // ---- vertical budget ---------------------------------------------------
    // The title's line box is ALWAYS titleBoxY(): one y for every band in the
    // app, priced or not (see action_band.h). A shrunk title centres its ink
    // inside that unchanged 36px box, so the downgrade is purely horizontal.
    //
    // The cost column is its own centred block, growing downward:
    //   block = n * 24, top = r.y + (h - block)/2 - inkNudge(SUB_SCALE)
    // Worst case is 3 entries (every cost table is cost[3]) in the 96px
    // Room/Outside cell: block 72 -> [11, 83), and the cooldown bar starts at
    // r.y + 96 - BAR_GUTTER = 84. So even a 3-line cost clears the bar by 1px,
    // which is what lets the two features coexist without a reservation rule.
    // Trade (h=80) -> [3, 75) vs the 78px inner frame ring; the modals (h=84) ->
    // [5, 77) vs 82. Both clear.
    int ty   = titleBoxY(r.y, r.h);
    int inkY = ty + (TITLE_GLYPH - 12 * tScale) / 2;

    if (hasSub) {
        cjk::drawText(c, r.x + EDGE_PAD, inkY, title, tScale);      // left column
        int subTop = r.y + (r.h - subN * SUB_GLYPH) / 2 - inkNudge(SUB_SCALE);
        int right  = r.x + r.w - EDGE_PAD;
        for (int i = 0; i < subN; i++) {
            int lw = cjk::textWidth(subLine[i], SUB_SCALE);
            cjk::drawText(c, right - lw, subTop + i * SUB_GLYPH, subLine[i], SUB_SCALE);
        }
    } else {
        // No cost column -> the title centres horizontally, unchanged from every
        // previous version (the user asked for this explicitly).
        cjk::drawText(c, r.x + (r.w - tw) / 2, inkY, title, tScale);
    }

    if (coolTotal > 0 && coolLeft > 0) {              // draining cooldown
        int barX0 = r.x + 12, barX1 = r.x + r.w - 12;
        int barY = r.y + r.h - BAR_GUTTER;
        c.drawRect(barX0, barY, barX1 - barX0, BAR_H, TFT_BLACK);
        int inner = barX1 - barX0 - 4;
        int fw = (int)((int64_t)inner * coolLeft / coolTotal);   // drains L-anchored
        if (fw > 0) c.fillRect(barX0 + 2, barY + 2, fw, BAR_H - 4, TFT_BLACK);
    }
}
