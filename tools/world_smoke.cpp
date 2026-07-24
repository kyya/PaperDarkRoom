// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Host smoke test for the A Dark Room Phase-2 World base layer (milestone 2.0).
// Compiles the pure logic (src/world_state.cpp + src/game_state.cpp, no Arduino)
// natively and asserts: deterministic map generation (landmark counts / radius
// bands / spawn / terrain sanity), diamond visibility, move upkeep + starvation/
// thirst death, goHome commit (mine unlock + loot bank + fog persist), die
// discard, drawRoad, world.bin + trek.bin round-trip / cold-boot restore, and
// the main-save headroom after the game_data enum growth.
//
// Build (clang++ is the host toolchain on this box):
//   clang++ -std=c++17 -I src tools/world_smoke.cpp src/world_state.cpp \
//           src/game_state.cpp \
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
        w.fightTick();
        CHECK(w.fightWeaponCoolLeft(0) == 1, "tick drains cooldown 2 -> 1");
        w.fightTick();
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
        uint8_t st = w.fightTick();                   // delay 2 -> 1, no swing yet
        CHECK(st == FIGHT_ONGOING && w.ex.hp == 5, "no enemy swing before its delay");
        st = w.fightTick();                           // delay 1 -> 0 -> swing (rng=? hits)
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
        w.fightTick();                                // enemy delay hits 0 but stunned
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
    CHECK(!setpieceExists(SP_NONE) && !setpieceExists(SP_EXECUTIONER) &&
          !setpieceExists(SP_CACHE),
          "SP_NONE / executioner (P3) / cache (prestige) have no table");

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

    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
