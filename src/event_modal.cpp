// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Random-event modal — full-screen overlay for the event engine's forced
// choice. See event_modal.h. Every string is routed through tr() (strings_zh.h)
// so only the official Simplified-Chinese translation reaches the sparse 12px
// CJK face (§8.3 glyph-closure iron law); layout obeys §9 (title 36px, narrative
// 24px, <=20 汉字/行, >=80px long-press bands, content clears y=928). The choice
// bands are the shared action_band (36px label over a 24px cost line, v0.12).
#include "event_modal.h"
#include "action_band.h"         // the app-wide button band (shared with every page)
#include "event_engine.h"        // events:: queries/commands (+ game_data RES_KEY)
#include "game_state.h"          // adr::Result, GameState (save)
#include "cjk_text.h"            // cjk::drawText/drawWrapped/textWidth, tr()
#include "status_bar.h"
#include "pager.h"
#include "beeper.h"
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
constexpr int SCALE_BODY  = 2;                  // 12px grid x2 = 24px narrative

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
                                                // (36 + 6 + 24 = 66 of the 84px
                                                // band — action_band owns the
                                                // label/cost type scale and the
                                                // vertical derivation now)

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

// The rect of choice band `i` of `n` — the ONE description of where a choice
// button is, shared by drawButton's frame and handleHold's invert-flash so the
// two cannot drift apart (they used to restate BTN_X/BTN_W/BTN_H separately).
pages::Rect btnRect(int i, int n) {
    return pages::Rect{ BTN_X, btnTop(i, n), BTN_W, BTN_H };
}

// One full-width choice band, through the shared renderer (v0.12: the modal's
// hand-rolled near-copy of the Trade band is gone — see action_band.h). The
// choice label rides the app-wide 36px title scale now, over its 24px cost line
// when the choice is priced: the label takes the band's LEFT column and each cost
// entry a right-aligned line opposite it. A free choice centres its label instead
// (变体 B, see action_band.h). Composing the cost text stays here — it reads the
// event engine, which the renderer knows nothing about.
void drawButton(m5gfx::M5Canvas& c, int i, int n) {
    char cost[64];
    bool hasCost = costLine(i, cost, sizeof(cost));
    action_band::draw(c, btnRect(i, n), tr(events::btnTextKey(i)),
                      hasCost ? cost : nullptr, events::btnAvailable(i), 0, 0);
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
    for (int i = 0; i < n; i++) drawButton(canvas, i, n);

    status_bar::drawOnto(canvas);   // keep the clock/battery/dots chrome band
}

// Repaint after a choose that stayed in the event. s_scene used to pick between
// a QUALITY full-panel clean (a scene change replaces the whole narrative block,
// so it earned a grayscale wipe) and a FAST push (a repeat trade only flips a
// button's dashed frame, and flashing the panel on every trade was jarring).
// Neither waveform exists any more — every frame is one whole render at the
// driver's own rate — so the distinction is gone and only the scene BOOKKEEPING
// survives, because show() still uses it to know which scene it opened on.
void repaint() {
    s_scene = events::currentScene();
    pager::repaint();
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

void renderFrame() { render(); }

void show(uint32_t nowMs) {
    s_active = true;
    s_scene  = events::currentScene();
    s_openMs = nowMs;
    s_lastMs = nowMs;
    // s_active is already set, so pager::drawFrame routes to renderFrame() below
    // and this paints the overlay rather than the page under it. The deliberate
    // epd_quality flash that used to mark an event popping open is gone with the
    // waveforms; the two-note chime is the alert now, and losing the flash also
    // takes with it the phantom GT911 contact it used to induce (pager.cpp's
    // modal-open bounce guard).
    pager::repaint();
    // Event alert: a short rising two-note chime, distinct from the action chime
    // (1800) and the switcher tone (2000) so a pop-up is unmistakable.
    beeper::tone(1047, 90);
    delay(100);
    beeper::tone(1568, 150);
    Serial.printf("[event] show ev=%d scene=%d\n",
                  events::currentEventId(), events::currentScene());
}

bool handleHold(int x, int y) {
    s_lastMs = millis();                    // any long-press resets the idle clock
    int b = hitButton(x, y);
    if (b < 0) { beeper::tone(600, 120); return true; }  // missed every band

    // Press feedback: invert-flash the chosen choice band before resolving it
    // (pager::flashPressRect shows the frame with the band in reverse video for a
    // beat, then puts the normal frame back itself).
    pages::Rect pr = btnRect(b, events::btnCount());
    pager::flashPressRect(pr);

    adr::Result r = events::choose(b);
    if (r == adr::RC_OK) {
        beeper::tone(1800, 80);          // settle chime (same as page actions)
        g_game.save();                      // events mutate stores; persist now
        if (events::active()) repaint();    // stayed (repeat trade / new scene)
        else                  closeAndRestore();  // event ended
        return true;                        // the repaint overwrote the flash
    }
    // No committing choice: put a clean frame back up (flashPressRect already
    // restored one, but the reject may still have changed a button's state).
    // Only RC_ERR_COST low-beeps (unchanged); a bare no-op stays silent.
    if (r == adr::RC_ERR_COST) beeper::tone(600, 120);      // unaffordable
    pager::repaint();
    return true;
}

void endForSleep() {
    if (!s_active) return;
    s_active = false;
    s_scene  = -1;
    Serial.println("[event] forced sleep -> overlay released");
}

void checkTimeout(uint32_t nowMs) {
    if (!s_active) return;
    // Clock-backwards guard (same time-base constraint as setpiece_modal::
    // checkTimeout): handleHold stamps s_lastMs with a FRESH millis(), but main.cpp
    // drives this watchdog with its loop-top `now` snapshot — read BEFORE handleTouch
    // ran the press this same pass. A keep-open mis-tap (missed band / unaffordable)
    // thus leaves s_lastMs a hair AHEAD of nowMs, and an unsigned nowMs - s_lastMs
    // underflows to ~4.29e9 >= TIMEOUT_MS, spuriously dismissing the event. Treat any
    // backwards reading as zero elapsed idle; the next pass has a fresh, ahead `now`.
    if (nowMs < s_lastMs) return;
    if (nowMs - s_lastMs < TIMEOUT_MS) return;
    Serial.println("[event] 2min idle -> dismissDefault");
    events::dismissDefault();               // no-cost safe exit -> ends the event
    g_game.save();
    closeAndRestore();
}

}  // namespace event_modal
