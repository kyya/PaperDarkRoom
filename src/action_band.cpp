// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "action_band.h"
#include "cjk_text.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>

namespace {

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

// The band's box, in both states. Enabled is the 2px black ring, now ROUNDED:
// two concentric drawRoundRect calls, the inner one inset 1px AND at CORNER_R-1.
// The radius must drop with the inset or the two arcs are no longer concentric —
// an inner arc of the same radius crosses the outer one and leaves a visible
// notch on each diagonal, which is exactly what the old square double-drawRect
// never had to worry about. Disabled keeps the identical geometry and drops to a
// single 1px stroke in DISABLED_FRAME grey; it used to be a black dashed square,
// but dashes and rounded corners cannot both survive a 4-on/4-off run through an
// arc, and grey says "unavailable" with far less visual noise (see the grey
// ladder in action_band.h).
void action_band::drawFrame(m5gfx::M5Canvas& c, const pages::Rect& r, bool enabled) {
    if (enabled) {
        c.drawRoundRect(r.x, r.y, r.w, r.h, CORNER_R, TFT_BLACK);
        c.drawRoundRect(r.x + 1, r.y + 1, r.w - 2, r.h - 2, CORNER_R - 1, TFT_BLACK);
    } else {
        c.drawRoundRect(r.x, r.y, r.w, r.h, CORNER_R, DISABLED_FRAME);
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
    // on the 240px Room/Outside cells. RE-MEASURED at EDGE_PAD 16 / MID_GAP 4
    // (240px cells -> 208px usable), over every label+cost the game can produce —
    // every Room craftable at its worst-case wood surcharge, both fire verbs, and
    // Outside's two priced verbs. EIGHT titles overflow and drop to 24px (it was
    // four back at 8/8), and all eight then fit:
    //   狩猎小屋  144 + 4 + 108 = 256 > 208  -> 24px: 96 + 4 + 108 = 208
    //   贸易站    108 + 4 + 108 = 220 > 208  -> 24px: 72 + 4 + 108 = 184
    //   制革屋    108 + 4 + 108 = 220 > 208  -> 24px: 72 + 4 + 108 = 184
    //   熏肉房    108 + 4 + 108 = 220 > 208  -> 24px: 72 + 4 + 108 = 184
    //   双肩包    108 + 4 + 108 = 220 > 208  -> 24px: 72 + 4 + 108 = 184
    //   炼钢坊    108 + 4 + 120 = 232 > 208  -> 24px: 72 + 4 + 120 = 196
    //   军械坊    108 + 4 + 120 = 232 > 208  -> 24px: 72 + 4 + 120 = 196
    //   查看陷阱  144 + 4 +  96 = 244 > 208  -> 24px: 96 + 4 +  96 = 196
    // 狩猎小屋 is the BINDING case for the whole layout and lands on 208 EXACTLY —
    // it is the single constraint behind both numbers in action_band.h
    // (2*EDGE_PAD + MID_GAP <= 36). It is also the only band in the game whose
    // drawn middle gap is as small as MID_GAP: the next tightest are 16px (小屋,
    // 炼钢坊, 军械坊, 车队, 查看陷阱) and everything else is >=28px, which is why
    // spending the middle gap to buy edge padding was the right trade.
    // Worst-case costs fold in the count-scaling wood surcharge (陷阱 -100 木头 at
    // 9 built, 小屋 -1050 木头 at 19) and 查看陷阱's full ten traps (-10 诱饵);
    // none of those is the binding case.
    // Nothing on the 492px bands (Trade / event / setpiece) comes close: the
    // widest there is 购买外星合金 at 216 + 4 + 120 = 340 against 460 usable.
    // The 240px fight grid is unaffected too — every one of its labels is
    // subtitle-less, so it is measured against `tw` alone (108px at worst).
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
    // RE-VERIFIED after the bar was slimmed 8px -> BAR_H 6 (2026-08): the bar is
    // still anchored by its TOP at r.y + h - BAR_GUTTER, so shrinking it only
    // moved its BOTTOM up — the 3-line worst case still ends at 83 against a bar
    // top of 84, unchanged 1px net. What did change is the slack UNDER the bar:
    // it now occupies [84, 90) instead of [84, 92), so it clears the 2px frame
    // ring's inner edge at 94 by 4px instead of 2 and no longer looks welded to
    // the band's bottom. The rounded corners cost nothing here either: at rows
    // 84-90 the r=8 bottom-left arc has pulled the frame in by at most ~1px from
    // r.x, and the bar starts at r.x + EDGE_PAD.
    // Trade (h=80) -> [3, 75) vs the 78px inner frame ring; the modals (h=84) ->
    // [5, 77) vs 82. Both clear (neither ever passes a cooldown).
    int ty   = titleBoxY(r.y, r.h);
    int inkY = ty + (TITLE_GLYPH - 12 * tScale) / 2;

    // ---- ink tiers ---------------------------------------------------------
    // The title is the band's subject and stays pure black when the action is
    // available; the cost column steps back one tier so the eye lands on the verb
    // first. Disabled puts BOTH columns on one lighter tier — still readable (a
    // player has to be able to see WHAT they cannot afford, and by how much), but
    // unmistakably off. See the grey ladder in action_band.h.
    uint16_t titleInk = enabled ? TFT_BLACK : DISABLED_INK;
    uint16_t costInk  = enabled ? COST_INK  : DISABLED_INK;

    if (hasSub) {
        cjk::drawText(c, r.x + EDGE_PAD, inkY, title, tScale, titleInk);  // left col
        int subTop = r.y + (r.h - subN * SUB_GLYPH) / 2 - inkNudge(SUB_SCALE);
        int right  = r.x + r.w - EDGE_PAD;
        for (int i = 0; i < subN; i++) {
            int lw = cjk::textWidth(subLine[i], SUB_SCALE);
            cjk::drawText(c, right - lw, subTop + i * SUB_GLYPH, subLine[i],
                          SUB_SCALE, costInk);
        }
    } else {
        // No cost column -> the title centres horizontally, unchanged from every
        // previous version (the user asked for this explicitly).
        cjk::drawText(c, r.x + (r.w - tw) / 2, inkY, title, tScale, titleInk);
    }

    if (coolTotal > 0 && coolLeft > 0) {              // draining cooldown
        // Same inset as the text columns (EDGE_PAD), so the bar's ends line up
        // with the title's left edge and the cost column's right edge instead of
        // being a third tuned number. It was a literal 12 before; deriving it means
        // the bar tracks the padding instead of drifting out of alignment with the
        // text every time the padding is retuned. At EDGE_PAD 16 the bar is 8px
        // shorter than in v0.16 — it is a progress hint, and losing 8 of 208px
        // costs it nothing (see the quantization note below).
        int barX0 = r.x + EDGE_PAD, barX1 = r.x + r.w - EDGE_PAD;
        int barY = r.y + r.h - BAR_GUTTER;
        // Track: 1px grey pill, not the old 2px black square box — at BAR_H 6 a
        // 2px black border WAS most of the bar, so the empty and full states
        // barely differed. A faint track plus a solid black fill puts the whole
        // contrast budget on the thing that actually moves.
        c.drawRoundRect(barX0, barY, barX1 - barX0, BAR_H, BAR_R, BAR_TRACK);
        int inner = barX1 - barX0 - 2;      // 1px track each side (was 2px)
        // Drains L-anchored in BAR_LEVELS discrete steps, not pixel-by-pixel — see
        // THE QUANTIZED COOLDOWN BAR in action_band.h for why (refresh budget).
        int fw = inner * barLevel(coolLeft, coolTotal) / BAR_LEVELS;
        // BAR_FILL_R is a legal radius for every fill this can produce: the
        // narrowest band carrying a cooldown is a 240px grid cell -> inner 206 at
        // EDGE_PAD 16 (was 214 at 12), and barLevel() never returns 0 here (the
        // caller gated on coolLeft > 0), so fw >= 206/16 = 12 — still comfortably
        // past the 2*BAR_FILL_R = 4 the primitive needs to draw both arcs.
        if (fw > 0)
            c.fillRoundRect(barX0 + 1, barY + 1, fw, BAR_H - 2, BAR_FILL_R, TFT_BLACK);
    }
}
