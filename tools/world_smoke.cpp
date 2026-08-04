// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Host smoke test for the A Dark Room Phase-2 World base layer (milestone 2.0).
// Compiles the pure logic (src/world_state.cpp + src/game_state.cpp, no Arduino)
// natively and asserts: deterministic map generation (landmark counts / radius
// bands / spawn / terrain sanity), diamond visibility, move upkeep + starvation/
// thirst death, goHome commit (mine unlock + loot bank + fog persist), die
// discard, drawRoad, world.bin + trek.bin round-trip / cold-boot restore, the
// main-save headroom after the game_data enum growth, (Phase 3a) the W
// landmark chain: salvage -> walk home -> the starship page unlocks, versus
// salvage -> die -> it does not and the wreck is salvageable again, and
// (Phase 3c-2) the X landmark's two doors: the prologue until it is cleared,
// the elevator hall for every visit after, with the wings banked only by a trip
// the wanderer walks home from.
//
// Build (clang++ is the host toolchain on this box):
//   clang++ -std=c++17 -I src tools/world_smoke.cpp src/world_state.cpp \
//           src/game_state.cpp src/setpiece_engine.cpp \
//           -DADR_SAVE_PATH='"world_smoke_game.json"' \
//           -DADR_WORLD_PATH='"world_smoke_world.bin"' \
//           -DADR_TREK_PATH='"world_smoke_trek.bin"' \
//           -o world_smoke.exe
#include "world_state.h"
#include "game_state.h"
#include "setpiece_engine.h"
#include "setpieces_data.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

using namespace adr;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else      { g_fail++; printf("  [FAIL] %s\n", msg); } } while (0)

static int manhattan(int x, int y) {
    int dx = x - VILLAGE_X, dy = y - VILLAGE_Y;
    return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
}
// Manhattan distance of the (assumed single) cell of `tile` from the village.
static int landmarkDist(const WorldState& w, uint8_t tile) {
    for (int y = 0; y < WORLD_DIM; y++)
        for (int x = 0; x < WORLD_DIM; x++)
            if (w.tileAt(x, y) == tile) return manhattan(x, y);
    return -1;
}

int main() {
    printf("== [gen] deterministic + village at center + forest ring ==\n");
    WorldState a, b;
    a.init(); b.init();
    a.generateMap(0xC0FFEE);
    b.generateMap(0xC0FFEE);
    CHECK(memcmp(a.tiles, b.tiles, sizeof a.tiles) == 0,
          "same seed -> identical map (reproducible)");
    WorldState c; c.init(); c.generateMap(0xC0FFEF);
    CHECK(memcmp(a.tiles, c.tiles, sizeof a.tiles) != 0,
          "different seed -> different map");
    CHECK(a.tileAt(VILLAGE_X, VILLAGE_Y) == T_VILLAGE, "village at center [30,30]");
    {
        // chooseTile forces every village-adjacent cell to FOREST at generation
        // time; a minRadius-0 landmark (e.g. a house) may later overwrite one on
        // its terrain — upstream-faithful. So each neighbor is FOREST or a
        // landmark, and (the real invariant) NEVER field/barrens.
        const int nx[4] = { VILLAGE_X, VILLAGE_X, VILLAGE_X - 1, VILLAGE_X + 1 };
        const int ny[4] = { VILLAGE_Y - 1, VILLAGE_Y + 1, VILLAGE_Y, VILLAGE_Y };
        bool ok = true;
        for (int i = 0; i < 4; i++) {
            uint8_t t = a.tileAt(nx[i], ny[i]);
            if (t != T_FOREST && !isLandmark(t)) ok = false;
        }
        CHECK(ok, "village neighbors are forest (or a landmark), never field/barrens");
    }

    printf("== [gen] landmark counts match World.LANDMARKS ==\n");
    CHECK(a.countTiles(T_IRON_MINE) == 1,    "1 iron mine");
    CHECK(a.countTiles(T_COAL_MINE) == 1,    "1 coal mine");
    CHECK(a.countTiles(T_SULPHUR_MINE) == 1, "1 sulphur mine");
    CHECK(a.countTiles(T_HOUSE) == 10,       "10 houses");
    CHECK(a.countTiles(T_CAVE) == 5,         "5 caves");
    CHECK(a.countTiles(T_TOWN) == 10,        "10 towns");
    CHECK(a.countTiles(T_CITY) == 20,        "20 cities");
    CHECK(a.countTiles(T_SHIP) == 1,         "1 ship");
    CHECK(a.countTiles(T_BOREHOLE) == 10,    "10 boreholes");
    CHECK(a.countTiles(T_BATTLEFIELD) == 5,  "5 battlefields");
    CHECK(a.countTiles(T_SWAMP) == 1,        "1 swamp");
    CHECK(a.countTiles(T_EXECUTIONER) == 1,  "1 executioner");
    CHECK(a.countTiles(T_OUTPOST) == 0,      "0 outposts at gen (spawned by clearing)");
    CHECK(a.countTiles(T_CACHE) == 0,        "0 caches (prestige-only, skipped)");

    printf("== [gen] fixed-radius landmarks pinned to exact Manhattan distance ==\n");
    CHECK(landmarkDist(a, T_IRON_MINE) == 5,     "iron mine at Manhattan 5");
    CHECK(landmarkDist(a, T_COAL_MINE) == 10,    "coal mine at Manhattan 10");
    CHECK(landmarkDist(a, T_SULPHUR_MINE) == 20, "sulphur mine at Manhattan 20");
    CHECK(landmarkDist(a, T_SHIP) == 28,         "ship at Manhattan 28");
    CHECK(landmarkDist(a, T_EXECUTIONER) == 28,  "executioner at Manhattan 28");

    printf("== [gen] banded landmarks land inside their annulus, on terrain ==\n");
    {
        bool caveOk = true, townOk = true, cityOk = true, allLand = true;
        for (int y = 0; y < WORLD_DIM; y++)
            for (int x = 0; x < WORLD_DIM; x++) {
                int d = manhattan(x, y);
                switch (a.tileAt(x, y)) {
                    case T_CAVE: if (d < 3 || d >= 10) caveOk = false; break;
                    case T_TOWN: if (d < 10 || d >= 20) townOk = false; break;
                    case T_CITY: if (d < 20 || d >= 45) cityOk = false; break;
                    default: break;
                }
                // no landmark may sit on a non-generated (void) cell
                if (isLandmark(a.tileAt(x, y)) && a.tileAt(x, y) == T_VOID) allLand = false;
            }
        CHECK(caveOk, "every cave in [3,10)");
        CHECK(townOk, "every town in [10,20)");
        CHECK(cityOk, "every city in [20,45)");
        CHECK(allLand, "no landmark on a void cell");
    }

    printf("== [gen] terrain distribution sanity (barrens dominant, all present) ==\n");
    {
        int f = a.countTiles(T_FOREST), fi = a.countTiles(T_FIELD),
            ba = a.countTiles(T_BARRENS);
        printf("     forest=%d field=%d barrens=%d\n", f, fi, ba);
        CHECK(f > 100 && fi > 100 && ba > 100, "all three terrains well-represented");
        CHECK(ba > f && ba > fi, "barrens is the plurality (TILE_PROBS 0.50)");
    }

    printf("== [embark] gates on cured meat; fills water/hp from equipment ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(42);
        int16_t noMeat[RES_COUNT] = { 0 };
        CHECK(!w.embark(gs, noMeat, nullptr, 1), "embark refused without cured meat");
        gs.items[I_S_ARMOUR] = 1;    // -> maxHealth 45
        gs.items[I_CASK] = 1;        // -> maxWater 30
        int16_t out[RES_COUNT] = { 0 };
        out[R_CURED_MEAT] = 5;
        gs.stores[R_CURED_MEAT] = 5 * FP;
        CHECK(w.embark(gs, out, nullptr, 1), "embark ok with cured meat");
        CHECK(w.ex.active, "expedition active");
        CHECK(w.ex.x == VILLAGE_X && w.ex.y == VILLAGE_Y, "start at village");
        CHECK(w.ex.hp == 45 && w.ex.maxHp == 45, "hp filled from s armour (45)");
        CHECK(w.ex.water == 30 && w.ex.maxWater == 30, "water filled from cask (30)");
        CHECK(gs.whole(R_CURED_MEAT) == 0, "embark deducted the 5 meat from stores");
        CHECK(w.ex.outfitRes[R_CURED_MEAT] == 5, "5 meat now in the bag");
    }

    printf("== [visibility] embark reveals home diamond; far cells hidden ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(7);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 3;
        w.embark(gs, out, nullptr, 1);
        CHECK(w.exRevealed(VILLAGE_X, VILLAGE_Y), "village revealed");
        CHECK(w.exRevealed(VILLAGE_X + LIGHT_RADIUS, VILLAGE_Y),
              "cell at light radius revealed");
        CHECK(!w.exRevealed(VILLAGE_X + LIGHT_RADIUS + 1, VILLAGE_Y),
              "cell just beyond light radius hidden");
        CHECK(!w.exRevealed(0, 0), "far corner hidden");
        // a move reveals the new neighborhood
        w.move(gs, DIR_EAST);
        CHECK(w.exRevealed(VILLAGE_X + 1 + LIGHT_RADIUS, VILLAGE_Y),
              "moving east reveals new ground ahead");
    }

    printf("== [upkeep] water drains/step, meat eaten every 2 steps, heals hp ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(11);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 10;
        w.embark(gs, out, nullptr, 1);
        for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = T_FOREST;  // all plain
        w.ex.hp = 1;                            // wounded, so a heal is visible
        w.move(gs, DIR_EAST);                   // step 1: water--, no eat
        CHECK(w.ex.water == 9, "step1: water 10 -> 9");
        CHECK(w.ex.outfitRes[R_CURED_MEAT] == 10, "step1: no meat eaten yet");
        w.move(gs, DIR_EAST);                   // step 2: eat 1 meat, heal +8
        CHECK(w.ex.water == 8, "step2: water -> 8");
        CHECK(w.ex.outfitRes[R_CURED_MEAT] == 9, "step2: ate 1 meat (10 -> 9)");
        CHECK(w.ex.hp == 9, "step2: meat healed hp 1 -> 9 (MEAT_HEAL 8)");
    }

    printf("== [death] starvation: warn tick then die (no HP drain) ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(13);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 1;
        gs.items[I_WATER_TANK] = 1;             // 60 water, so thirst can't win the race
        w.embark(gs, out, nullptr, 1);
        for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = T_FOREST;
        StepResult r{ STEP_MOVED, 0 };
        int steps = 0;
        while (w.ex.active && steps < 40) { r = w.move(gs, DIR_EAST); steps++; }
        CHECK(r.kind == STEP_DIED, "starvation kills the wanderer");
        CHECK(steps == 6, "death on step 6 (eat@2 runs out, @4 warn, @6 die)");
        CHECK(w.ex.dead && !w.ex.active, "expedition marked dead + inactive");
        CHECK(w.ex.hp > 0, "HP never drained by starvation (only combat drains)");
    }

    printf("== [death] thirst: warn tick then die ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(17);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 100;  // meat won't run out
        w.embark(gs, out, nullptr, 1);          // no water upgrade -> maxWater 10
        for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = T_FOREST;
        StepResult r{ STEP_MOVED, 0 };
        int steps = 0;
        while (w.ex.active && steps < 40) { r = w.move(gs, DIR_EAST); steps++; }
        CHECK(r.kind == STEP_DIED, "thirst kills the wanderer");
        CHECK(steps == 12, "death on step 12 (water 10 -> 0 @10, warn @11, die @12)");
    }

    printf("== [notice] meat/water one-shot \"just ran out\" fires once, distinct from the\n"
           "            persisting starving/thirsty latch (§3.3) ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(9003);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 1;
        gs.items[I_WATER_TANK] = 1;             // 60 water: thirst can't race the meat
        w.embark(gs, out, nullptr, 1);
        for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = T_FOREST;
        // step1: water-- only (plenty left, no notice). step2: eats the last piece.
        StepResult r1 = w.move(gs, DIR_EAST);
        CHECK(r1.notice == nullptr, "step1: no meat/water notice yet");
        StepResult r2 = w.move(gs, DIR_EAST);
        CHECK(r2.notice != nullptr && strcmp(r2.notice, "the meat has run out") == 0,
              "step2: \"the meat has run out\" fires the instant the last piece is eaten");
        StepResult r3 = w.move(gs, DIR_EAST);   // movesPerFood=2: no food tick this step
        CHECK(r3.notice == nullptr, "step3: no food tick (movesPerFood=2), no repeat notice");
        StepResult r4 = w.move(gs, DIR_EAST);   // the starving LATCH sets here
        CHECK(r4.notice == nullptr,
              "step4: \"starvation sets in\" is the persisting latch, not a one-shot notice");
        CHECK(w.ex.starving, "step4: ex.starving latched (hudMessage's own channel)");
    }

    printf("== [notice] water one-shot \"there is no more water\" fires once ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(9004);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 100;   // meat can't run out
        w.embark(gs, out, nullptr, 1);          // no water upgrade -> maxWater 10
        for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = T_FOREST;
        StepResult r; const char* firedAt = nullptr; int fireStep = -1;
        for (int step = 1; step <= 10; step++) {
            r = w.move(gs, DIR_EAST);
            if (r.notice) { firedAt = r.notice; fireStep = step; }
        }
        CHECK(fireStep == 10 && firedAt && strcmp(firedAt, "there is no more water") == 0,
              "\"there is no more water\" fires exactly on step 10 (water 10 -> 0)");
        StepResult r11 = w.move(gs, DIR_EAST);   // step11: the thirsty LATCH sets here
        CHECK(r11.notice == nullptr, "step11: thirst latch is persisting state, not r.notice");
        CHECK(w.ex.thirsty, "step11: ex.thirsty latched");
    }

    printf("== [danger] checkDanger edge-triggers at the armour/distance thresholds (§3.1/§4.4) ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(9001);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 3;
        w.embark(gs, out, nullptr, 1);
        // T_ROAD: not terrain, so neither narrateMove nor a random encounter can
        // fire and mask/compete with the danger notice under test.
        for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = T_ROAD;
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + VILLAGE_X] = T_VILLAGE;
        w.ex.outfitRes[R_CURED_MEAT] = 30000;
        w.ex.maxWater = 30000; w.ex.water = 30000;   // supplies can't run out mid-walk

        // No armour: the notice fires exactly once, at Manhattan distance 8.
        int fireAt = -1, fireCount = 0;
        for (int d = 1; d <= 12; d++) {
            StepResult r = w.move(gs, DIR_EAST);
            if (r.notice) { fireCount++; fireAt = d; }
        }
        CHECK(fireCount == 1 && fireAt == 8, "no armour: single danger notice, exactly at distance 8");

        // Walking back below 8 fires the "safer here" transition exactly once.
        fireCount = 0; int leaveAt = -1;
        for (int d = 11; d >= 1; d--) {
            StepResult r = w.move(gs, DIR_WEST);
            if (r.notice) { fireCount++; leaveAt = d; }
        }
        CHECK(fireCount == 1 && leaveAt == 7, "walking back to safety fires once, at distance 7");
    }
    {
        GameState gs; gs.init();
        gs.items[I_I_ARMOUR] = 1;                 // iron+ satisfies the 8-away gate
        WorldState w; w.init(); w.generateMap(9002);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 3;
        w.embark(gs, out, nullptr, 1);
        for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = T_ROAD;
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + VILLAGE_X] = T_VILLAGE;
        w.ex.outfitRes[R_CURED_MEAT] = 30000;
        w.ex.maxWater = 30000; w.ex.water = 30000;
        int fireAt = -1, fireCount = 0;
        for (int d = 1; d <= 20; d++) {
            StepResult r = w.move(gs, DIR_EAST);
            if (r.notice) { fireCount++; fireAt = d; }
        }
        CHECK(fireCount == 1 && fireAt == 18,
              "iron armour: no notice at 8, fires at the steel threshold 18");
    }

    printf("== [narrate] terrain-change notice fires only across two DIFFERENT plain-terrain\n"
           "             tiles (§7.3) ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(9005);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 30000;
        w.embark(gs, out, nullptr, 1);
        w.ex.outfitRes[R_CURED_MEAT] = 30000;
        w.ex.maxWater = 30000; w.ex.water = 30000;
        // village -> forest -> field -> field(same) -> barrens -> road, due east.
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + (VILLAGE_X + 1)] = T_FOREST;
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + (VILLAGE_X + 2)] = T_FIELD;
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + (VILLAGE_X + 3)] = T_FIELD;
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + (VILLAGE_X + 4)] = T_BARRENS;
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + (VILLAGE_X + 5)] = T_ROAD;
        StepResult r1 = w.move(gs, DIR_EAST);   // village -> forest: village isn't terrain
        CHECK(r1.notice == nullptr, "leaving the village doesn't narrate (village isn't terrain)");
        StepResult r2 = w.move(gs, DIR_EAST);   // forest -> field
        CHECK(r2.notice != nullptr && strcmp(r2.notice,
              "the trees yield to dry grass. the yellowed brush rustles in the wind.") == 0,
              "forest -> field narrates");
        StepResult r3 = w.move(gs, DIR_EAST);   // field -> field
        CHECK(r3.notice == nullptr, "same-terrain step doesn't narrate");
        StepResult r4 = w.move(gs, DIR_EAST);   // field -> barrens
        CHECK(r4.notice != nullptr &&
              strcmp(r4.notice, "the grasses thin. soon, only dust remains.") == 0,
              "field -> barrens narrates");
        StepResult r5 = w.move(gs, DIR_EAST);   // barrens -> road: road isn't terrain
        CHECK(r5.notice == nullptr, "stepping onto a road doesn't narrate (road isn't terrain)");
    }

    printf("== [compass] compassFromVillage: 8-way direction from the ship on the\n"
           "             COMMITTED map (§2.7, the one-time compass-purchase notice) ==\n");
    {
        WorldState w; w.init(); w.generateMap(31337);
        auto plantShip = [&](int dx, int dy) {
            for (int i = 0; i < WORLD_CELLS; i++) if (w.tiles[i] == T_SHIP) w.tiles[i] = T_BARRENS;
            w.tiles[(VILLAGE_Y + dy) * WORLD_DIM + (VILLAGE_X + dx)] = T_SHIP;
        };
        char key[40];
        plantShip(0, -10);                        // due north
        CHECK(w.compassFromVillage(key, sizeof key) &&
              strcmp(key, "the compass points north") == 0, "ship due north of the village");
        plantShip(10, 10);                        // equal axes -> diagonal
        w.compassFromVillage(key, sizeof key);
        CHECK(strcmp(key, "the compass points southeast") == 0,
              "ship southeast (equal axes -> diagonal)");
        plantShip(-20, 1);                        // strongly west
        w.compassFromVillage(key, sizeof key);
        CHECK(strcmp(key, "the compass points west") == 0,
              "ship strongly west -> primary axis west");
        for (int i = 0; i < WORLD_CELLS; i++) if (w.tiles[i] == T_SHIP) w.tiles[i] = T_BARRENS;
        CHECK(!w.compassFromVillage(key, sizeof key), "no ship on the committed map -> false");
    }

    printf("== [goHome] commit: mine unlock + loot bank + tile/fog persist ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(2024);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;
        w.embark(gs, out, nullptr, 1);
        // simulate a trip: pick up loot, clear a mine + a dungeon on the WORKING map
        w.ex.outfitRes[R_TEETH] = 7;
        w.ex.outfitItem[I_BAYONET] = 1;
        w.ex.clearedIron = true;
        w.clearDungeon(35, VILLAGE_Y);          // some terrain cell -> outpost + road
        CHECK(w.tileAt(35, VILLAGE_Y) != T_OUTPOST,
              "committed tile still un-cleared mid-trip (working copy only)");
        int teethBefore = gs.whole(R_TEETH);
        int bayonetBefore = gs.items[I_BAYONET];
        // walk onto the village -> goHome
        w.ex.x = VILLAGE_X + 1; w.ex.y = VILLAGE_Y;
        StepResult r = w.move(gs, DIR_WEST);
        CHECK(r.kind == STEP_HOME, "stepping onto the village triggers goHome");
        CHECK(!w.ex.active, "expedition ended");
        CHECK(gs.buildings[B_IRON_MINE] == 1, "iron mine unlocked in game.buildings");
        CHECK(gs.hasUnlockedJob(), "a miner job is now staffable (JOB_REQ_BLD wired)");
        {
            uint8_t jobs[JOB_COUNT]; int n = gs.unlockedJobs(jobs, JOB_COUNT);
            bool sawIron = false;
            for (int i = 0; i < n; i++) if (jobs[i] == J_IRON_MINER) sawIron = true;
            CHECK(sawIron, "iron miner appears in unlockedJobs()");
        }
        CHECK(gs.whole(R_TEETH) == teethBefore + 7, "7 teeth banked to stores");
        CHECK(gs.hasSeen(R_TEETH), "banked loot latches the seen bit");
        CHECK(gs.items[I_BAYONET] == bayonetBefore + 1, "bayonet banked to items");
        CHECK(w.tileAt(35, VILLAGE_Y) == T_OUTPOST, "cleared dungeon committed to map");
    }

    printf("== [die] discards the trip: committed map + game state untouched ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(2025);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;
        gs.stores[R_CURED_MEAT] = 5 * FP;
        w.embark(gs, out, nullptr, 1);
        w.ex.clearedCoal = true;
        w.clearDungeon(34, VILLAGE_Y);
        uint8_t committedBefore = w.tileAt(34, VILLAGE_Y);
        w.die();
        CHECK(w.ex.dead && !w.ex.active, "die() ends the expedition");
        CHECK(w.tileAt(34, VILLAGE_Y) == committedBefore,
              "committed map unchanged (trip discarded)");
        CHECK(gs.buildings[B_COAL_MINE] == 0, "coal mine NOT unlocked (died before goHome)");
        CHECK(gs.whole(R_CURED_MEAT) == 0, "bag forfeited: the 5 embarked meat is lost");
        CHECK(w.ex.outfitRes[R_CURED_MEAT] == 0, "bag emptied on death");
    }

    printf("== [outfit] goHome remembers the loadout (leaveItAtHome §3.5); raw loot drops off ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(4242);
        // Pack meat + medicine + a bayonet; the trip also loots raw teeth and a
        // steel sword, and 3 meat get eaten en route.
        gs.stores[R_CURED_MEAT] = 10 * FP;
        gs.stores[R_MEDICINE]   = 3 * FP;
        gs.items[I_BAYONET]     = 1;
        int16_t out[RES_COUNT]  = { 0 };
        int16_t outI[ITEM_COUNT] = { 0 };
        out[R_CURED_MEAT] = 10; out[R_MEDICINE] = 3; outI[I_BAYONET] = 1;
        CHECK(w.embark(gs, out, outI, 1), "embark with meat + medicine + bayonet");
        w.ex.outfitRes[R_TEETH]         = 6;   // raw loot picked up
        w.ex.outfitItem[I_STEEL_SWORD]  = 1;   // weapon loot picked up
        w.ex.outfitRes[R_CURED_MEAT]    = 7;   // ate 3 of the 10 meat
        w.ex.x = VILLAGE_X + 1; w.ex.y = VILLAGE_Y;
        StepResult r = w.move(gs, DIR_WEST);
        CHECK(r.kind == STEP_HOME, "reached the village -> goHome");
        // Everything banks to stores (returnOutfit adds every slot).
        CHECK(gs.whole(R_TEETH) == 6,        "raw teeth banked to stores");
        CHECK(gs.items[I_STEEL_SWORD] == 1,  "looted steel sword banked to items");
        CHECK(gs.whole(R_CURED_MEAT) == 7,   "the 7 surviving meat banked to stores");
        CHECK(gs.whole(R_MEDICINE) == 3,     "the 3 medicine banked back to stores");
        // Remembered outfit: supplies + weapons stay; raw material is left at home.
        CHECK(gs.savedOutfitRes[R_CURED_MEAT] == 7,  "meat remembered for next trip");
        CHECK(gs.savedOutfitRes[R_MEDICINE] == 3,    "medicine remembered");
        CHECK(gs.savedOutfitItem[I_BAYONET] == 1,    "packed bayonet remembered (weapon)");
        CHECK(gs.savedOutfitItem[I_STEEL_SWORD] == 1,"looted steel sword remembered (weapon)");
        CHECK(gs.savedOutfitRes[R_TEETH] == 0,       "raw teeth NOT remembered (left at home)");

        // Re-embark from the remembered loadout — the engine slice of the Path
        // pre-fill: pack min(remembered, current stock) (prefillOutfit clamp #1).
        // Stock still covers everything here, so the full loadout re-packs.
        int16_t out2[RES_COUNT] = { 0 }, out2I[ITEM_COUNT] = { 0 };
        for (int i = 0; i < RES_COUNT; i++) {
            int wnt = gs.savedOutfitRes[i], stk = gs.whole((uint8_t)i);
            out2[i] = (int16_t)(wnt < stk ? wnt : stk);
        }
        for (int i = 0; i < ITEM_COUNT; i++) {
            int wnt = gs.savedOutfitItem[i], stk = gs.items[i];
            out2I[i] = (int16_t)(wnt < stk ? wnt : stk);
        }
        int meatBefore = gs.whole(R_CURED_MEAT);
        CHECK(w.embark(gs, out2, out2I, 2),           "re-embark from the remembered loadout");
        CHECK(w.ex.outfitRes[R_CURED_MEAT] == 7,      "pre-filled 7 meat carried into the trek");
        CHECK(w.ex.outfitItem[I_BAYONET] == 1,        "pre-filled bayonet carried into the trek");
        CHECK(gs.whole(R_CURED_MEAT) == meatBefore-7, "re-embark deducted the pre-filled meat");
    }

    printf("== [outfit] stock spent below the remembered amount -> pre-fill clamps to stock ==\n");
    {
        GameState gs; gs.init();
        gs.savedOutfitRes[R_CURED_MEAT] = 7;      // last trip remembered 7 meat
        gs.stores[R_CURED_MEAT] = 2 * FP;         // but a charcutier shortfall left only 2
        // prefillOutfit clamp #1: pack min(remembered, stock).
        int want = gs.savedOutfitRes[R_CURED_MEAT], stock = gs.whole(R_CURED_MEAT);
        int packed = want < stock ? want : stock;
        CHECK(packed == 2, "pre-fill packs only the 2 in stock, not the remembered 7");
    }

    printf("== [outfit] death KEEPS the remembered loadout (deliberate divergence from upstream) ==\n");
    {
        // Design decision: the death penalty is the lost PHYSICAL bag (ex.outfit),
        // not the pre-fill memory. die() empties the trek bag but must leave
        // gs.savedOutfit intact so the next trip still pre-fills (min-clamped to
        // stock -> no exploit). Upstream die() would $SM.remove('outfit'); we don't.
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(7777);
        gs.savedOutfitRes[R_CURED_MEAT] = 5;   // a loadout remembered from a past goHome
        gs.savedOutfitItem[I_BAYONET]   = 1;
        gs.stores[R_CURED_MEAT] = 5 * FP;
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;
        w.embark(gs, out, nullptr, 1);
        w.ex.outfitRes[R_CURED_MEAT] = 5;      // physical bag carried into the trek
        w.die();
        CHECK(w.ex.dead && !w.ex.active,             "die() ended the expedition");
        CHECK(w.ex.outfitRes[R_CURED_MEAT] == 0,     "physical bag forfeited on death (penalty stands)");
        CHECK(gs.savedOutfitRes[R_CURED_MEAT] == 5,  "remembered meat SURVIVES death (pre-fill memory kept)");
        CHECK(gs.savedOutfitItem[I_BAYONET] == 1,    "remembered bayonet SURVIVES death");
        // Only a fresh game wipes the memory (init -> clearSavedOutfit).
        gs.clearSavedOutfit();
        bool empty = true;
        for (int i = 0; i < RES_COUNT; i++)  if (gs.savedOutfitRes[i])  empty = false;
        for (int i = 0; i < ITEM_COUNT; i++) if (gs.savedOutfitItem[i]) empty = false;
        CHECK(empty, "clearSavedOutfit (fresh-game only) empties both arrays");
    }

    printf("== [outfit] remembered loadout survives a game.json round-trip (sparse pairs) ==\n");
    {
        GameState gs; gs.init();
        gs.savedOutfitRes[R_CURED_MEAT]  = 9;
        gs.savedOutfitRes[R_MEDICINE]    = 2;
        gs.savedOutfitItem[I_LASER_RIFLE] = 1;
        gs.savedOutfitItem[I_BOLAS]       = 3;
        char buf[8192]; gs.toJson(buf, sizeof buf);
        GameState g2; g2.fromJson(buf);
        CHECK(g2.savedOutfitRes[R_CURED_MEAT] == 9,   "meat count survives round-trip");
        CHECK(g2.savedOutfitRes[R_MEDICINE] == 2,     "medicine count survives round-trip");
        CHECK(g2.savedOutfitItem[I_LASER_RIFLE] == 1, "laser rifle survives round-trip");
        CHECK(g2.savedOutfitItem[I_BOLAS] == 3,       "bolas count survives round-trip");
        CHECK(g2.savedOutfitRes[R_TEETH] == 0,        "an unset slot stays zero");
        // Old saves lack the keys entirely -> empty remembered outfit (no bump).
        GameState g3; g3.fromJson("{\"v\":3,\"ts\":0,\"stores\":[]}");
        bool empty = true;
        for (int i = 0; i < RES_COUNT; i++)  if (g3.savedOutfitRes[i])  empty = false;
        for (int i = 0; i < ITEM_COUNT; i++) if (g3.savedOutfitItem[i]) empty = false;
        CHECK(empty, "a save without the outfit keys loads as an empty loadout");
    }

    printf("== [outpost] one-shot use persists across expeditions; die() discards ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(31337);
        const int ox = VILLAGE_X + 1, oy = VILLAGE_Y;   // outpost right next to home
        w.tiles[oy * WORLD_DIM + ox] = T_OUTPOST;
        w.saveWorld();
        auto stock = [&]() {                            // re-arm the embark gate
            gs.stores[R_CURED_MEAT] = 5 * FP;
        };
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;

        // Trip 1: step onto the fresh outpost (a landmark event), then DIE.
        stock();
        CHECK(w.embark(gs, out, nullptr, 1), "trip1 embark");
        StepResult r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_OUTPOST,
              "trip1: fresh outpost fires its setpiece");
        w.die();                                        // trip1's use is discarded

        // Trip 2: the outpost is STILL fresh (die() dropped trip1's use), then
        // walk home so goHome commits the use into the committed layer.
        out[R_CURED_MEAT] = 5; stock();
        CHECK(w.embark(gs, out, nullptr, 1), "trip2 embark");
        r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_LANDMARK,
              "trip2: outpost still fresh after a death (die discards its use)");
        r = w.move(gs, DIR_WEST);                       // back onto the village
        CHECK(r.kind == STEP_HOME, "trip2: reached home -> goHome commits the used flag");

        // Trip 3: the outpost is now permanently spent (committed one-shot).
        out[R_CURED_MEAT] = 5; stock();
        CHECK(w.embark(gs, out, nullptr, 1), "trip3 embark");
        r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_MOVED,
              "trip3: used outpost is safe, no event (persisted across expeditions)");

        // ...and it survives a committed world.bin round-trip.
        CHECK(w.saveWorld(), "save committed world (with the used-outpost mask)");
        WorldState wl; wl.init();
        CHECK(wl.loadWorld(), "reload committed world");
        GameState gs2; gs2.init(); gs2.stores[R_CURED_MEAT] = 5 * FP;
        int16_t out2[RES_COUNT] = { 0 }; out2[R_CURED_MEAT] = 5;
        CHECK(wl.embark(gs2, out2, nullptr, 1), "embark on the reloaded map");
        r = wl.move(gs2, DIR_EAST);
        CHECK(r.kind == STEP_MOVED, "used-outpost flag survived the world.bin round-trip");
    }

    printf("== [death cooldown] embark locked for DEATH_COOLDOWN_S after a death ==\n");
    {
        WorldState w; w.init(); w.generateMap(4242);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;

        // A death at epoch 100000 (UI stamps gs.deathAt at the death frame).
        GameState gs; gs.init(); gs.stores[R_CURED_MEAT] = 5 * FP; gs.deathAt = 100000;
        CHECK(!w.embark(gs, out, nullptr, 1, 100000),
              "embark refused at the instant of death");
        CHECK(!w.embark(gs, out, nullptr, 1, 100000 + DEATH_COOLDOWN_S - 1),
              "embark refused 1s before the cooldown expires");
        CHECK(w.embark(gs, out, nullptr, 1, 100000 + DEATH_COOLDOWN_S),
              "embark allowed exactly when the cooldown elapses");

        // Fail-open: a 0 (no-RTC) clock read never traps the player in a lockout.
        GameState gs2; gs2.init(); gs2.stores[R_CURED_MEAT] = 5 * FP; gs2.deathAt = 100000;
        int16_t out2[RES_COUNT] = { 0 }; out2[R_CURED_MEAT] = 5;
        CHECK(w.embark(gs2, out2, nullptr, 1, 0), "no-RTC (epoch 0) fails open -> embark allowed");

        // No death recorded -> never locked.
        GameState gs3; gs3.init(); gs3.stores[R_CURED_MEAT] = 5 * FP;
        int16_t out3[RES_COUNT] = { 0 }; out3[R_CURED_MEAT] = 5;
        CHECK(w.embark(gs3, out3, nullptr, 1, 500000), "no death recorded -> embark always allowed");
    }

    printf("== [drawRoad] L-path of ROAD connects a cleared point to the village ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(5);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;
        w.embark(gs, out, nullptr, 1);
        for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = T_FOREST;  // all terrain
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + VILLAGE_X] = T_VILLAGE;       // restore home
        w.clearDungeon(40, VILLAGE_Y);          // 10 east of the village
        CHECK(w.exTileAt(40, VILLAGE_Y) == T_OUTPOST, "cleared cell became outpost");
        CHECK(w.exTileAt(35, VILLAGE_Y) == T_ROAD, "road drawn along the row");
        CHECK(w.exTileAt(31, VILLAGE_Y) == T_ROAD, "road reaches the village's doorstep");
        // every intermediate terrain cell on the row is road (continuous path)
        bool continuous = true;
        for (int x = 31; x <= 39; x++)
            if (w.exTileAt(x, VILLAGE_Y) != T_ROAD) continuous = false;
        CHECK(continuous, "road is continuous from village to the outpost");
    }

    printf("== [persist] world.bin round-trip ==\n");
    {
        WorldState w; w.init(); w.generateMap(0xABCD);
        CHECK(w.saveWorld(), "saveWorld ok");
        WorldState wl; wl.init();
        CHECK(wl.loadWorld(), "loadWorld ok");
        CHECK(wl.generated && wl.seed == 0xABCD, "seed + generated flag restored");
        CHECK(memcmp(wl.tiles, w.tiles, sizeof w.tiles) == 0, "tiles round-trip exactly");
        CHECK(memcmp(wl.revealed, w.revealed, sizeof w.revealed) == 0, "fog round-trips");
        CHECK(wl.tileAt(VILLAGE_X, VILLAGE_Y) == T_VILLAGE, "village survived round-trip");
    }

    printf("== [persist] trek.bin round-trip + cold-boot restore ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(0x1357);
        CHECK(w.saveWorld(), "committed map on disk");
        int16_t out[RES_COUNT] = { 0 };
        out[R_CURED_MEAT] = 8; out[R_MEDICINE] = 2;
        gs.stores[R_CURED_MEAT] = 8 * FP; gs.stores[R_MEDICINE] = 2 * FP;
        gs.items[I_RIFLE] = 1;
        int16_t outi[ITEM_COUNT] = { 0 }; outi[I_RIFLE] = 1;
        w.embark(gs, out, outi, 0x99);          // embark saves trek.bin
        w.move(gs, DIR_EAST); w.move(gs, DIR_EAST);   // a couple of steps
        // cold boot: a fresh device restores committed + the interrupted trip
        WorldState cold; cold.init();
        CHECK(cold.restore(), "restore() finds an interrupted expedition (trek.bin)");
        CHECK(cold.ex.active, "restored expedition is active");
        CHECK(cold.ex.x == w.ex.x && cold.ex.y == w.ex.y, "position restored");
        CHECK(cold.ex.hp == w.ex.hp && cold.ex.water == w.ex.water, "hp/water restored");
        CHECK(cold.ex.foodMove == w.ex.foodMove && cold.ex.rng == w.ex.rng,
              "upkeep counters + expedition rng restored");
        CHECK(cold.ex.outfitRes[R_CURED_MEAT] == w.ex.outfitRes[R_CURED_MEAT] &&
              cold.ex.outfitItem[I_RIFLE] == 1, "bag (res + items) round-trips");
        CHECK(memcmp(cold.ex.tiles, w.ex.tiles, sizeof w.ex.tiles) == 0,
              "working map round-trips");
        CHECK(memcmp(cold.ex.revealed, w.ex.revealed, sizeof w.ex.revealed) == 0,
              "working fog round-trips");
        // finishing the trip clears trek.bin -> a later boot finds no expedition
        w.ex.x = VILLAGE_X + 1; w.ex.y = VILLAGE_Y;
        w.move(gs, DIR_WEST);                   // goHome -> clearTrek
        WorldState cold2; cold2.init();
        CHECK(!cold2.restore(), "after goHome, restore() finds no active expedition");
        CHECK(cold2.generated, "committed map still loads after the trip ended");
    }

    printf("== [save] main JSON headroom after enum growth (4096 cap) ==\n");
    {
        // A heavy-but-plausible late-game state: big stores, every building &
        // item stocked, a full log ring — measure toJson() vs the 4096 SD cap.
        GameState gs; gs.init();
        gs.settle(1000);
        for (int i = 0; i < RES_COUNT; i++) gs.stores[i] = 5000000 * FP;  // 7-digit
        for (int i = 0; i < BLD_COUNT; i++) gs.buildings[i] = 20;
        for (int i = 0; i < ITEM_COUNT; i++) gs.items[i] = 99;
        for (int i = 0; i < JOB_COUNT; i++) gs.workers[i] = 9999;
        gs.population = 65535;
        gs.seen = 0xFFFFFFFF; gs.craftShown = 0xFFFFFFFF;
        gs.shipUnlocked = gs.shipSeenWarning = true;    // P3a: the v4 ship keys
        gs.shipHull = 32767; gs.shipThrusters = 32767; gs.cdLiftoff = 4294967295u;
        // 8 DISTINCT long entries so the log ring is genuinely full (pushLog
        // collapses repeats of the newest key, so identical pushes wouldn't
        // stress it) — an honest worst case for the byte measurement.
        for (int i = 0; i < LOG_CAP; i++) {
            char k[LOG_KEY_MAX];
            snprintf(k, sizeof k,
                     "the stranger is standing by the fire, log line number %d of the ring", i);
            gs.pushLog(k, 65535, true);
        }
        char buf[8192];
        size_t n = gs.toJson(buf, sizeof buf);
        printf("     toJson() = %zu bytes, headroom = %d / 4096\n", n, 4096 - (int)n);
        CHECK(n < 4096, "heavy save still fits the 4096-byte SD buffer");
        CHECK(gs.save(), "save() accepts the heavy state (n < 4096)");
    }

    // ======================= combat (milestone 2.3) =======================

    // A live expedition planted on a given tile at a given position, seeded rng.
    auto plant = [](WorldState& w, GameState& gs, int x, int y, uint8_t tile,
                    uint32_t rng) {
        gs.init(); w.init(); w.generateMap(999);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 3;
        w.embark(gs, out, nullptr, 1);
        for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = tile;
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + VILLAGE_X] = T_VILLAGE;
        w.ex.x = (int16_t)x; w.ex.y = (int16_t)y; w.ex.rng = rng;
    };

    printf("== [fight] encounter selection by distance tier + terrain ==\n");
    {
        GameState gs; WorldState w;
        // fixed-position picks: single-member pools resolve deterministically.
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_FOREST, 1);
        CHECK(w.chooseEncounter() == E_SNARLING_BEAST, "d5 forest -> snarling beast (T1)");
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_BARRENS, 1);
        CHECK(w.chooseEncounter() == E_GAUNT_MAN, "d5 barrens -> gaunt man (T1)");
        plant(w, gs, VILLAGE_X + 15, VILLAGE_Y, T_FOREST, 1);
        CHECK(w.chooseEncounter() == E_MAN_EATER, "d15 forest -> man-eater (T2)");
        plant(w, gs, VILLAGE_X + 15, VILLAGE_Y, T_FIELD, 1);
        CHECK(w.chooseEncounter() == E_LIZARD, "d15 field -> lizard (T2)");
        plant(w, gs, VILLAGE_X + 25, VILLAGE_Y, T_FOREST, 1);
        CHECK(w.chooseEncounter() == E_FERAL_TERROR, "d25 forest -> feral terror (T3)");
        plant(w, gs, VILLAGE_X + 25, VILLAGE_Y, T_BARRENS, 1);
        CHECK(w.chooseEncounter() == E_SOLDIER, "d25 barrens -> soldier (T3)");
        plant(w, gs, VILLAGE_X + 25, VILLAGE_Y, T_FIELD, 1);
        CHECK(w.chooseEncounter() == E_SNIPER, "d25 field -> sniper (T3)");
        // two-member field pool (strange bird / two-headed) always resolves in-pool
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_FIELD, 7);
        int fp = w.chooseEncounter();
        CHECK(fp == E_STRANGE_BIRD || fp == E_TWO_HEADED, "d5 field -> in the T1 field pool");
        // roads carry no encounter
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_ROAD, 1);
        CHECK(w.chooseEncounter() == -1, "road tile -> no encounter (-1)");
    }

    printf("== [fight] weapon list: ammo-gated, fists fallback ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_FOREST, 1);
        w.beginFight(E_SNARLING_BEAST);
        CHECK(w.fightWeaponCount() == 1 && w.fightWeaponId(0) == WEAPON_FISTS,
              "no weapons packed -> fists only");
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_FOREST, 1);
        w.ex.outfitItem[I_RIFLE] = 1;                 // rifle but no bullets
        w.beginFight(E_SNARLING_BEAST);
        CHECK(w.fightWeaponCount() == 1 && w.fightWeaponId(0) == WEAPON_FISTS,
              "rifle w/o bullets doesn't count -> fists fallback");
        w.ex.outfitRes[R_BULLETS] = 3;                // now it has ammo
        w.beginFight(E_SNARLING_BEAST);
        CHECK(w.fightWeaponCount() == 1 && w.fightWeaponId(0) == WEAPON_RIFLE,
              "rifle with bullets counts, fists dropped");
        w.ex.outfitItem[I_BAYONET] = 1;               // add a melee (no ammo needed)
        w.beginFight(E_SNARLING_BEAST);
        CHECK(w.fightWeaponCount() == 2, "bayonet + rifle both listed");
    }

    printf("== [fight] hit/damage/victory + loot into the bag ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_FOREST, 1);  // rng=1 -> first hit lands
        w.ex.outfitItem[I_BAYONET] = 1;               // dmg 8 one-shots a 5-hp beast
        w.beginFight(E_SNARLING_BEAST);
        uint8_t st = w.fightAttack(gs, 0);
        CHECK(st == FIGHT_WON, "bayonet (8) kills the snarling beast (5 hp)");
        CHECK(w.cx.won && w.cx.enemyHp == 0, "victory state, enemy at 0");
        bool onlyValidLoot = true;
        // banked loot may only be fur / meat / teeth (the beast's table); the
        // pre-loaded cured meat (3, from embark) is the only other bag resource.
        for (int r = 0; r < RES_COUNT; r++)
            if (w.ex.outfitRes[r] > 0 && r != R_FUR && r != R_MEAT &&
                r != R_TEETH && r != R_CURED_MEAT)
                onlyValidLoot = false;
        CHECK(onlyValidLoot, "banked loot is only from the beast's table");
        // determinism: same seed + kill -> identical loot
        GameState gs2; WorldState w2;
        plant(w2, gs2, VILLAGE_X + 5, VILLAGE_Y, T_FOREST, 1);
        w2.ex.outfitItem[I_BAYONET] = 1;
        w2.beginFight(E_SNARLING_BEAST);
        w2.fightAttack(gs2, 0);
        CHECK(w.ex.outfitRes[R_FUR] == w2.ex.outfitRes[R_FUR] &&
              w.ex.outfitRes[R_MEAT] == w2.ex.outfitRes[R_MEAT] &&
              w.ex.outfitRes[R_TEETH] == w2.ex.outfitRes[R_TEETH],
              "loot is deterministic for a fixed rng");
    }

    printf("== [fight] weapon cooldown gates rapid attacks ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 15, VILLAGE_Y, T_FOREST, 3);   // man-eater, 25 hp
        w.beginFight(E_MAN_EATER);                    // fists cd 2
        w.fightAttack(gs, 0);
        CHECK(w.fightWeaponCoolLeft(0) == 2, "fists set a 2s cooldown");
        CHECK(w.fightAttack(gs, 0) == FIGHT_NOOP, "attacking while cooling is a no-op");
        CHECK(!w.fightWeaponEnabled(0), "cooling weapon reads disabled");
        w.fightTick(gs);
        CHECK(w.fightWeaponCoolLeft(0) == 1, "tick drains cooldown 2 -> 1");
        w.fightTick(gs);
        CHECK(w.fightWeaponEnabled(0), "cooldown cleared -> weapon ready again");
    }

    printf("== [fight] eat meat / use meds heal (cooldown-gated, below max) ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_FOREST, 1);
        w.beginFight(E_SNARLING_BEAST);
        w.ex.maxHp = 20; w.ex.hp = 1;
        w.ex.outfitRes[R_CURED_MEAT] = 2;
        w.ex.outfitRes[R_MEDICINE] = 1;
        CHECK(w.fightEat() == FIGHT_ONGOING, "eat meat accepted");
        CHECK(w.ex.hp == 9 && w.ex.outfitRes[R_CURED_MEAT] == 1,
              "meat: hp 1 -> 9 (+8), one meat spent");
        CHECK(w.fightEat() == FIGHT_NOOP, "eating again while cooling is a no-op");
        w.cx.eatCool = 0;                             // clear the eat cooldown
        CHECK(w.fightMeds() == FIGHT_ONGOING, "use meds accepted");
        CHECK(w.ex.hp == 20 && w.ex.outfitRes[R_MEDICINE] == 0,
              "meds: hp 9 -> 20 (+20, capped), one medicine spent");
        CHECK(w.fightEat() == FIGHT_NOOP, "no heal at full hp");
    }

    printf("== [fight] enemy swing on its delay; player death -> die() ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 25, VILLAGE_Y, T_BARRENS, 1);  // soldier dmg 8, delay 2
        w.beginFight(E_SOLDIER);
        w.ex.hp = 5;
        uint8_t st = w.fightTick(gs);                   // delay 2 -> 1, no swing yet
        CHECK(st == FIGHT_ONGOING && w.ex.hp == 5, "no enemy swing before its delay");
        st = w.fightTick(gs);                           // delay 1 -> 0 -> swing (rng=? hits)
        CHECK(st == FIGHT_LOST, "enemy swing (8) kills the 5-hp wanderer");
        CHECK(w.ex.dead && !w.ex.active, "player death routed through die()");
        CHECK(!w.cx.active, "combat ended on death");
        CHECK(w.ex.outfitRes[R_CURED_MEAT] == 0, "die() emptied the bag");
    }

    printf("== [fight] bolas stuns (no damage); stunned enemy skips its swing ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 15, VILLAGE_Y, T_FOREST, 1);  // man-eater, delay 1
        w.ex.outfitItem[I_BOLAS] = 1;
        w.beginFight(E_MAN_EATER);
        int before = w.cx.enemyHp;
        uint8_t st = w.fightAttack(gs, 0);            // bolas: rng=1 -> hit -> stun
        CHECK(st == FIGHT_ONGOING && w.cx.enemyHp == before, "bolas deals no HP damage");
        CHECK(w.cx.enemyStunLeft == FIGHT_STUN_S, "bolas stuns for 4s");
        CHECK(w.ex.outfitItem[I_BOLAS] == 0, "bolas consumed itself");
        int hp = w.ex.hp;
        w.fightTick(gs);                                // enemy delay hits 0 but stunned
        CHECK(w.ex.hp == hp, "stunned enemy skips its swing (no damage)");
    }

    printf("== [fight] flee ends combat, keeps the bag, expedition continues ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_FOREST, 1);
        w.ex.outfitRes[R_CURED_MEAT] = 3;
        w.beginFight(E_SNARLING_BEAST);
        w.fightFlee();
        CHECK(!w.cx.active, "flee ends combat");
        CHECK(w.ex.active, "expedition still live after a flee");
        CHECK(w.ex.outfitRes[R_CURED_MEAT] == 3, "flee forfeits no bag items");
    }

    printf("== [fight] move() surfaces STEP_FIGHT with a terrain-matched enemy ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 4, VILLAGE_Y, T_FOREST, 1);
        w.ex.outfitRes[R_CURED_MEAT] = 1000;          // meat + water can't run out
        w.ex.maxWater = 30000; w.ex.water = 30000;    // during the walk-to-fight
        w.ex.fightMove = FIGHT_DELAY;                 // next roll is eligible
        StepResult r{ STEP_MOVED, 0 };
        int guard = 0;
        while (r.kind != STEP_FIGHT && guard++ < 400) r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_FIGHT, "a forest walk eventually triggers a fight");
        CHECK(r.scene < ENCOUNTER_COUNT &&
              ENCOUNTERS[r.scene].terrain == T_FOREST,
              "STEP_FIGHT.scene carries a forest-terrain enemy");
    }

    // ======================= setpieces (milestone 2.4) ====================

    // Drive an armed setpiece fight to victory without the enemy ever swinging
    // (no fightTick), so the player can't die — a controlled kill for testing the
    // setpiece win -> resolveCombat handoff. Cooldown is zeroed each swing.
    auto simWin = [](WorldState& w, GameState& gs) {
        for (int guard = 0; guard < 400 && w.cx.active && !w.cx.won; guard++) {
            w.cx.weaponCool[0] = 0;
            w.fightAttack(gs, 0);
        }
    };

    printf("== [setpiece] table coverage: implemented vs Phase-3/prestige gaps ==\n");
    CHECK(setpieceExists(SP_OUTPOST) && setpieceExists(SP_IRONMINE) &&
          setpieceExists(SP_COALMINE) && setpieceExists(SP_SULPHURMINE) &&
          setpieceExists(SP_HOUSE) && setpieceExists(SP_CAVE) &&
          setpieceExists(SP_TOWN) && setpieceExists(SP_CITY) &&
          setpieceExists(SP_BOREHOLE) && setpieceExists(SP_BATTLEFIELD) &&
          setpieceExists(SP_SWAMP) && setpieceExists(SP_SHIP),
          "all Phase-2 setpieces have a table");
    CHECK(setpieceExists(SP_EXEC_INTRO) && setpieceExists(SP_EXEC_ANTE) &&
          setpieceExists(SP_EXEC_ENG) && setpieceExists(SP_EXEC_MAR) &&
          setpieceExists(SP_EXEC_MED) && setpieceExists(SP_EXEC_CMD),
          "all six Executioner setpieces have a table (Phase 3c-2)");
    CHECK(!setpieceExists(SP_NONE) && !setpieceExists(SP_CACHE),
          "SP_NONE / cache (prestige-only) still have no table");

    printf("== [setpiece] iron mine: torch gate -> fight -> clear -> goHome miner ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_FOREST, 12345);
        setpiece::bind(&w, &gs);
        // no torch -> the "go inside" button is unaffordable
        CHECK(setpiece::begin(SP_IRONMINE), "iron mine setpiece begins");
        CHECK(!setpiece::awaitingCombat(), "start scene is narrative, no fight yet");
        CHECK(!setpiece::btnAvailable(0), "go inside is locked without a torch");
        CHECK(setpiece::choose(0) == RC_ERR_COST, "go inside refused (no torch)");
        w.ex.outfitItem[I_TORCH] = 1;
        w.ex.outfitItem[I_BAYONET] = 1;                 // a weapon to win with
        CHECK(setpiece::btnAvailable(0), "go inside unlocks with a torch");
        CHECK(setpiece::choose(0) == RC_OK, "go inside accepted");
        CHECK(w.ex.outfitItem[I_TORCH] == 0, "the torch was spent");
        CHECK(setpiece::awaitingCombat(), "combat scene armed the fight");
        CHECK(w.fightActive() && w.combat().setpiece, "world combat is a setpiece fight");
        simWin(w, gs);
        CHECK(w.fightWon(), "the matriarch is dead");
        setpiece::resolveCombat(true);
        CHECK(!setpiece::awaitingCombat(), "victory resolved, buttons live");
        CHECK(setpiece::btnCount() == 1, "victory scene offers the leave->cleared button");
        CHECK(setpiece::choose(0) == RC_OK, "advance to cleared");
        CHECK(w.ex.clearedIron, "cleared scene set the iron-mine flag");
        CHECK(setpiece::choose(0) == RC_OK, "leave the cleared mine");
        CHECK(!setpiece::active(), "setpiece ended");
        // walk home -> the flag unlocks the village building + miner job
        w.ex.x = VILLAGE_X + 1; w.ex.y = VILLAGE_Y;
        StepResult r = w.move(gs, DIR_WEST);
        CHECK(r.kind == STEP_HOME, "reached the village");
        CHECK(gs.buildings[B_IRON_MINE] == 1, "iron mine building unlocked at goHome");
        uint8_t jobs[JOB_COUNT]; int nj = gs.unlockedJobs(jobs, JOB_COUNT);
        bool sawIron = false;
        for (int i = 0; i < nj; i++) if (jobs[i] == J_IRON_MINER) sawIron = true;
        CHECK(sawIron, "iron miner is now staffable");
    }

    printf("== [setpiece] coal mine: 3-stage fight chain then clear ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 10, VILLAGE_Y, T_FOREST, 222);
        w.ex.outfitItem[I_BAYONET] = 1;
        setpiece::bind(&w, &gs);
        setpiece::begin(SP_COALMINE);
        CHECK(setpiece::choose(0) == RC_OK, "attack -> stage 1");   // start.attack
        int stages = 0;
        for (int guard = 0; guard < 6 && setpiece::active(); guard++) {
            if (setpiece::awaitingCombat()) {
                simWin(w, gs); setpiece::resolveCombat(true); stages++;
                setpiece::choose(0);                    // continue
            } else break;                               // reached 'cleared' (narrative)
        }
        CHECK(stages == 3, "fought man, man, chief (3 stages)");
        CHECK(w.ex.clearedCoal, "coal mine cleared flag set");
        setpiece::choose(0);                            // leave cleared
        w.ex.x = VILLAGE_X + 1; w.ex.y = VILLAGE_Y;
        w.move(gs, DIR_WEST);
        CHECK(gs.buildings[B_COAL_MINE] == 1, "coal mine unlocked at goHome");
    }

    printf("== [setpiece] outpost refills water; borehole banks alien alloy ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 5, VILLAGE_Y, T_FOREST, 7);
        w.ex.water = 1; w.ex.maxWater = 30;
        setpiece::bind(&w, &gs);
        CHECK(setpiece::begin(SP_OUTPOST), "outpost begins");
        CHECK(w.ex.water == 30, "outpost refilled water to max (useOutpost)");
        CHECK(setpiece::lootCount() >= 1, "outpost banked cured meat");
        setpiece::choose(0);                            // leave
        CHECK(!setpiece::active(), "outpost left");

        plant(w, gs, VILLAGE_X + 20, VILLAGE_Y, T_FOREST, 9);
        setpiece::bind(&w, &gs);
        setpiece::begin(SP_BOREHOLE);
        CHECK(w.ex.outfitRes[R_ALIEN_ALLOY] >= 1, "borehole banked alien alloy into the bag");
        CHECK(setpiece::lootCount() == 1, "one loot line shown");
    }

    printf("== [setpiece] swamp charm -> gastronome perk (persists, x2 meat heal) ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 18, VILLAGE_Y, T_FOREST, 55);
        w.ex.outfitRes[R_CHARM] = 1;
        setpiece::bind(&w, &gs);
        setpiece::begin(SP_SWAMP);                      // start
        CHECK(setpiece::choose(0) == RC_OK, "enter -> cabin");
        CHECK(!setpiece::btnAvailable(0) == false, "talk affordable with a charm");
        CHECK(setpiece::choose(0) == RC_OK, "talk (spends the charm)");
        CHECK(w.ex.outfitRes[R_CHARM] == 0, "charm consumed");
        CHECK(gs.hasPerk(PK_GASTRONOME), "gastronome granted on the game state");
        CHECK(w.ex.gastronome, "expedition flagged gastronome (x2 this trip)");
        // x2 heal check: wounded, eat one meat -> +16 not +8
        w.beginFight(E_SNARLING_BEAST);
        w.ex.maxHp = 40; w.ex.hp = 1; w.ex.outfitRes[R_CURED_MEAT] = 2;
        w.fightEat();
        CHECK(w.ex.hp == 1 + MEAT_HEAL * 2, "gastronome doubles the meat heal (8 -> 16)");
        // persists across a new expedition
        GameState gs2; WorldState w2; w2.init(); w2.generateMap(1);
        gs2 = gs;                                       // same perks
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 3;
        gs2.stores[R_CURED_MEAT] = 3 * FP;
        w2.embark(gs2, out, nullptr, 1);
        CHECK(w2.ex.gastronome, "gastronome captured from the game state at embark");
    }

    printf("== [setpiece] leaving a start scene ends without side effects ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 8, VILLAGE_Y, T_FOREST, 3);
        int alloyBefore = w.ex.outfitRes[R_ALIEN_ALLOY];
        setpiece::bind(&w, &gs);
        setpiece::begin(SP_BATTLEFIELD);
        CHECK(setpiece::lootCount() >= 0, "battlefield start banks its war-tech loot");
        // battlefield banks on load; leaving just ends
        int btn = setpiece::btnCount() - 1;
        CHECK(setpiece::choose(btn) == RC_OK, "leave the battlefield");
        CHECK(!setpiece::active(), "setpiece ended on leave");
        (void)alloyBefore;
    }

    printf("== [setpiece] markVisited stops a landmark re-triggering (upstream 'H!') ==\n");
    {
        GameState gs; WorldState w;
        // A house 5 east of the walker; plain forest everywhere else.
        plant(w, gs, VILLAGE_X + 4, VILLAGE_Y, T_FOREST, 4242);
        const int hx = VILLAGE_X + 5, hy = VILLAGE_Y;
        w.ex.tiles[hy * WORLD_DIM + hx] = T_HOUSE;
        w.ex.outfitRes[R_CURED_MEAT] = 1000;              // supplies can't run out
        w.ex.maxWater = 30000; w.ex.water = 30000;
        w.ex.fightMove = -30000;                          // no random fight interferes

        // First arrival: the fresh house fires its setpiece (no upkeep this step).
        StepResult r = w.move(gs, DIR_EAST);              // step east onto the house
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_HOUSE,
              "fresh house fires its setpiece on first arrival");
        CHECK(!w.exVisited(hx, hy), "house not yet visited (no terminal scene has run)");

        // Drive the setpiece into a terminal outcome. All of medicine/supplies/
        // occupied markVisited on load (occupied before the fight is armed), so the
        // tile is spent regardless of which branch the roll picks.
        setpiece::bind(&w, &gs);
        CHECK(setpiece::begin(SP_HOUSE), "house setpiece begins");
        CHECK(!w.exVisited(hx, hy), "start scene does NOT mark visited (leave-and-return ok)");
        CHECK(setpiece::choose(0) == RC_OK, "go inside -> a terminal outcome");
        CHECK(w.exVisited(hx, hy), "a terminal house scene marks the tile visited");
        setpiece::end();

        // Re-step onto the now-visited house: upstream 'H!' misses LANDMARKS, so it
        // is plain terrain — supplies are consumed and NO setpiece fires.
        w.ex.x = hx - 1; w.ex.y = hy;                     // stand just west of it
        int waterBefore = w.ex.water;
        r = w.move(gs, DIR_EAST);                         // back onto the visited house
        CHECK(r.kind != STEP_LANDMARK, "a visited house no longer fires its setpiece");
        CHECK(w.ex.water == waterBefore - 1,
              "visited house is plain terrain: water upkeep ran (useSupplies)");
    }

    printf("== [setpiece] visited committed on goHome, discarded on die (World.state layering) ==\n");
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(0xDEAD01);
        const int hx = VILLAGE_X + 1, hy = VILLAGE_Y;     // house right next to home
        w.tiles[hy * WORLD_DIM + hx] = T_HOUSE;
        w.saveWorld();
        int16_t out[RES_COUNT] = { 0 };
        auto stock = [&]() { gs.stores[R_CURED_MEAT] = 5 * FP; out[R_CURED_MEAT] = 5; };
        auto visitHouse = [&]() {                         // drive house to a terminal scene
            setpiece::bind(&w, &gs);
            setpiece::begin(SP_HOUSE);
            setpiece::choose(0);                          // go inside -> markVisited outcome
            setpiece::end();
        };

        // Trip 1: mark the house visited, then DIE -> the mark is discarded.
        stock(); CHECK(w.embark(gs, out, nullptr, 1), "trip1 embark");
        StepResult r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_HOUSE, "trip1: fresh house triggers");
        visitHouse();
        CHECK(w.exVisited(hx, hy), "trip1: house visited on the working map");
        w.die();

        // Trip 2: die() dropped the mark -> the house triggers again; visit it and
        // walk HOME so goHome commits the mark into the committed layer.
        stock(); CHECK(w.embark(gs, out, nullptr, 1), "trip2 embark");
        r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_HOUSE,
              "trip2: house triggers again (die discarded the visited mark)");
        visitHouse();
        r = w.move(gs, DIR_WEST);                         // back onto the village
        CHECK(r.kind == STEP_HOME, "trip2: reached home -> goHome commits the visited mark");

        // Trip 3: the committed mark makes the house plain terrain across expeditions.
        stock(); CHECK(w.embark(gs, out, nullptr, 1), "trip3 embark");
        r = w.move(gs, DIR_EAST);
        CHECK(r.kind != STEP_LANDMARK,
              "trip3: committed-visited house no longer triggers (persisted like 'H!')");
    }

    // ======================= Phase 3a: the W landmark ======================
    // research-phase3.md §11 3a acceptance 1/2/7: salvage the crashed starship,
    // walk home ALIVE, and the ship page unlocks; die on the way back and it does
    // not — because clearedShip lives on the Expedition, which die() discards.
    printf("== [3a ship] salvage -> goHome unlocks the starship page ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 3, VILLAGE_Y, T_BARRENS, 4242);
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + (VILLAGE_X + 4)] = T_SHIP;
        setpiece::bind(&w, &gs);
        CHECK(!gs.shipUnlocked, "a fresh game has no starship page");

        StepResult r = w.move(gs, DIR_EAST);              // step onto the W
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_SHIP,
              "stepping onto W triggers the ship setpiece");
        CHECK(setpiece::begin(r.scene), "ship setpiece begins");
        CHECK(setpiece::btnCount() == 1, "one scene, one 「salvage」 button");
        CHECK(setpiece::choose(0) == RC_OK, "salvage");
        CHECK(!setpiece::active(), "salvage ends the setpiece (no combat, no cost)");
        CHECK(w.ex.clearedShip, "salvage recorded the ship on the EXPEDITION");
        CHECK(w.exVisited(VILLAGE_X + 4, VILLAGE_Y),
              "the W tile is spent (markVisited) so it can't re-trigger");
        CHECK(w.exTileAt(VILLAGE_X + 4, VILLAGE_Y) == T_SHIP,
              "the W tile itself survives (clearMine keeps the landmark glyph)");
        CHECK(w.exTileAt(VILLAGE_X + 2, VILLAGE_Y) == T_ROAD &&
              w.exTileAt(VILLAGE_X + 3, VILLAGE_Y) == T_ROAD,
              "salvage drew the road back toward the village");
        CHECK(!gs.shipUnlocked, "still locked mid-trip: goHome is what opens it");

        // Walk home. Two steps west, the second lands on the village.
        w.ex.x = VILLAGE_X + 1; w.ex.y = VILLAGE_Y;
        r = w.move(gs, DIR_WEST);
        CHECK(r.kind == STEP_HOME, "reached the village -> goHome");
        CHECK(gs.shipUnlocked, "goHome unlocked the starship page");
        CHECK(gs.shipHull == SHIP_BASE_HULL, "hull seeded to BASE_HULL (0 — can't fly yet)");
        CHECK(gs.shipThrusters == SHIP_BASE_THRUSTERS, "thrusters seeded to BASE_THRUSTERS (1)");
        {   // Ship.onArrival's one-shot notice landed in the log — compared
            // EXACTLY against the upstream key, because a key that got truncated
            // by LOG_KEY_MAX would still pass a substring test but would miss
            // strings_zh.h at tr() time and render as raw English on the panel.
            const char* key = "somewhere above the debris cloud, the wanderer "
                              "fleet hovers. been on this rock too long.";
            int hits = 0;
            for (int i = 0; i < gs.logCount; i++)
                if (strcmp(gs.log[i].enKey, key) == 0) hits++;
            CHECK(hits == 1, "the arrival notice was pushed once, key intact");
        }
        // A SECOND cleared trip must not reset a reinforced ship back to base.
        gs.stores[R_ALIEN_ALLOY] = 2 * FP;
        gs.reinforceHull(); gs.upgradeEngine();
        gs.unlockShip();
        CHECK(gs.shipHull == 1 && gs.shipThrusters == 2,
              "unlockShip is idempotent: a later trip can't reset the stats");

        // ...and the wreck stays spent across expeditions (goHome committed the
        // visited mark), so a second trip out there is a plain walk.
        gs.stores[R_CURED_MEAT] = 5 * FP;
        int16_t out2[RES_COUNT] = { 0 }; out2[R_CURED_MEAT] = 5;
        CHECK(w.embark(gs, out2, nullptr, 3), "re-embark after the successful trip");
        w.ex.x = VILLAGE_X + 3; w.ex.y = VILLAGE_Y;
        r = w.move(gs, DIR_EAST);                        // back onto the W
        CHECK(r.kind != STEP_LANDMARK, "stepping on the spent W does nothing");
    }

    printf("== [3a ship] salvage then DIE -> the page stays locked ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 3, VILLAGE_Y, T_BARRENS, 4243);
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + (VILLAGE_X + 4)] = T_SHIP;
        setpiece::bind(&w, &gs);
        StepResult r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_SHIP, "W triggers");
        setpiece::begin(r.scene);
        setpiece::choose(0);                              // salvage
        CHECK(w.ex.clearedShip, "salvaged");
        w.die();
        CHECK(!gs.shipUnlocked, "died before reaching home -> no starship page");
        // die() discarded the WORKING map, so the visited mark never reached the
        // committed layer either: next trip the wreck is there to salvage again.
        // (Checked through a re-embark, which is what re-seeds ex from committed.)
        gs.stores[R_CURED_MEAT] = 5 * FP;
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;
        CHECK(w.embark(gs, out, nullptr, 2), "re-embark after the death");
        CHECK(!w.exVisited(VILLAGE_X + 4, VILLAGE_Y),
              "the wreck is unspent again: a lost trip costs the salvage, not the map");
    }

    // ================= Phase 3c-2: the X landmark, both paths ==============
    // world.js doSpace:573-576 — the battleship is the ONE landmark that is never
    // markVisited'd: it opens the prologue until the prologue is cleared, then the
    // elevator hall, every single visit, until the command deck clears the tile.
    printf("== [3c-2 X] the battleship: prologue first, elevator hall after ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 3, VILLAGE_Y, T_BARRENS, 5150);
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + (VILLAGE_X + 4)] = T_EXECUTIONER;
        setpiece::bind(&w, &gs);
        CHECK(!gs.execEntered, "a fresh game has never boarded the battleship");

        StepResult r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_EXEC_INTRO,
              "the first visit routes to the prologue");
        CHECK(setpiece::begin(r.scene), "the prologue opens");
        CHECK(setpiece::btnCount() == 2, "enter / leave");
        CHECK(setpiece::choose(1) == RC_OK && !setpiece::active(),
              "`leave` closes it without entering");

        // Walking back onto the tile re-opens the SAME prologue: no markVisited.
        w.ex.x = VILLAGE_X + 3;
        r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_EXEC_INTRO,
              "leaving does not spend the tile — the prologue is offered again");

        // Clear the prologue by hand (its scene 7 effect) and step on it again.
        w.clearMine(w.ex.x, w.ex.y, T_EXECUTIONER);
        CHECK(w.ex.clearedExec, "the prologue's last scene records the boarding");
        w.ex.x = VILLAGE_X + 3;
        r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_EXEC_ANTE,
              "now the SAME tile opens the elevator hall instead");

        // Die on the way home: like the W wreck, the boarding is forfeit.
        w.die();
        CHECK(!gs.execEntered, "dying banks nothing — the prologue is due again");
        gs.stores[R_CURED_MEAT] = 5 * FP;
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;
        CHECK(w.embark(gs, out, nullptr, 7), "re-embark after the death");
        CHECK(!w.ex.clearedExec, "the new trip starts with the battleship sealed again");
    }

    printf("== [3c-2 X] walk home and the elevator hall is permanent ==\n");
    {
        GameState gs; WorldState w;
        plant(w, gs, VILLAGE_X + 3, VILLAGE_Y, T_BARRENS, 5151);
        w.ex.tiles[VILLAGE_Y * WORLD_DIM + (VILLAGE_X + 4)] = T_EXECUTIONER;
        setpiece::bind(&w, &gs);
        w.move(gs, DIR_EAST);
        w.clearMine(w.ex.x, w.ex.y, T_EXECUTIONER);        // the prologue's effect
        w.ex.wingMartial = true;                           // ...and one wing cleared
        w.ex.x = VILLAGE_X + 1; w.ex.y = VILLAGE_Y;
        StepResult r = w.move(gs, DIR_WEST);
        CHECK(r.kind == STEP_HOME, "reached the village -> goHome");
        CHECK(gs.execEntered && gs.wingMartial,
              "goHome banks the boarding AND the cleared wing");
        gs.stores[R_CURED_MEAT] = 5 * FP;
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;
        CHECK(w.embark(gs, out, nullptr, 8), "a later expedition sets out");
        CHECK(w.ex.clearedExec && w.ex.wingMartial,
              "and starts already knowing both (World.state seeded from the save)");
        w.ex.x = VILLAGE_X + 3; w.ex.y = VILLAGE_Y;
        r = w.move(gs, DIR_EAST);
        CHECK(r.kind == STEP_LANDMARK && r.scene == SP_EXEC_ANTE,
              "so the tile opens the elevator hall on a brand new trip");
        CHECK(setpiece::begin(r.scene) && !setpiece::btnAvailable(2),
              "with the martial band already greyed out");
        setpiece::end();
    }

    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
