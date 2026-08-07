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
//
// v0.17: each of the 13 events carries an illustration (event_art_data.h) drawn
// between the title rule and the narrative — see the ART_/NARR_TOP block below
// for how big it is allowed to be and why.
#include "event_modal.h"
#include "action_band.h"         // the app-wide button band (shared with every page)
#include "event_engine.h"        // events:: queries/commands (+ game_data RES_KEY)
#include "event_art_data.h"      // EVENT_ART[] — this is its ONE includer
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
constexpr int SCALE_BODY  = 2;                  // 12px grid x2 = 24px narrative

// Vertical budget: title band up top, a rule, the event illustration, the
// narrative reflowing below it, and the button column bottom-anchored so the
// last band clears the status bar.
constexpr int TITLE_Y   = 20;                   // 36px title -> [20, 56]
constexpr int RULE_Y    = 72;                   // 2px separator under the title
constexpr int ART_X     = PAD;                  // flush with the rule and the bands
constexpr int ART_Y     = 88;                   // 14px of air under the 2px rule
constexpr int ART_GAP   = 16;                   // air between the art and the text
constexpr int NARR_TOP  = ART_Y + EVENT_ART_H + ART_GAP;   // 88+276+16 = 380
constexpr int NARR_LINEH = 34;                  // 24px glyph + 10 leading

// ---- why the art is 492x276 ------------------------------------------------
// The source plates are 1376x768 (aspect 1.792). Width is the easy axis: the art
// takes the full CONTENT_W column at ART_X == PAD, so it lines up pixel-exact
// with the title rule above it and the choice bands below it — 492px, no side
// margin of its own. At the source aspect that wants 492/1.792 = 274.6px of
// height; 276 is the nearest even row count (4bpp packs two pixels per byte, and
// an even height keeps the preview/packing arithmetic boring), a 0.5% vertical
// stretch that nobody can see.
//
// Height is the axis with a real ceiling, so it was checked against every scene
// rather than eyeballed. Replaying cjk::drawWrapped's exact greedy wrap over the
// official zh translation of all 37 scenes in events_data.h, at CONTENT_W=492 /
// SCALE_BODY=2 / NARR_LINEH=34, the narrative is at most 3 lines (102px). The
// button column is bottom-anchored at BTN_BOTTOM, so more choices means a lower
// ceiling for the text: 4 bands (the worst case in the game — The Nomad's three
// buys plus goodbye) put btnTop at 912 - (4*84 + 3*12) = 540.
//
// The Nomad is therefore the binding constraint, and it is also a 3-line scene:
//   narrative bottom = NARR_TOP + 3*34 = 380 + 102 = 482
//   button column top                                = 540
//   slack                                            =  58px
// Every other scene has >=160px. 58px is ~1.7 spare lines, which is the margin a
// future retranslation gets to spend before this needs revisiting. Growing the
// art is what eats that margin, and at 492 wide the source aspect has already
// capped the height at 276 anyway — a taller plate would mean cropping the sides
// of the drawing, which is not worth 58px.
// -----------------------------------------------------------------------------

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

static_assert(sizeof(EVENT_ART) / sizeof(EVENT_ART[0]) == (size_t)adr::EVENT_COUNT,
              "event art table must have one entry per adr::EventId");

// Blit event `eventId`'s illustration into the canvas at (ART_X, ART_Y).
//
// The stored form is RLE over a 4bpp pixel stream (format in event_art_data.h),
// and both layers are unpacked in ONE forward pass straight into the canvas —
// no intermediate buffer. An 8bpp copy of a plate would be 492*276 = 133KB of
// PSRAM to allocate, memset and free on every event just to memcpy it away
// again; there is no reason to touch the heap when the destination rows are
// already sitting there. The canvas is grayscale_8bit (main.cpp), i.e. one byte
// of luma per pixel with a row stride of canvas.width(), so a nibble expands
// with a *17 (15 -> 255 paper white, 0 -> ink black) and lands directly.
//
// The cursor is kept in PACKED bytes: `bx` counts byte-pairs across the row and
// wraps into the next canvas row at rowPack, so a single run can span rows the
// way it does in the stream. A missing plate (nullptr / out-of-range id) just
// leaves the area white — the modal still works, it is only unillustrated.
void drawArt(int eventId) {
    if (eventId < 0 || eventId >= (int)adr::EVENT_COUNT) return;
    const uint8_t* p = EVENT_ART[eventId];
    if (!p) return;
    uint8_t* buf = (uint8_t*)canvas.getBuffer();
    if (!buf) return;

    const int stride  = canvas.width();
    const int rowPack = EVENT_ART_W / 2;        // 246 packed bytes per row
    uint8_t* row = buf + (size_t)ART_Y * stride + ART_X;
    int bx = 0, by = 0;
    while (by < EVENT_ART_H) {
        uint8_t head = *p++;
        const uint8_t* lit = nullptr;
        uint8_t val = 0;
        int n;
        if (head == 0) { n = *p++; lit = p; p += n; }   // literal packet
        else           { n = head; val = *p++; }        // run packet
        for (int i = 0; i < n; i++) {
            uint8_t b = lit ? lit[i] : val;
            row[bx * 2]     = (uint8_t)((b >> 4) * 17);
            row[bx * 2 + 1] = (uint8_t)((b & 0x0F) * 17);
            if (++bx == rowPack) {
                bx = 0;
                if (++by == EVENT_ART_H) return;        // last row consumed
                row += stride;
            }
        }
    }
}

// Compose the panel into the shared canvas (no push).
//
// `full` distinguishes the two callers. show() passes true: clear everything and
// paint the header, the rule and the illustration as well. repaint() passes
// false, and then ONLY the rows below the art are cleared and redrawn. That is
// not just a speed trick — the title is per-EVENT (events::eventTitleKey), so
// nothing above NARR_TOP can change while an event is on screen, and leaving the
// decoded plate untouched in the canvas is what lets the FAST same-scene repaint
// (a repeat trade) stay quiet: the panel's per-pixel diff sees an identical art
// region and drives none of it. A full fillSprite here would blank the plate and
// then have to re-drive all 492x276 of it on every single trade.
void render(bool full) {
    if (full) {
        canvas.fillSprite(TFT_WHITE);
        const char* title = tr(events::eventTitleKey());
        if (title) cjk::drawText(canvas, PAD, TITLE_Y, title, SCALE_TITLE);
        canvas.fillRect(PAD, RULE_Y, CONTENT_W, 2, TFT_BLACK);
        drawArt(events::currentEventId());
    } else {
        // Everything the narrative or the choice bands could occupy. The status
        // bar clears its own band, so running to the canvas bottom is harmless.
        canvas.fillRect(0, NARR_TOP, canvas.width(), canvas.height() - NARR_TOP,
                        TFT_WHITE);
    }

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

// Repaint after a choose that stayed in the event. A scene change replaces the
// whole narrative block, so it takes a QUALITY full-panel clean (no ghost of the
// previous scene's text); a same-scene repaint (repeat trade -> only a button's
// dashed state may flip) is FAST, so trading twice doesn't flash the panel each
// time.
void repaint() {
    int sc = events::currentScene();
    bool sceneChanged = (sc != s_scene);
    s_scene = sc;
    render(false);   // header + illustration stay as show() left them
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
    render(true);
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

    // Press feedback: invert-flash the chosen choice band before resolving it
    // (pager::flashPressRect leaves the canvas restored but the screen showing the
    // inverted rect). An RC_OK repaint (repaint / closeAndRestore) paints over it;
    // the non-repainting branches rebound the rect so the black flash bounces off.
    pages::Rect pr = btnRect(b, events::btnCount());
    pager::flashPressRect(pr);

    adr::Result r = events::choose(b);
    if (r == adr::RC_OK) {
        M5.Speaker.tone(1800, 80);          // settle chime (same as page actions)
        g_game.save();                      // events mutate stores; persist now
        if (events::active()) repaint();    // stayed (repeat trade / new scene)
        else                  closeAndRestore();  // event ended
        return true;                        // the repaint overwrote the flash
    }
    // No committing choice: nothing repaints the panel, so rebound the flashed
    // band. Only RC_ERR_COST low-beeps (unchanged); a bare no-op stays silent.
    if (r == adr::RC_ERR_COST) M5.Speaker.tone(600, 120);   // unaffordable
    pager::partialRefresh(pr, pages::RefreshMode::FASTEST);
    return true;
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
