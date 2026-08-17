// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room — Phase 2 World state engine implementation. See world_state.h.
// Faithful port of upstream script/world.js map/move/upkeep logic; only the
// saveWorld/loadWorld/saveTrek/loadTrek helpers touch the platform (SD under
// ARDUINO, stdio on host).
#include "world_state.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <SD.h>          // file scope, NOT inside namespace (frame_store parity)
#endif

namespace adr {

// ===================== small helpers ======================================

static inline int widx(int x, int y) { return y * WORLD_DIM + x; }
static inline bool inBounds(int x, int y) {
    return x >= 0 && x < WORLD_DIM && y >= 0 && y < WORLD_DIM;
}
static inline bool getBit(const uint8_t* m, int i) {
    return (m[i >> 3] >> (i & 7)) & 1;
}
static inline void setBit(uint8_t* m, int i) { m[i >> 3] |= (uint8_t)(1 << (i & 7)); }
static inline int   iabs(int v) { return v < 0 ? -v : v; }
static inline uint32_t xorshift(uint32_t& s) {
    uint32_t x = s ? s : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    s = x;
    return x;
}

// Little-endian byte (de)serialization over a moving cursor.
static inline void putU32(uint8_t* b, size_t& o, uint32_t v) {
    b[o++] = (uint8_t)v; b[o++] = (uint8_t)(v >> 8);
    b[o++] = (uint8_t)(v >> 16); b[o++] = (uint8_t)(v >> 24);
}
static inline uint32_t getU32(const uint8_t* b, size_t& o) {
    uint32_t v = (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) |
                 ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24);
    o += 4; return v;
}
static inline void putI16(uint8_t* b, size_t& o, int16_t v) {
    uint16_t u = (uint16_t)v; b[o++] = (uint8_t)u; b[o++] = (uint8_t)(u >> 8);
}
static inline int16_t getI16(const uint8_t* b, size_t& o) {
    uint16_t u = (uint16_t)b[o] | ((uint16_t)b[o + 1] << 8); o += 2;
    return (int16_t)u;
}
static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        for (int bit = 0; bit < 8; bit++)
            c = (c >> 1) ^ (0xedb88320u & (uint32_t)-(int)(c & 1));
    }
    return ~c;
}

// ---- binary sizes (documented layout; see world_state.h) ------------------
// v1 = header + tiles + revealed + visited (legacy, loaded via migration);
// v2 appends the used-outpost mask.
constexpr size_t WORLD_BIN_SIZE_V1 =
    12 + WORLD_CELLS + 2 * WORLD_MASK_BYTES;                       // legacy v1
constexpr size_t WORLD_BIN_SIZE_V2 =
    12 + WORLD_CELLS + 3 * WORLD_MASK_BYTES;                       // legacy v2
constexpr size_t WORLD_BIN_SIZE_CRC = WORLD_BIN_SIZE_V2 + 4;
constexpr size_t TREK_HDR = 8 + 2 + 18 + 2 + 4 + 1 + 1 + 32;       // 68
constexpr size_t TREK_BIN_SIZE_V1 =
    TREK_HDR + RES_COUNT * 2 + ITEM_COUNT * 2 +
    WORLD_CELLS + 2 * WORLD_MASK_BYTES;
constexpr size_t TREK_BIN_SIZE_CRC = TREK_BIN_SIZE_V1 + 4;

// ===================== platform file I/O ==================================

#ifdef ARDUINO
static bool w_writeAtomic(const char* path, const char* tmp, const char* bak,
                          const uint8_t* d, size_t n) {
    File f = SD.open(tmp, FILE_WRITE);
    if (!f) return false;
    size_t wr = f.write(d, n);
    f.close();
    if (wr != n) { SD.remove(tmp); return false; }

    const bool hadOld = SD.exists(path);
    if (hadOld) {
        SD.remove(bak);
        if (!SD.rename(path, bak)) { SD.remove(tmp); return false; }
    }
    if (!SD.rename(tmp, path)) {
        SD.remove(tmp);
        if (hadOld) { SD.remove(path); SD.rename(bak, path); }
        return false;
    }
    return true;
}
static int w_read(const char* path, uint8_t* buf, size_t cap) {
    File f = SD.open(path, FILE_READ);
    if (!f) return -1;
    size_t len = f.size();
    if (len == 0 || len > cap) { f.close(); return -1; }
    size_t rd = f.read(buf, len);
    f.close();
    return rd == len ? (int)rd : -1;
}
static bool w_exists(const char* path) { return SD.exists(path); }
static void w_remove(const char* path) { SD.remove(path); }
#else
static bool w_writeAtomic(const char* path, const char* tmp, const char* bak,
                          const uint8_t* d, size_t n) {
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    size_t wr = fwrite(d, 1, n, f);
    bool ok = (wr == n) && (fclose(f) == 0);
    if (!ok) { remove(tmp); return false; }

    FILE* old = fopen(path, "rb");
    const bool hadOld = old != nullptr;
    if (old) fclose(old);
    if (hadOld) {
        remove(bak);
        if (rename(path, bak) != 0) { remove(tmp); return false; }
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        if (hadOld) { remove(path); rename(bak, path); }
        return false;
    }
    return true;
}
static int w_read(const char* path, uint8_t* buf, size_t cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size <= 0 || (size_t)size > cap || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); return -1;
    }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    return rd == (size_t)size ? (int)rd : -1;
}
static bool w_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}
static void w_remove(const char* path) { remove(path); }
#endif

// ===================== lifecycle ==========================================

void WorldState::init() {
    seed = 0;
    generated = false;
    memset(tiles, T_VOID, sizeof tiles);
    memset(revealed, 0, sizeof revealed);
    memset(visited, 0, sizeof visited);
    memset(usedOutpost, 0, sizeof usedOutpost);
    memset(&ex, 0, sizeof ex);
    memset(&cx, 0, sizeof cx);      // no combat at boot (RAM-only)
    genRng = 0;
}

// ===================== map generation (world.js) ==========================

uint32_t WorldState::mapRand() { return xorshift(genRng); }

// world.js chooseTile — stickiness-weighted terrain pick. Integer permille: the
// weights ALWAYS sum to exactly 1000 (each present terrain adds STICKINESS to
// itself and removes it from the nonSticky pool that is redistributed by
// TILE_PROBS, so total = added-stickiness + nonSticky*sum(probs)=nonSticky =
// 1000), so this reproduces upstream's float model with no rounding drift.
uint8_t WorldState::chooseTile(int x, int y) {
    uint8_t nb[4];
    int nn = 0;
    const int nx[4] = { x, x, x - 1, x + 1 };
    const int ny[4] = { y - 1, y + 1, y, y };
    for (int i = 0; i < 4; i++) {
        if (!inBounds(nx[i], ny[i])) continue;
        uint8_t t = tiles[widx(nx[i], ny[i])];
        if (t == T_VOID) continue;           // not generated yet -> ignore
        if (t == T_VILLAGE) return T_FOREST;  // village must be ringed by forest
        nb[nn++] = t;
    }
    int w[TI_COUNT] = { 0, 0, 0 };
    int nonSticky = 1000;
    for (int ti = 0; ti < TI_COUNT; ti++) {
        bool present = false;
        for (int k = 0; k < nn; k++)
            if (nb[k] == TERRAIN_TILE[ti]) { present = true; break; }
        if (present) { w[ti] += STICKINESS_PM; nonSticky -= STICKINESS_PM; }
    }
    for (int ti = 0; ti < TI_COUNT; ti++)
        w[ti] += TILE_PROBS_PM[ti] * nonSticky / 1000;
    int total = w[0] + w[1] + w[2];
    if (total <= 0) return T_BARRENS;
    int r = (int)(mapRand() % (uint32_t)total);
    int c = 0;
    for (int ti = 0; ti < TI_COUNT; ti++) {
        c += w[ti];
        if (r < c) return TERRAIN_TILE[ti];
    }
    return T_BARRENS;
}

// world.js placeLandmark — reject until it lands on terrain inside the annulus.
void WorldState::placeLandmark(const LandmarkDef& l) {
    for (int guard = 0; guard < 100000; guard++) {
        int span = (int)l.maxR - (int)l.minR;
        int r = (int)l.minR + (span > 0 ? (int)(mapRand() % (uint32_t)span) : 0);
        int xDist = (r > 0) ? (int)(mapRand() % (uint32_t)r) : 0;
        int yDist = r - xDist;
        if ((mapRand() & 1) == 0) xDist = -xDist;
        if ((mapRand() & 1) == 0) yDist = -yDist;
        int x = VILLAGE_X + xDist, y = VILLAGE_Y + yDist;
        if (x < 0) x = 0; if (x >= WORLD_DIM) x = WORLD_DIM - 1;
        if (y < 0) y = 0; if (y >= WORLD_DIM) y = WORLD_DIM - 1;
        if (isTerrain(tiles[widx(x, y)])) { tiles[widx(x, y)] = l.tile; return; }
    }
    // Unreachable in practice (ample terrain); give up rather than spin forever.
}

void WorldState::generateMap(uint32_t s) {
    seed = s;
    genRng = s ? s : 0x9e3779b9u;
    memset(tiles, T_VOID, sizeof tiles);
    memset(revealed, 0, sizeof revealed);   // committed fog starts empty
    memset(visited, 0, sizeof visited);
    memset(usedOutpost, 0, sizeof usedOutpost);  // a fresh map has no used outposts
    tiles[widx(VILLAGE_X, VILLAGE_Y)] = T_VILLAGE;
    // Spiral out ring by ring so a cell's inner (closer) neighbors are already
    // generated when chooseTile reads them. Each cell is chosen exactly once
    // (the T_VOID guard skips the mirror duplicates at t==0 / t==r), so the RNG
    // stream — and thus the map for a given seed — is fully deterministic.
    for (int r = 1; r <= WORLD_RADIUS; r++) {
        for (int t = 0; t <= r; t++) {
            const int pts[4][2] = {
                { VILLAGE_X + t, VILLAGE_Y + (r - t) },
                { VILLAGE_X + t, VILLAGE_Y - (r - t) },
                { VILLAGE_X - t, VILLAGE_Y + (r - t) },
                { VILLAGE_X - t, VILLAGE_Y - (r - t) },
            };
            for (int k = 0; k < 4; k++) {
                int x = pts[k][0], y = pts[k][1];
                if (inBounds(x, y) && tiles[widx(x, y)] == T_VOID)
                    tiles[widx(x, y)] = chooseTile(x, y);
            }
        }
    }
    for (int i = 0; i < LANDMARK_ROWS; i++) {
        const LandmarkDef& l = LANDMARKS[i];
        if (l.prestigeOnly) continue;         // cache is prestige-only (deferred)
        for (int n = 0; n < l.num; n++) placeLandmark(l);
    }
    generated = true;
}

bool WorldState::ensureGenerated(uint32_t s) {
    if (!generated) { generateMap(s); saveWorld(); }
    return generated;
}

// ===================== equipment-derived caps =============================

int WorldState::maxWater(const GameState& gs) {
    if (gs.items[I_WATER_TANK] > 0) return WATER_TANK;
    if (gs.items[I_CASK] > 0)       return WATER_CASK;
    if (gs.items[I_WATERSKIN] > 0)  return WATER_WATERSKIN;
    return WATER_BASE;
}
int WorldState::maxHealth(const GameState& gs) {
    if (gs.items[I_S_ARMOUR] > 0) return HEALTH_S_ARMOUR;
    if (gs.items[I_I_ARMOUR] > 0) return HEALTH_I_ARMOUR;
    if (gs.items[I_L_ARMOUR] > 0) return HEALTH_L_ARMOUR;
    return HEALTH_BASE;
}
int WorldState::bagCapacityCenti(const GameState& gs) {
    if (gs.items[I_CONVOY] > 0)   return BAG_CONVOY;
    if (gs.items[I_WAGON] > 0)    return BAG_WAGON;
    if (gs.items[I_RUCKSACK] > 0) return BAG_RUCKSACK;
    return BAG_BASE_CENTI;
}

// ===================== visibility =========================================

void WorldState::lightMap(int x, int y) {
    int r = LIGHT_RADIUS;                     // scout perk (x2) not modeled yet
    for (int i = -r; i <= r; i++) {
        int span = r - iabs(i);
        for (int j = -span; j <= span; j++) {
            int cx = x + i, cy = y + j;
            if (inBounds(cx, cy)) setBit(ex.revealed, widx(cx, cy));
        }
    }
}

// ===================== expedition =========================================

bool WorldState::embark(GameState& gs, const int16_t* outfitRes,
                        const int16_t* outfitItem, uint32_t trekSeed,
                        uint32_t nowEpoch) {
    if (!generated) return false;
    if (!outfitRes || outfitRes[R_CURED_MEAT] <= 0) return false;  // embark gate
    // Post-death embark lockout (World.DEATH_COOLDOWN_S, §1.5/§3.4). Epoch-based so
    // a deep sleep past the window expires it; nowEpoch==0 (no RTC) or a clock
    // at/behind the death epoch fails open — never a permanent lockout.
    if (gs.deathAt && nowEpoch >= gs.deathAt &&
        nowEpoch - gs.deathAt < (uint32_t)DEATH_COOLDOWN_S) return false;

    // Deduct the chosen outfit from the village stores (caller validated the
    // bag against capacity/inventory; clamp defensively at 0).
    for (int i = 0; i < RES_COUNT; i++) {
        if (outfitRes[i] <= 0) continue;
        gs.stores[i] -= (int32_t)outfitRes[i] * FP;
        if (gs.stores[i] < 0) gs.stores[i] = 0;
    }
    if (outfitItem)
        for (int i = 0; i < ITEM_COUNT; i++) {
            if (outfitItem[i] <= 0) continue;
            gs.items[i] = (uint8_t)(gs.items[i] >= outfitItem[i]
                                    ? gs.items[i] - outfitItem[i] : 0);
        }

    memset(&ex, 0, sizeof ex);
    memset(&cx, 0, sizeof cx);      // fresh trip -> no lingering combat
    ex.active = true;
    ex.dead = false;
    ex.x = VILLAGE_X; ex.y = VILLAGE_Y;
    ex.maxHp = (int16_t)maxHealth(gs);   ex.hp = ex.maxHp;
    ex.maxWater = (int16_t)maxWater(gs); ex.water = ex.maxWater;  // onArrival fills
    ex.gastronome = gs.hasPerk(PK_GASTRONOME);   // meat heals x2 (persisted perk)
    ex.rng = trekSeed ? trekSeed : 0x1a2b3c4du;
    memcpy(ex.tiles, tiles, sizeof tiles);
    memcpy(ex.revealed, revealed, sizeof revealed);
    memcpy(ex.visited, visited, sizeof visited);
    for (int i = 0; i < RES_COUNT; i++) ex.outfitRes[i] = outfitRes[i];
    if (outfitItem)
        for (int i = 0; i < ITEM_COUNT; i++) ex.outfitItem[i] = outfitItem[i];
    lightMap(ex.x, ex.y);
    saveTrek();
    return true;
}

bool WorldState::outpostUsed(int x, int y) const {
    // Two layers: the committed bitmap (used on a PAST expedition, one-shot is
    // global) and this trip's ring (used already this expedition).
    if (inBounds(x, y) && getBit(usedOutpost, widx(x, y))) return true;
    for (int i = 0; i < ex.usedOutpostN; i++)
        if (ex.usedOutpostX[i] == x && ex.usedOutpostY[i] == y) return true;
    return false;
}
void WorldState::markOutpostUsed(int x, int y) {
    if (outpostUsed(x, y)) return;
    if (ex.usedOutpostN < 16) {
        ex.usedOutpostX[ex.usedOutpostN] = (uint8_t)x;
        ex.usedOutpostY[ex.usedOutpostN] = (uint8_t)y;
        ex.usedOutpostN++;
    }
}

// world.js useSupplies — food/water upkeep. Returns false if the wanderer died
// (die() has already dropped the trip). No progressive HP drain: meat/water run
// out -> one warning tick -> next tick kills (research §3.3). `notice` carries a
// one-shot "just ran out" tr() key (nullptr if neither ran out this step); if
// BOTH run out on the same step (movesPerFood/movesPerWater can coincide), food's
// notice wins — it's set first, matching upstream's food-then-water order.
bool WorldState::useSupplies(GameState& gs, const char*& notice) {
    (void)gs;
    ex.foodMove++;
    ex.waterMove++;
    const int movesPerFood = MOVES_PER_FOOD;    // slow-metabolism perk (x2) TODO
    const int movesPerWater = MOVES_PER_WATER;  // desert-rat perk (x2) TODO

    if (ex.foodMove >= movesPerFood) {
        ex.foodMove = 0;
        int num = ex.outfitRes[R_CURED_MEAT] - 1;
        if (num == 0) {
            ex.outfitRes[R_CURED_MEAT] = 0;      // ate the last piece, no heal
            notice = "the meat has run out";
        } else if (num < 0) {
            ex.outfitRes[R_CURED_MEAT] = 0;
            if (!ex.starving) ex.starving = true;   // "starvation sets in"
            else { die(); return false; }            // starved to death
        } else {
            ex.outfitRes[R_CURED_MEAT] = (int16_t)num;
            ex.starving = false;
            ex.hp += MEAT_HEAL * (ex.gastronome ? 2 : 1);   // gastronome (swamp perk)
            if (ex.hp > ex.maxHp) ex.hp = ex.maxHp;
        }
    }

    if (ex.waterMove >= movesPerWater) {
        ex.waterMove = 0;
        ex.water--;
        if (ex.water == 0) {
            if (!notice) notice = "there is no more water";   // warning only
        } else if (ex.water < 0) {
            ex.water = 0;
            if (!ex.thirsty) ex.thirsty = true;      // "thirst becomes unbearable"
            else { die(); return false; }             // died of thirst
        } else {
            ex.thirsty = false;
        }
    }
    return true;
}

// world.js checkFight — after FIGHT_DELAY steps, FIGHT_CHANCE per step, then
// triggerFight picks an available encounter. Returns true (with enemyOut set)
// only when a fight actually starts; a chance that lands on a non-terrain tile
// (road/void) with no available encounter resolves to no fight, faithful to
// upstream (its possibleFights pool would be empty there).
bool WorldState::rollFight(int& enemyOut) {
    ex.fightMove++;
    if (ex.fightMove <= FIGHT_DELAY) return false;
    if ((int)(xorshift(ex.rng) % 1000u) >= FIGHT_CHANCE_PM) return false;  // no roll
    ex.fightMove = 0;                              // upstream resets on the chance hit
    int e = chooseEncounter();
    if (e < 0) return false;
    enemyOut = e;
    return true;
}

// encounters.js triggerFight / isAvailable — the pool of encounters whose tier
// (Manhattan distance band) AND terrain match the current tile, one drawn from
// ex.rng. -1 on a non-terrain tile (roads/void carry no encounter). Every
// terrain×tier pair has >=1 encounter, so n>=1 on any FOREST/FIELD/BARRENS cell.
int WorldState::chooseEncounter() {
    uint8_t tile = ex.tiles[widx(ex.x, ex.y)];
    if (!isTerrain(tile)) return -1;
    int dist = iabs(ex.x - VILLAGE_X) + iabs(ex.y - VILLAGE_Y);
    int tier = fightTier(dist);
    uint8_t pool[ENCOUNTER_COUNT];
    int n = 0;
    for (int i = 0; i < ENCOUNTER_COUNT; i++)
        if (ENCOUNTERS[i].tier == tier && ENCOUNTERS[i].terrain == tile)
            pool[n++] = (uint8_t)i;
    if (n == 0) return -1;
    return pool[(int)(xorshift(ex.rng) % (uint32_t)n)];
}

// world.js World.CHANGE_MSG — one-shot narration when a step crosses between two
// DIFFERENT plain-terrain tiles (both the old and new cell must be terrain; a
// landmark/road/village endpoint never narrates — matches narrateMove's own
// isTerrain guard). All 6 directed pairs, transcribed from strings_zh.h (§7.3).
struct TerrainChangeMsg { uint8_t from, to; const char* key; };
static const TerrainChangeMsg TERRAIN_CHANGE[] = {
    { T_FOREST,  T_FIELD,
      "the trees yield to dry grass. the yellowed brush rustles in the wind." },
    { T_FIELD,   T_FOREST,
      "trees loom on the horizon. grasses gradually yield to a forest floor of "
      "dry branches and fallen leaves." },
    { T_FIELD,   T_BARRENS, "the grasses thin. soon, only dust remains." },
    { T_BARRENS, T_FIELD,
      "the barrens break at a sea of dying grass, swaying in the arid breeze." },
    { T_FOREST,  T_BARRENS,
      "the trees are gone. parched earth and blowing dust are poor replacements." },
    { T_BARRENS, T_FOREST,
      "a wall of gnarled trees rises from the dust. their branches twist into a "
      "skeletal canopy overhead." },
};
static const char* terrainChangeKey(uint8_t from, uint8_t to) {
    for (const auto& r : TERRAIN_CHANGE)
        if (r.from == from && r.to == to) return r.key;
    return nullptr;
}

// world.js checkDanger — edge-triggered: notice only on the step where the
// danger state FLIPS (armour vs Manhattan distance from the village), not every
// step spent inside the zone. Reads gs.items the SAME WAY maxHealth() does
// (iron+ armour is safe past 8, steel+ safe past 18); "iron+" counts steel too
// (a higher tier implies the lower one, matching upstream's armour ladder).
void WorldState::checkDanger(const GameState& gs, StepResult& res) {
    int dist = iabs(ex.x - VILLAGE_X) + iabs(ex.y - VILLAGE_Y);
    bool hasIronPlus  = gs.items[I_I_ARMOUR] > 0 || gs.items[I_S_ARMOUR] > 0;
    bool hasSteelPlus = gs.items[I_S_ARMOUR] > 0;
    bool danger = (dist >= 8 && !hasIronPlus) || (dist >= 18 && !hasSteelPlus);
    if (danger == ex.danger) return;
    res.notice = danger
        ? "dangerous to be this far from the village without proper protection"
        : "safer here";
    ex.danger = danger;
}

StepResult WorldState::move(GameState& gs, uint8_t dir) {
    StepResult res{ STEP_BLOCKED, SP_NONE, nullptr };
    if (!ex.active) return res;

    Vec2 v = DIR_VEC[dir & 3];
    int nx = ex.x + v.dx, ny = ex.y + v.dy;
    if (nx < 0) nx = 0; if (nx >= WORLD_DIM) nx = WORLD_DIM - 1;
    if (ny < 0) ny = 0; if (ny >= WORLD_DIM) ny = WORLD_DIM - 1;
    uint8_t oldTile = ex.tiles[widx(ex.x, ex.y)];        // narrateMove's "from" tile
    ex.x = (int16_t)nx; ex.y = (int16_t)ny;
    lightMap(nx, ny);

    // doSpace(): village -> goHome; landmark -> setpiece hook (no upkeep this
    // step); otherwise upkeep + fight roll on plain ground.
    uint8_t tile = ex.tiles[widx(nx, ny)];

    // world.js narrateMove — lowest-priority notice this step: only claims the
    // slot if useSupplies (meat/water just ran out) doesn't overwrite it below.
    if (isTerrain(oldTile) && isTerrain(tile) && oldTile != tile)
        res.notice = terrainChangeKey(oldTile, tile);

    if (tile == T_VILLAGE) {
        goHome(gs);
        res.kind = STEP_HOME; res.scene = SP_NONE;
        return res;                                   // trek already cleared
    }
    // world.js doSpace: a markVisited'd landmark (tile char gains a '!') misses the
    // LANDMARKS lookup and falls through to plain terrain (useSupplies + checkFight).
    // We model that '!' with the working visited mask. Outposts are exempt — they
    // carry their OWN one-shot (outpostUsed) and are never markVisited'd upstream.
    bool spentLandmark = tile != T_OUTPOST && getBit(ex.visited, widx(nx, ny));
    if (isLandmark(tile) && !spentLandmark) {
        if (tile == T_OUTPOST && outpostUsed(nx, ny)) {
            res.kind = STEP_MOVED;                    // used outpost = safe, no upkeep
        } else {
            if (tile == T_OUTPOST) markOutpostUsed(nx, ny);
            res.kind = STEP_LANDMARK; res.scene = landmarkScene(tile);
        }
    } else {
        const char* supplyNotice = nullptr;
        if (!useSupplies(gs, supplyNotice)) {
            res.kind = STEP_DIED; res.scene = SP_NONE;
            return res;                                   // trek already cleared
        }
        if (supplyNotice) res.notice = supplyNotice;      // meat/water-out beats terrain
        int enemy = -1;
        if (rollFight(enemy)) {
            res.kind = STEP_FIGHT; res.scene = (uint8_t)enemy;  // enemy for the fight
        } else {
            res.kind = STEP_MOVED;
        }
    }
    // checkDanger is the lowest priority of all: it only speaks up when nothing
    // else did this step (terrain narration / meat-or-water-just-ran-out already
    // claimed the single HUD notice slot otherwise — §3.1's own e-ink throttle).
    if (!res.notice) checkDanger(gs, res);
    saveTrek();                                       // persist the step
    return res;
}

// world.js World.leaveItAtHome for a Res slot: the consumable supplies stay
// packed for the next trip (cured meat / bullets / energy cell / charm / medicine
// — stim/hypo are P3, absent from Res); everything else (raw loot fur/iron/teeth/
// scales/cloth/leather, alien alloy, compass) is banked to stores and dropped from
// the outfit. Returns true == leave at home.
static bool leaveResAtHome(int r) {
    switch (r) {
        case R_CURED_MEAT: case R_BULLETS: case R_ENERGY_CELL:
        case R_CHARM:      case R_MEDICINE:
            return false;
        default:
            return true;
    }
}

void WorldState::goHome(GameState& gs) {
    // Commit the working map -> committed (cleared dungeons + revealed fog stay).
    memcpy(tiles, ex.tiles, sizeof tiles);
    memcpy(revealed, ex.revealed, sizeof revealed);
    memcpy(visited, ex.visited, sizeof visited);
    // Persist outposts used this trip into the committed one-shot bitmap (global
    // across expeditions). die() skips this, so a discarded trip's uses are lost.
    for (int i = 0; i < ex.usedOutpostN; i++)
        setBit(usedOutpost, widx(ex.usedOutpostX[i], ex.usedOutpostY[i]));
    // Unlock cleared mines (economic closure: staffs the miner jobs).
    if (ex.clearedIron && gs.buildings[B_IRON_MINE] == 0)
        gs.buildings[B_IRON_MINE] = 1;
    if (ex.clearedCoal && gs.buildings[B_COAL_MINE] == 0)
        gs.buildings[B_COAL_MINE] = 1;
    if (ex.clearedSulphur && gs.buildings[B_SULPHUR_MINE] == 0)
        gs.buildings[B_SULPHUR_MINE] = 1;
    // clearedShip / clearedExec unlock Ship / Fabricator (Phase 3) — deferred.
    // Bank the bag: EVERYTHING returns to the village stores (upstream returnOutfit
    // does $SM.add('stores[k]', outfit[k]) for every k). Then the leaveItAtHome
    // nicety writes the RETAINED slots into the persistent Path outfit so the next
    // embark pre-packs them (they are banked in stores AND remembered here — the
    // Path panel re-deducts min(remembered, stock) next trip, no double count).
    for (int i = 0; i < RES_COUNT; i++) {
        if (ex.outfitRes[i] > 0) {
            gs.stores[i] += (int32_t)ex.outfitRes[i] * FP;
            gs.markSeen((uint8_t)i);
        }
        gs.savedOutfitRes[i] = leaveResAtHome(i) ? 0 : ex.outfitRes[i];  // §3.5
        ex.outfitRes[i] = 0;
    }
    for (int i = 0; i < ITEM_COUNT; i++) {
        if (ex.outfitItem[i] > 0) {
            int c = gs.items[i] + ex.outfitItem[i];
            gs.items[i] = (uint8_t)(c > 255 ? 255 : c);
        }
        // leaveItAtHome is false for EVERY item: 0..I_RIFLE are Room craftables,
        // I_BAYONET..I_BOLAS are World weapons — all stay packed for next trip.
        gs.savedOutfitItem[i] = ex.outfitItem[i];
        ex.outfitItem[i] = 0;
    }
    ex.active = false;
    saveWorld();       // persist the newly committed map
    clearTrek();       // trip over
}

void WorldState::die() {
    // Drop the working map (this trip's clears/fog are lost) and empty the bag —
    // committed map and game state are untouched (World.state=null, outfit={}).
    ex.dead = true;
    ex.active = false;
    for (int i = 0; i < RES_COUNT; i++) ex.outfitRes[i] = 0;
    for (int i = 0; i < ITEM_COUNT; i++) ex.outfitItem[i] = 0;
    clearTrek();
}

// ===================== setpiece map hooks (working map) ===================

bool WorldState::findClosestRoad(int sx, int sy, int& rx, int& ry) const {
    for (int r = 1; r < WORLD_DIM; r++) {
        for (int t = 0; t <= r; t++) {
            const int pts[4][2] = {
                { sx + t, sy + (r - t) }, { sx + t, sy - (r - t) },
                { sx - t, sy + (r - t) }, { sx - t, sy - (r - t) },
            };
            for (int k = 0; k < 4; k++) {
                int cx = pts[k][0], cy = pts[k][1];
                if (!inBounds(cx, cy)) continue;
                if (cx == sx && cy == sy) continue;
                uint8_t tt = ex.tiles[widx(cx, cy)];
                if (tt == T_ROAD || tt == T_VILLAGE || tt == T_OUTPOST) {
                    rx = cx; ry = cy; return true;
                }
            }
        }
    }
    return false;
}

// world.js drawRoad — L-shaped path from (x,y) to the nearest road/outpost/
// village, converting only terrain cells to ROAD (landmarks untouched). We draw
// the horizontal leg first then the vertical; the leg order only shifts the
// single corner cell, so connectivity is identical to upstream's ordered form.
void WorldState::drawRoad(int x, int y) {
    int rx = VILLAGE_X, ry = VILLAGE_Y;
    findClosestRoad(x, y, rx, ry);                // fallback stays VILLAGE_POS
    int x0 = x < rx ? x : rx, x1 = x < rx ? rx : x;
    for (int ix = x0; ix <= x1; ix++)
        if (isTerrain(ex.tiles[widx(ix, y)])) ex.tiles[widx(ix, y)] = T_ROAD;
    int y0 = y < ry ? y : ry, y1 = y < ry ? ry : y;
    for (int iy = y0; iy <= y1; iy++)
        if (isTerrain(ex.tiles[widx(rx, iy)])) ex.tiles[widx(rx, iy)] = T_ROAD;
}

void WorldState::clearDungeon(int x, int y) {
    if (!inBounds(x, y)) return;
    ex.tiles[widx(x, y)] = T_OUTPOST;             // cave/town/city -> outpost
    drawRoad(x, y);
}

void WorldState::clearMine(int x, int y, uint8_t tile) {
    if (!inBounds(x, y)) return;
    switch (tile) {
        case T_IRON_MINE:    ex.clearedIron = true;    break;
        case T_COAL_MINE:    ex.clearedCoal = true;    break;
        case T_SULPHUR_MINE: ex.clearedSulphur = true; break;
        case T_SHIP:         ex.clearedShip = true;    break;
        case T_EXECUTIONER:  ex.clearedExec = true;    break;
        default: break;
    }
    drawRoad(x, y);                               // mine tile itself is kept
}

// ===================== combat (events.js, 1s discrete) ====================
// The upstream real-time fight (enemy setInterval, per-weapon cooldown seconds)
// runs here as a 1s tick driven by fight_modal. State lives in `cx` (RAM-only);
// only the OUTCOMES touch the persisted expedition — ex.hp (damage / heals) and
// ex.outfit (ammo spend / banked loot) — so a power-off mid-fight loses the
// fight but not the trek (research decision 7). See combat_data.h.

// Total carried weight in centi-units (path.js getWeight model, default 1.00).
// Loot capacity checks the same way path_page's freeCenti does.
int WorldState::exBagUsedCenti() const {
    int used = 0;
    for (int i = 0; i < RES_COUNT; i++)
        used += (int)ex.outfitRes[i] * weightCenti(RES_KEY[i]);
    for (int i = 0; i < ITEM_COUNT; i++)
        used += (int)ex.outfitItem[i] * weightCenti(ITEM_KEY[i]);
    return used;
}

// Fill the attack-button weapon list from the carried outfit (events.js
// startCombat): every carried weapon that can fire NOW (ammo present); fists is
// the sole fallback when nothing else qualifies. Shared by both begin* paths.
void WorldState::armWeapons() {
    cx.weaponN = 0;
    for (int i = 1; i < WEAPON_COUNT; i++) {         // skip fists (index 0) here
        const WeaponDef& w = WEAPONS[i];
        if (ex.outfitItem[w.itemSlot] <= 0) continue;            // not carried
        if (w.ammoRes != RES_NONE && ex.outfitRes[w.ammoRes] <= 0) continue;  // no ammo
        cx.weapons[cx.weaponN++] = (uint8_t)i;
    }
    if (cx.weaponN == 0) cx.weapons[cx.weaponN++] = WEAPON_FISTS;
}

void WorldState::beginFight(uint8_t enemyId) {
    memset(&cx, 0, sizeof cx);
    if (enemyId >= ENCOUNTER_COUNT) return;          // defensive
    const Encounter& e = ENCOUNTERS[enemyId];
    cx.active = true;
    cx.setpiece = false;
    cx.enemyId = enemyId;
    cx.enemyHp = cx.enemyMaxHp = e.health;
    cx.enemyDelayLeft = e.attackDelayS;
    cx.enemyDamage = e.damage; cx.enemyHitPM = e.hitPM; cx.enemyDelayS = e.attackDelayS;
    cx.enemyChara   = e.chara;
    cx.enemyNameKey = e.name; cx.enemyNotifKey = e.notif; cx.enemyDeathKey = e.death;
    cx.lootTbl = e.loot; cx.lootTblN = (int16_t)e.lootN;
    armWeapons();
}

// Arm combat against a setpiece's inline enemy. cx.setpiece flags the win/flee
// hand-back to setpiece_modal; the name/death keys are null (the setpiece owns
// the victory panel, not fight_modal).
void WorldState::beginFightSetpiece(const SetpieceEnemy& e) {
    memset(&cx, 0, sizeof cx);
    cx.active = true;
    cx.setpiece = true;
    cx.enemyId = 0xFF;
    cx.enemyHp = cx.enemyMaxHp = e.health;
    cx.enemyDelayLeft = e.attackDelayS;
    cx.enemyDamage = e.damage; cx.enemyHitPM = e.hitPM; cx.enemyDelayS = e.attackDelayS;
    cx.enemyChara   = e.chara;
    cx.enemyNameKey = nullptr; cx.enemyNotifKey = e.notif; cx.enemyDeathKey = nullptr;
    cx.lootTbl = e.loot; cx.lootTblN = (int16_t)e.lootN;
    armWeapons();
}

uint8_t WorldState::fightWeaponId(int s) const {
    return (s >= 0 && s < cx.weaponN) ? cx.weapons[s] : (uint8_t)WEAPON_FISTS;
}
int WorldState::fightWeaponCoolLeft(int s) const {
    return (s >= 0 && s < cx.weaponN) ? cx.weaponCool[s] : 0;
}
bool WorldState::fightWeaponEnabled(int s) const {
    if (s < 0 || s >= cx.weaponN) return false;
    if (cx.weaponCool[s] > 0) return false;
    const WeaponDef& w = WEAPONS[cx.weapons[s]];
    if (w.itemSlot == WSLOT_NONE) return true;                   // fists
    if (w.selfAmmo)            return ex.outfitItem[w.itemSlot] > 0;
    if (w.ammoRes != RES_NONE) return ex.outfitRes[w.ammoRes] > 0;
    return ex.outfitItem[w.itemSlot] > 0;                        // melee, still carried
}

// events.js drawLoot — per drop line: chance roll, then a count draw on a hit
// (consumed even when max==min, matching upstream). Banked into the bag capped by
// free weight (§1/§4.7); excess is dropped (the e-ink port auto-banks rather than
// running upstream's take/drop menu — research decision 4).
// Shared loot banker (events.js drawLoot): per drop line, a chance roll then a
// count draw on a hit (a random is consumed even when max==min, matching
// upstream). Banked into the bag capped by free weight (§1/§4.7); excess is
// dropped (the e-ink port auto-banks rather than running upstream's take/drop
// menu — research decision 4). Reused by combat victory (rollLoot -> cx.loot) and
// by setpiece narrative-scene loot (setpiece_engine -> its own display buffer).
int WorldState::bankLootTable(GameState& gs, const LootDrop* tbl, int n,
                              LootLine* out, int outCap) {
    int outN = 0;
    int cap = bagCapacityCenti(gs);
    int freeC = cap - exBagUsedCenti();
    if (freeC < 0) freeC = 0;
    for (int i = 0; i < n; i++) {
        const LootDrop& d = tbl[i];
        bool hit = (int)(xorshift(ex.rng) % 1000u) < d.chancePM;
        if (!hit) continue;
        uint32_t c = xorshift(ex.rng);               // count draw (on hit)
        int num = (d.mx > d.mn)
                ? (int)(c % (uint32_t)(d.mx - d.mn)) + d.mn
                : d.mn;
        const char* key = d.isItem ? ITEM_KEY[d.slot] : RES_KEY[d.slot];
        int wt = weightCenti(key); if (wt <= 0) wt = 1;
        int canFit = freeC / wt;
        if (num > canFit) num = canFit;              // capacity clamp
        if (num <= 0) continue;
        freeC -= num * wt;
        if (d.isItem) {
            int v = (int)ex.outfitItem[d.slot] + num;
            ex.outfitItem[d.slot] = (int16_t)(v > 255 ? 255 : v);
        } else {
            ex.outfitRes[d.slot] += (int16_t)num;
        }
        if (out && outN < outCap) {
            out[outN].isItem = d.isItem;
            out[outN].slot   = d.slot;
            out[outN].got    = (int16_t)num;
            outN++;
        }
    }
    return outN;
}

void WorldState::rollLoot(GameState& gs) {
    cx.lootN = bankLootTable(gs, cx.lootTbl, cx.lootTblN, cx.loot, MAX_LOOT);
}

uint8_t WorldState::fightAttack(GameState& gs, int s) {
    if (!cx.active || cx.won) return FIGHT_NOOP;
    if (s < 0 || s >= cx.weaponN) return FIGHT_NOOP;
    if (cx.weaponCool[s] > 0) return FIGHT_NOOP;
    const WeaponDef& w = WEAPONS[cx.weapons[s]];
    // Spend ammo (grenade/bolas spend themselves; rifle/laser spend a Res).
    if (w.selfAmmo) {
        if (ex.outfitItem[w.itemSlot] <= 0) return FIGHT_NOOP;
        ex.outfitItem[w.itemSlot]--;
    } else if (w.ammoRes != RES_NONE) {
        if (ex.outfitRes[w.ammoRes] <= 0) return FIGHT_NOOP;
        ex.outfitRes[w.ammoRes]--;
    }
    cx.weaponCool[s] = w.cooldownS;
    bool hit = (int)(xorshift(ex.rng) % 1000u) < BASE_HIT_CHANCE_PM;  // 0.8, no perks
    cx.lastMiss = !hit;
    if (!hit) return FIGHT_ONGOING;
    if (w.damage == DMG_STUN) { cx.enemyStunLeft = FIGHT_STUN_S; return FIGHT_ONGOING; }
    cx.enemyHp -= w.damage;
    if (cx.enemyHp <= 0) {
        cx.enemyHp = 0;
        rollLoot(gs);                                // bank the drops
        cx.won = true;
        return FIGHT_WON;
    }
    return FIGHT_ONGOING;
}

uint8_t WorldState::fightTick() {
    if (!cx.active || cx.won) return FIGHT_ONGOING;
    for (int i = 0; i < cx.weaponN; i++)
        if (cx.weaponCool[i] > 0) cx.weaponCool[i]--;
    if (cx.eatCool > 0)  cx.eatCool--;
    if (cx.medsCool > 0) cx.medsCool--;
    // Enemy swings on its interval; a stunned enemy's swing is SKIPPED (the clock
    // still runs, matching upstream's interval + `if(stunned) return`).
    bool stunned = cx.enemyStunLeft > 0;
    if (stunned) cx.enemyStunLeft--;
    if (cx.enemyDelayLeft > 0) cx.enemyDelayLeft--;
    if (cx.enemyDelayLeft <= 0) {
        cx.enemyDelayLeft = cx.enemyDelayS;          // rearm for the next swing
        if (!stunned) {
            bool hit = (int)(xorshift(ex.rng) % 1000u) < cx.enemyHitPM;
            if (hit) {
                ex.hp -= cx.enemyDamage;
                if (ex.hp <= 0) {
                    ex.hp = 0;
                    cx.active = false;
                    die();                            // discards trip + empties bag
                    return FIGHT_LOST;
                }
            }
        }
    }
    return FIGHT_ONGOING;
}

uint8_t WorldState::fightEat() {
    if (!cx.active || cx.won) return FIGHT_NOOP;
    if (cx.eatCool > 0) return FIGHT_NOOP;
    if (ex.outfitRes[R_CURED_MEAT] <= 0) return FIGHT_NOOP;
    if (ex.hp >= ex.maxHp) return FIGHT_NOOP;         // heal only below max (setHeal)
    ex.outfitRes[R_CURED_MEAT]--;
    ex.hp += MEAT_HEAL * (ex.gastronome ? 2 : 1); if (ex.hp > ex.maxHp) ex.hp = ex.maxHp;
    cx.eatCool = FIGHT_EAT_COOLDOWN_S;
    return FIGHT_ONGOING;
}

uint8_t WorldState::fightMeds() {
    if (!cx.active || cx.won) return FIGHT_NOOP;
    if (cx.medsCool > 0) return FIGHT_NOOP;
    if (ex.outfitRes[R_MEDICINE] <= 0) return FIGHT_NOOP;
    if (ex.hp >= ex.maxHp) return FIGHT_NOOP;
    ex.outfitRes[R_MEDICINE]--;
    ex.hp += MEDS_HEAL; if (ex.hp > ex.maxHp) ex.hp = ex.maxHp;
    cx.medsCool = FIGHT_MEDS_COOLDOWN_S;
    return FIGHT_ONGOING;
}

// Abandon the fight (no loot, damage taken kept). Random encounters have no
// upstream flee (§4.8); the slow e-ink panel adds it so a bad-matchup fight isn't
// a guaranteed death — and it doubles as the power-off-mid-fight resolution
// (research decision 7). Persist the (damaged) expedition so the flee sticks.
void WorldState::fightFlee() {
    cx.active = false;
    cx.won = false;
    saveTrek();
}

// Dismiss the victory panel: loot is already banked into ex; persist and end.
void WorldState::fightEndVictory() {
    cx.active = false;
    cx.won = false;
    saveTrek();
}

// ===================== setpiece effects ===================================
// Swamp charm -> gastronome (setpieces.js $SM.addPerk). Persist it on the game
// state (survives death + future trips) AND flip the live expedition flag so the
// x2 meat heal applies this trip too.
void WorldState::spGrantGastronome(GameState& gs) {
    gs.addPerk(PK_GASTRONOME);
    ex.gastronome = true;
}

// world.js markVisited — flag the current landmark tile as consumed on the working
// map. move() then routes a re-step through the plain-terrain branch (no re-trigger),
// exactly as upstream's 'H!' tile char misses the LANDMARKS lookup. Committed at
// goHome, discarded at die() (working-layer, same as revealed/tiles).
void WorldState::spMarkVisited() {
    if (inBounds(ex.x, ex.y)) setBit(ex.visited, widx(ex.x, ex.y));
}

int WorldState::spRand1000() { return (int)(xorshift(ex.rng) % 1000u); }

// ===================== read helpers =======================================

uint8_t WorldState::tileAt(int x, int y) const {
    return inBounds(x, y) ? tiles[widx(x, y)] : (uint8_t)T_VOID;
}
uint8_t WorldState::exTileAt(int x, int y) const {
    return inBounds(x, y) ? ex.tiles[widx(x, y)] : (uint8_t)T_VOID;
}
bool WorldState::isRevealed(int x, int y) const {
    return inBounds(x, y) && getBit(revealed, widx(x, y));
}
bool WorldState::exRevealed(int x, int y) const {
    return inBounds(x, y) && getBit(ex.revealed, widx(x, y));
}
bool WorldState::exVisited(int x, int y) const {
    return inBounds(x, y) && getBit(ex.visited, widx(x, y));
}
int WorldState::countTiles(uint8_t tile) const {
    int n = 0;
    for (int i = 0; i < WORLD_CELLS; i++) if (tiles[i] == tile) n++;
    return n;
}

// world.js compassDir — the ship's fixed 8-way direction FROM THE VILLAGE on the
// committed map (§2.7); mapSearch(SHIP) + the same |dx|/2 vs |dy| primary-axis
// heuristic world_page's shipCompassKey uses for its own (position-relative) HUD
// hint, but rooted at VILLAGE_X/Y — this answers "which way from home", the
// question the one-time compass-purchase notice (§1.6) needs.
bool WorldState::compassFromVillage(char* out, size_t cap) const {
    int sx = -1, sy = -1;
    for (int y = 0; y < WORLD_DIM && sx < 0; y++)
        for (int x = 0; x < WORLD_DIM; x++)
            if (tiles[widx(x, y)] == T_SHIP) { sx = x; sy = y; break; }
    if (sx < 0) return false;
    int dx = sx - VILLAGE_X, dy = sy - VILLAGE_Y;
    int a = iabs(dx), b = iabs(dy);
    const char* dir;
    char diag[16];
    if (b / 2 > a)      dir = dy < 0 ? "north" : "south";
    else if (a / 2 > b) dir = dx < 0 ? "west"  : "east";
    else {
        snprintf(diag, sizeof diag, "%s%s",
                 dy < 0 ? "north" : "south", dx < 0 ? "west" : "east");
        dir = diag;
    }
    snprintf(out, cap, "the compass points %s", dir);
    return true;
}

// ===================== SD persistence =====================================
// New files use magic + version + payload + CRC32. Legacy v1/v2 world files
// and v1 trek files are accepted only at their exact historical sizes, validated,
// and immediately rewritten in the checksummed format.

static bool validWorldPayload(const uint8_t* buf, size_t n, uint8_t ver) {
    if (ver != 1 && ver != 2 && ver != WORLD_VER) return false;
    const size_t expected = ver == 1 ? WORLD_BIN_SIZE_V1 : WORLD_BIN_SIZE_V2;
    if (n != expected) return false;
    size_t o = 0;
    if (getU32(buf, o) != WORLD_MAGIC) return false;
    if (buf[o++] != ver) return false;
    o += 3;
    (void)getU32(buf, o);
    for (int i = 0; i < WORLD_CELLS; i++)
        if (buf[o + i] >= TILE_COUNT) return false;
    return true;
}
static bool validTrekPayload(const uint8_t* buf, size_t n, uint8_t ver) {
    if ((ver != 1 && ver != TREK_VER) || n != TREK_BIN_SIZE_V1) return false;
    size_t o = 0;
    if (getU32(buf, o) != TREK_MAGIC) return false;
    if (buf[o++] != ver) return false;
    o += 3;
    const int16_t x = getI16(buf, o), y = getI16(buf, o);
    const int16_t hp = getI16(buf, o), maxHp = getI16(buf, o);
    const int16_t water = getI16(buf, o), maxWater = getI16(buf, o);
    o += 6; // foodMove, waterMove, fightMove
    o += 2; // starving, thirsty
    o += 4; // rng
    o += 1; // cleared flags
    uint8_t used = buf[o++];
    if (used > 16 || x < 0 || x >= WORLD_DIM || y < 0 || y >= WORLD_DIM ||
        maxHp < 0 || hp < 0 || hp > maxHp ||
        maxWater < 0 || water < 0 || water > maxWater) return false;
    for (int i = 0; i < used; i++) {
        if (buf[o + i] >= WORLD_DIM || buf[o + 16 + i] >= WORLD_DIM) return false;
    }
    o += 32;
    for (int i = 0; i < RES_COUNT + ITEM_COUNT; i++) {
        if (getI16(buf, o) < 0) return false;
    }
    for (int i = 0; i < WORLD_CELLS; i++)
        if (buf[o + i] >= TILE_COUNT) return false;
    return true;
}

bool WorldState::saveWorld() const {
    static uint8_t buf[WORLD_BIN_SIZE_CRC];
    size_t o = 0;
    putU32(buf, o, WORLD_MAGIC);
    buf[o++] = WORLD_VER; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
    putU32(buf, o, seed);
    memcpy(buf + o, tiles, WORLD_CELLS);            o += WORLD_CELLS;
    memcpy(buf + o, revealed, WORLD_MASK_BYTES);    o += WORLD_MASK_BYTES;
    memcpy(buf + o, visited, WORLD_MASK_BYTES);     o += WORLD_MASK_BYTES;
    memcpy(buf + o, usedOutpost, WORLD_MASK_BYTES); o += WORLD_MASK_BYTES;
    uint32_t sum = crc32(buf, o);
    putU32(buf, o, sum);
    return w_writeAtomic(ADR_WORLD_PATH, ADR_WORLD_TMP_PATH, ADR_WORLD_BAK_PATH, buf, o);
}

bool WorldState::loadWorld() {
    static uint8_t buf[WORLD_BIN_SIZE_CRC];
    int n = w_read(ADR_WORLD_PATH, buf, sizeof buf);
    if (n < 12) return false;
    size_t payload = 0;
    uint8_t ver = buf[4];
    if (ver == WORLD_VER) {
        if (n != (int)WORLD_BIN_SIZE_CRC) return false;
        payload = n - 4;
        uint32_t stored = getU32(buf, payload);
        if (stored != crc32(buf, payload) ||
            !validWorldPayload(buf, payload, ver)) return false;
    } else if (ver == 1 || ver == 2) {
        payload = n;
        if (!validWorldPayload(buf, payload, ver)) return false;
    } else return false;

    size_t o = 0;
    if (getU32(buf, o) != WORLD_MAGIC) return false;
    ver = buf[o++]; o += 3;
    seed = getU32(buf, o);
    memcpy(tiles, buf + o, WORLD_CELLS);            o += WORLD_CELLS;
    memcpy(revealed, buf + o, WORLD_MASK_BYTES);    o += WORLD_MASK_BYTES;
    memcpy(visited, buf + o, WORLD_MASK_BYTES);     o += WORLD_MASK_BYTES;
    if (ver == 2 || ver == WORLD_VER)
        memcpy(usedOutpost, buf + o, WORLD_MASK_BYTES);
    else
        memset(usedOutpost, 0, sizeof usedOutpost);
    generated = true;
    if (ver != WORLD_VER) saveWorld();
    return true;
}

bool WorldState::saveTrek() const {
    static uint8_t buf[TREK_BIN_SIZE_CRC];
    size_t o = 0;
    putU32(buf, o, TREK_MAGIC);
    buf[o++] = TREK_VER; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;
    buf[o++] = ex.active ? 1 : 0;
    buf[o++] = ex.dead ? 1 : 0;
    putI16(buf, o, ex.x);        putI16(buf, o, ex.y);
    putI16(buf, o, ex.hp);       putI16(buf, o, ex.maxHp);
    putI16(buf, o, ex.water);    putI16(buf, o, ex.maxWater);
    putI16(buf, o, ex.foodMove); putI16(buf, o, ex.waterMove);
    putI16(buf, o, ex.fightMove);
    buf[o++] = ex.starving ? 1 : 0;
    buf[o++] = ex.thirsty ? 1 : 0;
    putU32(buf, o, ex.rng);
    uint8_t cf = (ex.clearedIron ? 1 : 0) | (ex.clearedCoal ? 2 : 0) |
                 (ex.clearedSulphur ? 4 : 0) | (ex.clearedShip ? 8 : 0) |
                 (ex.clearedExec ? 16 : 0) | (ex.gastronome ? 32 : 0) |
                 (ex.danger ? 64 : 0);
    buf[o++] = cf;
    if (ex.usedOutpostN > 16) return false;
    buf[o++] = ex.usedOutpostN;
    memcpy(buf + o, ex.usedOutpostX, 16); o += 16;
    memcpy(buf + o, ex.usedOutpostY, 16); o += 16;
    for (int i = 0; i < RES_COUNT; i++)  putI16(buf, o, ex.outfitRes[i]);
    for (int i = 0; i < ITEM_COUNT; i++) putI16(buf, o, ex.outfitItem[i]);
    memcpy(buf + o, ex.tiles, WORLD_CELLS);          o += WORLD_CELLS;
    memcpy(buf + o, ex.revealed, WORLD_MASK_BYTES);  o += WORLD_MASK_BYTES;
    memcpy(buf + o, ex.visited, WORLD_MASK_BYTES);   o += WORLD_MASK_BYTES;
    uint32_t sum = crc32(buf, o);
    putU32(buf, o, sum);
    return w_writeAtomic(ADR_TREK_PATH, ADR_TREK_TMP_PATH, ADR_TREK_BAK_PATH, buf, o);
}

bool WorldState::loadTrek() {
    static uint8_t buf[TREK_BIN_SIZE_CRC];
    int n = w_read(ADR_TREK_PATH, buf, sizeof buf);
    if (n < 0) { ex.active = false; return false; }

    size_t payload = 0;
    uint8_t ver = buf[4];
    if (ver == TREK_VER) {
        if (n != (int)TREK_BIN_SIZE_CRC) { ex.active = false; return false; }
        payload = n - 4;
        uint32_t stored = getU32(buf, payload);
        if (stored != crc32(buf, payload) ||
            !validTrekPayload(buf, payload, ver)) { ex.active = false; return false; }
    } else if (ver == 1) {
        payload = n;
        if (!validTrekPayload(buf, payload, ver)) { ex.active = false; return false; }
    } else { ex.active = false; return false; }

    size_t o = 0;
    if (getU32(buf, o) != TREK_MAGIC) { ex.active = false; return false; }
    ver = buf[o++]; o += 3;
    memset(&ex, 0, sizeof ex);
    ex.active = buf[o++] != 0;
    ex.dead   = buf[o++] != 0;
    ex.x = getI16(buf, o);        ex.y = getI16(buf, o);
    ex.hp = getI16(buf, o);       ex.maxHp = getI16(buf, o);
    ex.water = getI16(buf, o);    ex.maxWater = getI16(buf, o);
    ex.foodMove = getI16(buf, o); ex.waterMove = getI16(buf, o);
    ex.fightMove = getI16(buf, o);
    ex.starving = buf[o++] != 0;
    ex.thirsty  = buf[o++] != 0;
    ex.rng = getU32(buf, o);
    uint8_t cf = buf[o++];
    ex.clearedIron = cf & 1; ex.clearedCoal = cf & 2; ex.clearedSulphur = cf & 4;
    ex.clearedShip = cf & 8; ex.clearedExec = cf & 16; ex.gastronome = cf & 32;
    ex.danger = cf & 64;
    ex.usedOutpostN = buf[o++];
    memcpy(ex.usedOutpostX, buf + o, 16); o += 16;
    memcpy(ex.usedOutpostY, buf + o, 16); o += 16;
    for (int i = 0; i < RES_COUNT; i++)  ex.outfitRes[i] = getI16(buf, o);
    for (int i = 0; i < ITEM_COUNT; i++) ex.outfitItem[i] = getI16(buf, o);
    memcpy(ex.tiles, buf + o, WORLD_CELLS);          o += WORLD_CELLS;
    memcpy(ex.revealed, buf + o, WORLD_MASK_BYTES);  o += WORLD_MASK_BYTES;
    memcpy(ex.visited, buf + o, WORLD_MASK_BYTES);   o += WORLD_MASK_BYTES;
    if (ver != TREK_VER) saveTrek();
    return true;
}

void WorldState::clearTrek() { w_remove(ADR_TREK_PATH); }

bool WorldState::restore() {
    memset(&ex, 0, sizeof ex);
    memset(&cx, 0, sizeof cx);
    if (!loadWorld()) { generated = false; return false; }
    if (w_exists(ADR_TREK_PATH) && loadTrek() && ex.active) return true;
    ex.active = false;
    return false;
}
}  // namespace adr
