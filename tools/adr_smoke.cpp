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

// The two idle-crew notice keys (game_state applyIncomeSource).
static const char* const K_IDLE = "{0} short of {1}, {2} idle";
static const char* const K_BACK = "{0} back to work";

// How many entries in the log ring carry `key`, and the newest one. The
// anti-flood checks need the COUNT (a sighting says nothing about repeats).
static int countLog(const GameState& gs, const char* key) {
    int n = 0;
    for (int i = 0; i < gs.logCount; i++)
        if (strcmp(gs.log[i].enKey, key) == 0) n++;
    return n;
}
static const LogEntry* findLog(const GameState& gs, const char* key) {
    for (int i = gs.logCount - 1; i >= 0; i--)
        if (strcmp(gs.log[i].enKey, key) == 0) return &gs.log[i];
    return nullptr;
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
    // v0.3.1 feedback 1: the fire/temp state used to be a persistent header
    // line; now it only reaches the user as a log notification on change
    // (room.js onFireChange parity). Check right here, before the log ring
    // (LOG_CAP) evicts this entry under the builder-chain's later pushes.
    {
        bool sawFireLog = false;
        for (int i = 0; i < gs.logCount; i++)
            if (strcmp(gs.log[i].enKey, "the fire is {0}") == 0 &&
                gs.log[i].hasArg && gs.log[i].arg == FIRE_BURNING)
                sawFireLog = true;
        CHECK(sawFireLog, "onFireChange pushes \"the fire is {0}\" (fire=burning)");
    }

    printf("== settle 1h: builder chain + warm room ==\n");
    advance(gs, now, 3600);
    CHECK(gs.outsideUnlocked, "forest unlocked (stranger needed wood)");
    // v0.4.3 fix 1: the fire now cools while AWAKE (it used to be frozen). Over
    // this passive 1h settle (no manual stoking) a burning fire is auto-stoked
    // down to flickering by the Helping builder, so the room settles to mild.
    // It DID pass through warm early on — that transient warmth is exactly what
    // advanced the builder up the 1->2->3 chain to Helping (asserted below).
    CHECK(gs.temp >= TEMP_MILD, "room settled to >= mild (awake fire cooled to flickering)");
    CHECK(gs.builderLevel == 4, "builder now helping (level 4)");
    CHECK(gs.craftablesUnlocked, "craftables unlocked");
    // v0.3.1 feedback 1: adjustTemp() notifies on every temp change (room.js
    // parity). v0.4.3: the 1h-settled ring above is now dominated by the awake
    // fire-cool churn ("builder stokes the fire" / "the fire is {0}"), which
    // evicts the early temp lines — so verify the temp notification on a short
    // fresh settle where it still sits in the ring.
    {
        GameState gt; gt.init(); uint32_t tt = 111000; gt.settle(tt);
        gt.lightFire(tt);                          // fire -> burning (free first light)
        for (int k = 0; k < 6; k++) { tt += 30; gt.settle(tt); }   // temp drifts up
        bool sawTempLog = false;
        for (int i = 0; i < gt.logCount; i++)
            if (strcmp(gt.log[i].enKey, "the room is {0}") == 0 && gt.log[i].hasArg)
                sawTempLog = true;
        CHECK(sawTempLog, "adjustTemp pushes \"the room is {0}\" on change");
    }
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
    // intro, because the intro and each catch were separate log entries and the
    // newest-on-top ring floated the catches ABOVE their own intro. checkTraps
    // now emits ONE compound entry (intro key + each drop key, LOG_KEY_SEP
    // joined), so assert the whole catch rides on the newest entry.
    printf("     log tail after checkTraps (raw en_key, newest last):\n");
    for (int i = 0; i < gs.logCount; i++)
        printf("       [%d] \"%s\"%s\n", i, gs.log[i].enKey,
               gs.log[i].hasArg ? " (has arg)" : "");
    const LogEntry& trapEntry = gs.log[gs.logCount - 1];
    bool sawIntro = strncmp(trapEntry.enKey, "the traps contain |",
                            strlen("the traps contain |")) == 0;
    bool sawCatch = false, strayCatch = false;
    for (int j = 0; j < 6; j++)
        if (strstr(trapEntry.enKey, TRAP_DROPS[j].msg)) sawCatch = true;
    for (int i = 0; i < gs.logCount - 1; i++)
        for (int j = 0; j < 6; j++)
            if (strcmp(gs.log[i].enKey, TRAP_DROPS[j].msg) == 0) strayCatch = true;
    CHECK(sawIntro, "trap-check log is one entry opening with the intro key");
    CHECK(sawCatch, "trap-check log names what was actually caught (bug repro)");
    CHECK(!strayCatch, "no catch spills into its own log entry above the intro");

    printf("== trap catch of several types folds into ONE compound entry ==\n");
    {
        // Ten traps + bait give 20 drop rolls, so every type is near-certain to
        // land; assert the round costs exactly one log line and that the line
        // lists every type actually credited this round.
        GameState gt; gt.init();
        gt.settle(1000);
        gt.buildings[B_TRAP] = 10;
        gt.stores[R_BAIT]    = 10 * FP;
        int before[6];
        for (int j = 0; j < 6; j++) before[j] = gt.whole(TRAP_DROPS[j].res);
        int logBefore = gt.logCount;
        CHECK(gt.checkTraps(2000) == RC_OK, "10 baited traps check ok");
        CHECK(gt.logCount == logBefore + 1, "the whole catch is a single log entry");
        const char* k = gt.log[gt.logCount - 1].enKey;
        printf("     compound key = \"%s\" (%zu/%d bytes)\n", k, strlen(k), LOG_KEY_MAX);
        int types = 0, listed = 0;
        for (int j = 0; j < 6; j++) {
            if (gt.whole(TRAP_DROPS[j].res) <= before[j]) continue;
            types++;
            if (strstr(k, TRAP_DROPS[j].msg)) listed++;
        }
        printf("     types caught = %d, named in the key = %d\n", types, listed);
        CHECK(types >= 3, "a 20-roll round catches several distinct types");
        // Every caught type is named unless the 96-byte key ran out of room,
        // which only the all-six round can do (18 + 6 keys + 6 joiners = 112).
        CHECK(listed == types || types == 6,
              "the one entry names every type caught (all-six may drop its tail)");
        CHECK(strlen(k) < LOG_KEY_MAX, "the compound key stays inside LOG_KEY_MAX");
        // Round-trips through the hand-rolled JSON codec unescaped.
        CHECK(gt.save(), "save() accepts the compound key");
        GameState gl; gl.init();
        CHECK(gl.load(), "load() reads the save back");
        CHECK(strcmp(gl.log[gl.logCount - 1].enKey, k) == 0,
              "compound key survives the save/load round-trip byte for byte");
    }

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

    printf("== [v0.14] stepper ±10 truncates to what is available ==\n");
    // The AssignPage stepper's coarse column passes ±stepper::MANY (10) straight
    // to assignWorker, relying on it to spend min(delta, available) exactly as
    // upstream does (outside.js:376 `Math.min(available, btn.data)`). These cases
    // pin that contract down at the engine, which is where the truncation lives —
    // the page itself only decides the number.
    {
        GameState g3;
        g3.init();
        uint32_t t3 = 500;
        g3.settle(t3);
        g3.buildings[B_LODGE] = 1;              // unlock hunter/trapper
        g3.population = 25;
        g3.workers[J_HUNTER] = 0;

        // (a) plenty idle -> a ±10 tap moves the full 10
        CHECK(g3.assignWorker(J_HUNTER, 10) == RC_OK, "+10 hunters accepted");
        CHECK(g3.workers[J_HUNTER] == 10, "+10 with 25 idle assigns all 10");

        // (b) fewer idle than asked -> TRUNCATE, do not refuse. 25 pop, 10 already
        // hunting, 12 parked in another job leaves 3 idle; +10 must move exactly 3.
        g3.workers[J_TRAPPER] = 12;
        CHECK(g3.numGatherers() == 3, "3 villagers idle");
        CHECK(g3.assignWorker(J_HUNTER, 10) == RC_OK, "+10 with only 3 idle accepted");
        CHECK(g3.workers[J_HUNTER] == 13, "+10 with 3 idle assigns exactly 3");
        CHECK(g3.numGatherers() == 0, "no idle villagers left");

        // (c) nothing idle at all -> no move, and the page's before/after compare
        // (which is what low-beeps) sees no change.
        int beforeNone = (int)g3.workers[J_HUNTER];
        g3.assignWorker(J_HUNTER, 10);
        CHECK((int)g3.workers[J_HUNTER] == beforeNone, "+10 with 0 idle moves nobody");

        // (d) removing truncates at the job's own count, never below zero.
        CHECK(g3.assignWorker(J_TRAPPER, -10) == RC_OK, "-10 trappers accepted");
        CHECK(g3.workers[J_TRAPPER] == 2, "-10 from 12 leaves 2");
        CHECK(g3.assignWorker(J_TRAPPER, -10) == RC_OK, "-10 again accepted");
        CHECK(g3.workers[J_TRAPPER] == 0, "-10 from 2 truncates to 0, not negative");
        int beforeZero = (int)g3.workers[J_TRAPPER];
        g3.assignWorker(J_TRAPPER, -10);
        CHECK((int)g3.workers[J_TRAPPER] == beforeZero, "-10 at 0 moves nobody");
    }

    printf("== [v0.10.2] unlockedJobs() covers all 9 P2 jobs, not just the first 6 ==\n");
    // AssignPage used to feed a MAX_JOBS=6 cap straight into unlockedJobs(),
    // truncating the query itself once P2's 3 miner/steelworker/armourer jobs
    // pushed the real total to 9 — steelworker (8th in enum order) and armourer
    // (9th) fell off silently, even though the Outside page kept showing them
    // (its buffer was always JOB_COUNT-sized). This pins the ENGINE contract a
    // sufficiently-sized caller now relies on: with every non-gatherer job's
    // building standing, unlockedJobs() must return all 9, in enum order, with
    // NONE dropped — regardless of how many the caller's buffer can hold.
    {
        GameState g5; g5.init();
        g5.buildings[B_LODGE]      = 1;   // hunter, trapper
        g5.buildings[B_TANNERY]    = 1;   // tanner
        g5.buildings[B_SMOKEHOUSE] = 1;   // charcutier
        g5.buildings[B_IRON_MINE]    = 1; // iron miner
        g5.buildings[B_COAL_MINE]    = 1; // coal miner
        g5.buildings[B_SULPHUR_MINE] = 1; // sulphur miner
        g5.buildings[B_STEELWORKS] = 1;   // steelworker
        g5.buildings[B_ARMOURY]    = 1;   // armourer

        uint8_t jobs[JOB_COUNT];
        int n = g5.unlockedJobs(jobs, (int)sizeof(jobs));
        CHECK(n == 9, "all 9 non-gatherer jobs unlocked");
        bool sawSteelworker = false, sawArmourer = false;
        for (int i = 0; i < n; i++) {
            if (jobs[i] == J_STEELWORKER) sawSteelworker = true;
            if (jobs[i] == J_ARMOURER)    sawArmourer    = true;
        }
        CHECK(sawSteelworker, "steelworker present (8th in enum order)");
        CHECK(sawArmourer,    "armourer present (9th in enum order)");
        CHECK(g5.hasUnlockedJob(), "hasUnlockedJob true with all 9 unlocked");

        // A caller with a smaller page-sized buffer (AssignPage's per-page
        // MAX_BANDS=8) must still see the query itself return the true total —
        // pagination is the caller's job now, not a silent engine-level drop.
        uint8_t page[8];
        int n8 = g5.unlockedJobs(page, 8);
        CHECK(n8 == 8, "a capped 8-slot caller still gets 8, not silently fewer");
    }

    printf("== break-on-shortage (断料停产) ==\n");
    // A trapper consumes meat(-1) to make bait(+1). With NO meat at all the crew
    // that can be fed is empty, so it must make NOTHING, not go negative. (The
    // partial-crew rule below covers the "some but not enough" middle ground.)
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

    printf("== partial-crew income (材料只够部分工人) ==\n");
    // Shortage truncates the CREW, it does not zero the whole source. 4 iron
    // miners each eat 1 cured meat to make 1 iron; with only 3 cured meat
    // banked, exactly 3 of them work this tick. Regression guard: the old
    // all-or-nothing rule paid 0 iron here, so adding a 4th miner to a working
    // 3-miner mine dropped output from 3/tick to 0/tick.
    {
        GameState g5; g5.init(); uint32_t t5 = 700; g5.settle(t5);
        g5.buildings[B_IRON_MINE] = 1;         // unlock iron miner
        g5.population = 4;
        g5.craftablesUnlocked = true;
        CHECK(g5.assignWorker(J_IRON_MINER, 4) == RC_OK, "4 iron miners assigned");
        CHECK(g5.numGatherers() == 0, "no gatherers left to muddy the ledger");
        g5.stores[R_CURED_MEAT] = 3 * FP;      // feeds only 3 of the 4 miners
        g5.stores[R_IRON]       = 0;
        t5 += INCOME_TICK_S; g5.settle(t5);    // exactly one income tick
        CHECK(g5.stores[R_IRON] == 3 * FP,
              "4 miners + 3 cured meat -> exactly 3 iron (partial crew, not 0)");
        CHECK(g5.stores[R_CURED_MEAT] == 0,
              "4 miners + 3 cured meat -> exactly 3 consumed, none stranded");
    }
    // Multi-input truncation: a charcutier burns 5 meat AND 5 wood per head, so
    // the SCARCEST input sets the crew size. Meat feeds 6, wood feeds 3 -> 3.
    {
        GameState g5b; g5b.init(); uint32_t t5b = 800; g5b.settle(t5b);
        g5b.buildings[B_SMOKEHOUSE] = 1;       // unlock charcutier
        g5b.population = 4;
        g5b.craftablesUnlocked = true;
        CHECK(g5b.assignWorker(J_CHARCUTIER, 4) == RC_OK, "4 charcutiers assigned");
        g5b.stores[R_MEAT]       = 30 * FP;    // feeds 6
        g5b.stores[R_WOOD]       = 15 * FP;    // feeds 3  <- the binding input
        g5b.stores[R_CURED_MEAT] = 0;
        t5b += INCOME_TICK_S; g5b.settle(t5b);
        CHECK(g5b.stores[R_CURED_MEAT] == 3 * FP,
              "multi-input: scarcest input (wood, 3) caps the crew at 3");
        CHECK(g5b.stores[R_MEAT] == 15 * FP && g5b.stores[R_WOOD] == 0,
              "multi-input: 3 workers' worth of BOTH inputs consumed");
    }

    printf("== idle-worker log (缺料停工提示) ==\n");
    // Passive income was the one system that never wrote a log line, so a player
    // who staffed a job past its supply just saw the extra workers produce
    // nothing, with no hint of the bottleneck. It now reports WHICH job idled,
    // on WHICH missing input, and HOW MANY — once per transition, never per tick.
    {
        GameState g7; g7.init(); uint32_t t7 = 900; g7.settle(t7);
        g7.buildings[B_IRON_MINE] = 1;         // unlock iron miner
        g7.population = 4;
        g7.craftablesUnlocked = true;
        CHECK(g7.assignWorker(J_IRON_MINER, 4) == RC_OK, "4 iron miners assigned");
        g7.stores[R_CURED_MEAT] = 3 * FP;      // feeds only 3 of the 4
        int before = g7.logCount;
        t7 += INCOME_TICK_S; g7.settle(t7);
        CHECK(g7.logCount == before + 1 && countLog(g7, K_IDLE) == 1,
              "a starved crew pushes exactly one idle notice");
        const LogEntry* e7 = findLog(g7, K_IDLE);
        CHECK(e7 && e7->hasArg && idleArgJob(e7->arg) == J_IRON_MINER,
              "idle notice names the job (iron miner)");
        CHECK(e7 && idleArgRes(e7->arg) == R_CURED_MEAT,
              "idle notice names the input that ran short (cured meat)");
        CHECK(e7 && idleArgIdle(e7->arg) == 1, "idle notice carries the headcount (1)");

        // The anti-flood core: the shortage now DEEPENS (the store emptied, so
        // all 4 idle) and 20 more 10s ticks run. Not one further line — a moving
        // idle headcount is deliberately silent while the crew stays short, and
        // it must not sneak in as a collapsed "xN" repeat counter either.
        for (int i = 0; i < 20; i++) { t7 += INCOME_TICK_S; g7.settle(t7); }
        CHECK(g7.logCount == before + 1 && countLog(g7, K_IDLE) == 1,
              "20 more starved ticks add no further idle notice");
        e7 = findLog(g7, K_IDLE);
        CHECK(e7 && e7->count == 1,
              "a sustained shortage does not tick the repeat counter either");

        // Restock the whole crew -> exactly one recovery line, then silence.
        g7.stores[R_CURED_MEAT] = 100 * FP;
        t7 += INCOME_TICK_S; g7.settle(t7);
        CHECK(countLog(g7, K_BACK) == 1, "supply restored -> one back-to-work notice");
        const LogEntry* b7 = findLog(g7, K_BACK);
        CHECK(b7 && b7->hasArg && idleArgJob(b7->arg) == J_IRON_MINER,
              "back-to-work notice names the job");
        for (int i = 0; i < 5; i++) { t7 += INCOME_TICK_S; g7.settle(t7); }
        CHECK(countLog(g7, K_BACK) == 1, "a fed crew stays silent afterwards");

        // A FRESH shortage re-arms the notice (the latch cleared on recovery).
        g7.stores[R_CURED_MEAT] = 0;
        t7 += INCOME_TICK_S; g7.settle(t7);
        CHECK(countLog(g7, K_IDLE) == 2, "a new shortage is announced again");
        e7 = findLog(g7, K_IDLE);
        CHECK(e7 && idleArgIdle(e7->arg) == 4,
              "the re-announcement carries the CURRENT headcount (all 4)");
    }
    // Offline catch-up stays silent: settle() replays up to 8640 ticks on a
    // cold-boot wake, and even one line per transition per job is noise the
    // player never lived through. The latch is left untouched offline, so the
    // first AWAKE tick still announces a shortage that is still standing.
    {
        GameState g8; g8.init(); uint32_t t8 = 2000; g8.settle(t8);
        g8.buildings[B_IRON_MINE] = 1;
        g8.population = 4;
        g8.craftablesUnlocked = true;
        CHECK(g8.assignWorker(J_IRON_MINER, 4) == RC_OK,
              "4 iron miners assigned (offline case)");
        g8.stores[R_CURED_MEAT] = 0;           // starved from the very first tick
        t8 += SETTLE_MAX_S;                    // a full day asleep
        uint32_t steps = g8.settle(t8, /*offline=*/true);
        CHECK(steps == (uint32_t)(SETTLE_MAX_S / INCOME_TICK_S),
              "offline settle ran the full 8640 ticks");
        CHECK(g8.logCount == 0, "offline catch-up writes no idle notices at all");
        t8 += INCOME_TICK_S; g8.settle(t8);    // first awake tick after the wake
        CHECK(countLog(g8, K_IDLE) == 1,
              "the first awake tick after a wake announces it once");
    }

    printf("== [feedback 2] cost-insufficient build/craft pushes \"not enough X\" ==\n");
    // room.js build()/craft(): notify "not enough " + the first short cost key.
    // v0.3.1 feedback 2: a disabled build/craft band used to fail silently.
    {
        GameState g4; g4.init(); uint32_t t4 = 900; g4.settle(t4);
        g4.temp = TEMP_MILD;               // not cold, so RC_ERR_COLD can't preempt
        g4.craftablesUnlocked = true;
        g4.stores[R_WOOD] = 0;             // trap costs 10 wood -> force insufficiency
        Result rr = g4.build(C_TRAP);
        CHECK(rr == RC_ERR_COST, "build trap fails on insufficient wood");
        bool sawNotEnoughWood = false;
        for (int i = 0; i < g4.logCount; i++)
            if (strcmp(g4.log[i].enKey, "not enough wood") == 0) sawNotEnoughWood = true;
        CHECK(sawNotEnoughWood, "cost failure pushes \"not enough wood\"");
    }

    printf("== [v0.3.2] trading post: buy() ==\n");
    // room.js buy(): gated on the trading post standing (RC_ERR_LOCKED before),
    // then the same "not enough X" cost-fail logging as build/craft, then a
    // successful buy grants exactly 1 whole unit of the good with no success
    // notification (upstream buy() has none — good.buildMsg is undefined on
    // Room.TradeGoods entries, so Notifications.notify() no-ops; see
    // game_state.cpp GameState::buy()).
    {
        GameState g6; g6.init(); uint32_t t6 = 970; g6.settle(t6);
        g6.stores[R_FUR] = 1000 * FP;       // affords every P1 trade good
        CHECK(g6.buy(T_SCALES).status == RC_ERR_LOCKED,
              "buy before trading post -> RC_ERR_LOCKED");

        g6.buildings[B_TRADING_POST] = 1;   // trading post now stands
        int scalesBefore = g6.whole(R_SCALES);
        int furBefore    = g6.whole(R_FUR);
        BuyResult rb = g6.buy(T_SCALES);        // costs 150 fur (game_data.h TRADE)
        CHECK(rb.status == RC_OK && rb.purchased == 1, "buy scales ok once trading post stands");
        CHECK(g6.whole(R_SCALES) == scalesBefore + 1, "buy scales: +1 scales");
        CHECK(g6.whole(R_FUR) == furBefore - 150, "buy scales: -150 fur");

        g6.stores[R_FUR] = 0;                // force insufficiency
        BuyResult rc = g6.buy(T_SCALES);
        CHECK(rc.status == RC_ERR_COST, "buy scales fails on insufficient fur");
        bool sawNotEnoughFur = false;
        for (int i = 0; i < g6.logCount; i++)
            if (strcmp(g6.log[i].enKey, "not enough fur") == 0) sawNotEnoughFur = true;
        CHECK(sawNotEnoughFur, "buy cost failure pushes \"not enough fur\"");
    }

    printf("== [feedback 3] duplicate log lines collapse into count ==\n");
    // Repeatedly long-pressing a cost-disabled band (e.g. the same "not
    // enough wood") used to scroll a fresh duplicate line every time;
    // pushLog() now collapses a repeat of the newest entry into its count.
    {
        GameState g5; g5.init(); uint32_t t5 = 950; g5.settle(t5);
        int before = g5.logCount;
        g5.pushLog("not enough wood");
        int afterFirst = g5.logCount;
        CHECK(g5.log[afterFirst - 1].count == 1, "fresh entry starts at count 1");
        g5.pushLog("not enough wood");           // identical repeat
        CHECK(g5.logCount == afterFirst, "repeat of newest entry does not grow logCount");
        CHECK(g5.log[g5.logCount - 1].count == 2, "repeat collapses into count=2");
        g5.pushLog("dry brush and dead branches litter the forest floor");  // different key
        CHECK(g5.logCount == afterFirst + 1, "a different key pushes a new entry");
        CHECK(g5.log[g5.logCount - 1].count == 1, "new entry starts fresh at count 1");
        CHECK(g5.log[g5.logCount - 2].count == 2, "the collapsed entry keeps its count");
        CHECK(g5.logCount == before + 2, "net: 2 distinct entries logged, not 3");

        // save/load round-trip: count must survive the JSON write/read.
        CHECK(g5.save(), "save with a collapsed log entry ok");
        GameState g5l; g5l.init();
        CHECK(g5l.load(), "load ok");
        CHECK(g5l.logCount == g5.logCount, "log length round-trips");
        CHECK(g5l.log[g5l.logCount - 2].count == 2,
              "collapsed entry's count round-trips through save/load");
    }

    printf("== [v0.3.3] fmtAmount: compact quantity abbreviation ==\n");
    // game_data.h fmtAmount — pure, host-compilable, drives the Outside inventory
    // box and the Trade balance row. v<1000 verbatim; 1e3..1e4 -> "1.2K"
    // (TRUNCATED tenths); 1e4..1e6 -> integer K; 1e6.. -> M.
    {
        char b[8];
        fmtAmount(999, b, sizeof(b));     CHECK(strcmp(b, "999") == 0,  "999 -> \"999\"");
        fmtAmount(1000, b, sizeof(b));    CHECK(strcmp(b, "1.0K") == 0, "1000 -> \"1.0K\"");
        fmtAmount(1234, b, sizeof(b));    CHECK(strcmp(b, "1.2K") == 0, "1234 -> \"1.2K\" (truncated)");
        fmtAmount(1999, b, sizeof(b));    CHECK(strcmp(b, "1.9K") == 0, "1999 -> \"1.9K\" (truncated, not 2.0K)");
        fmtAmount(56789, b, sizeof(b));   CHECK(strcmp(b, "56K") == 0,  "56789 -> \"56K\"");
        fmtAmount(999999, b, sizeof(b));  CHECK(strcmp(b, "999K") == 0, "999999 -> \"999K\"");
        fmtAmount(1234567, b, sizeof(b)); CHECK(strcmp(b, "1.2M") == 0, "1234567 -> \"1.2M\"");
        fmtAmount(12000000, b, sizeof(b));CHECK(strcmp(b, "12M") == 0,  "12000000 -> \"12M\"");
    }

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
        events::tick(0, t, false);
        CHECK(events::nextEventAt() >= t + 180 && events::nextEventAt() <= t + 300,
              "fresh tick seeds next event 3-5 min out");
        CHECK(!events::active(), "no event on the seeding tick");

        // Wake past the due time (long gap = deep sleep): must NOT fire on the
        // waking instant; re-arms to now+60..120s.
        uint32_t w = events::nextEventAt() + 10000;
        events::tick(0, w, false);
        CHECK(!events::active(), "waking past due does not fire immediately");
        CHECK(events::nextEventAt() >= w + 60 && events::nextEventAt() <= w + 120,
              "wake re-arms to now+60..120s");

        // Awake continuation (1s ticks): when the re-armed time arrives, an
        // available event fires.
        uint32_t due = events::nextEventAt();
        e.rng = 1;
        for (uint32_t s = w + 1; s <= due + 1 && !events::active(); s++)
            events::tick(0, s, false);
        CHECK(events::active(), "an available event fires when due while awake");
        events::dismissDefault();
        CHECK(!events::active() && events::nextEventAt() > due,
              "dismiss ends it and reschedules the next");
    }

    // =====================================================================
    // v0.4.3 fix 1 — awake fire cooling (room.js coolFire) vs offline freeze.
    // Fire cools by real 5-min steps ONLY while awake (settle offline=false);
    // an offline catch-up (offline=true) leaves fire AND its tFireCool timer
    // frozen (research.md §5.3). FIRE_COOL_S = 300s = 30 income ticks.
    // =====================================================================
    printf("== [v0.4.3] awake fire cooling: Roaring -> Burning in 5 min ==\n");
    {
        GameState fc; fc.init(); uint32_t tf = 200000; fc.settle(tf);
        fc.fire = FIRE_ROARING; fc.temp = TEMP_HOT;
        fc.builderLevel = 4;                 // Helping; roaring is above the auto-stoke line
        fc.stores[R_WOOD] = 0;
        fc.tFireCool = FIRE_COOL_S;
        advance(fc, tf, 300);                // 5 min AWAKE (offline=false)
        CHECK(fc.fire == FIRE_BURNING, "awake 5min: Roaring -> Burning (one level)");
    }

    printf("== [v0.4.3] offline freeze: fire + cool timer untouched ==\n");
    {
        GameState fo; fo.init(); uint32_t to = 210000; fo.settle(to);
        fo.fire = FIRE_ROARING; fo.temp = TEMP_HOT;
        fo.builderLevel = -1;
        fo.tFireCool = FIRE_COOL_S;
        fo.settle(to + 3600, /*offline=*/true);   // 1h deep-sleep catch-up
        CHECK(fo.fire == FIRE_ROARING, "offline 1h: fire frozen (no cooling)");
        CHECK(fo.tFireCool == FIRE_COOL_S, "offline: fire cool timer frozen too");
    }

    printf("== [v0.4.3] builder auto-stoke holds a low fire (Helping + wood) ==\n");
    {
        GameState fs; fs.init(); uint32_t ts = 220000; fs.settle(ts);
        fs.fire = FIRE_FLICKERING; fs.temp = TEMP_MILD;
        fs.builderLevel = 4;                 // Helping: produces wood + auto-stokes
        fs.stores[R_WOOD] = 5 * FP;
        fs.tFireCool = FIRE_COOL_S;
        advance(fs, ts, 300);                // 5 min AWAKE -> one cool cycle
        CHECK(fs.fire == FIRE_FLICKERING,
              "Helping builder auto-stokes: fire held at flickering (stoke +1, cool -1)");
        bool sawStoke = false;
        for (int i = 0; i < fs.logCount; i++)
            if (strcmp(fs.log[i].enKey, "builder stokes the fire") == 0) sawStoke = true;
        CHECK(sawStoke, "auto-stoke pushes \"builder stokes the fire\"");
    }

    printf("== [v0.4.3] no builder -> low fire cools, no auto-stoke ==\n");
    {
        GameState fd; fd.init(); uint32_t td = 230000; fd.settle(td);
        fd.fire = FIRE_FLICKERING; fd.temp = TEMP_MILD;
        fd.builderLevel = -1;                // no builder: no income, no auto-stoke
        fd.stores[R_WOOD] = 0;
        fd.tFireCool = FIRE_COOL_S;
        advance(fd, td, 300);                // 5 min AWAKE
        CHECK(fd.fire == FIRE_SMOLDERING,
              "awake 5min, no builder: Flickering -> Smoldering (cools, no stoke)");
    }

    // =====================================================================
    // v0.4.3 fix 2 — craftables progressive unlock (room.js craftUnlocked):
    // builder Helping + (workshop for tools) + >=50% of the wood cost + every
    // other cost material "seen"; first satisfaction pushes the availableMsg
    // exactly once and latches craftShown.
    // =====================================================================
    printf("== [v0.4.3] craft progressive unlock + availableMsg ==\n");
    {
        GameState g; g.init(); uint32_t t = 240000; g.settle(t);
        g.builderLevel = 4; g.craftablesUnlocked = true;
        // lodge: wood 200, fur 10, meat 5 (building; no workshop gate).
        g.stores[R_WOOD] = 50 * FP;                    // < 100 (50% of 200)
        g.seen = (1u << R_FUR) | (1u << R_MEAT);
        CHECK(!g.craftUnlocked(C_LODGE), "lodge locked: wood < 50% of cost");

        g.stores[R_WOOD] = 100 * FP;                   // >= 50% now
        g.seen = (1u << R_FUR);                         // meat NOT seen yet
        CHECK(!g.craftUnlocked(C_LODGE), "lodge locked: a cost material unseen (meat)");

        g.seen = (1u << R_FUR) | (1u << R_MEAT);        // all non-wood materials seen
        CHECK(g.craftUnlocked(C_LODGE), "lodge unlocks: wood>=50% and materials seen");
        bool sawMsg = false;
        for (int i = 0; i < g.logCount; i++)
            if (strcmp(g.log[i].enKey, "villagers could help hunt, given the means") == 0)
                sawMsg = true;
        CHECK(sawMsg, "first unlock pushes the lodge availableMsg");
        CHECK((g.craftShown & (1u << C_LODGE)) != 0, "craftShown latched for lodge");

        int logAfter = g.logCount;
        CHECK(g.craftUnlocked(C_LODGE), "lodge stays unlocked (memoized)");
        CHECK(g.logCount == logAfter, "availableMsg pushed exactly once, not re-emitted");

        // Not-Helping short-circuits regardless of resources.
        GameState gn; gn.init(); gn.settle(t);
        gn.builderLevel = 3;                            // Sleeping, not Helping
        gn.stores[R_WOOD] = 1000 * FP;
        gn.seen = 0xFFFFFFFF;
        CHECK(!gn.craftUnlocked(C_LODGE), "locked while builder < Helping");

        // Workshop gate for tools (torch needs workshop).
        GameState gw; gw.init(); gw.settle(t);
        gw.builderLevel = 4; gw.craftablesUnlocked = true;
        gw.stores[R_WOOD] = 1000 * FP; gw.seen = 0xFFFFFFFF;
        CHECK(!gw.craftUnlocked(C_TORCH), "tool locked without workshop");
        gw.buildings[B_WORKSHOP] = 1;
        CHECK(gw.craftUnlocked(C_TORCH), "tool unlocks once workshop stands");

        // seen + craftShown round-trip through save/load (v3).
        CHECK(g.save(), "save v3 (seen/craftShown) ok");
        GameState gl2; gl2.init();
        CHECK(gl2.load(), "load v3 ok");
        CHECK(gl2.seen == g.seen, "seen bitset round-trips through save/load");
        CHECK(gl2.craftShown == g.craftShown, "craftShown bitset round-trips");
    }

    printf("== [v0.4.3] seen bit latches on a real stores gain (gatherWood) ==\n");
    {
        GameState g; g.init(); uint32_t t = 250000; g.settle(t);
        g.outsideUnlocked = true;
        CHECK((g.seen & (1u << R_WOOD)) == 0, "wood not yet seen");
        CHECK(g.gatherWood(t) == RC_OK, "gather wood ok");
        CHECK((g.seen & (1u << R_WOOD)) != 0, "gatherWood latches the wood seen bit");
    }

    printf("== [v0.4.3] v2 save migration: seen derived from stores, craftShown 0 ==\n");
    {
        // A v2 JSON (no "seen"/"cshow" keys) with fur>0, meat=0.
        char v2[4096]; size_t o = 0;
        o += (size_t)snprintf(v2 + o, sizeof v2 - o,
            "{\"v\":2,\"ts\":123,\"rng\":42,\"fire\":3,\"temp\":3,\"bl\":4,\"pop\":0,"
            "\"fl\":7,\"cd\":[0,0,0],\"tm\":[30,30,15,300,90],"
            "\"nev\":0,\"echo\":[255,0,0],\"stores\":[");
        for (int i = 0; i < RES_COUNT; i++)
            o += (size_t)snprintf(v2 + o, sizeof v2 - o, "%s%d",
                                  i ? "," : "", i == R_FUR ? 100 * FP : 0);
        o += (size_t)snprintf(v2 + o, sizeof v2 - o, "],\"bld\":[");
        for (int i = 0; i < BLD_COUNT; i++)
            o += (size_t)snprintf(v2 + o, sizeof v2 - o, "%s0", i ? "," : "");
        o += (size_t)snprintf(v2 + o, sizeof v2 - o, "],\"itm\":[");
        for (int i = 0; i < ITEM_COUNT; i++)
            o += (size_t)snprintf(v2 + o, sizeof v2 - o, "%s0", i ? "," : "");
        o += (size_t)snprintf(v2 + o, sizeof v2 - o, "],\"wrk\":[");
        for (int i = 0; i < JOB_COUNT; i++)
            o += (size_t)snprintf(v2 + o, sizeof v2 - o, "%s0", i ? "," : "");
        o += (size_t)snprintf(v2 + o, sizeof v2 - o, "],\"log\":[]}");
        (void)o;

        GameState e; e.init();
        CHECK(e.fromJson(v2), "v2 save loads (fromJson accepts v==2)");
        CHECK((e.seen & (1u << R_FUR)) != 0, "v2 migration: seen derived (fur>0 -> seen)");
        CHECK((e.seen & (1u << R_MEAT)) == 0, "v2 migration: unseen stays unseen (meat=0)");
        CHECK(e.craftShown == 0, "v2 migration: craftShown defaults to 0");
    }

    printf("== [v0.4.4] init() factory-resets EVERY field (GM adr:reset) ==\n");
    {
        // Dirty every persistent field, then init() must scrub them all — the
        // GM reset (main.cpp adr:reset) leans on this to wipe a game in place.
        GameState d; d.init();
        for (int i = 0; i < RES_COUNT; i++) d.stores[i] = 999 * FP;
        for (int i = 0; i < BLD_COUNT; i++) d.buildings[i] = 5;
        for (int i = 0; i < ITEM_COUNT; i++) d.items[i] = 5;
        for (int i = 0; i < JOB_COUNT; i++) d.workers[i] = 3;
        d.population = 42; d.fire = FIRE_ROARING; d.temp = TEMP_HOT;
        d.builderLevel = 4;
        d.outsideUnlocked = d.craftablesUnlocked = d.woodSeen = d.seenForest = true;
        d.seen = 0xFFFFFFFF; d.craftShown = 0xFFFFFFFF;
        d.cdFire = d.cdGather = d.cdTraps = 12345;
        d.needWoodActive = true; d.lastSettleTs = 55555;
        d.nextEventAt = 7777; d.armDelayedEcho(R_FUR, 300, 8888);
        d.pushLog("the wind howls outside");

        d.init();
        bool storesZ = true, bldZ = true, itmZ = true, wrkZ = true;
        for (int i = 0; i < RES_COUNT; i++) if (d.stores[i] != 0) storesZ = false;
        for (int i = 0; i < BLD_COUNT; i++) if (d.buildings[i] != 0) bldZ = false;
        for (int i = 0; i < ITEM_COUNT; i++) if (d.items[i] != 0) itmZ = false;
        for (int i = 0; i < JOB_COUNT; i++) if (d.workers[i] != 0) wrkZ = false;
        CHECK(storesZ && bldZ && itmZ && wrkZ, "init: stores/buildings/items/workers zeroed");
        CHECK(d.population == 0 && d.fire == FIRE_DEAD && d.temp == TEMP_FREEZING,
              "init: fresh dark room (pop 0, fire dead, freezing)");
        CHECK(d.builderLevel == -1, "init: builder absent (-1)");
        CHECK(!d.outsideUnlocked && !d.craftablesUnlocked && !d.woodSeen && !d.seenForest,
              "init: feature flags cleared");
        CHECK(d.seen == 0 && d.craftShown == 0, "init: v0.4.3 seen/craftShown bitsets cleared");
        CHECK(d.cdFire == 0 && d.cdGather == 0 && d.cdTraps == 0, "init: cooldowns cleared");
        CHECK(!d.needWoodActive && d.lastSettleTs == 0, "init: needWood + settle clock reset");
        CHECK(d.nextEventAt == 0 && d.echoRes == ECHO_NONE, "init: event scheduler + echo cleared");
        CHECK(d.logCount == 0, "init: log ring emptied");
    }

    printf("== [v0.4.8] B4: trade goods gated on trading post + seen (buyOfferable) ==\n");
    {
        GameState t; t.init(); uint32_t tt = 260000; t.settle(tt);
        // No trading post yet: nothing offerable, not even compass.
        CHECK(!t.buyOfferable(T_COMPASS), "no post -> compass not offered");
        CHECK(!t.buyOfferable(T_SCALES),  "no post -> scales not offered");

        t.buildings[B_TRADING_POST] = 1;
        // Post stands, but no resource seen yet: only compass is offered
        // (always, so the World can be unlocked); scales/teeth/iron hidden.
        CHECK(t.buyOfferable(T_COMPASS), "post up: compass always offered");
        CHECK(!t.buyOfferable(T_SCALES), "post up but scales unseen -> hidden");
        CHECK(!t.buyOfferable(T_IRON),   "post up but iron unseen -> hidden");

        // Seeing scales (e.g. a trap catch) reveals the scales buy band.
        t.seen |= (1u << R_SCALES);
        CHECK(t.buyOfferable(T_SCALES), "scales seen -> scales buy band appears");
        CHECK(!t.buyOfferable(T_TEETH), "teeth still unseen -> still hidden");

        // Compass caps at 1 (room.js goodsMax): once owned it leaves the list,
        // and a second buy is rejected — with NO maxMsg (upstream trade goods
        // define none; the notify no-ops on undefined), so the log stays clean.
        // Fund the actual compass cost (fur 400 / scales 20 / teeth 10).
        t.stores[R_FUR]    = 1000 * FP;
        t.stores[R_SCALES] =  100 * FP;
        t.stores[R_TEETH]  =  100 * FP;
        int logBefore = t.logCount;
        CHECK(t.buy(T_COMPASS).status == RC_OK, "buy compass ok");
        CHECK(t.whole(R_COMPASS) == 1, "compass owned == 1");
        CHECK(!t.buyOfferable(T_COMPASS), "compass at max -> leaves the buy list");
        CHECK(t.buy(T_COMPASS).status == RC_ERR_MAX, "second compass buy -> RC_ERR_MAX");
        // The two buy() calls log nothing on success/max (buy() has no notify);
        // assert no stray max-notification key crept in.
        CHECK(t.logCount == logBefore, "compass at max pushes no maxMsg (upstream parity)");
    }

    printf("== [v0.20] buy(n): the x10 column truncates to what is affordable ==\n");
    {
        // User report: buying 1 iron needs 50 scales, i.e. FIFTY presses at x1
        // before the iron band is even affordable (1 steel = 101, 1 alien alloy =
        // 1051). The Assign/Path rows solved the same "一个一个调节太费事" with a
        // x10 step in v0.14; the trade bands now carry it too, with upstream's
        // TRUNCATION rule (outside.js:376 Math.min(available, data)).
        GameState t; t.init(); t.buildings[B_TRADING_POST] = 1;
        t.stores[R_FUR] = 1000 * FP;                  // scales cost 150 -> 6 affordable
        BuyResult rx = t.buy(T_SCALES, 10);
        CHECK(rx.status == RC_OK && rx.purchased == 6,
              "x10 with only 6 affordable -> RC_OK purchased=6");
        CHECK(t.whole(R_SCALES) == 6, "bought exactly the 6 it could pay for");
        CHECK(t.whole(R_FUR) == 100, "charged 6*150, not 10*150");

        // A shortage is only a shortage when NOTHING is affordable.
        GameState p; p.init(); p.buildings[B_TRADING_POST] = 1;
        p.stores[R_FUR] = 100 * FP;
        int lb = p.logCount;
        CHECK(p.buy(T_SCALES, 10).status == RC_ERR_COST, "none affordable -> RC_ERR_COST");
        CHECK(p.whole(R_FUR) == 100, "a refused buy charges nothing");
        CHECK(p.logCount == lb + 1, "and pushes the 'not enough' notice once");

        // A multi-resource good truncates on its SCARCEST cost line.
        GameState m; m.init(); m.buildings[B_TRADING_POST] = 1;
        m.stores[R_FUR] = 10000 * FP; m.stores[R_SCALES] = 120 * FP;
        CHECK(m.buy(T_IRON, 10).status == RC_OK, "iron x10 -> RC_OK");
        CHECK(m.whole(R_IRON) == 2, "iron truncated to 2 by the 50-scale line");
        CHECK(m.whole(R_SCALES) == 20 && m.whole(R_FUR) == 9700,
              "both cost lines charged for exactly 2");

        // maximum still caps, and only the bought units are paid for.
        GameState c; c.init(); c.buildings[B_TRADING_POST] = 1;
        c.stores[R_FUR] = 100000 * FP; c.stores[R_SCALES] = 1000 * FP;
        c.stores[R_TEETH] = 1000 * FP;
        CHECK(c.buy(T_COMPASS, 10).status == RC_OK, "compass x10 -> RC_OK");
        CHECK(c.whole(R_COMPASS) == 1, "compass still capped at its maximum of 1");
        CHECK(c.whole(R_FUR) == 99600, "charged for ONE compass, not ten");

        // n <= 0 is a caller bug, never a free buy.
        GameState z; z.init(); z.buildings[B_TRADING_POST] = 1;
        z.stores[R_FUR] = 99999 * FP;
        CHECK(z.buy(T_SCALES, 0).status == RC_ERR_INVALID, "n=0 rejected");
        CHECK(z.buy(T_SCALES, -5).status == RC_ERR_INVALID, "negative n rejected");
        CHECK(z.whole(R_SCALES) == 0, "neither guard bought anything");
    }

    printf("== [v0.4.9] The Sick Man: trigger medicine>0, help consumes + reward branch ==\n");
    {
        GameState e; e.init(); uint32_t t = 270000; e.settle(t);
        events::bind(&e);
        CHECK(!events::startEvent(EV_SICK_MAN, t), "Sick Man needs medicine > 0");
        e.stores[R_MEDICINE] = 2 * FP;
        e.rng = 1;                                   // first rand1000()==369 -> scales branch
        CHECK(events::startEvent(EV_SICK_MAN, t), "Sick Man activates (medicine>0)");
        CHECK(strcmp(events::eventTitleKey(), "The Sick Man") == 0, "title == The Sick Man");
        int medB = e.whole(R_MEDICINE), scB = e.whole(R_SCALES);
        CHECK(events::choose(0) == RC_OK, "give 1 medicine ok");
        CHECK(e.whole(R_MEDICINE) == medB - 1, "help consumes exactly 1 medicine");
        CHECK(e.whole(R_SCALES) == scB + 5 && events::currentScene() == (int)S_SICK_SCALES,
              "rng=1 -> scales reward branch (+5 scales)");
        // v0.4.9 follow-up: an event reward latches the seen bit (addStores ->
        // markSeen), exactly as upstream $SM.add defines the store key — so the
        // rewarded resource now unlocks its craft/buy gate.
        CHECK(e.hasSeen(R_SCALES), "event reward latches seen (addStores -> markSeen)");
        events::choose(0);                           // say goodbye
        CHECK(!events::active(), "goodbye ends the Sick Man");
    }

    printf("== [v0.4.9] markSeen: reward/give injection marks a resource seen ==\n");
    {
        // The GM adr:give path (main.cpp applyPendingGameCmd, out of host scope)
        // calls the same GameState::markSeen after injecting stores — so a
        // GM-given resource is treated as "owned" and unlocks its craft/buy gate.
        // Exercise the shared primitive here.
        GameState g; g.init();
        CHECK(!g.hasSeen(R_MEDICINE), "medicine unseen on a fresh game");
        g.stores[R_MEDICINE] += 5 * FP;              // mirrors adr:give's stores write
        g.markSeen(R_MEDICINE);                      // ...and its markSeen call
        CHECK(g.hasSeen(R_MEDICINE), "give-style injection marks the resource seen");
        g.buildings[B_TRADING_POST] = 1;
        CHECK(g.buyOfferable(T_MEDICINE), "seen medicine now offered at the trading post");
    }

    printf("== [v0.4.9] Sickness: pop-gated, heal consumes 1 medicine, ignore kills ==\n");
    {
        GameState e; e.init(); uint32_t t = 271000; e.settle(t);
        e.outsideUnlocked = true; e.stores[R_MEDICINE] = 1 * FP;
        e.population = 5;                            // outside (10,50) range
        events::bind(&e);
        CHECK(!events::startEvent(EV_SICKNESS, t), "Sickness needs population in (10,50)");
        e.population = 20;
        CHECK(events::startEvent(EV_SICKNESS, t), "Sickness activates (pop 20, medicine>0)");
        int medB = e.whole(R_MEDICINE);
        CHECK(events::choose(0) == RC_OK, "heal (1 medicine) ok");
        CHECK(e.whole(R_MEDICINE) == medB - 1, "heal consumes 1 medicine");
        CHECK(e.population == 20, "healed in time -> no villagers lost");
        events::choose(0);
        CHECK(!events::active(), "go home ends Sickness (healed)");

        // fresh instance: ignore -> the sickness kills [1..floor(pop/2)] villagers
        GameState d; d.init(); d.settle(t);
        d.outsideUnlocked = true; d.stores[R_MEDICINE] = 1 * FP; d.population = 20; d.rng = 1;
        events::bind(&d);
        CHECK(events::startEvent(EV_SICKNESS, t), "Sickness activates again");
        int popB = d.population;
        CHECK(events::choose(1) == RC_OK, "ignore it ok");
        CHECK(d.population < popB, "ignoring the sickness kills villagers");
        CHECK(d.population >= popB - 10, "deaths bounded by floor(pop/2)=10");
    }

    printf("== [v0.4.9] Plague: pop>50, buy medicine (SCENE_STAY), heal 5, do-nothing kills ==\n");
    {
        GameState e; e.init(); uint32_t t = 272000; e.settle(t);
        e.outsideUnlocked = true; e.stores[R_MEDICINE] = 5 * FP; e.population = 30;
        events::bind(&e);
        CHECK(!events::startEvent(EV_PLAGUE, t), "Plague needs population > 50");
        e.population = 60;
        e.stores[R_SCALES] = 200 * FP; e.stores[R_TEETH] = 200 * FP;
        CHECK(events::startEvent(EV_PLAGUE, t), "Plague activates (pop>50, medicine>0)");
        CHECK(events::btnCount() == 3, "plague start offers buy/heal/do-nothing");
        int medB = e.whole(R_MEDICINE), scB = e.whole(R_SCALES), teB = e.whole(R_TEETH);
        CHECK(events::choose(0) == RC_OK, "buy medicine ok");
        CHECK(e.whole(R_MEDICINE) == medB + 1 && e.whole(R_SCALES) == scB - 70 &&
              e.whole(R_TEETH) == teB - 50, "buy medicine: -70 scales -50 teeth +1 medicine");
        CHECK(events::active() && events::currentScene() == (int)S_PLAGUE_START,
              "buy medicine stays on the start scene (SCENE_STAY)");
        e.rng = 1;
        int popB = e.population, medC = e.whole(R_MEDICINE);
        CHECK(events::choose(1) == RC_OK, "heal (5 medicine) ok");
        CHECK(e.whole(R_MEDICINE) == medC - 5, "heal consumes 5 medicine");
        CHECK(e.population < popB && e.population >= popB - 6, "plague healed still loses [2..6]");

        // fresh instance: do nothing -> plague rips through [10..89]
        GameState d; d.init(); d.settle(t);
        d.outsideUnlocked = true; d.stores[R_MEDICINE] = 1 * FP; d.population = 100; d.rng = 1;
        events::bind(&d);
        CHECK(events::startEvent(EV_PLAGUE, t), "Plague activates (pop 100)");
        int popB2 = d.population;
        CHECK(events::choose(2) == RC_OK, "do nothing ok");
        CHECK(d.population < popB2, "plague death kills villagers");
        CHECK(d.population >= popB2 - 89, "deaths bounded by [10..89]");
    }

    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
