// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Random-event modal — full-screen overlay for the event engine's forced
// choice. See event_modal.h. Every string is routed through tr() (strings_zh.h)
// so only the official Simplified-Chinese translation reaches the sparse 12px
// CJK face (§8.3 glyph-closure iron law); layout obeys §9 (title 36px, body/
// button 24px, <=20 汉字/行, >=80px long-press bands, content clears y=928).
#include "event_modal.h"
#include "event_engine.h"        // events:: queries/commands (+ game_data RES_KEY)
#include "game_state.h"          // adr::Result, GameState (save)
#include "cjk_text.h"            // cjk::drawText/drawWrapped/textWidth, tr()
#include "status_bar.h"
#include "pager.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>

// main.cpp owns both the game model and the full-screen sprite.
extern adr::GameState g_game;
extern M5Canvas canvas;

namespace event_modal {

namespace {
constexpr int PAD         = 24;                 // shared host pad
constexpr int CONTENT_W   = 540 - 2 * PAD;      // 492 usable (§9.2)
constexpr int SCALE_TITLE = 3;                  // 12px grid x3 = 36px title
constexpr int SCALE_BODY  = 2;                  // 12px grid x2 = 24px body/button
constexpr int GLYPH       = 12 * SCALE_BODY;    // 24px body line box

// Vertical budget: title band up top, a rule, the narrative reflowing below it,
// and the button column bottom-anchored so the last band clears the status bar.
constexpr int TITLE_Y   = 20;                   // 36px title -> [20, 56]
constexpr int RULE_Y    = 72;                   // 2px separator under the title
constexpr int NARR_TOP  = 92;                   // narrative first line box top
constexpr int NARR_LINEH = 34;                  // 24px glyph + 10 leading

constexpr int BTN_X      = PAD;                 // full-width single column
constexpr int BTN_W      = CONTENT_W;           // 492
constexpr int BTN_H      = 84;                  // long-press band (§9.3: >=80px)
constexpr int BTN_GAP    = 12;
constexpr int BTN_BOTTOM = 912;                 // bands stack UP from here; the
                                                // 32px status bar owns [928,960]
constexpr int SUBGAP     = 6;                   // label -> cost sub-line gap

constexpr uint32_t TIMEOUT_MS = 120u * 1000u;   // §5.4 idle auto-dismiss (2 min)

// ---- overlay state (RAM-only) --------------------------------------------
bool     s_active = false;
int      s_scene  = -1;      // last rendered scene — a change forces a QUALITY
                             // re-clean; a same-scene repaint (repeat trade) is
                             // FAST so it doesn't flash on every trade.
uint32_t s_openMs = 0;
uint32_t s_lastMs = 0;       // last interaction (millis) — idle-timeout clock

// Top of button band `i` of `n`, bottom-anchored at BTN_BOTTOM.
int btnTop(int i, int n) {
    int total = n * BTN_H + (n - 1) * BTN_GAP;
    return (BTN_BOTTOM - total) + i * (BTN_H + BTN_GAP);
}

// The button whose band contains (x,y), or -1. Full-width bands, so the y-band
// picks it; x is range-checked only to reject the extreme edges.
int hitButton(int x, int y) {
    if (x < BTN_X || x >= BTN_X + BTN_W) return -1;
    int n = events::btnCount();
    for (int i = 0; i < n; i++) {
        int top = btnTop(i, n);
        if (y >= top && y < top + BTN_H) return i;
    }
    return -1;
}

// Build a button's cost sub-line ("-100 毛皮", resource name via strings table),
// joining multiple cost entries with two spaces. Returns false (empty) when the
// button is free.
bool costLine(int localBtn, char* out, size_t cap) {
    out[0] = 0;
    const adr::ResAmt* c = events::btnCost(localBtn);
    if (!c || c[0].res == adr::RA_END) return false;
    size_t used = 0;
    for (int i = 0; i < 3 && c[i].res != adr::RA_END; i++) {
        const char* rz = tr(adr::RES_KEY[c[i].res]);
        int wrote = snprintf(out + used, cap - used, "%s-%ld %s",
                             used ? "  " : "", (long)c[i].amt, rz);
        if (wrote < 0) break;
        used += (size_t)wrote;
        if (used >= cap) { used = cap - 1; break; }
    }
    return used > 0;
}

// 1px dashed rectangle, 4px on / 4px off — the global unavailable-button frame
// (matches room_page/outside_page's drawDashedRect exactly).
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

// One full-width choice band. Available -> the two solid rings (2px); unavailable
// (cost not met) -> a single 1px dashed frame — the same availability cue the
// room/outside pages use. Label 24px centered; a button with a cost gets a second
// 24px line under it, the two-line block vertically centered in the band.
void drawButton(m5gfx::M5Canvas& c, int top, int localBtn) {
    bool avail = events::btnAvailable(localBtn);
    if (avail) {
        c.drawRect(BTN_X, top, BTN_W, BTN_H, TFT_BLACK);
        c.drawRect(BTN_X + 1, top + 1, BTN_W - 2, BTN_H - 2, TFT_BLACK);
    } else {
        drawDashedRect(c, BTN_X, top, BTN_W, BTN_H);
    }

    const char* label = tr(events::btnTextKey(localBtn));
    char cost[64];
    bool hasCost = costLine(localBtn, cost, sizeof(cost));
    if (hasCost) {
        int block = GLYPH * 2 + SUBGAP;
        int ly = top + (BTN_H - block) / 2;
        int lw = cjk::textWidth(label, SCALE_BODY);
        cjk::drawText(c, BTN_X + (BTN_W - lw) / 2, ly, label, SCALE_BODY);
        int cw = cjk::textWidth(cost, SCALE_BODY);
        cjk::drawText(c, BTN_X + (BTN_W - cw) / 2, ly + GLYPH + SUBGAP, cost, SCALE_BODY);
    } else {
        int lw = cjk::textWidth(label, SCALE_BODY);
        cjk::drawText(c, BTN_X + (BTN_W - lw) / 2, top + (BTN_H - GLYPH) / 2 - 4,
                      label, SCALE_BODY);
    }
}

// Compose the whole panel into the shared canvas (no push).
void render() {
    canvas.fillSprite(TFT_WHITE);
    const char* title = tr(events::eventTitleKey());
    if (title) cjk::drawText(canvas, PAD, TITLE_Y, title, SCALE_TITLE);
    canvas.fillRect(PAD, RULE_Y, CONTENT_W, 2, TFT_BLACK);

    int y = NARR_TOP;
    int nt = events::sceneTextCount();
    for (int i = 0; i < nt; i++) {
        const char* body = tr(events::sceneTextKey(i));
        if (body) y = cjk::drawWrapped(canvas, PAD, y, CONTENT_W, body,
                                       SCALE_BODY, NARR_LINEH);
    }

    int n = events::btnCount();
    for (int i = 0; i < n; i++) drawButton(canvas, btnTop(i, n), i);

    status_bar::drawOnto(canvas);   // keep the clock/battery/dots chrome band
}

// Repaint after a choose that stayed in the event. A scene change replaces the
// whole narrative block, so it takes a QUALITY full-panel clean (no ghost of the
// previous scene's text); a same-scene repaint (repeat trade -> only a button's
// dashed state may flip) is FAST, so trading twice doesn't flash the panel each
// time.
void repaint() {
    int sc = events::currentScene();
    bool sceneChanged = (sc != s_scene);
    s_scene = sc;
    render();
    auto& disp = M5.Display;
    disp.setEpdMode(sceneChanged ? epd_mode_t::epd_quality : epd_mode_t::epd_fast);
    canvas.pushSprite(0, 0);
    disp.setEpdMode(epd_mode_t::epd_fast);
}

// Clear the overlay and repaint the page underneath (the shared exit path for a
// choose that ended the event and for the idle timeout). Releasing s_active first
// lets pager::showPage past the modal guard.
void closeAndRestore() {
    s_active = false;
    s_scene  = -1;
    pager::showPage(pager::currentRingIndex(), false);
}
}  // namespace

// ---- public ---------------------------------------------------------------

bool active() { return s_active; }

void show(uint32_t nowMs) {
    s_active = true;
    s_scene  = events::currentScene();
    s_openMs = nowMs;
    s_lastMs = nowMs;
    render();
    auto& disp = M5.Display;
    disp.setEpdMode(epd_mode_t::epd_quality);   // one deliberate flash on entry
    canvas.pushSprite(0, 0);
    disp.setEpdMode(epd_mode_t::epd_fast);
    // Event alert: a short rising two-note chime, distinct from the action chime
    // (1800) and the switcher tone (2000) so a pop-up is unmistakable.
    M5.Speaker.tone(1047, 90);
    delay(100);
    M5.Speaker.tone(1568, 150);
    Serial.printf("[event] show ev=%d scene=%d\n",
                  events::currentEventId(), events::currentScene());
}

bool handleHold(int x, int y) {
    s_lastMs = millis();                    // any long-press resets the idle clock
    int b = hitButton(x, y);
    if (b < 0) { M5.Speaker.tone(600, 120); return true; }  // missed every band

    adr::Result r = events::choose(b);
    if (r == adr::RC_ERR_COST) {
        M5.Speaker.tone(600, 120);          // unaffordable: low beep, no change
        return true;
    }
    if (r == adr::RC_OK) {
        M5.Speaker.tone(1800, 80);          // settle chime (same as page actions)
        g_game.save();                      // events mutate stores; persist now
        if (events::active()) repaint();    // stayed (repeat trade / new scene)
        else                  closeAndRestore();  // event ended
    }
    return true;
}

void checkTimeout(uint32_t nowMs) {
    if (!s_active) return;
    if (nowMs - s_lastMs < TIMEOUT_MS) return;
    Serial.println("[event] 2min idle -> dismissDefault");
    events::dismissDefault();               // no-cost safe exit -> ends the event
    g_game.save();
    closeAndRestore();
}

}  // namespace event_modal
