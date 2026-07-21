// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Room page — Phase 1 CJK VERIFICATION build. Not yet the real fire/wood/
// builder loop; this proves the whole Chinese pipeline on the panel: the
// official translation table (tr(), strings_zh.h) feeding the sparse 12px
// bitmap font (cjk_text / cjk_font12.h) at both 24px (2x) and mixed CN/EN/digit
// rows, plus the §9 screen budget (24px CJK ~2.6mm, ~20 汉字/行 @492px, a 92px
// long-press button band). Every string comes from tr() — no hard-coded Chinese
// (the §8.3 铁律 that keeps the glyph closure complete).
#include "room_page.h"
#include "cjk_text.h"
#include "page_header.h"
#include "pomo_page.h"          // PAD, HDR_DIV_Y (shared layout authority)
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>

namespace {
constexpr int SCALE   = 2;               // 12px grid x2 = 24px CJK
constexpr int CONTENT_W = 540 - 2 * PAD; // 492px usable (§9)
constexpr int ROOM_BTN_H   = 92;              // long-press band (§9: >=80px, ~7mm)

// Splice arg into a "...{0}..." template (the game's own placeholder form), so
// state lines read from the official templated translations, not hard-coded
// sentences. Only {0} is handled (all Phase-1 room/fire lines use just one).
void fmt1(char* out, size_t cap, const char* tmpl, const char* arg) {
    const char* h = strstr(tmpl, "{0}");
    if (!h) { snprintf(out, cap, "%s", tmpl); return; }
    int pre = (int)(h - tmpl);
    snprintf(out, cap, "%.*s%s%s", pre, tmpl, arg, h + 3);
}
}  // namespace

bool RoomPage::draw(m5gfx::M5Canvas& canvas) {
    canvas.fillSprite(TFT_WHITE);

    // Standard clock header (every page carries it) — big HH:MM + weekday/date
    // + dashed rule at HDR_DIV_Y=112.
    page_header::draw(canvas);

    int y = HDR_DIV_Y + 20;

    // Title "小黑屋" (tr "A Dark Room"), 24px, centered.
    const char* title = tr("A Dark Room");
    int tw = cjk::textWidth(title, SCALE);
    cjk::drawText(canvas, (540 - tw) / 2, y, title, SCALE);
    y += 52;

    // Mixed CN / EN / digit row on one baseline: "wood 木头 x 42".
    char mixed[64];
    snprintf(mixed, sizeof(mixed), "wood %s x 42", tr("wood"));
    cjk::drawText(canvas, PAD, y, mixed, SCALE);
    y += 48;

    // Two state lines from the official TEMPLATED translations — the room's
    // first-play state (fire = Dead, temperature = Freezing, per room.js).
    char line[96];
    fmt1(line, sizeof(line), tr("the fire is {0}"), tr("dead"));      // 火堆熄灭了
    cjk::drawText(canvas, PAD, y, line, SCALE);
    y += 40;
    fmt1(line, sizeof(line), tr("the room is {0}"), tr("freezing"));  // 房间寒冷刺骨
    cjk::drawText(canvas, PAD, y, line, SCALE);
    y += 52;

    // Auto-wrapped narrative (24+ CJK chars -> 2 lines at 492px), an official
    // notification line. Exercises per-char CJK line breaking.
    const char* narr = tr("the stranger is standing by the fire. "
                          "she says she can help. says she builds things.");
    y = cjk::drawWrapped(canvas, PAD, y, CONTENT_W, narr, SCALE, 36);
    y += 20;

    // A §9 long-press button band (92px), label "生火" (tr "light fire") 24px
    // centered — visual verification of the button budget.
    int bx0 = PAD, bx1 = 540 - PAD;
    canvas.drawRect(bx0, y, bx1 - bx0, ROOM_BTN_H, TFT_BLACK);
    canvas.drawRect(bx0 + 1, y + 1, bx1 - bx0 - 2, ROOM_BTN_H - 2, TFT_BLACK);
    const char* btn = tr("light fire");
    int bw = cjk::textWidth(btn, SCALE);
    cjk::drawText(canvas, (540 - bw) / 2, y + (ROOM_BTN_H - 24) / 2, btn, SCALE);

    return true;
}

const pages::Region* RoomPage::regions(int* n) const {
    *n = 0;
    return nullptr;
}
