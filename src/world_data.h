// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room (Doublespeak Games, MPL-2.0) — Phase 2 World numeric data tables.
// Every constant here is transcribed from upstream script/world.js and
// script/path.js — the values ARE the port, so this file is a derivative of the
// MPL game and carries the MPL header. Pure data + tiny pure helpers, no
// Arduino/M5 dependency, so world_state's logic can be host-compiled for the
// smoke test (tools/world_smoke.cpp). Tile chars / landmark labels double as
// tr() keys where they surface in UI later (that UI is NOT part of milestone 2.0).
//
// PORTING NOTES (deviations from upstream, all deliberate, kept faithful to the
// observable game):
//  * Probabilities are integer permille, not JS floats — the stickiness weights
//    always sum to exactly 1000 (proof in world_state.cpp chooseTile), so this
//    is arithmetically identical to upstream's float model but reproducible bit-
//    for-bit on host and on device (no cross-platform float drift).
//  * Cells outside the generated diamond (|dx|+|dy| > RADIUS) are T_VOID, a
//    non-terrain blank. Upstream leaves them `undefined`; T_VOID reproduces the
//    same effect — landmarks (which only place on terrain) never land there, and
//    stepping onto one consumes supplies like empty ground.
#pragma once
#include <stdint.h>
#include <string.h>

namespace adr {

// ---- Dimensions (world.js RADIUS / getDimension) --------------------------
constexpr int      WORLD_RADIUS = 30;                     // world.js World.RADIUS
constexpr int      WORLD_DIM    = WORLD_RADIUS * 2 + 1;   // 61 (indices 0..60)
constexpr int      WORLD_CELLS  = WORLD_DIM * WORLD_DIM;  // 3721
constexpr int      WORLD_MASK_BYTES = (WORLD_CELLS + 7) / 8;  // 466
constexpr int      VILLAGE_X = WORLD_RADIUS;              // 30, VILLAGE_POS center
constexpr int      VILLAGE_Y = WORLD_RADIUS;

// ---- Survival / movement constants (world.js) -----------------------------
constexpr int STICKINESS_PM    = 500;   // World.STICKINESS 0.5, in permille
constexpr int LIGHT_RADIUS     = 2;     // World.LIGHT_RADIUS (Manhattan reveal)
constexpr int BASE_WATER       = 10;    // World.BASE_WATER
constexpr int BASE_HEALTH      = 10;    // World.BASE_HEALTH
constexpr int MOVES_PER_FOOD   = 2;     // World.MOVES_PER_FOOD (eat 1 meat / 2 steps)
constexpr int MOVES_PER_WATER  = 1;     // World.MOVES_PER_WATER (drink / step)
constexpr int DEATH_COOLDOWN_S = 120;   // World.DEATH_COOLDOWN (embark lockout)
constexpr int FIGHT_CHANCE_PM  = 200;   // World.FIGHT_CHANCE 0.20, permille
constexpr int FIGHT_DELAY      = 3;     // World.FIGHT_DELAY (min steps between fights)
constexpr int BASE_HIT_CHANCE_PM = 800; // World.BASE_HIT_CHANCE 0.8 (combat, P2.3)
constexpr int MEAT_HEAL        = 8;     // World.MEAT_HEAL
constexpr int MEDS_HEAL        = 20;    // World.MEDS_HEAL
constexpr int HYPO_HEAL        = 30;    // World.HYPO_HEAL (P3)

// Direction vectors (world.js NORTH/SOUTH/WEST/EAST). x east+, y south+.
enum Dir : uint8_t { DIR_NORTH = 0, DIR_SOUTH, DIR_WEST, DIR_EAST };
struct Vec2 { int8_t dx, dy; };
static const Vec2 DIR_VEC[4] = {
    { 0, -1 },   // NORTH
    { 0,  1 },   // SOUTH
    { -1, 0 },   // WEST
    {  1, 0 },   // EAST
};

// ---- Tiles (world.js World.TILE) ------------------------------------------
// One byte per cell (20 tile types > 16, so 4-bit packing can't hold them —
// simple correctness over density, per the milestone brief). T_VOID (index 0,
// char ' ') is our non-terrain blank for ungenerated corners.
enum Tile : uint8_t {
    T_VOID = 0,          // ungenerated / blank (not terrain, no event)
    T_VILLAGE,           // A — home
    T_IRON_MINE,         // I
    T_COAL_MINE,         // C
    T_SULPHUR_MINE,      // S
    T_FOREST,            // ; — terrain
    T_FIELD,             // , — terrain
    T_BARRENS,           // . — terrain
    T_ROAD,              // #
    T_HOUSE,             // H
    T_CAVE,              // V
    T_TOWN,              // O
    T_CITY,              // Y
    T_OUTPOST,           // P — spawned by clearing a dungeon
    T_SHIP,              // W
    T_BOREHOLE,          // B
    T_BATTLEFIELD,       // F
    T_SWAMP,             // M
    T_CACHE,             // U — prestige only
    T_EXECUTIONER,       // X
    TILE_COUNT
};
// world.js World.TILE character map (used by the future map renderer + tests).
static const char TILE_CHAR[TILE_COUNT] = {
    ' ',  // T_VOID
    'A',  // T_VILLAGE
    'I',  // T_IRON_MINE
    'C',  // T_COAL_MINE
    'S',  // T_SULPHUR_MINE
    ';',  // T_FOREST
    ',',  // T_FIELD
    '.',  // T_BARRENS
    '#',  // T_ROAD
    'H',  // T_HOUSE
    'V',  // T_CAVE
    'O',  // T_TOWN
    'Y',  // T_CITY
    'P',  // T_OUTPOST
    'W',  // T_SHIP
    'B',  // T_BOREHOLE
    'F',  // T_BATTLEFIELD
    'M',  // T_SWAMP
    'U',  // T_CACHE
    'X',  // T_EXECUTIONER
};
constexpr char PLAYER_CHAR = '@';  // world.js: the wanderer, render-only

// world.js World.isTerrain — only these three are walk-on generatable ground.
inline bool isTerrain(uint8_t t) {
    return t == T_FOREST || t == T_FIELD || t == T_BARRENS;
}

// ---- Terrain probabilities (world.js World.TILE_PROBS, permille, sum 1000) --
// Index by terrain, in the fixed order FOREST/FIELD/BARRENS used by chooseTile.
enum TerrainIdx : uint8_t { TI_FOREST = 0, TI_FIELD, TI_BARRENS, TI_COUNT };
static const int TILE_PROBS_PM[TI_COUNT] = { 150, 350, 500 };  // 0.15 / 0.35 / 0.50
static const uint8_t TERRAIN_TILE[TI_COUNT] = { T_FOREST, T_FIELD, T_BARRENS };

// ---- Setpiece / scene ids (placeholders for P2.3 setpieces.js) ------------
// A landmark step returns one of these as a hook; the setpiece + combat engine
// lands in milestone 2.3. SP_NONE means "no event" (e.g. a used outpost).
enum SetpieceId : uint8_t {
    SP_NONE = 0, SP_OUTPOST, SP_IRONMINE, SP_COALMINE, SP_SULPHURMINE,
    SP_HOUSE, SP_CAVE, SP_TOWN, SP_CITY, SP_SHIP, SP_BOREHOLE,
    SP_BATTLEFIELD, SP_SWAMP, SP_EXECUTIONER, SP_CACHE
};

// ---- Landmark distribution (world.js World.LANDMARKS) ---------------------
// num copies placed in the annulus [minRadius, maxRadius) (Manhattan), on
// terrain only. minR==maxR pins the exact Manhattan distance. RADIUS*1.5 = 45.
// OUTPOST num=0 (only spawned by clearing a dungeon). CACHE is prestige-only
// (skipped by generateMap). label is the tr() map-tooltip key.
struct LandmarkDef {
    uint8_t     tile;
    uint8_t     num;
    uint8_t     minR;
    uint8_t     maxR;
    uint8_t     scene;        // SetpieceId (2.3 hook)
    const char* label;        // tr() key
    bool        prestigeOnly;
};
static const LandmarkDef LANDMARKS[] = {
    { T_OUTPOST,      0,  0,  0, SP_OUTPOST,      "An Outpost",           false },
    { T_IRON_MINE,    1,  5,  5, SP_IRONMINE,     "Iron Mine",            false },
    { T_COAL_MINE,    1, 10, 10, SP_COALMINE,     "Coal Mine",            false },
    { T_SULPHUR_MINE, 1, 20, 20, SP_SULPHURMINE,  "Sulphur Mine",         false },
    { T_HOUSE,       10,  0, 45, SP_HOUSE,        "An Old House",         false },
    { T_CAVE,         5,  3, 10, SP_CAVE,         "A Damp Cave",          false },
    { T_TOWN,        10, 10, 20, SP_TOWN,         "An Abandoned Town",    false },
    { T_CITY,        20, 20, 45, SP_CITY,         "A Ruined City",        false },
    { T_SHIP,         1, 28, 28, SP_SHIP,         "A Crashed Starship",   false },
    { T_BOREHOLE,    10, 15, 45, SP_BOREHOLE,     "A Borehole",           false },
    { T_BATTLEFIELD,  5, 18, 45, SP_BATTLEFIELD,  "A Battlefield",        false },
    { T_SWAMP,        1, 15, 45, SP_SWAMP,        "A Murky Swamp",        false },
    { T_EXECUTIONER,  1, 28, 28, SP_EXECUTIONER,  "A Ravaged Battleship", false },
    { T_CACHE,        1, 10, 45, SP_CACHE,        "A Destroyed Village",  true  },
};
constexpr int LANDMARK_ROWS = (int)(sizeof(LANDMARKS) / sizeof(LANDMARKS[0]));

// Map a landmark tile -> its SetpieceId + tooltip (doSpace hook / renderer).
// Returns SP_NONE when the tile is not a landmark.
inline uint8_t landmarkScene(uint8_t tile) {
    for (int i = 0; i < LANDMARK_ROWS; i++)
        if (LANDMARKS[i].tile == tile) return LANDMARKS[i].scene;
    return SP_NONE;
}
inline bool isLandmark(uint8_t tile) { return landmarkScene(tile) != SP_NONE; }

// ---- Carry weights (path.js Path.Weight) ----------------------------------
// Centi-units (x100) so the 0.1 / 0.2 / 0.5 fractional weights stay integer.
// Anything not listed weighs 1.00 == 100 (path.js getWeight: non-number -> 1).
struct WeightRow { const char* key; int16_t centi; };
static const WeightRow WEIGHTS[] = {
    { "bone spear",   200 },
    { "iron sword",   300 },
    { "steel sword",  500 },
    { "rifle",        500 },
    { "laser rifle",  500 },
    { "plasma rifle", 500 },   // P3
    { "bolas",         50 },
    { "bullets",       10 },
    { "energy cell",   20 },
};
constexpr int WEIGHT_ROWS = (int)(sizeof(WEIGHTS) / sizeof(WEIGHTS[0]));
constexpr int16_t DEFAULT_WEIGHT_CENTI = 100;  // path.js default weight 1

// path.js Path.getWeight(thing): looked-up weight, else 1.00 (centi).
inline int16_t weightCenti(const char* key) {
    for (int i = 0; i < WEIGHT_ROWS; i++)
        if (strcmp(WEIGHTS[i].key, key) == 0) return WEIGHTS[i].centi;
    return DEFAULT_WEIGHT_CENTI;
}

// ---- Bag capacity tiers (path.js Path.getCapacity, centi-units) -----------
// Highest matching tier wins (NOT additive). cargo drone is P3 (Fabricator).
constexpr int16_t BAG_BASE_CENTI  = 1000;   // Path.DEFAULT_BAG_SPACE 10
constexpr int16_t BAG_RUCKSACK    = 2000;   // +10
constexpr int16_t BAG_WAGON       = 4000;   // +30
constexpr int16_t BAG_CONVOY      = 7000;   // +60
constexpr int16_t BAG_CARGO_DRONE = 11000;  // +100 (P3)

// ---- Water tiers (world.js getMaxWater) — whole units ---------------------
constexpr int WATER_BASE       = 10;
constexpr int WATER_WATERSKIN  = 20;   // +10
constexpr int WATER_CASK       = 30;   // +20
constexpr int WATER_TANK       = 60;   // +50
constexpr int WATER_RECYCLER   = 110;  // +100 (P3)

// ---- Health tiers (world.js getMaxHealth) — whole units -------------------
constexpr int HEALTH_BASE      = 10;
constexpr int HEALTH_L_ARMOUR  = 15;   // +5
constexpr int HEALTH_I_ARMOUR  = 25;   // +15
constexpr int HEALTH_S_ARMOUR  = 45;   // +35
constexpr int HEALTH_KINETIC   = 85;   // +75 (P3)

}  // namespace adr
