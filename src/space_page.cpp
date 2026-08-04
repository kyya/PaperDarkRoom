// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Space level — panel, finger and buzzer. See space_page.h for why this is a
// blocking loop, and space_game.h for the rules it drives.
//
// LAYOUT (§8.5, 540x960). The level owns the whole frame and draws no
// status_bar, exactly like fight_modal::renderFrame():
//
//   y   0  HUD 84px: hull pips (24x24 blocks, left) · 层名 (36px, right)
//   y  84  ══ 2px rule
//          playfield 540x704 — asteroids (solid circles) fall from y=84;
//          the ship is a solid 48x36 at a FIXED y=740; a 12px altitude gauge
//          fills from the bottom against the right edge
//   y 788  ══ 2px rule
//   y 792  control band 168px: 11 tick marks + an up-caret at the ship's x
//
// WHY EVERY SHAPE IS SOLID. The driver takes 161 ms to flip a pixel and a logic
// frame is 92 ms, so anything that moves is drawn over its own 1.75-frame trail
// (§8.4). A solid disc trails into a solid capsule and still reads as one object;
// a glyph, an outline or a hairline trails into grey fog. That is also why the
// hull readout is BLOCKS rather than a number (§8.5): "3" becoming "2" spends
// half a second as an unreadable overlap, whereas a block just stops being there.
#include "space_page.h"
#include "space_game.h"
#include "action_band.h"
#include "cjk_text.h"
#include "game_state.h"
#include "game_data.h"
#include "msg_bridge.h"
#include "pager.h"
#include "touch_gt911.h"
#include "beeper.h"
#include "rtc_bm8563.h"
#include <M5Unified.h>
#include <time.h>
#include <Arduino.h>
#include <stdio.h>
#include <string.h>

extern adr::GameState g_game;
extern M5Canvas canvas;

using namespace adr;

namespace space_page {
namespace {

namespace sp = adr::space;

// ---- HUD metrics -----------------------------------------------------------
constexpr int PAD        = 16;
constexpr int PIP        = 24;        // hull block edge
constexpr int PIP_GAP    = 8;
constexpr int PIP_MAX    = 12;        // §8.5: past this it is "12 blocks + N"
constexpr int PIP_Y      = 30;
constexpr int LAYER_Y    = 24;        // 36px 层名 box, 24..60
constexpr int RULE_H     = 2;

// The altitude gauge (§8.5). Right edge of the playfield, and it STOPS ABOVE THE
// SHIP'S ROW: the spec's y range [96,776] runs straight through y=740, where the
// ship sits whenever the player has driven it to the right-hand limit (x=516
// spans 492..540). Two solid black shapes overlapping is not a rendering fault,
// but the gauge's EMPTY part is an outline and the ship would erase it, so the
// gauge would appear to jump. Ending at 704 costs 72 px of scale and nothing else.
constexpr int GAUGE_X    = 516;
constexpr int GAUGE_W    = 12;
constexpr int GAUGE_TOP  = 96;
constexpr int GAUGE_BOT  = 704;

// The control band (§8.3/§8.5). CTRL_TOP is the drawn top AND the hit threshold —
// §8.3's code sketch says 800 and §8.5's diagram says 792; taking the lower of
// the two only ever makes the band easier to find, and 792 is the number that
// makes the band exactly the 168 px the diagram labels it.
constexpr int TICKS      = 11;
constexpr int TICK_H     = 12;
constexpr int TICK_Y     = sp::CTRL_TOP + 12;
constexpr int CARET_W    = 24;
constexpr int CARET_H    = 20;
constexpr int CARET_Y    = sp::CTRL_TOP + 40;

// Score screen.
constexpr int SC_TITLE_Y = 120;
constexpr int SC_LINE0_Y = 260;
constexpr int SC_LINE1_Y = 320;
constexpr int SC_BAND_Y  = 700;
constexpr int SC_BAND_H  = 96;

// Firmware-local literals. Both are spelled entirely out of glyphs the
// strings_zh.h closure already contains (星舰坠毁了 / 返回), so neither costs a
// font rebuild — the same rule ship_page.cpp's hint line follows. tr() falls
// through to the key it was handed, so a Chinese literal renders verbatim.
const char* const CRASH_LINE = "星舰坠毁了";
const char* const BACK_LABEL = "返回";

// ---- live state ------------------------------------------------------------
bool     s_active = false;
sp::Game s_g;
bool     s_scoreScreen = false;      // renderFrame draws the settlement page
uint32_t s_score = 0;
// The hull block that JUST went (§8.5): its slot is outlined for three frames as
// a "you lost this one" cue, then it is simply gone.
int      s_lostPip = -1;
int      s_lostPipFrames = 0;
// Acceptance §11.3b.7 wants evidence that no image-mode push happens INSIDE the
// loop. This counts every deghost the level performs; the summary line prints it
// and the number must be 2 (entry, exit).
int      s_deghosts = 0;

// ---- helpers ---------------------------------------------------------------
// RTC -> Unix epoch, the same four lines ship_page/room_page/world_page each
// carry. The liftoff cooldown is stamped on THIS clock (not millis), so a deep
// sleep simply expires it.
uint32_t epochNow() {
    rtc::Date d; rtc::Time t;
    rtc::getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}

void fmt1(char* out, size_t cap, const char* tmpl, const char* arg) {
    const char* h = strstr(tmpl, "{0}");
    if (!h) { snprintf(out, cap, "%s", tmpl); return; }
    int pre = (int)(h - tmpl);
    snprintf(out, cap, "%.*s%s%s", pre, tmpl, arg, h + 3);
}

void centred(const char* utf8, int y, int scale) {
    cjk::drawText(canvas, (sp::UI_W - cjk::textWidth(utf8, scale)) / 2, y, utf8, scale);
}

// ---- HUD -------------------------------------------------------------------
void drawHud() {
    // Hull blocks, left. A block is 24x24 solid; the one just lost is an outline
    // in the slot it vacated for three frames.
    int shown = s_g.hull < PIP_MAX ? s_g.hull : PIP_MAX;
    for (int i = 0; i < shown; i++)
        canvas.fillRect(PAD + i * (PIP + PIP_GAP), PIP_Y, PIP, PIP, TFT_BLACK);
    if (s_lostPipFrames > 0 && s_lostPip >= 0 && s_lostPip < PIP_MAX)
        canvas.drawRect(PAD + s_lostPip * (PIP + PIP_GAP), PIP_Y, PIP, PIP, TFT_BLACK);
    if (s_g.hull > PIP_MAX) {
        char more[8];
        snprintf(more, sizeof(more), "+%d", s_g.hull - PIP_MAX);
        cjk::drawText(canvas, PAD + PIP_MAX * (PIP + PIP_GAP) + PIP_GAP, PIP_Y, more, 2);
    }

    // 层名, right-aligned. §8.5 asks for centred, which cannot survive its own
    // block budget: twelve blocks reach x=392 and a centred 3-glyph name at
    // scale 3 starts at x=216. Right-aligning puts the name at 416..524, clear of
    // a full hull row, and it still reads as the one big label on the bar.
    const char* name = tr(sp::LAYER_KEY[s_g.layer]);
    cjk::drawText(canvas, (sp::UI_W - PAD) - cjk::textWidth(name, 3), LAYER_Y, name, 3);
}

// The altitude gauge: an outline that fills from the bottom, plus a 3px notch at
// each layer boundary. It grows ~1 px a logic frame, far under the trail
// threshold, so it adds no ghosting of its own (§8.5).
void drawGauge() {
    canvas.drawRect(GAUGE_X, GAUGE_TOP, GAUGE_W, GAUGE_BOT - GAUGE_TOP, TFT_BLACK);
    int span = GAUGE_BOT - GAUGE_TOP;
    int filled = (int)((uint32_t)span * s_g.gameMs / (uint32_t)(sp::GAME_SECONDS * 1000));
    if (filled > span) filled = span;
    if (filled > 0)
        canvas.fillRect(GAUGE_X, GAUGE_BOT - filled, GAUGE_W, filled, TFT_BLACK);
    for (int i = 0; i < sp::LAYER_EDGE_COUNT; i++) {
        int km = sp::LAYER_EDGE[i];
        if (km >= sp::GAME_SECONDS) continue;          // 60 is the gauge's own top
        int y = GAUGE_BOT - span * km / sp::GAME_SECONDS;
        canvas.fillRect(GAUGE_X - 6, y - 1, 6, 3, TFT_BLACK);
    }
}

void drawShip(int x, int y, bool reversed) {
    int left = x - sp::SHIP_W / 2, top = y - sp::SHIP_H / 2;
    if (!reversed) { canvas.fillRect(left, top, sp::SHIP_W, sp::SHIP_H, TFT_BLACK); return; }
    // Hit feedback (§8.4): the ship's own rect reversed for two frames. A
    // full-screen flash would read better still and cost far more ghosting debt
    // than the hit is worth (§9).
    canvas.fillRect(left, top, sp::SHIP_W, sp::SHIP_H, TFT_WHITE);
    canvas.drawRect(left, top, sp::SHIP_W, sp::SHIP_H, TFT_BLACK);
    canvas.drawRect(left + 1, top + 1, sp::SHIP_W - 2, sp::SHIP_H - 2, TFT_BLACK);
}

void drawAsteroids() {
    for (int i = 0; i < sp::MAX_ASTEROIDS; i++) {
        const sp::Asteroid& a = s_g.ast[i];
        if (a.alive) canvas.fillCircle(a.x, a.y, a.r, TFT_BLACK);
    }
}

// The control band. Deliberately sparse ink: it redraws every frame and only the
// caret moves, so a heavy band would be 168 px of permanently re-driven pixels.
void drawCtrlBand() {
    for (int i = 0; i < TICKS; i++) {
        int x = i * (sp::UI_W - 1) / (TICKS - 1);
        if (x >= sp::UI_W - 1) x = sp::UI_W - 2;
        canvas.fillRect(x, TICK_Y, 2, TICK_H, TFT_BLACK);
    }
    canvas.fillTriangle(s_g.shipX, CARET_Y,
                        s_g.shipX - CARET_W / 2, CARET_Y + CARET_H,
                        s_g.shipX + CARET_W / 2, CARET_Y + CARET_H, TFT_BLACK);
}

// §9.3: the transition is a video-mode white field, NOT an image-mode push. One
// present clears the playfield to white and the next 161 ms of driving keeps it
// there — 276 ms of white is a soft wash of the one region that accumulates
// debt, bought for three frames instead of deghost()'s four.
void drawXition() {
    const char* name = tr(sp::LAYER_KEY[s_g.layer]);
    centred(name, (sp::PF_TOP + sp::PF_BOT) / 2 - 18, 3);
    int span = sp::PF_BOT - sp::PF_TOP;
    int y = sp::PF_TOP + span * (s_g.phaseFrame + 1) / sp::XITION_FRAMES;
    if (y > sp::PF_BOT - 4) y = sp::PF_BOT - 4;
    canvas.fillRect(0, y, sp::UI_W, 4, TFT_BLACK);
}

void drawScoreScreen() {
    canvas.fillSprite(TFT_WHITE);
    centred(tr("Space"), SC_TITLE_Y, 3);
    char num[16], line[64];
    snprintf(num, sizeof(num), "%lu", (unsigned long)s_score);
    fmt1(line, sizeof(line), tr("score for this game: {0}"), num);
    centred(line, SC_LINE0_Y, 2);
    snprintf(num, sizeof(num), "%lu", (unsigned long)g_game.scoreTotal);
    fmt1(line, sizeof(line), tr("total score: {0}"), num);
    centred(line, SC_LINE1_Y, 2);
    // 返回, not upstream's 「重启」 (space.js:526 restart. -> Engine.confirmDelete).
    // Upstream's win DELETES THE SAVE and rolls a prestige carry-over; §12 Q1
    // parks that decision in 3d, and a button labelled 重启 that does not restart
    // anything would be worse than not offering it yet.
    action_band::draw(canvas, pages::Rect{ 24, SC_BAND_Y, sp::UI_W - 48, SC_BAND_H },
                      BACK_LABEL, nullptr, true, 0, 0);
}

pages::Rect scoreBandRect() {
    return pages::Rect{ 24, SC_BAND_Y, sp::UI_W - 48, SC_BAND_H };
}

// ---- input ------------------------------------------------------------------
// The finger's x if a contact is DOWN inside the control band, else -1.
//
// This deliberately does NOT go through pager::handleTouch (§8.3, and the same
// bypass the three modals take by intercepting at the top of it): the pager
// discards the outer 24 px of the panel, debounces taps at 350 ms and latches a
// three-finger grip. Every one of those is right for a page of buttons and fatal
// for a control band — the band lives at the very bottom edge, hugging it is the
// normal way to play, and a 350 ms debounce is four logic frames of dead input.
int sampleBand() {
    int n = touch::count();
    for (int i = 0; i < n; i++) {
        touch::Detail d = touch::detail(i);
        if (!d.down) continue;                       // the new level bit (§7 gap 2)
        if (d.y < sp::CTRL_TOP) continue;
        return d.x;                                  // 1:1 absolute map, no filter
    }
    return -1;
}

// Has the panel read completely untouched at least once? The flight can be
// started from a band the finger is still resting on (the Starship page's liftoff
// band, or the confirmation event's 'fly' button, both low on the screen), and
// without this the ship would teleport to that x on frame one. Same
// swallow-until-lift shape as pager.cpp's modal-open guard, and it terminates on
// reality rather than on a timer.
bool     s_liftSeen = false;
int      gatedBand() {
    if (!s_liftSeen) {
        if (touch::count() == 0) s_liftSeen = true;
        return -1;
    }
    return sampleBand();
}

}  // namespace

// ---- the pager's view of us -------------------------------------------------
bool active() { return s_active; }

void renderFrame() {
    if (s_scoreScreen) { drawScoreScreen(); return; }

    canvas.fillSprite(TFT_WHITE);

    const bool crashText = (s_g.phase == sp::PH_CRASH_TEXT) ||
                           (s_g.phase == sp::PH_OVER && s_g.crashed);
    if (crashText) {
        // §8.6 step 3. No HUD, no band: the flight is over and the only thing on
        // the glass is the line the player is about to read through the deghost.
        centred(CRASH_LINE, (sp::UI_H - 36) / 2, 3);
        return;
    }

    drawHud();

    // Everything below is clipped to the playfield so a circle straddling the
    // y=84 boundary grows out from under the rule instead of scribbling on the
    // HUD, and so the ship's exit on a win simply runs out of canvas.
    canvas.setClipRect(0, sp::PF_TOP, sp::UI_W, sp::PF_BOT - sp::PF_TOP);
    switch (s_g.phase) {
    case sp::PH_PLAY:
        drawGauge();
        drawAsteroids();
        drawShip(s_g.shipX, s_g.shipY, s_g.hitFlash > 0);
        break;
    case sp::PH_XITION:
        // Playfield stays white — that IS the transition (§9.3).
        drawXition();
        break;
    case sp::PH_CRASH_POP:
        drawGauge();
        drawAsteroids();
        canvas.fillRect(s_g.shipX - 48, s_g.shipY - 48, 96, 96, TFT_BLACK);
        break;
    case sp::PH_CRASH_BLACK:
        canvas.fillRect(0, sp::PF_TOP, sp::UI_W, sp::PF_BOT - sp::PF_TOP, TFT_BLACK);
        break;
    case sp::PH_WIN_RISE:
        drawGauge();
        drawAsteroids();
        drawShip(s_g.shipX, s_g.shipY, false);
        break;
    case sp::PH_WIN_WHITE:
    case sp::PH_OVER:
        drawGauge();
        break;
    default:
        break;
    }
    canvas.clearClipRect();

    canvas.fillRect(0, sp::PF_TOP, sp::UI_W, RULE_H, TFT_BLACK);
    canvas.fillRect(0, sp::PF_BOT, sp::UI_W, RULE_H, TFT_BLACK);
    drawCtrlBand();
}

// ---- the flight -------------------------------------------------------------
void run() {
    // Ship.getMaxHull() is the persistent hull (§1.2); the level spends a COPY.
    int hull = g_game.shipHull > 0 ? g_game.shipHull : 1;
    sp::reset(s_g, hull, g_game.shipThrusters, (uint32_t)millis() * 2654435761u + 1u);
    s_active       = true;
    s_scoreScreen  = false;
    s_liftSeen     = false;
    s_lostPip      = -1;
    s_lostPipFrames = 0;
    s_deghosts     = 0;

    // §9.2: settle the whole ghosting debt BEFORE the flight, not during it. This
    // is the biggest content replacement of the session (a page of text becomes a
    // moving field), so it is the right place to start from clean glass — and it
    // composes OUR first frame, because pager::drawFrame now dispatches to
    // renderFrame() while active() is true.
    pager::deghost();
    s_deghosts++;

    uint32_t sumGap = 0, maxGap = 0, gaps = 0;
    uint32_t prevStart = 0;
    // Prime the scan counters so the line printed after the flight describes THIS
    // flight and nothing else (statsLine differences against its previous call).
    // The 3 s heartbeat cannot cover a flight — appLoop is blocked for its whole
    // duration — so this is the only place §11.3b.1's fps / frame_us_max /
    // dma_timeouts can actually be read from.
    { char prime[96]; msg_bridge::statsLine(millis(), prime, sizeof prime); }
    beeper::tone(660, 120);                     // LIFT_OFF (space.js Ship.liftOff)

    for (;;) {
        uint32_t frameStart = millis();
        if (prevStart) {
            uint32_t gap = frameStart - prevStart;
            sumGap += gap; gaps++;
            if (gap > maxGap) maxGap = gap;
        }
        prevStart = frameStart;

        touch::update(frameStart);
        int hullBefore = s_g.hull;
        sp::FrameOut ev = sp::step(s_g, gatedBand());

        if (ev.hit) {
            // The block that vanished is the highest-numbered one still shown.
            s_lostPip = hullBefore - 1;
            if (s_lostPip >= PIP_MAX) s_lostPip = -1;   // off the shown row
            s_lostPipFrames = 3;
            beeper::tone(sp::hitToneHz(s_g.altitude), 60);
        } else if (s_lostPipFrames > 0) {
            s_lostPipFrames--;
        }
        if (ev.layerUp) beeper::tone(1400, 60);
        if (ev.crashed) beeper::tone(300, 240);        // world_page's death tone
        if (ev.won)     beeper::tone(1047, 120);

        renderFrame();
        msg_bridge::present();

        // THE METRONOME (§8.2). present() blocks to the next VSYNC only while the
        // scan is running; the governor parks it whenever the picture stops
        // changing, which is exactly what the crash freeze and the victory white
        // do. Without this loop those phases would spin at the render rate.
        while ((int32_t)(millis() - frameStart) < (int32_t)sp::FRAME_MS) delay(2);
        beeper::tick(millis());

        if (sp::done(s_g)) break;
    }

    // Land the outcome in the model FIRST — before the score screen, because that
    // screen's second line is the running total and it has to include the flight
    // the player just finished (upstream calls Prestige.save() before printing
    // it, space.js:440). The epoch clock is the ship page's own (RTC, not millis)
    // because that is what liftoffCooldownLeft measures against.
    uint32_t now = epochNow();
    if (s_g.won) {
        s_score = g_game.score();
        g_game.onSpaceVictory(now, s_score);
    } else {
        g_game.onSpaceCrash(now);
    }
    g_game.save();

    // §8.6 step 4, both endings: pay the whole session's ghosting debt here. On a
    // crash it runs UNDER the crash line, which is already on the glass, so the
    // 400 ms reads as a beat rather than a stall; on a win it fills the gap
    // between the empty sky and the numbers.
    pager::deghost();
    s_deghosts++;
    if (s_g.won) {
        s_scoreScreen = true;
        beeper::tone(1568, 200);
    }

    // The acceptance line (§11.3b.1/.5/.6/.7), all of it measured rather than
    // asserted: the logic-frame interval (target 92 +/- 5 ms, max < 120), the
    // scan's own health over the flight, the transition count, and the number of
    // image-mode deghosts — which MUST be 2, one on the way in and one on the way
    // out, and never one from inside the loop.
    char scan[96]; msg_bridge::statsLine(millis(), scan, sizeof scan);
    Serial.printf("[space] frames=%lu hits=%u alt=%d layers=%u result=%s "
                  "score=%lu gap_avg=%lums gap_max=%lums deghosts=%d | %s\n",
                  (unsigned long)s_g.frames, s_g.hits, s_g.altitude, s_g.xitions,
                  s_g.won ? "win" : "crash", (unsigned long)(s_g.won ? s_score : 0),
                  (unsigned long)(gaps ? sumGap / gaps : 0), (unsigned long)maxGap,
                  s_deghosts, scan);

    if (s_scoreScreen) {
        // A static page in every sense that matters (§8.6): one composed frame,
        // no game loop, no debt. It waits for a press on its one band, gated on
        // the panel reading untouched first — the deghost that just ran drives
        // all 540x960 hard and that is a known source of phantom contacts
        // (pager.cpp's modal-open guard exists for the same reason).
        renderFrame();
        msg_bridge::present();
        // Idle watchdog, the same 2 minutes event_modal and setpiece_modal use.
        // Without one this loop owns the app task forever: appLoop is what runs
        // the sleep timer, so a player who walks away from the score screen would
        // leave the card awake until the battery went.
        const uint32_t SC_TIMEOUT_MS = 120u * 1000u;
        uint32_t shownAt = millis();
        bool lifted = false;
        for (;;) {
            uint32_t t = millis();
            if (t - shownAt > SC_TIMEOUT_MS) break;
            touch::update(t);
            beeper::tick(t);
            int n = touch::count();
            if (!lifted) { if (n == 0) lifted = true; delay(20); continue; }
            bool press = false;
            for (int i = 0; i < n; i++) {
                touch::Detail d = touch::detail(i);
                if (!d.clicked && !d.held) continue;
                pages::Rect r = scoreBandRect();
                if (d.x >= r.x && d.x < r.x + r.w && d.y >= r.y && d.y < r.y + r.h)
                    press = true;
            }
            if (press) { pager::flashPressRect(scoreBandRect()); break; }
            delay(20);
        }
    }

    // Back to the Starship page. Both outcomes restamped the 120 s cooldown
    // (space.js:376), so the page the player lands on is already showing the
    // drained bar — which is the whole visible consequence of a crash.
    s_active      = false;
    s_scoreScreen = false;

    int ring = pager::ringIndexByName("ship");
    if (ring >= 0) pager::showPage(ring, false);
    else           pager::repaint();
}

}  // namespace space_page
