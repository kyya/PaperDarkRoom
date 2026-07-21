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
// Build (clang++ is the available host toolchain on this box):
//   clang++ -std=c++17 -I src tools/adr_smoke.cpp src/game_state.cpp \
//           src/event_engine.cpp \
//           -DADR_SAVE_PATH='"adr_smoke_save.json"' -o adr_smoke.exe
#include "game_state.h"
#include "event_engine.h"
#include "events_data.h"
#include <cstdio>
#include <cstdint>
#include <cstring>

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

    // Bug repro: "陷阱捕获到XXX" on real hardware showed nothing after the
    // intro. Print the raw log tail and assert the catch itself (which
    // resource(s) got caught) actually made it into the log ring, not just
    // the bare "the traps contain " intro with no follow-up entry.
    printf("     log tail after checkTraps (raw en_key, newest last):\n");
    for (int i = 0; i < gs.logCount; i++)
        printf("       [%d] \"%s\"%s\n", i, gs.log[i].enKey,
               gs.log[i].hasArg ? " (has arg)" : "");
    bool sawIntro = false, sawCatch = false;
    for (int i = 0; i < gs.logCount; i++) {
        if (strcmp(gs.log[i].enKey, "the traps contain ") == 0) sawIntro = true;
        for (int j = 0; j < 6; j++)
            if (strcmp(gs.log[i].enKey, TRAP_DROPS[j].msg) == 0) sawCatch = true;
    }
    CHECK(sawIntro, "trap-check log has the \"the traps contain \" intro");
    CHECK(sawCatch, "trap-check log names what was actually caught (bug repro)");

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

    // =====================================================================
    // Random-event engine (v0.3.0). Deterministic branches: seed gs.rng, then
    // the first rand1000() draw of each flow is the branch/echo roll.
    //   rng=1 -> rand1000()==369  (< 500)
    //   rng=2 -> rand1000()==738  (in [500,800))
    // =====================================================================

    printf("== [events] The Nomad: full trade + goodbye ==\n");
    {
        GameState e; e.init(); uint32_t t = 2000; e.settle(t);
        e.stores[R_FUR] = 500 * FP;               // afford scales/teeth/bait
        events::bind(&e);
        CHECK(events::startEvent(EV_NOMAD, t), "Nomad activates (fur>0)");
        CHECK(events::active(), "event is active");
        CHECK(strcmp(events::eventTitleKey(), "The Nomad") == 0, "title == The Nomad");
        CHECK(events::btnCount() == 4, "start scene has 4 buttons");
        int furB = e.whole(R_FUR), scB = e.whole(R_SCALES);
        CHECK(events::choose(0) == RC_OK, "buy scales ok");
        CHECK(e.whole(R_FUR) == furB - 100 && e.whole(R_SCALES) == scB + 1,
              "buy scales: -100 fur, +1 scale");
        CHECK(events::active(), "stays open after a trade (SCENE_STAY)");
        int furC = e.whole(R_FUR);
        CHECK(events::choose(2) == RC_OK, "buy bait ok");
        CHECK(e.whole(R_FUR) == furC - 5 && e.whole(R_BAIT) == 1,
              "buy bait: -5 fur, +1 bait");
        CHECK(events::defaultBtnIndex() == 3, "default (safe-exit) btn is 'say goodbye' (3)");
        CHECK(events::choose(3) == RC_OK, "say goodbye ok");
        CHECK(!events::active(), "event ended after goodbye");
    }

    printf("== [events] reproducible probability branches ==\n");
    {
        // Noises Inside: rng=1 -> investigate roll 369 < 500 -> scales branch.
        GameState e; e.init(); uint32_t t = 3000; e.settle(t);
        e.stores[R_WOOD] = 1000 * FP;
        events::bind(&e);
        e.rng = 1;
        CHECK(events::startEvent(EV_NOISES_IN, t), "Noises(inside) activates (wood>0)");
        CHECK(events::choose(0) == RC_OK, "investigate");
        CHECK(e.whole(R_SCALES) > 0 && e.whole(R_TEETH) == 0 && e.whole(R_CLOTH) == 0,
              "rng=1 -> scales branch (reproducible)");
        CHECK(events::active() && events::currentScene() == S_NI_SCALES,
              "landed on the scales sub-scene");
        events::choose(0);                         // 'leave' -> end
        CHECK(!events::active(), "leave ends the event");
    }
    {
        // Beggar: rng=1 -> give50 roll 369 < 500 -> scales (+20).
        GameState e; e.init(); uint32_t t = 3100; e.settle(t);
        e.stores[R_FUR] = 500 * FP;
        events::bind(&e);
        e.rng = 1;
        CHECK(events::startEvent(EV_BEGGAR, t), "Beggar activates");
        CHECK(events::choose(0) == RC_OK, "give 50");
        CHECK(e.whole(R_SCALES) == 20 && e.whole(R_TEETH) == 0,
              "rng=1 -> Beggar scales +20");
    }
    {
        // Same event, rng=2 -> roll 738 in [500,800) -> teeth (+20): a DIFFERENT,
        // still-reproducible branch.
        GameState e; e.init(); uint32_t t = 3200; e.settle(t);
        e.stores[R_FUR] = 500 * FP;
        events::bind(&e);
        e.rng = 2;
        CHECK(events::startEvent(EV_BEGGAR, t), "Beggar activates (seed 2)");
        CHECK(events::choose(0) == RC_OK, "give 50 (seed 2)");
        CHECK(e.whole(R_TEETH) == 20 && e.whole(R_SCALES) == 0,
              "rng=2 -> Beggar teeth +20 (branch differs, reproducible)");
    }

    printf("== [events] cost gating: unaffordable buttons ==\n");
    {
        GameState e; e.init(); uint32_t t = 4000; e.settle(t);
        e.stores[R_FUR] = 50 * FP;                 // bait(5) ok; scales(100)/teeth(200) not
        events::bind(&e);
        CHECK(events::startEvent(EV_NOMAD, t), "Nomad activates (fur=50)");
        CHECK(!events::btnAvailable(0), "buy scales unaffordable (need 100)");
        CHECK(!events::btnAvailable(1), "buy teeth unaffordable (need 200)");
        CHECK(events::btnAvailable(2), "buy bait affordable (need 5)");
        CHECK(events::btnAvailable(3), "say goodbye always available (free)");
        CHECK(events::choose(0) == RC_ERR_COST, "choose unaffordable -> RC_ERR_COST");
        CHECK(events::active() && e.whole(R_FUR) == 50, "failed buy left state untouched");
        events::dismissDefault();
    }

    printf("== [events] dismissDefault: timeout safe exit ==\n");
    {
        GameState e; e.init(); uint32_t t = 5000; e.settle(t);
        e.stores[R_FUR] = 300 * FP;
        events::bind(&e);
        CHECK(events::startEvent(EV_NOMAD, t), "Nomad activates");
        int fur = e.whole(R_FUR);
        events::dismissDefault();
        CHECK(!events::active(), "dismissDefault ended the event");
        CHECK(e.whole(R_FUR) == fur, "dismissDefault is cost-free");
    }

    printf("== [events] Mysterious Wanderer: delayed echo pays off ==\n");
    {
        GameState e; e.init(); uint32_t t = 6000; e.settle(t);
        e.stores[R_WOOD] = 200 * FP;               // afford give 100
        events::bind(&e);
        e.rng = 1;                                 // echo roll 369 < 500 -> arms
        CHECK(events::startEvent(EV_WANDER_WOOD, t), "Wanderer(wood) activates");
        CHECK(events::choose(0) == RC_OK, "give 100 wood");
        CHECK(e.whole(R_WOOD) == 100, "wood 200 -> 100 after giving");
        CHECK(e.echoRes == R_WOOD && e.echoAmt == 300, "echo armed: +300 wood pending");
        CHECK(e.echoDueEpoch == t + 60, "echo due 60s later");
        CHECK(events::choose(0) == RC_OK, "say goodbye (Wanderer scene)");
        CHECK(!events::active(), "event ended");
        CHECK(!e.redeemDelayedEcho(t + 30), "not redeemed before due");
        CHECK(e.echoRes == R_WOOD, "echo still pending at +30s");
        int woodPre = e.whole(R_WOOD);
        e.settle(t + 120);                         // wake past the due time
        CHECK(e.echoRes == ECHO_NONE, "echo redeemed offline via settle()");
        CHECK(e.whole(R_WOOD) >= woodPre + 300, "wood +300 from the wanderer's return");
        bool sawReturn = false;
        for (int i = 0; i < e.logCount; i++)
            if (strcmp(e.log[i].enKey,
                       "the mysterious wanderer returns, cart piled high with wood.") == 0)
                sawReturn = true;
        CHECK(sawReturn, "wanderer return logged");
    }

    printf("== [events] save/load round-trip: event fields ==\n");
    {
        GameState e; e.init(); e.settle(7000);
        e.nextEventAt = 7777;
        e.armDelayedEcho(R_FUR, 1500, 8888);
        CHECK(e.save(), "save v2 ok");
        GameState el; el.init();
        CHECK(el.load(), "load v2 ok");
        CHECK(el.nextEventAt == 7777, "nextEventAt round-trips");
        CHECK(el.echoRes == R_FUR && el.echoAmt == 1500 && el.echoDueEpoch == 8888,
              "delayed-echo slot round-trips");
    }

    printf("== [events] v1 save migration: fields default, no data loss ==\n");
    {
        // A complete v1 JSON (no "nev"/"echo" keys) built with known values.
        char v1[4096]; size_t o = 0;
        o += (size_t)snprintf(v1 + o, sizeof v1 - o,
            "{\"v\":1,\"ts\":123,\"rng\":42,\"fire\":3,\"temp\":3,\"bl\":4,\"pop\":7,"
            "\"fl\":7,\"cd\":[0,0,0],\"tm\":[30,30,15,300,90],\"stores\":[");
        for (int i = 0; i < RES_COUNT; i++)
            o += (size_t)snprintf(v1 + o, sizeof v1 - o, "%s%d",
                                  i ? "," : "", i == R_WOOD ? 400 * FP : 0);
        o += (size_t)snprintf(v1 + o, sizeof v1 - o, "],\"bld\":[");
        for (int i = 0; i < BLD_COUNT; i++)
            o += (size_t)snprintf(v1 + o, sizeof v1 - o, "%s0", i ? "," : "");
        o += (size_t)snprintf(v1 + o, sizeof v1 - o, "],\"itm\":[");
        for (int i = 0; i < ITEM_COUNT; i++)
            o += (size_t)snprintf(v1 + o, sizeof v1 - o, "%s0", i ? "," : "");
        o += (size_t)snprintf(v1 + o, sizeof v1 - o, "],\"wrk\":[");
        for (int i = 0; i < JOB_COUNT; i++)
            o += (size_t)snprintf(v1 + o, sizeof v1 - o, "%s0", i ? "," : "");
        o += (size_t)snprintf(v1 + o, sizeof v1 - o, "],\"log\":[]}");

        GameState e; e.init();
        CHECK(e.fromJson(v1), "v1 save loads (fromJson accepts v==1)");
        CHECK(e.population == 7 && e.builderLevel == 4, "v1 core fields loaded");
        CHECK(e.whole(R_WOOD) == 400, "v1 stores loaded");
        CHECK(e.nextEventAt == 0, "v1 migration: nextEventAt defaults to 0 (reroll)");
        CHECK(e.echoRes == ECHO_NONE, "v1 migration: echo slot empty");
    }

    printf("== [events] scheduler: seed / wake-grace / awake-fire (§5.4) ==\n");
    {
        GameState e; e.init(); uint32_t t = 100000; e.settle(t);
        e.stores[R_FUR] = 500 * FP;                // Room fur events available
        events::bind(&e);
        // Fresh: nextEventAt==0 -> first tick seeds it 3-5 min out, fires nothing.
        events::tick(0, t);
        CHECK(events::nextEventAt() >= t + 180 && events::nextEventAt() <= t + 300,
              "fresh tick seeds next event 3-5 min out");
        CHECK(!events::active(), "no event on the seeding tick");

        // Wake past the due time (long gap = deep sleep): must NOT fire on the
        // waking instant; re-arms to now+60..120s.
        uint32_t w = events::nextEventAt() + 10000;
        events::tick(0, w);
        CHECK(!events::active(), "waking past due does not fire immediately");
        CHECK(events::nextEventAt() >= w + 60 && events::nextEventAt() <= w + 120,
              "wake re-arms to now+60..120s");

        // Awake continuation (1s ticks): when the re-armed time arrives, an
        // available event fires.
        uint32_t due = events::nextEventAt();
        e.rng = 1;
        for (uint32_t s = w + 1; s <= due + 1 && !events::active(); s++)
            events::tick(0, s);
        CHECK(events::active(), "an available event fires when due while awake");
        events::dismissDefault();
        CHECK(!events::active() && events::nextEventAt() > due,
              "dismiss ends it and reschedules the next");
    }

    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
