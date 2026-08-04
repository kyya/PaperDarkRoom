// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Space level rules — see space_game.h for the upstream-vs-port table. Pure
// C++: this file is compiled BOTH into the firmware and into
// tools/mechanics_test.cpp, which is the only way a 653-frame action level can
// be regression-tested at all on a box with no panel.
#include "space_game.h"

namespace adr {
namespace space {

// The six official层名 keys, in altitude order (§2.6 / §6.1 — all six are in
// strings_zh.h already, so this costs no new glyph).
const char* const LAYER_KEY[LAYER_COUNT] = {
    "Troposphere", "Stratosphere", "Mesosphere",
    "Thermosphere", "Exosphere",   "Space" };

// Upstream's own thresholds (§2.6). 45 is the odd one out and is upstream's
// intent, not a typo: the Thermosphere is the thick layer, so it gets 15 km.
const int LAYER_EDGE[LAYER_EDGE_COUNT] = { 10, 20, 30, 45, 60 };

uint8_t layerOf(int km) {
    if (km < 10) return 0;
    if (km < 20) return 1;
    if (km < 30) return 2;
    if (km < 45) return 3;
    if (km < 60) return 4;
    return 5;
}

bool isLayerEdge(int km) {
    for (int i = 0; i < LAYER_EDGE_COUNT; i++) if (LAYER_EDGE[i] == km) return true;
    return false;
}

// §8.3's table, as its formula. Written as min(72, 32 + 8*t) so the table and
// the code are the same object; thrusters is >= 1 by construction (ship.js
// BASE_THRUSTERS), but a corrupt save reading 0 must not produce a ship that
// cannot move at all, hence the floor.
int shipMaxStep(int thrusters) {
    if (thrusters < 1) thrusters = 1;
    int s = 32 + 8 * thrusters;
    return s > 72 ? 72 : s;
}

// §8.4 d4. Upstream's `1000 - altitude*10` ms (§2.4) is NOT used: at 92 ms a
// frame and 48 px circles it tops out at 15 asteroids/second over a 540 px
// field, which is 20+ alive at once and a black playfield. The re-standardised
// curve keeps the SHAPE (density rises with altitude, in steps that coincide
// with the layer boundaries the transition 演出 already announces) and caps the
// end state at 6.5/s.
int spawnIntervalFrames(int km) {
    if (km <= 10) return 10;    // 920 ms  -> 1.1/s
    if (km <= 20) return 7;     // 644 ms  -> 1.6/s
    if (km <= 30) return 7;     // 644 ms  -> 3.1/s
    if (km <= 45) return 5;     // 460 ms  -> 4.3/s
    return 5;                   // 460 ms  -> 6.5/s
}

int waveCount(int km) {
    if (km <= 20) return 1;
    if (km <= 45) return 2;
    return 3;
}

// §2.5's three altitude bands, three pitches instead of eight samples (§8.4).
int hitToneHz(int km) {
    if (km > 40) return 1600;
    if (km > 20) return 1200;
    return 880;
}

namespace {

// xorshift32 — the same generator world_state uses for its expedition stream, so
// a seeded flight replays identically on host and device.
uint32_t nextRand(Game& g) {
    uint32_t x = g.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g.rng = x ? x : 0x1234567u;      // xorshift32 has one absorbing state
    return g.rng;
}

// Uniform in [lo, hi] inclusive.
int randRange(Game& g, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(nextRand(g) % (uint32_t)(hi - lo + 1));
}

// §8.4 d1: 40 / 48 / 56 px at 0.35 / 0.45 / 0.20.
int randDiameter(Game& g) {
    uint32_t r = nextRand(g) % 100u;
    if (r < 35) return AST_DIAM_SMALL;
    if (r < 80) return AST_DIAM_MID;
    return AST_DIAM_BIG;
}

// One asteroid into the first free slot. Returns false when the field is full —
// the §8.4 d4 cap, enforced per asteroid rather than per wave so a wave of 3 at
// 15 alive still places the two that fit.
bool spawnOne(Game& g) {
    int slot = -1;
    for (int i = 0; i < MAX_ASTEROIDS; i++) if (!g.ast[i].alive) { slot = i; break; }
    if (slot < 0) return false;
    int d = randDiameter(g);
    int r = d / 2;
    Asteroid& a = g.ast[slot];
    a.r     = (uint8_t)r;
    // Fully on-screen: upstream lets a glyph hang off the edge, which on a
    // 540 px field would make the two outermost columns unfairly safe.
    a.x     = (int16_t)randRange(g, r, UI_W - r);
    // §8.5: "asteroids are generated from y=84". The circle is clipped to the
    // playfield while its centre is still near the top rule, so it grows out of
    // the boundary instead of popping into existence whole.
    a.y     = (int16_t)PF_TOP;
    // §8.4 d5: uniform [24,48] px/frame, the spec's stated draw. It is the
    // random DURATION of upstream's `1500 - rand*975` re-expressed as a speed
    // and re-scaled to the readability window (§8.4 d3).
    a.vy    = (uint8_t)randRange(g, AST_SPEED_MIN, AST_SPEED_MAX);
    a.alive = true;
    return true;
}

void spawnWave(Game& g) {
    int n = waveCount(g.altitude);
    for (int i = 0; i < n; i++) if (!spawnOne(g)) break;
}

// §8.4 d5: AABB, the ship's real 48x36 body against the asteroid's bounding
// square. Upstream tests a POINT (the ship's CSS left/top, itself offset by a
// margin) against the glyph box — an implementation compromise we do not have to
// inherit, and one that would read as unfair at this scale.
bool overlaps(const Asteroid& a, int shipX, int shipY) {
    return !(a.x + a.r < shipX - SHIP_W / 2 || a.x - a.r > shipX + SHIP_W / 2 ||
             a.y + a.r < shipY - SHIP_H / 2 || a.y - a.r > shipY + SHIP_H / 2);
}

}  // namespace

void reset(Game& g, int hull, int thrusters, uint32_t seed) {
    g = Game{};
    g.rng      = seed ? seed : 0xA5A5F00Du;
    g.shipX    = UI_W / 2;                       // space.js:64 starts centred
    g.shipY    = SHIP_Y;
    g.maxHull  = (int16_t)(hull < 0 ? 0 : hull);
    g.hull     = g.maxHull;
    g.maxStep  = (int16_t)shipMaxStep(thrusters);
    g.altitude = 0;
    g.layer    = layerOf(0);
    g.spawnIn  = (int16_t)spawnIntervalFrames(0);
    g.phase    = PH_PLAY;
}

int aliveCount(const Game& g) {
    int n = 0;
    for (int i = 0; i < MAX_ASTEROIDS; i++) if (g.ast[i].alive) n++;
    return n;
}

bool done(const Game& g) { return g.phase == PH_OVER; }

FrameOut step(Game& g, int touchX) {
    FrameOut ev{};
    if (g.phase == PH_OVER) return ev;
    g.frames++;

    // ---- input ------------------------------------------------------------
    // Live only while the player can still affect the outcome. During the crash
    // and victory 演出 the ship is scenery, and a stray contact must not drag it
    // around under the burst.
    if ((g.phase == PH_PLAY || g.phase == PH_XITION) && touchX >= 0) {
        int dx = touchX - g.shipX;
        if (dx >  g.maxStep) dx =  g.maxStep;
        if (dx < -g.maxStep) dx = -g.maxStep;
        g.shipX = (int16_t)(g.shipX + dx);
        if (g.shipX < SHIP_X_MIN) g.shipX = SHIP_X_MIN;
        if (g.shipX > SHIP_X_MAX) g.shipX = SHIP_X_MAX;
    }
    if (g.hitFlash) g.hitFlash--;

    switch (g.phase) {
    case PH_PLAY:
    case PH_XITION: {
        // Asteroids keep falling through a transition (§9.3: "their positions
        // advance as usual, they are just not drawn"), so the field the player
        // gets back is the field that would have been there.
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            Asteroid& a = g.ast[i];
            if (!a.alive) continue;
            a.y = (int16_t)(a.y + a.vy);
            if (a.y - a.r > PF_BOT) a.alive = false;   // §8.5: gone past y=788
        }

        if (g.phase == PH_XITION) {
            // The white 演出 does NOT test collisions. This is a deviation from
            // "positions advance as usual" and a deliberate one: for 276 ms the
            // playfield is blank, so a hit taken here is one the player was
            // given no frame in which to avoid. §9.3 sells the transition as a
            // breather and a difficulty-shift cue; a breather that can kill you
            // invisibly is neither.
            if (++g.phaseFrame >= XITION_FRAMES) {
                g.phaseFrame = 0;
                if (g.altitude >= GAME_SECONDS) {
                    // §2.6: 60 km is the end of the flight. The Space transition
                    // plays first, so the last thing named is where they arrived.
                    g.phase = PH_WIN_RISE;
                    g.won   = true;
                    ev.won  = true;
                    // The first 240 px happen on THIS frame, so the three
                    // rendered rise frames are 500 / 260 / 20 and the third is
                    // clipped away by the playfield's top edge — §8.6's "flies
                    // off the top on frame 3", exactly.
                    g.shipY = (int16_t)(g.shipY - WIN_RISE_STEP);
                } else {
                    g.phase = PH_PLAY;
                }
            }
            break;
        }

        // ---- collisions ---------------------------------------------------
        // A hit removes the asteroid and costs exactly 1 hull, upstream's rule
        // including "one asteroid can only ever hit once" (§2.5). Several can
        // land in the same frame; hull is checked after each so the crash fires
        // on the one that actually emptied it.
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            Asteroid& a = g.ast[i];
            if (!a.alive || !overlaps(a, g.shipX, g.shipY)) continue;
            a.alive = false;
            g.hits++;
            ev.hit = true;
            g.hitFlash = HIT_FLASH_FRAMES;
            if (g.hull > 0) g.hull--;
            if (g.hull <= 0) {
                g.phase      = PH_CRASH_POP;
                g.phaseFrame = 0;
                g.crashed    = true;
                ev.crashed   = true;
                return ev;                    // the flight is over; stop stepping it
            }
        }

        // ---- spawning ------------------------------------------------------
        if (--g.spawnIn <= 0) {
            spawnWave(g);
            g.spawnIn = (int16_t)spawnIntervalFrames(g.altitude);
        }

        // ---- the altitude clock (§2.6) --------------------------------------
        // Upstream ticks it on a 1 s setInterval. Here it is derived from
        // accumulated GAME time so that pausing the clock across a transition
        // (§9.3) is a matter of not adding to it, and so the whole flight is a
        // pure function of the frame count.
        g.gameMs += FRAME_MS;
        int alt = (int)(g.gameMs / 1000u);
        if (alt > GAME_SECONDS) alt = GAME_SECONDS;
        if (alt != g.altitude) {
            g.altitude = (int16_t)alt;
            if (isLayerEdge(alt)) {
                g.layer      = layerOf(alt);
                g.phase      = PH_XITION;
                g.phaseFrame = 0;
                g.xitions++;
                ev.layerUp   = true;
            }
        }
        break;
    }

    case PH_CRASH_POP:
        if (++g.phaseFrame >= CRASH_POP_FRAMES) { g.phase = PH_CRASH_BLACK; g.phaseFrame = 0; }
        break;
    case PH_CRASH_BLACK:
        if (++g.phaseFrame >= CRASH_BLACK_FRAMES) { g.phase = PH_CRASH_TEXT; g.phaseFrame = 0; }
        break;
    case PH_CRASH_TEXT:
        if (++g.phaseFrame >= CRASH_TEXT_FRAMES) { g.phase = PH_OVER; ev.over = true; }
        break;

    case PH_WIN_RISE:
        // The ship climbs out of the top of the frame; the asteroids already on
        // the field keep falling and nothing new is spawned (§8.6 step 1).
        g.shipY = (int16_t)(g.shipY - WIN_RISE_STEP);
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            Asteroid& a = g.ast[i];
            if (!a.alive) continue;
            a.y = (int16_t)(a.y + a.vy);
            if (a.y - a.r > PF_BOT) a.alive = false;
        }
        if (++g.phaseFrame >= WIN_RISE_FRAMES) { g.phase = PH_WIN_WHITE; g.phaseFrame = 0; }
        break;

    case PH_WIN_WHITE:
        for (int i = 0; i < MAX_ASTEROIDS; i++) {
            Asteroid& a = g.ast[i];
            if (!a.alive) continue;
            a.y = (int16_t)(a.y + a.vy);
            if (a.y - a.r > PF_BOT) a.alive = false;
        }
        if (++g.phaseFrame >= WIN_WHITE_FRAMES) { g.phase = PH_OVER; ev.over = true; }
        break;

    default:
        break;
    }
    return ev;
}

}  // namespace space
}  // namespace adr
