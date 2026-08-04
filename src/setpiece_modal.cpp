// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Landmark setpiece modal — full-screen overlay for the setpiece engine's
// narrative choices. See setpiece_modal.h. The setpiece_engine owns scene/button/
// loot state; this file renders it, routes a press into setpiece::choose(), and
// orchestrates the fight_modal handoff for combat scenes. Layout follows
// event_modal (title 36px, narrative 24px, choice bands via the shared
// action_band, >=80px long-press bands, content clears the status bar) plus a
// banked-loot list between the narrative and the buttons. Every string routes
// through tr() (strings_zh.h, §8.3 glyph closure).
#include "setpiece_modal.h"
#include "action_band.h"         // the app-wide button band (shared with every page)
#include "setpiece_engine.h"     // setpiece:: queries/commands
#include "fight_modal.h"         // beginSetpiece (combat scene handoff)
#include "world_state.h"
#include "game_state.h"
#include "game_data.h"           // RES_KEY / ITEM_KEY for loot + cost lines
#include "cjk_text.h"            // cjk::drawText/drawWrapped/textWidth, tr()
#include "status_bar.h"
#include "pager.h"
#include "beeper.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>

extern adr::GameState  g_game;
extern adr::WorldState g_world;
extern M5Canvas canvas;

using namespace adr;

namespace setpiece_modal {

namespace {
constexpr int PAD         = 24;
constexpr int CONTENT_W   = 540 - 2 * PAD;      // 492
constexpr int SCALE_TITLE = 3;                  // 36px title
constexpr int SCALE_BODY  = 2;                  // 24px narrative / loot list

constexpr int TITLE_Y    = 20;
constexpr int RULE_Y     = 72;
constexpr int NARR_TOP   = 92;
constexpr int NARR_LINEH = 34;
constexpr int LOOT_LINEH  = 30;                 // banked-loot list line pitch

constexpr int BTN_X      = PAD;
constexpr int BTN_W      = CONTENT_W;
constexpr int BTN_H      = 84;
constexpr int BTN_GAP    = 12;
constexpr int BTN_BOTTOM = 912;

constexpr uint32_t TIMEOUT_MS = 120u * 1000u;   // idle auto-dismiss (2 min)

bool     s_active = false;
uint32_t s_lastMs = 0;

int btnTop(int i, int n) {
    int total = n * BTN_H + (n - 1) * BTN_GAP;
    return (BTN_BOTTOM - total) + i * (BTN_H + BTN_GAP);
}
int hitButton(int x, int y) {
    if (x < BTN_X || x >= BTN_X + BTN_W) return -1;
    int n = setpiece::btnCount();
    for (int i = 0; i < n; i++) {
        int top = btnTop(i, n);
        if (y >= top && y < top + BTN_H) return i;
    }
    return -1;
}

// A button's cost sub-line ("-1 火把", "-5 水", "-10 生命"). A bag cost is always
// one unit of one slot (torch / charm / grenade / alien alloy); the Executioner's
// burning corridor is the only place that bills water or blood, and those DO carry
// an amount. Returns false (empty) for a free button.
bool costLine(int localBtn, char* out, size_t cap) {
    out[0] = 0;
    uint8_t slot = setpiece::btnCostSlot(localBtn);
    if (slot == SP_NO_COST) return false;
    const char* nm;
    // "water" / "hp" are upstream's own cost keys (events.js getQuantity) and both
    // are already in the official zh table (水 / 生命) — no local string needed.
    if (slot == SP_COST_WATER)   nm = tr("water");
    else if (slot == SP_COST_HP) nm = tr("hp");
    else nm = setpiece::btnCostIsItem(localBtn) ? tr(ITEM_KEY[slot]) : tr(RES_KEY[slot]);
    snprintf(out, cap, "-%d %s", setpiece::btnCostAmt(localBtn), nm);
    return true;
}

// The rect of choice band `i` of `n` — one description of where the button is,
// shared by drawButton's frame and handleHold's invert-flash (event_modal
// parity; both used to restate BTN_X/BTN_W/BTN_H independently).
pages::Rect btnRect(int i, int n) {
    return pages::Rect{ BTN_X, btnTop(i, n), BTN_W, BTN_H };
}

// One choice band, through the shared renderer. v0.12 deleted this file's
// near-verbatim copy of event_modal's copy of the Trade band; the label now
// rides the app-wide 36px title scale, left-aligned beside its right-aligned cost
// line; a FREE choice centres that title instead (变体 B, see action_band.h).
// costLine stays local: it reads the setpiece engine, not the renderer.
void drawButton(m5gfx::M5Canvas& c, int i, int n) {
    char cost[64];
    bool hasCost = costLine(i, cost, sizeof cost);
    action_band::draw(c, btnRect(i, n), tr(setpiece::btnTextKey(i)),
                      hasCost ? cost : nullptr, setpiece::btnAvailable(i), 0, 0);
}

void render() {
    canvas.fillSprite(TFT_WHITE);
    const char* title = tr(setpiece::titleKey());
    if (title) cjk::drawText(canvas, PAD, TITLE_Y, title, SCALE_TITLE);
    canvas.fillRect(PAD, RULE_Y, CONTENT_W, 2, TFT_BLACK);

    int y = NARR_TOP;
    int nt = setpiece::sceneTextCount();
    for (int i = 0; i < nt; i++) {
        const char* body = tr(setpiece::sceneTextKey(i));
        if (body) y = cjk::drawWrapped(canvas, PAD, y, CONTENT_W, body,
                                       SCALE_BODY, NARR_LINEH);
    }
    // banked loot ("毛皮 x3"), one per line under the narrative
    int nl = setpiece::lootCount();
    if (nl > 0) y += 8;
    for (int i = 0; i < nl; i++) {
        uint8_t slot = setpiece::lootSlot(i);
        const char* key = setpiece::lootIsItem(i) ? ITEM_KEY[slot] : RES_KEY[slot];
        char line[48];
        snprintf(line, sizeof line, "%s x%d", tr(key), setpiece::lootGot(i));
        cjk::drawText(canvas, PAD, y, line, SCALE_BODY);
        y += LOOT_LINEH;
    }

    int n = setpiece::btnCount();
    for (int i = 0; i < n; i++) drawButton(canvas, i, n);

    status_bar::drawOnto(canvas);
}

// Put this overlay on the panel. It used to take a deliberate epd_quality flash
// on every scene change (a whole new narrative block earned a grayscale clean);
// there is no waveform to ask for now, just one whole-frame render at the
// driver's own rate. pager::repaint routes back into renderFrame() below while
// s_active is set.
void pushFull() { pager::repaint(); }

void closeToWorld() {
    s_active = false;
    pager::showPage(pager::currentRingIndex(), false);   // repaint the World map
}

// After a begin()/choose()/resolve that may have changed the engine state: launch
// a fight (combat scene armed), restore the World page (setpiece ended), or draw
// the next panel.
void afterTransition() {
    if (!setpiece::active()) { closeToWorld(); return; }
    if (setpiece::awaitingCombat()) { fight_modal::beginSetpiece(millis()); return; }
    pushFull();
}
}  // namespace

// ---- public ---------------------------------------------------------------

bool active() { return s_active; }

void renderFrame() { render(); }

void begin(uint8_t spId, uint32_t nowMs) {
    if (!setpiece::begin(spId)) return;      // no setpiece for this landmark: inert
    s_active = true;
    s_lastMs = nowMs;
    // Entry chime: a short rising two-note, distinct from the event pop and the
    // fight's falling chime.
    beeper::tone(1175, 90);
    delay(100);
    beeper::tone(1568, 150);
    Serial.printf("[setpiece] begin id=%u\n", (unsigned)spId);
    afterTransition();
}

bool handleHold(int x, int y) {
    s_lastMs = millis();
    int b = hitButton(x, y);
    if (b < 0) { beeper::tone(600, 120); return true; }

    pages::Rect pr = btnRect(b, setpiece::btnCount());
    pager::flashPressRect(pr);

    Result r = setpiece::choose(b);
    if (r == RC_OK) {
        beeper::tone(1800, 80);
        g_game.save();                        // perks / log / (trek saved by engine)
        afterTransition();
        return true;
    }
    // Can't afford it (no torch/charm/water/blood), or the `available` gate says
    // no (a wing already cleared): same low buzz, since both draw the same
    // disabled band and neither is a state the player can fix by pressing harder.
    if (r == RC_ERR_COST || r == RC_ERR_LOCKED) beeper::tone(600, 120);
    pushFull();                    // clean frame back over the press flash
    return true;
}

void checkTimeout(uint32_t nowMs) {
    if (!s_active) return;
    if (fight_modal::active()) { s_lastMs = nowMs; return; }   // fight owns the clock
    // Clock-backwards guard (the "弹窗秒关" bug): s_lastMs is stamped with a FRESH
    // millis() inside begin() — WorldPage opens us via begin(millis()) from within
    // pager::handleTouch, and that millis() is read AFTER main.cpp captured the
    // loop-top `now` snapshot it later passes here. begin() then blocks ~300ms, so
    // on the very frame the setpiece opens, s_lastMs sits AHEAD of this nowMs by
    // tens of ms. An unsigned nowMs - s_lastMs would underflow to ~4.29e9 >=
    // TIMEOUT_MS and instantly fire the 2-min default exit. Treat any backwards
    // reading as zero elapsed idle and wait for the next pass, where `now` is fresh
    // and ahead again. (Same shape holds for any caller whose snapshot lags the
    // interaction stamp — this is the general fix, not a begin()-specific patch.)
    if (nowMs < s_lastMs) return;
    if (nowMs - s_lastMs < TIMEOUT_MS) return;
    Serial.println("[setpiece] 2min idle -> default exit");
    int d = setpiece::defaultBtnIndex();
    if (d >= 0) setpiece::choose(d);
    g_game.save();
    afterTransition();
}

void onFightResult(bool won) {
    setpiece::resolveCombat(won);
    g_game.save();
    s_lastMs = millis();                      // the fight consumed time; reset idle
    if (!setpiece::active()) { closeToWorld(); return; }
    pushFull();                               // victory panel (loot + continue/run)
}

void abort() {
    setpiece::end();
    s_active = false;   // the caller (world_page::enterDeath) raises the death frame
}

void endForSleep() {
    if (!s_active) return;
    s_active = false;
    setpiece::end();    // abandon; the trek was saved each scene, resume pre-setpiece
    Serial.println("[setpiece] forced sleep -> abandon");
}

}  // namespace setpiece_modal
