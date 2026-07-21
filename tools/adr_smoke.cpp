// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Host smoke test for the A Dark Room Phase-1 game state engine. Compiles the
// pure logic (src/game_state.cpp, no Arduino) with a native compiler and drives
// the canonical opening loop:
//   light fire -> builder chain -> unlock forest -> gather wood -> build trap
//   -> check traps -> build hut -> villagers arrive -> build lodge
//   -> assign hunter, plus an explicit break-on-shortage (断料停产) check and a
//   save/load JSON round-trip.
//
// Build (Windhawk clang++ is the available host toolchain on this box):
//   clang++ -std=c++17 -I src tools/adr_smoke.cpp src/game_state.cpp \
//           -DADR_SAVE_PATH='"adr_smoke_save.json"' -o adr_smoke.exe
#include "game_state.h"
#include <cstdio>
#include <cstdint>

using namespace adr;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else      { g_fail++; printf("  [FAIL] %s\n", msg); } } while (0)

// Advance the virtual clock and let the economy settle in lockstep — exactly
// what the awake tick / cold-boot wake do on device.
static void advance(GameState& gs, uint32_t& now, uint32_t secs) {
    now += secs;
    gs.settle(now);
}

int main() {
    GameState gs;
    gs.init();
    uint32_t now = 1'000'000;      // arbitrary epoch base
    gs.settle(now);                // seed lastSettleTs

    printf("== opening: dark room ==\n");
    CHECK(gs.fire == FIRE_DEAD && gs.temp == TEMP_FREEZING, "fresh game: fire dead, freezing");
    CHECK(gs.builderLevel == -1, "builder not yet present");
    CHECK(gs.whole(R_WOOD) == 0, "no wood at start");

    printf("== light the fire (free first light) ==\n");
    Result r = gs.lightFire(now);
    CHECK(r == RC_OK, "lightFire ok");
    CHECK(gs.fire == FIRE_BURNING, "fire -> burning");
    CHECK(gs.builderLevel == 0, "builder approaching (level 0)");

    printf("== settle 1h: builder chain + warm room ==\n");
    advance(gs, now, 3600);
    CHECK(gs.outsideUnlocked, "forest unlocked (stranger needed wood)");
    CHECK(gs.temp >= TEMP_WARM, "room warmed to >= warm");
    CHECK(gs.builderLevel == 4, "builder now helping (level 4)");
    CHECK(gs.craftablesUnlocked, "craftables unlocked");
    int woodAfterBuilder = gs.whole(R_WOOD);
    CHECK(woodAfterBuilder > 4, "builder passively stocked wood (+2/tick)");
    printf("     wood after builder hour = %d\n", woodAfterBuilder);

    printf("== gather wood ==\n");
    int before = gs.whole(R_WOOD);
    r = gs.gatherWood(now);
    CHECK(r == RC_OK, "gatherWood ok");
    CHECK(gs.whole(R_WOOD) == before + 10, "gather +10 wood (no cart)");
    // cooldown enforced
    CHECK(gs.gatherWood(now) == RC_ERR_COOLDOWN, "gather is on cooldown");

    printf("== build a trap ==\n");
    r = gs.build(C_TRAP);
    CHECK(r == RC_OK, "build trap ok");
    CHECK(gs.buildings[B_TRAP] == 1, "trap count == 1");
    // trap cost scales: 2nd trap now costs 20 wood
    int wpre = gs.whole(R_WOOD);
    gs.build(C_TRAP);
    CHECK(gs.buildings[B_TRAP] == 2 && wpre - gs.whole(R_WOOD) == 20,
          "2nd trap costs 20 wood (escalating)");

    printf("== check traps ==\n");
    int drops0 = gs.whole(R_FUR) + gs.whole(R_MEAT) + gs.whole(R_SCALES) +
                 gs.whole(R_TEETH) + gs.whole(R_CLOTH) + gs.whole(R_CHARM);
    r = gs.checkTraps(now);
    CHECK(r == RC_OK, "checkTraps ok");
    int drops1 = gs.whole(R_FUR) + gs.whole(R_MEAT) + gs.whole(R_SCALES) +
                 gs.whole(R_TEETH) + gs.whole(R_CLOTH) + gs.whole(R_CHARM);
    CHECK(drops1 > drops0, "traps yielded loot");
    printf("     trap loot delta = %d\n", drops1 - drops0);

    printf("== build a hut ==\n");
    // top up wood for the 100-wood hut + later lodge/traps via the builder over time
    advance(gs, now, 3600);
    r = gs.build(C_HUT);
    CHECK(r == RC_OK, "build hut ok");
    CHECK(gs.buildings[B_HUT] == 1, "hut count == 1");
    CHECK(gs.maxPopulation() == 4, "max population == 4 (1 hut)");

    printf("== villagers arrive (settle) ==\n");
    advance(gs, now, 1200);        // ~20 min: several pop-increase rolls
    CHECK(gs.population > 0, "population grew");
    CHECK(gs.numGatherers() == (int)gs.population, "all villagers are gatherers");
    printf("     population = %u\n", gs.population);

    printf("== build lodge, gather fur/meat via traps ==\n");
    // build to max traps for faster loot, then run several trap checks
    while (gs.buildings[B_TRAP] < 10 && gs.build(C_TRAP) == RC_OK) {}
    for (int i = 0; i < 30 && (gs.whole(R_FUR) < 10 || gs.whole(R_MEAT) < 5); i++) {
        advance(gs, now, 100);     // clear the 90s trap cooldown
        gs.checkTraps(now);
    }
    CHECK(gs.whole(R_FUR) >= 10 && gs.whole(R_MEAT) >= 5, "have fur>=10, meat>=5 for lodge");
    advance(gs, now, 1800);        // builder tops wood back up for the 200-wood lodge
    r = gs.build(C_LODGE);
    CHECK(r == RC_OK, "build lodge ok");
    CHECK(gs.buildings[B_LODGE] == 1, "lodge built");

    printf("== assign a hunter ==\n");
    CHECK(gs.assignWorker(J_HUNTER, 1) == RC_OK, "assign 1 hunter ok");
    CHECK(gs.workers[J_HUNTER] == 1, "hunter count == 1");
    CHECK(gs.numGatherers() == (int)gs.population - 1, "gatherers dropped by 1");
    // hunter income over an OFFLINE 2h settle: +0.5 fur & +0.5 meat / 10s tick
    int furB = gs.stores[R_FUR], meatB = gs.stores[R_MEAT];
    advance(gs, now, 7200);        // 2h offline
    CHECK(gs.stores[R_FUR] > furB, "hunter produced fur offline");
    CHECK(gs.stores[R_MEAT] > meatB, "hunter produced meat offline");

    printf("== break-on-shortage (断料停产) ==\n");
    // A trapper consumes meat(-1) to make bait(+1). With no meat, it must make
    // NOTHING (all-or-nothing per source), not go negative.
    GameState g2;
    g2.init();
    uint32_t t2 = 500;
    g2.settle(t2);
    g2.buildings[B_LODGE] = 1;             // unlock trapper
    g2.population = 1;
    g2.craftablesUnlocked = true;
    g2.assignWorker(J_TRAPPER, 1);
    g2.stores[R_MEAT] = 0;                 // no input
    int baitB = g2.stores[R_BAIT];
    t2 += 3600; g2.settle(t2);
    CHECK(g2.stores[R_BAIT] == baitB && g2.stores[R_MEAT] == 0,
          "no meat -> trapper idles, no bait, meat not negative");
    // now give it meat and confirm it DOES produce, consuming meat
    g2.stores[R_MEAT] = 50 * FP;
    int baitC = g2.stores[R_BAIT], meatC = g2.stores[R_MEAT];
    t2 += 300; g2.settle(t2);              // 30 ticks
    CHECK(g2.stores[R_BAIT] > baitC && g2.stores[R_MEAT] < meatC,
          "with meat -> trapper makes bait, consumes meat");

    printf("== 24h settle cap ==\n");
    GameState g3;
    g3.init();
    g3.settle(100);
    g3.buildings[B_HUT] = 20;              // room for lots of villagers
    uint32_t steps = g3.settle(100 + 48 * 3600);   // ask for 48h
    CHECK(steps == (uint32_t)(24 * 3600 / INCOME_TICK_S), "single settle capped at 24h (8640 steps)");

    printf("== save / load round-trip ==\n");
    CHECK(gs.save(), "save ok");
    GameState gl;
    gl.init();
    CHECK(gl.load(), "load ok");
    CHECK(gl.population == gs.population, "population restored");
    CHECK(gl.fire == gs.fire && gl.temp == gs.temp, "fire/temp restored");
    CHECK(gl.builderLevel == gs.builderLevel, "builder level restored");
    CHECK(gl.stores[R_FUR] == gs.stores[R_FUR], "fur (fixed-point) restored exactly");
    CHECK(gl.buildings[B_LODGE] == gs.buildings[B_LODGE], "buildings restored");
    CHECK(gl.workers[J_HUNTER] == gs.workers[J_HUNTER], "workers restored");
    CHECK(gl.logCount == gs.logCount && gl.logCount > 0, "log restored");

    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
