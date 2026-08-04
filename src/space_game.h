// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Space — the asteroid run (Phase 3b). THE RULES ONLY: geometry, the difficulty
// curve, collisions, the altitude clock and the crash / victory state machines.
// No Arduino, no M5, no drawing, no touch — exactly the split game_state.h and
// world_state.h already use, so tools/mechanics_test.cpp can compile this file
// on the host and drive a whole 60-second flight deterministically. Everything
// that needs a panel or a finger lives in space_page.cpp.
//
// UPSTREAM vs THIS PORT (docs/research-phase3.md). The upstream level (space.js)
// is a 30 fps keyboard game: 8-way speed control at `3 + thrusters` px/frame,
// asteroids as spinning ASCII glyphs falling at 520-1486 px/s, one wave every
// `1000 - altitude*10` ms growing to 15 asteroids/second, point-vs-box hit
// tests. NONE of that survives contact with a 43 Hz 1bpp panel whose driver
// takes 161 ms to flip a pixel (§8.1), so §8.2-§8.4 re-derive the whole thing
// against the display and this file implements THAT:
//
//   upstream                      | here                      | why (spec)
//   30 fps                        | 10.85 fps (92 ms frame)   | §8.2
//   speed control, 3+thrusters    | absolute 1:1 finger map,  | §8.3 — speed
//                                 | thrusters = px/frame cap  | control overshoots
//                                 |                           | at 200 ms latency
//   ASCII glyphs, 520-1486 px/s   | solid circles, 24-48      | §8.4 d1/d3 —
//                                 | px/frame (260-521 px/s)   | readable under a
//                                 |                           | 161 ms trail
//   1000-alt*10 ms, up to 15/s    | the §8.4 d4 table, <=6.5  | §8.4 d4 — 15/s
//                                 | /s and <=16 alive         | paints the field
//                                 |                           | black
//   point vs glyph box            | AABB, ship 48x36          | §8.4 d5
//
// THREE THINGS ARE KEPT EXACTLY (§8.1): the 60-second flight, hull as the only
// resource, and a crash costing nothing but the 120 s cooldown. Those three are
// what make an action level playable at all on e-ink — a miss has to be cheap.
#pragma once
#include <stdint.h>

namespace adr {
namespace space {

// ---- geometry (§8.5, the 540x960 portrait content frame) -------------------
constexpr int UI_W    = 540;
constexpr int UI_H    = 960;
constexpr int PF_TOP  = 84;     // playfield top; the HUD owns 0..84
constexpr int PF_BOT  = 788;    // playfield bottom; the control band owns 792..960
constexpr int CTRL_TOP = 792;   // absolute-mapping control band, full width

constexpr int SHIP_W  = 48;
constexpr int SHIP_H  = 36;
constexpr int SHIP_Y  = 740;                    // fixed: this is a 1-D level (§8.3)
constexpr int SHIP_X_MIN = SHIP_W / 2;          // 24  — acceptance §11.3b.2
constexpr int SHIP_X_MAX = UI_W - SHIP_W / 2;   // 516 — acceptance §11.3b.2

// ---- tempo (§8.2) ----------------------------------------------------------
// 4 scan frames at the measured 23.05 ms = 92.2 ms. Rounded DOWN to 92 so the
// logic frame is never longer than the 4 scans it is meant to cover.
constexpr uint32_t FRAME_MS = 92;
// Upstream's altitude clock: +1 km a second, 0..60, and 60 s IS the flight
// (§2.6). Layer transitions pause it, so the wall clock reads ~61.4 s (§9.3).
constexpr int GAME_SECONDS = 60;

// ---- asteroids (§8.4) ------------------------------------------------------
constexpr int MAX_ASTEROIDS  = 16;   // §8.4 d4 hard cap; over it a wave is skipped
constexpr int AST_SPEED_MIN  = 24;   // px per logic frame == 260 px/s
constexpr int AST_SPEED_MAX  = 48;   // px per logic frame == 521 px/s
// Three diameters at 0.35 / 0.45 / 0.20 (§8.4 d1). 40 is the hard floor: below
// it the 161 ms trail smears a circle into a vertical line.
constexpr int AST_DIAM_SMALL = 40;
constexpr int AST_DIAM_MID   = 48;
constexpr int AST_DIAM_BIG   = 56;

// ---- 演出 lengths, in logic frames -----------------------------------------
constexpr int XITION_FRAMES      = 3;   // §9.3 layer transition: 276 ms of white
constexpr int CRASH_POP_FRAMES   = 1;   // §8.6 the 96x96 "burst"
constexpr int CRASH_BLACK_FRAMES = 3;   // §8.6 playfield filled black
constexpr int CRASH_TEXT_FRAMES  = 1;   // §8.6 white + the crash line
constexpr int WIN_RISE_FRAMES    = 3;   // §8.6 the ship climbs out of frame
constexpr int WIN_WHITE_FRAMES   = 8;   // §8.6 empty sky, stragglers fall out
constexpr int WIN_RISE_STEP      = 240; // px per frame; 3 frames clears y=740
constexpr int HIT_FLASH_FRAMES   = 2;   // §8.4 ship reversed on a hit (184 ms)

// ---- atmosphere layers (§2.6) ---------------------------------------------
// SIX names, and the thresholds are UPSTREAM's own. What is NOT upstream's is
// WHEN they change: space.js only calls setTitle() on `altitude % 10 === 0`, so
// its 45 km boundary does not take effect until 50 (§2.6 bug / §12 Q4). We
// switch on the threshold — the spec's recommendation, and forced anyway because
// our layer transition 演出 fires AT 45 and would otherwise announce the old name.
constexpr int LAYER_COUNT = 6;
extern const char* const LAYER_KEY[LAYER_COUNT];   // tr() keys; all six are already
                                                  // in the official table (§6.1)
// The five altitudes that both rename the layer and play a transition (§9.3).
constexpr int LAYER_EDGE_COUNT = 5;
extern const int LAYER_EDGE[LAYER_EDGE_COUNT];     // {10, 20, 30, 45, 60}

// Which layer name altitude `km` reads under. <10 / <20 / <30 / <45 / <60 / >=60.
uint8_t layerOf(int km);
// True when `km` is one of LAYER_EDGE (i.e. crossing it plays a transition).
bool    isLayerEdge(int km);

// ---- derived rules ---------------------------------------------------------
// §8.3: thrusters stop being a SPEED (there is no speed under an absolute map)
// and become how far the ship may close on the finger in one logic frame. 1 ->
// 40 px/frame (434 px/s), +8 a level, capped at 72 (the finger is matched
// instantly). The cap is a deliberate 收敛 of upstream's unbounded engine (§12
// Q12): past 5 thrusters there is nothing left to buy on this axis.
int shipMaxStep(int thrusters);
// §8.4 d4: logic frames between waves, and asteroids per wave, by altitude.
int spawnIntervalFrames(int km);
int waveCount(int km);
// §8.4: the hit tone. Upstream's three altitude bands (§2.5) survive; its eight
// samples collapse to three pitches.
int hitToneHz(int km);

// ---- state -----------------------------------------------------------------
struct Asteroid {
    int16_t x, y;      // CENTRE, portrait px
    uint8_t r;         // radius
    uint8_t vy;        // px per logic frame
    bool    alive;
};

enum Phase : uint8_t {
    PH_PLAY = 0,
    PH_XITION,       // §9.3 layer transition (white playfield, new name)
    PH_CRASH_POP,    // §8.6 crash step 1
    PH_CRASH_BLACK,  //              step 2
    PH_CRASH_TEXT,   //              step 3
    PH_WIN_RISE,     // §8.6 victory step 2
    PH_WIN_WHITE,    //               step 3
    PH_OVER          // finished; renders as the last 演出 frame
};

struct Game {
    uint32_t rng;              // xorshift32, seeded per flight
    Asteroid ast[MAX_ASTEROIDS];
    int16_t  shipX, shipY;
    int16_t  hull, maxHull;    // TEMPORARY (§1.2): the persistent hull is untouched
    int16_t  maxStep;          // shipMaxStep(thrusters), fixed for the flight
    int16_t  altitude;         // km, 0..60
    uint32_t gameMs;           // game time; frozen during transitions (§9.3)
    int16_t  spawnIn;          // logic frames until the next wave
    uint8_t  phase;            // Phase
    uint8_t  phaseFrame;       // frames spent in the current non-PLAY phase
    uint8_t  layer;            // index into LAYER_KEY
    uint8_t  hitFlash;         // frames left of the reversed-ship hit feedback
    uint8_t  xitions;          // transitions played (5 by the end — §11.3b.6)
    uint16_t hits;             // asteroids taken, for the serial summary
    uint32_t frames;           // logic frames stepped, for the serial summary
    bool     crashed, won;
};

// What ONE step() did, for the sound/serial layer above. Every field is an EDGE
// (true for the single frame the thing happened), never a level.
struct FrameOut {
    bool hit;       // an asteroid connected (hull already decremented)
    bool layerUp;   // a layer transition began
    bool crashed;   // the crash sequence began (hull hit 0)
    bool won;       // the victory sequence began (60 km)
    bool over;      // the last 演出 frame has been reached; run() may stop
};

// Arm a flight. `hull` is Ship.getMaxHull() (== the persistent shipHull) and
// `thrusters` the persistent shipThrusters; NEITHER is written back — upstream
// crashes cost nothing (§2.7 / §12 Q14). `seed` makes the flight reproducible on
// the host.
void reset(Game& g, int hull, int thrusters, uint32_t seed);

// Advance one 92 ms logic frame. `touchX` is the finger's x if it is inside the
// control band this frame, or <0 for "no contact" — on which the ship HOLDS its
// position (§8.3: no re-centring, no deceleration; an absolute map has no
// momentum to bleed off).
FrameOut step(Game& g, int touchX);

// The run is finished (phase == PH_OVER). The frame for it has already been
// composed by the caller's renderer, so this means "stop stepping", not "stop
// drawing".
bool done(const Game& g);

// Asteroids currently on the field — the §8.4 d4 cap is enforced against this.
int aliveCount(const Game& g);

}  // namespace space
}  // namespace adr
