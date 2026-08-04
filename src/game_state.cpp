// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room — Phase 1 game state engine implementation. See game_state.h.
// Logic is a faithful port of upstream room.js / outside.js / state_manager.js;
// only save()/load() are platform specific (SD under ARDUINO, stdio on host).
#include "game_state.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef ARDUINO
#include <SD.h>          // must be included at file scope, NOT inside namespace
#endif

namespace adr {

// ===================== lifecycle / new game ===============================

void GameState::init() {
    memset(stores, 0, sizeof stores);
    memset(buildings, 0, sizeof buildings);
    memset(items, 0, sizeof items);
    memset(workers, 0, sizeof workers);
    population = 0;
    fire = FIRE_DEAD;                 // first play: dead fire, freezing room
    temp = TEMP_FREEZING;
    builderLevel = -1;
    outsideUnlocked = craftablesUnlocked = woodSeen = seenForest = false;
    shipUnlocked = shipSeenWarning = false;
    spaceWon = false;
    spacePending = false;
    scoreTotal = 0;
    execEntered = wingEngineering = wingMartial = wingMedical = false;
    blueprints = 0;
    shipHull = SHIP_BASE_HULL;        // 0 — a fresh ship cannot fly (ship.js:8)
    shipThrusters = SHIP_BASE_THRUSTERS;
    seen = 0;
    craftShown = 0;
    perks = 0;
    deathAt = 0;                      // no post-death embark lockout on a fresh game
    clearSavedOutfit();               // fresh game: empty remembered Path outfit
    cdFire = cdGather = cdTraps = 0;
    cdLiftoff = 0;
    tTemp = ROOM_WARM_S;
    tBuilder = BUILDER_STATE_S;
    tNeedWood = NEED_WOOD_S;
    tFireCool = FIRE_COOL_S;
    tPop = POP_DELAY_MIN_S;
    needWoodActive = false;
    lastSettleTs = 0;                 // set on first settle()
    rng = 0x1a2b3c4du;                // fixed seed -> deterministic traps/pop/events
    nextEventAt = 0;                  // engine rerolls on first tick
    echoRes = ECHO_NONE;              // no pending Wanderer echo
    echoAmt = 0;
    echoDueEpoch = 0;
    logCount = 0;
    memset(log, 0, sizeof log);
}

// ===================== log ring ===========================================

void GameState::pushLog(const char* enKey, int32_t arg, bool hasArg) {
    // v0.3.1: a repeat of the newest entry (same key + arg/hasArg — e.g. a
    // cost-disabled band pressed over and over, or the fire settling at
    // roaring under repeated stoking) collapses into that entry's counter
    // instead of scrolling a duplicate line into the ring.
    if (logCount > 0) {
        LogEntry& last = log[logCount - 1];
        if (strcmp(last.enKey, enKey) == 0 && last.arg == arg && last.hasArg == hasArg) {
            if (last.count < 99) last.count++;
            return;
        }
    }
    if (logCount >= LOG_CAP) {        // drop oldest, shift down
        memmove(&log[0], &log[1], sizeof(LogEntry) * (LOG_CAP - 1));
        logCount = LOG_CAP - 1;
    }
    LogEntry& e = log[logCount++];
    snprintf(e.enKey, LOG_KEY_MAX, "%s", enKey);
    e.arg = arg;
    e.hasArg = hasArg;
    e.count = 1;
}

// ===================== deterministic PRNG (xorshift32) ====================

uint32_t GameState::nextRand() {
    uint32_t x = rng ? rng : 0x9e3779b9u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng = x;
    return x;
}
int GameState::rand1000() { return (int)(nextRand() % 1000u); }

// ===================== read helpers =======================================

int GameState::numGatherers() const {
    int n = (int)population;
    for (int j = J_HUNTER; j < JOB_COUNT; j++) n -= (int)workers[j];
    return n < 0 ? 0 : n;
}

// A job is unlocked once its required building stands; gatherer is derived (no
// row) and miners map to BLD_NONE (P2), so both fall out of the J_HUNTER.. scan.
int GameState::unlockedJobs(uint8_t* out, int cap) const {
    int n = 0;
    for (uint8_t j = J_HUNTER; j < JOB_COUNT && n < cap; j++) {
        uint8_t reqB = JOB_REQ_BLD[j];
        if (reqB != BLD_NONE && buildings[reqB] > 0) out[n++] = j;
    }
    return n;
}

bool GameState::hasUnlockedJob() const {
    for (uint8_t j = J_HUNTER; j < JOB_COUNT; j++)
        if (JOB_REQ_BLD[j] != BLD_NONE && buildings[JOB_REQ_BLD[j]] > 0) return true;
    return false;
}

// ===================== starship (ship.js) =================================
// The W landmark's payoff. Every gate here is upstream's; the only 3a deviation
// is liftOff() itself, which has no Space module to hand off to yet.

void GameState::unlockShip() {
    if (shipUnlocked) return;             // world.js goHome's !features.location
                                          // .spaceShip guard: a later cleared trip
                                          // must not reset a reinforced ship.
    shipUnlocked = true;
    shipHull = SHIP_BASE_HULL;
    shipThrusters = SHIP_BASE_THRUSTERS;
    // Ship.onArrival's one-shot notice (game.spaceShip.seenShip upstream). The
    // unlock itself happens exactly once, so the guard above IS the one-shot latch
    // and no separate seenShip bit is needed.
    pushLog("somewhere above the debris cloud, the wanderer fleet hovers. "
            "been on this rock too long.");
}

// reinforceHull / upgradeEngine are deliberately two near-identical functions
// rather than one parameterised helper: upstream writes them out twice too
// (ship.js:100-125), they will diverge the moment either stat grows a rule of
// its own, and a shared body with a "which stat" flag would cost more to read
// than the four duplicated lines it saves.
Result GameState::reinforceHull() {
    if (!shipUnlocked) return RC_ERR_LOCKED;
    if (stores[R_ALIEN_ALLOY] < (int32_t)ALLOY_PER_HULL * FP) {
        pushLog("not enough alien alloy");
        return RC_ERR_COST;
    }
    stores[R_ALIEN_ALLOY] -= (int32_t)ALLOY_PER_HULL * FP;
    shipHull++;                           // no maximum, on purpose (game_data.h)
    return RC_OK;
}

Result GameState::upgradeEngine() {
    if (!shipUnlocked) return RC_ERR_LOCKED;
    if (stores[R_ALIEN_ALLOY] < (int32_t)ALLOY_PER_THRUSTER * FP) {
        pushLog("not enough alien alloy");
        return RC_ERR_COST;
    }
    stores[R_ALIEN_ALLOY] -= (int32_t)ALLOY_PER_THRUSTER * FP;
    shipThrusters++;
    return RC_OK;
}

// Same epoch arithmetic as cooldownLeft(), including `now < last` -> 0: an RTC
// that jumped backwards (a resync, a battery swap) must never strand the button
// for hours, so a backwards clock reads as "ready" rather than as a huge remainder.
int GameState::liftoffCooldownLeft(uint32_t now) const {
    if (cdLiftoff == 0 || now < cdLiftoff) return 0;
    uint32_t el = now - cdLiftoff;
    return el >= (uint32_t)LIFTOFF_COOLDOWN_S ? 0 : (LIFTOFF_COOLDOWN_S - (int)el);
}

Result GameState::startLiftoff(uint32_t now) {
    if (!shipUnlocked) return RC_ERR_LOCKED;
    // ship.js:142 — `if(hull <= 0) Button.setDisabled(liftoffButton)`. The UI
    // already draws the band dashed in this state; this is the engine-side twin
    // so a stale region table can't fly an unreinforced ship.
    if (shipHull <= 0) return RC_ERR_LOCKED;
    if (liftoffCooldownLeft(now) > 0) return RC_ERR_COOLDOWN;
    // The cooldown starts on the PRESS, before checkLiftOff() decides between the
    // warning and the flight — which is exactly why 「裹足徘徊」 has to clear it
    // again (ship.js:157).
    cdLiftoff = now;
    return RC_OK;
}

void GameState::liftOff() {
    // ship.js:165-171 — the panel slide plus `Engine.activeModule = Space`. Both
    // of our callers are inside a touch handler (ShipPage's band, and the
    // confirmation event's 'fly' button), and the Space level is a BLOCKING
    // full-screen loop that owns the panel and the app task for a minute; it
    // cannot be launched from under a modal that is still on screen. So this
    // raises the flag and main.cpp starts the flight on its next pass.
    spacePending = true;
}

// space.js crash() (§2.7). The ONLY lasting consequence is the cooldown, and
// that is the whole reason the level is playable: the persistent hull is a
// maximum, not a pool (§1.2), so there is nothing for a crash to spend.
void GameState::onSpaceCrash(uint32_t now) {
    spacePending = false;
    cdLiftoff = now;                    // Button.cooldown($('#liftoffButton'))
}

void GameState::onSpaceVictory(uint32_t now, uint32_t gameScore) {
    spacePending = false;
    spaceWon = true;
    // Saturating: 32 bits is ~4.3e9 and a single flight tops out in the low
    // millions, so this is a formality — but a wrapped lifetime score would read
    // as a bug forever after, and there is no way back from it.
    if (scoreTotal > 0xFFFFFFFFu - gameScore) scoreTotal = 0xFFFFFFFFu;
    else                                      scoreTotal += gameScore;
    cdLiftoff = now;
}

// scoring.js calculateScore(). Upstream's factor table is transcribed here in
// HALVES (so 1.5 is 3) against the port's own store slots, and the sum is halved
// at the end — integer arithmetic that lands on the same number JS prints,
// without a float anywhere near a save file.
//
// `torch` and the six weapons live in items[] rather than stores[] in this port
// (game_data.h splits craftables out of the resource array), which is the only
// reason this is two loops instead of one.
uint32_t GameState::score() const {
    struct W { uint8_t slot; uint16_t half; };   // 300 (rifle x150) overflows a byte
    // prestige.js storesMap order, resource half:
    static const W RES_W[] = {
        { R_WOOD, 2 }, { R_FUR, 3 }, { R_MEAT, 2 }, { R_IRON, 4 }, { R_COAL, 4 },
        { R_SULPHUR, 6 }, { R_STEEL, 6 }, { R_CURED_MEAT, 4 }, { R_SCALES, 4 },
        { R_TEETH, 4 }, { R_LEATHER, 4 }, { R_BAIT, 3 }, { R_CLOTH, 2 },
        { R_BULLETS, 6 }, { R_ENERGY_CELL, 6 },
        // scoring.js:21 — `alien alloy` is scored outside the storesMap loop, at
        // 10 a unit. Same arithmetic, so it rides the same table.
        { R_ALIEN_ALLOY, 20 },
    };
    static const W ITEM_W[] = {
        { I_TORCH, 2 }, { I_BONE_SPEAR, 20 }, { I_IRON_SWORD, 60 },
        { I_STEEL_SWORD, 100 }, { I_BAYONET, 200 }, { I_RIFLE, 300 },
        { I_LASER_RIFLE, 300 }, { I_GRENADE, 10 }, { I_BOLAS, 8 },
    };
    uint64_t halves = 0;
    for (const W& w : RES_W) {
        int32_t n = whole(w.slot);
        if (n > 0) halves += (uint64_t)n * w.half;
    }
    for (const W& w : ITEM_W) halves += (uint64_t)items[w.slot] * w.half;
    // scoring.js:23 — Ship.getMaxHull() * 50. `fleet beacon` * 500 has no slot
    // to read yet (3c); when it lands it adds one more line here.
    if (shipHull > 0) halves += (uint64_t)shipHull * 100;
    uint64_t total = halves / 2;
    return total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)total;
}

int GameState::cooldownLeft(int action, uint32_t now) const {
    uint32_t last = action == 0 ? cdFire : action == 1 ? cdGather : cdTraps;
    int cd = action == 0 ? STOKE_COOLDOWN_S
           : action == 1 ? GATHER_DELAY_S : TRAPS_DELAY_S;
    if (last == 0 || now < last) return 0;
    uint32_t el = now - last;
    return el >= (uint32_t)cd ? 0 : (cd - (int)el);
}

// ===================== settle (offline / awake economy) ===================

uint32_t GameState::settle(uint32_t nowEpoch, bool offline) {
    // epochNow's mktime() failed to produce a valid reading (invalid RTC) —
    // bail before touching any anchor rather than latching lastSettleTs to 0,
    // which would masquerade as "never settled" and re-arm the clock-rewind
    // path below on the next call with a real reading.
    if (nowEpoch == 0) return 0;
    if (lastSettleTs == 0) { lastSettleTs = nowEpoch; return 0; }
    if (nowEpoch < lastSettleTs) {
        // Clock rewind: the BM8563 RTC is powered by the main battery, and a
        // full battery drain resets it to ~2000-01-01. On the next boot,
        // nowEpoch (~2000) is permanently behind every persisted future
        // epoch (lastSettleTs, nextEventAt, echoDueEpoch), which without this
        // re-anchor makes the `nowEpoch <= lastSettleTs` guard below early-out
        // forever — the builder state machine / unlockForest / income /
        // population all freeze, and the player is stuck in the fire-lighting
        // room until the RTC happens to be recalibrated (e.g. via BLE time
        // sync). Re-anchor the whole persisted timeline onto the new clock
        // instead of trying to preserve elapsed-time bookkeeping across a
        // discontinuity that upstream never has to model.
        lastSettleTs = nowEpoch;
        nextEventAt = 0;              // reroll on next tick (existing 0 = unscheduled path)
        if (echoRes != ECHO_NONE) echoDueEpoch = nowEpoch;  // redeem on next check
        return 0;
    }
    if (nowEpoch == lastSettleTs) return 0;
    uint32_t elapsed = nowEpoch - lastSettleTs;
    uint32_t steps = elapsed / INCOME_TICK_S;
    const uint32_t maxSteps = SETTLE_MAX_S / INCOME_TICK_S;   // 8640 (24h)
    bool capped = false;
    if (steps > maxSteps) { steps = maxSteps; capped = true; }
    for (uint32_t i = 0; i < steps; i++) stepOnce(offline);
    if (capped) lastSettleTs = nowEpoch;
    else        lastSettleTs += steps * INCOME_TICK_S;
    // A Wanderer echo that came due while offline is redeemed on wake as a lump
    // (its payout does not retroactively feed the just-simulated economy).
    redeemDelayedEcho(nowEpoch);
    return steps;
}

void GameState::stepOnce(bool offline) {
    // -- temperature drift (room.js adjustTemp, every 30s) --
    tTemp -= INCOME_TICK_S;
    if (tTemp <= 0) { adjustTemp(); tTemp = ROOM_WARM_S; }

    // -- builder state machine (every 30s while level 0..3) --
    if (builderLevel >= 0 && builderLevel < 4) {
        tBuilder -= INCOME_TICK_S;
        if (tBuilder <= 0) { advanceBuilder(); tBuilder = BUILDER_STATE_S; }
    }
    // -- stranger collapses -> needs wood 15s later -> unlockForest --
    if (needWoodActive) {
        tNeedWood -= INCOME_TICK_S;
        if (tNeedWood <= 0) { unlockForest(); needWoodActive = false; }
    }

    // -- fire cooling (room.js coolFire, every 5min). Awake only by default: the
    // fire and its cool timer tFireCool are FROZEN across an offline catch-up
    // (research.md §5.3 — a 5min cool vs a 15min wake cycle would extinguish the
    // fire on every wake, pure punishment). Because tFireCool is NOT decremented
    // while offline, the 5min countdown resumes on wake exactly where it slept —
    // the fire is frozen in time, not merely un-cooled. ADR_FIRE_OFFLINE_DECAY
    // (default off) opts the offline path back in to real-time cooling. --
    bool coolFireNow;
#if ADR_FIRE_OFFLINE_DECAY
    (void)offline;               // offline path also cools when the switch is on
    coolFireNow = true;
#else
    coolFireNow = !offline;
#endif
    if (coolFireNow && fire > FIRE_DEAD) {
        tFireCool -= INCOME_TICK_S;
        if (tFireCool <= 0) {
            // builder auto-stokes a low fire if wood remains (room.js coolFire):
            // notify, spend 1 wood, bump the fire up — then the cool below nets it
            // back down, so a Helping builder holds a low fire at cost of wood.
            if (fire <= FIRE_FLICKERING && builderLevel > 3 &&
                stores[R_WOOD] > 0) {
                pushLog("builder stokes the fire");
                stores[R_WOOD] -= 1 * FP;
                if (fire < FIRE_ROARING) fire++;
            }
            fire--;              // guarded fire>FIRE_DEAD above (auto-stoke only raises)
            onFireChange();      // "the fire is {0}" + resets tFireCool to FIRE_COOL_S
        }
    }

    // -- worker income (every 10s tick), all-or-nothing per source --
    applyIncomeSource(J_GATHERER, numGatherers());
    for (int j = J_HUNTER; j < JOB_COUNT; j++)
        applyIncomeSource(j, (int)workers[j]);
    if (builderLevel >= 4) {           // builder Helping: +2 wood / tick
        stores[R_WOOD] += BUILDER_WOOD_DFP;
        markSeen(R_WOOD);
    }

    // -- population growth (only once there are huts) --
    if (buildings[B_HUT] > 0) {
        tPop -= INCOME_TICK_S;
        if (tPop <= 0) { increasePopulation(); tPop = POP_DELAY_MIN_S; }
    }
}

void GameState::applyIncomeSource(uint8_t job, int count) {
    if (count <= 0) return;
    const IncomeDef& d = INCOME[job];
    for (int i = 0; i < d.n; i++) {          // break-on-shortage (断料停产):
        int32_t delta = d.items[i].dfp * count;
        if (stores[d.items[i].res] + delta < 0) return;   // ANY input short -> skip all
    }
    for (int i = 0; i < d.n; i++) {
        stores[d.items[i].res] += d.items[i].dfp * count;
        if (d.items[i].dfp > 0) markSeen(d.items[i].res);   // a produced resource is "seen"
    }
}

// room.js adjustTemp(): notify on EVERY temperature change (not just when the
// stranger's warmth threshold is crossed) — v0.3.1 feedback 1: the fire/temp
// state used to live in a persistent header line; now that the line is gone
// (room_page.cpp), a change is the only way the user ever sees it, exactly
// matching upstream's behavior.
void GameState::adjustTemp() {
    if (temp > TEMP_FREEZING && temp > fire) {
        temp--;
        pushLog("the room is {0}", temp, true);
    }
    if (temp < TEMP_HOT && temp < fire) {
        temp++;
        pushLog("the room is {0}", temp, true);
    }
}

void GameState::advanceBuilder() {
    switch (builderLevel) {
        case 0:                                // Approaching -> Collapsed
            builderLevel = 1;
            pushLog("a ragged stranger stumbles through the door and collapses in the corner");
            needWoodActive = true;
            tNeedWood = NEED_WOOD_S;
            break;
        case 1:                                // needs Warm to recover
            if (temp >= TEMP_WARM) {
                builderLevel = 2;
                pushLog("the stranger shivers, and mumbles quietly. her words are unintelligible.");
            }
            break;
        case 2:
            if (temp >= TEMP_WARM) {
                builderLevel = 3;
                pushLog("the stranger in the corner stops shivering. her breathing calms.");
            }
            break;
        case 3:                                // Sleeping -> Helping (income + craft)
            builderLevel = 4;
            craftablesUnlocked = true;
            pushLog("the stranger is standing by the fire. she says she can help. says she builds things.");
            break;
        default: break;
    }
}

void GameState::unlockForest() {
    stores[R_WOOD] = 4 * FP;                    // room.js: set stores.wood = 4
    markSeen(R_WOOD);
    woodSeen = true;
    outsideUnlocked = true;
    pushLog("the wind howls outside");
    pushLog("the wood is running out");
}

void GameState::increasePopulation() {
    int space = (int)maxPopulation() - (int)population;
    if (space <= 0) return;
    // room.js: floor(rand*(space/2) + space/2) == floor(space*(1+rand)/2)
    int num = (int)(((int64_t)space * (1000 + rand1000())) / 2000);
    if (num == 0) num = 1;
    if (num > space) num = space;
    population += (uint16_t)num;
    const char* k;
    if (num == 1)       k = "a stranger arrives in the night";
    else if (num < 5)   k = "a weathered family takes up in one of the huts.";
    else if (num < 10)  k = "a small group arrives, all dust and bones.";
    else if (num < 30)  k = "a convoy lurches in, equal parts worry and hope.";
    else                k = "the town's booming. word does get around.";
    pushLog(k, num, true);
}

// ===================== fire actions =======================================

void GameState::onFireChange() {
    // room.js onFireChange(): notify on EVERY fire change (v0.3.1 feedback 1 —
    // see adjustTemp() above for why this now has to be the sole way the fire
    // state reaches the user).
    pushLog("the fire is {0}", fire, true);
    if (fire > FIRE_SMOLDERING && builderLevel < 0) {   // fire.value > 1
        builderLevel = 0;
        tBuilder = BUILDER_STATE_S;
        pushLog("the light from the fire spills from the windows, out into the dark");
    }
    tFireCool = FIRE_COOL_S;
}

Result GameState::lightFire(uint32_t now) {
    if (cooldownLeft(0, now) > 0) return RC_ERR_COOLDOWN;
    if (woodSeen) {                       // once wood is a real number, it costs 5
        if (stores[R_WOOD] < LIGHT_FIRE_WOOD * FP) {
            pushLog("not enough wood to get the fire going");
            return RC_ERR_COST;
        }
        stores[R_WOOD] -= LIGHT_FIRE_WOOD * FP;
    }
    // else: opening play, wood is "undefined" -> free first light (room.js quirk)
    fire = FIRE_BURNING;
    cdFire = now;
    onFireChange();
    return RC_OK;
}

Result GameState::stokeFire(uint32_t now) {
    if (cooldownLeft(0, now) > 0) return RC_ERR_COOLDOWN;
    if (stores[R_WOOD] < STOKE_FIRE_WOOD * FP) {
        pushLog("the wood has run out");
        return RC_ERR_COST;
    }
    stores[R_WOOD] -= STOKE_FIRE_WOOD * FP;
    if (fire < FIRE_ROARING) fire++;
    cdFire = now;
    onFireChange();
    return RC_OK;
}

// ===================== outside actions ====================================

Result GameState::gatherWood(uint32_t now) {
    if (!outsideUnlocked) return RC_ERR_LOCKED;
    if (cooldownLeft(1, now) > 0) return RC_ERR_COOLDOWN;
    int amt = buildings[B_CART] > 0 ? 50 : 10;
    stores[R_WOOD] += amt * FP;
    markSeen(R_WOOD);
    woodSeen = true;
    cdGather = now;
    pushLog("dry brush and dead branches litter the forest floor");
    return RC_OK;
}

Result GameState::checkTraps(uint32_t now) {
    if (buildings[B_TRAP] == 0) return RC_ERR_LOCKED;
    if (cooldownLeft(2, now) > 0) return RC_ERR_COOLDOWN;
    int numTraps = buildings[B_TRAP];
    int numBait = whole(R_BAIT); if (numBait < 0) numBait = 0;
    int baited = numBait < numTraps ? numBait : numTraps;   // extra roll per bait
    int numDrops = numTraps + baited;
    bool caught[6] = { false, false, false, false, false, false };
    for (int i = 0; i < numDrops; i++) {
        int roll = rand1000();
        for (int j = 0; j < 6; j++) {
            if (roll < TRAP_DROPS[j].rollUnderMilli) {
                stores[TRAP_DROPS[j].res] += 1 * FP;
                markSeen(TRAP_DROPS[j].res);
                caught[j] = true;
                break;
            }
        }
    }
    stores[R_BAIT] -= baited * FP;
    cdTraps = now;
    // "the traps contain " is an intro key, not a full sentence (see
    // strings_zh.h "陷阱捕获到") — the actual catch has to follow as its own
    // log line(s), one per distinct resource type this round, or the intro
    // renders with nothing after it (the reported "陷阱捕获到XXX后面缺失" bug).
    pushLog("the traps contain ");
    for (int j = 0; j < 6; j++)
        if (caught[j]) pushLog(TRAP_DROPS[j].msg);
    return RC_OK;
}

// ===================== craft / build / buy / assign =======================

// room.js craftUnlocked(thing) — the progressive-unlock gate that room_page's
// craftOfferable() consults so the build/craft buttons appear one at a time as
// the economy grows, instead of all at once the moment the builder starts
// Helping. Latches craftShown (upstream Room.buttons) so it evaluates — and
// notifies — once; thereafter it short-circuits true. NOTE the "seen" gate is a
// deliberate refinement of upstream: room.js tests `!stores[c]` (the material is
// currently >0), which flickers a button off if you spend a material back to 0;
// we test the seen bitset (ever owned) so an unlocked button stays unlocked
// (research/task §5.3 fix-2). The >=50% wood-cost gate reads live wood, as
// upstream does.
bool GameState::craftUnlocked(uint8_t craftId) {
    if (craftId >= CRAFT_COUNT) return false;
    if (craftShown & (1u << craftId)) return true;   // already unlocked (memoized)
    if (builderLevel < 4) return false;              // builder not yet Helping
    const Craftable& c = CRAFT[craftId];
    if (craftNeedsWorkshop(c.type) && buildings[B_WORKSHOP] == 0) return false;

    // Already built at least once -> unlocked, but no availableMsg (upstream).
    bool bld = craftIsBuilding(craftId);
    uint8_t slot = craftSlot(craftId);
    int count = bld ? buildings[slot] : items[slot];
    if (count > 0) { craftShown |= (1u << craftId); return true; }

    // Need >=50% of the wood cost on hand (room.js: stores.wood < cost.wood*0.5).
    // A craftable with no wood cost skips this gate (upstream compares against
    // undefined -> NaN -> false).
    int woodCost = 0;
    for (int i = 0; i < 3 && c.cost[i].res != RA_END; i++)
        if (c.cost[i].res == R_WOOD) woodCost = c.cost[i].amt;
    if (woodCost > 0 && stores[R_WOOD] < (int32_t)woodCost * FP / 2) return false;

    // Every OTHER cost material must have been seen (wood handled above).
    for (int i = 0; i < 3 && c.cost[i].res != RA_END; i++) {
        if (c.cost[i].res == R_WOOD) continue;
        if (!(seen & (1u << c.cost[i].res))) return false;
    }

    craftShown |= (1u << craftId);
    if (c.availableMsg) pushLog(c.availableMsg);   // first-unlock builder hint
    return true;
}

// room.js buyUnlocked(thing) — the trade-good display gate the Trade page reads.
// Fix B4 (v0.4.8): the old Trade page only gated on the trading post standing +
// the maximum, so every good appeared the instant the post went up. Upstream
// only offers a good once its product resource has been SEEN (stores key
// defined) — compass excepted, always offered so the World can be unlocked. We
// reuse the same seen bitset craftUnlocked consults. NOTE: upstream trade goods
// carry no availableMsg (no notify on unlock) and no maxMsg (compass has none —
// notify no-ops on undefined), so this is a pure const predicate: a capped good
// simply leaves the list once owned (the firmware's hide-at-max convention,
// same as craftOfferable), with no notification.
bool GameState::buyOfferable(uint8_t tradeId) const {
    if (tradeId >= TRADE_COUNT) return false;
    if (buildings[B_TRADING_POST] == 0) return false;   // post must stand
    const TradeGood& g = TRADE[tradeId];
    if (g.product != R_COMPASS && !hasSeen(g.product)) return false;   // "seen" gate
    int have = whole(g.product); if (have < 0) have = 0;
    if (g.maximum >= 0 && have >= g.maximum) return false;   // goodsMax (compass caps at 1)
    return true;
}

Result GameState::build(uint8_t craftId) {
    if (craftId >= CRAFT_COUNT || CRAFT[craftId].type != CT_BUILDING)
        return RC_ERR_INVALID;
    return makeCraftable(craftId);
}

Result GameState::craft(uint8_t craftId) {
    if (craftId >= CRAFT_COUNT || CRAFT[craftId].type == CT_BUILDING)
        return RC_ERR_INVALID;
    return makeCraftable(craftId);
}

Result GameState::makeCraftable(uint8_t craftId) {
    const Craftable& c = CRAFT[craftId];
    // room.js build(): too cold -> "builder just shivers"
    if (temp <= TEMP_COLD) { pushLog("builder just shivers"); return RC_ERR_COLD; }
    // craftables only exist once the builder is Helping; workshop items gated
    if (!craftablesUnlocked) return RC_ERR_LOCKED;
    if (craftNeedsWorkshop(c.type) && buildings[B_WORKSHOP] == 0)
        return RC_ERR_LOCKED;

    bool bld = craftIsBuilding(craftId);
    uint8_t slot = craftSlot(craftId);
    int count = bld ? buildings[slot] : items[slot];
    if (c.maximum >= 0 && count >= c.maximum) return RC_ERR_MAX;

    // cost (trap/hut scale wood by existing count). room.js build()/craft():
    // notify "not enough " + the FIRST short resource's key (loop breaks there,
    // same as this one) — v0.3.1 feedback 2: a disabled build/craft band used to
    // fail silently; RES_KEY[] carries the exact upstream store key, and every
    // resource ever costed here has a "not enough <key>" translation.
    for (int i = 0; i < 3 && c.cost[i].res != RA_END; i++) {
        int need = c.cost[i].amt;
        if (c.cost[i].res == R_WOOD) need += (int)c.woodIncrPerN * count;
        if (stores[c.cost[i].res] < need * FP) {
            char key[40];
            snprintf(key, sizeof(key), "not enough %s", RES_KEY[c.cost[i].res]);
            pushLog(key);
            return RC_ERR_COST;
        }
    }
    for (int i = 0; i < 3 && c.cost[i].res != RA_END; i++) {
        int need = c.cost[i].amt;
        if (c.cost[i].res == R_WOOD) need += (int)c.woodIncrPerN * count;
        stores[c.cost[i].res] -= need * FP;
    }
    if (bld) {
        buildings[slot]++;
        if (slot == B_HUT && buildings[B_HUT] == 1) tPop = POP_DELAY_MIN_S;
    } else {
        items[slot]++;
    }
    pushLog(c.buildMsg);
    return RC_OK;
}

Result GameState::buy(uint8_t tradeId) {
    if (tradeId >= TRADE_COUNT) return RC_ERR_INVALID;
    if (buildings[B_TRADING_POST] == 0) return RC_ERR_LOCKED;
    const TradeGood& g = TRADE[tradeId];
    int have = whole(g.product); if (have < 0) have = 0;
    if (g.maximum >= 0 && have >= g.maximum) return RC_ERR_MAX;
    // room.js buy(): Notifications.notify(Room, _("not enough " + k)) on the
    // FIRST short cost resource (loop breaks there) — same v0.3.1 feedback-2
    // pattern makeCraftable() already carries for build/craft; a cost-disabled
    // trade band used to fail silently, exactly like the pre-0.3.1 build/craft
    // bug this mirrors.
    for (int i = 0; i < 3 && g.cost[i].res != RA_END; i++) {
        if (stores[g.cost[i].res] < g.cost[i].amt * FP) {
            char key[40];
            snprintf(key, sizeof(key), "not enough %s", RES_KEY[g.cost[i].res]);
            pushLog(key);
            return RC_ERR_COST;
        }
    }
    for (int i = 0; i < 3 && g.cost[i].res != RA_END; i++)
        stores[g.cost[i].res] -= g.cost[i].amt * FP;
    stores[g.product] += 1 * FP;
    markSeen(g.product);
    // room.js buy(): Notifications.notify(Room, good.buildMsg) on success — but
    // Room.TradeGoods entries carry no buildMsg property (only type/cost/audio),
    // so good.buildMsg is undefined, and Notifications.notify() no-ops on an
    // undefined message (notifications.js: "if (typeof text == 'undefined')
    // return;"). Upstream's buy() therefore has NO success notification —
    // matched exactly here: no pushLog on the RC_OK path.
    return RC_OK;
}

Result GameState::assignWorker(uint8_t job, int delta) {
    if (job == J_GATHERER || job >= JOB_COUNT) return RC_ERR_INVALID;
    uint8_t reqB = JOB_REQ_BLD[job];
    if (reqB == BLD_NONE || buildings[reqB] == 0) return RC_ERR_LOCKED;
    if (delta > 0) {
        int avail = numGatherers();
        if (avail <= 0) return RC_ERR_COST;
        int n = delta < avail ? delta : avail;
        workers[job] += (uint16_t)n;
    } else if (delta < 0) {
        int n = -delta;
        if (n > (int)workers[job]) n = workers[job];
        workers[job] -= (uint16_t)n;
    }
    return RC_OK;
}

// ===================== random-event side effects ==========================

void GameState::killVillagers(int num) {
    int p = (int)population - num;
    if (p < 0) p = 0;
    population = (uint16_t)p;
    // If more villagers are assigned to jobs than remain, strip workers in job
    // order until the derived gatherer count is non-negative (outside.js parity).
    int raw = (int)population;
    for (int j = J_HUNTER; j < JOB_COUNT; j++) raw -= (int)workers[j];
    if (raw < 0) {
        int gap = -raw;
        for (int j = J_HUNTER; j < JOB_COUNT && gap > 0; j++) {
            int nw = (int)workers[j];
            if (nw < gap) { gap -= nw; workers[j] = 0; }
            else          { workers[j] = (uint16_t)(nw - gap); gap = 0; }
        }
    }
}

int GameState::destroyHuts(int num) {
    int dead = 0;
    for (int i = 0; i < num; i++) {
        int pop  = (int)population;
        int full = pop / HUT_ROOM;                        // fully occupied huts
        int huts = (pop + HUT_ROOM - 1) / HUT_ROOM;       // ceil(pop/HUT_ROOM)
        if (huts == 0) break;
        int target = (int)((rand1000() * (uint32_t)huts) / 1000u) + 1;  // 1..huts
        int inhabitants = 0;
        if (target <= full)          inhabitants = HUT_ROOM;
        else if (target == full + 1) inhabitants = pop % HUT_ROOM;
        if (buildings[B_HUT] > 0) buildings[B_HUT]--;
        if (inhabitants) { killVillagers(inhabitants); dead += inhabitants; }
    }
    return dead;
}

void GameState::armDelayedEcho(uint8_t res, int32_t amtWhole, uint32_t dueEpoch) {
    echoRes = res;
    echoAmt = amtWhole;
    echoDueEpoch = dueEpoch;
}

bool GameState::redeemDelayedEcho(uint32_t nowEpoch) {
    if (echoRes == ECHO_NONE) return false;
    if (nowEpoch < echoDueEpoch) return false;
    stores[echoRes] += echoAmt * FP;
    markSeen(echoRes);
    pushLog(echoRes == R_FUR
            ? "the mysterious wanderer returns, cart piled high with furs."
            : "the mysterious wanderer returns, cart piled high with wood.");
    echoRes = ECHO_NONE;
    echoAmt = 0;
    echoDueEpoch = 0;
    return true;
}

// ===================== JSON (de)serialization =============================

namespace {
void apStr(char* out, size_t cap, size_t& o, const char* s) {
    if (o < cap) out[o] = '"'; o++;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') { if (o < cap) out[o] = '\\'; o++; }
        if (o < cap) out[o] = c; o++;
    }
    if (o < cap) out[o] = '"'; o++;
}
// Read a JSON string starting at *pp (which must point at the opening quote).
// Writes up to cap-1 chars + NUL into buf; advances *pp past the closing quote.
bool readStr(const char*& p, char* buf, size_t cap) {
    while (*p && *p != '"') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) c = *p++;
        if (i + 1 < cap) buf[i++] = c;
    }
    if (*p != '"') return false;
    p++;
    buf[i] = 0;
    return true;
}
const char* afterKey(const char* j, const char* key) {   // -> char after "key":
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char* p = strstr(j, pat);
    return p ? p + strlen(pat) : nullptr;
}
long readLong(const char* p) { return p ? strtol(p, nullptr, 10) : 0; }
// Parse n ints from a "[a,b,...]" starting at p (p just after the key).
void readIntArr(const char* p, int32_t* out, int n) {
    for (int i = 0; i < n; i++) out[i] = 0;
    if (!p) return;
    const char* q = strchr(p, '[');
    if (!q) return;
    q++;
    for (int i = 0; i < n && *q; i++) {
        while (*q && *q != '-' && (*q < '0' || *q > '9') && *q != ']') q++;
        if (*q == ']' || !*q) break;
        out[i] = (int32_t)strtol(q, (char**)&q, 10);
    }
}
}  // namespace

size_t GameState::toJson(char* out, size_t cap) const {
    size_t o = 0;
#define AP(...) do { int _n = snprintf(out + (o < cap ? o : cap), \
        o < cap ? cap - o : 0, __VA_ARGS__); if (_n > 0) o += (size_t)_n; } while (0)
    AP("{\"v\":%d,\"ts\":%lu,\"rng\":%lu,", SAVE_VER,
       (unsigned long)lastSettleTs, (unsigned long)rng);
    AP("\"fire\":%d,\"temp\":%d,\"bl\":%d,\"pop\":%u,",
       fire, temp, builderLevel, population);
    // Bits 32/64 are the v4 starship flags. They ride the EXISTING "fl" integer
    // rather than two new keys because a bitfield that a pre-v4 reader parses as a
    // plain int is upgrade-safe in both directions: an old firmware reading a v4
    // save picks up flags it does not know and masks them off, and a v4 firmware
    // reading a v1..v3 save simply finds those bits clear == "no ship yet".
    int flags = (outsideUnlocked ? 1 : 0) | (craftablesUnlocked ? 2 : 0) |
                (woodSeen ? 4 : 0) | (seenForest ? 8 : 0) |
                (needWoodActive ? 16 : 0) |
                (shipUnlocked ? 32 : 0) | (shipSeenWarning ? 64 : 0) |
                (spaceWon ? 128 : 0) |
                // 3c-2 Executioner progress: the same bitfield-growth argument
                // as the starship flags above.
                (execEntered ? 256 : 0) | (wingEngineering ? 512 : 0) |
                (wingMartial ? 1024 : 0) | (wingMedical ? 2048 : 0);
    AP("\"fl\":%d,", flags);
    AP("\"cd\":[%lu,%lu,%lu],", (unsigned long)cdFire,
       (unsigned long)cdGather, (unsigned long)cdTraps);
    // Starship stats + the liftoff cooldown epoch as three flat keys, NOT appended
    // to the "cd" array: readIntArr stops at the ']' of a short array, so a 4th
    // slot would read back as 0 on every v3 save — silently correct today, but it
    // would make the array's length load-bearing across versions. A named key that
    // is simply absent is the convention the rest of this file already relies on.
    AP("\"shiph\":%d,\"shipt\":%d,\"cdlift\":%lu,",
       shipHull, shipThrusters, (unsigned long)cdLiftoff);
    // v5 Space outcome. Same flat-key rule as the three above: absent on a
    // v1..v4 save, where init()'s 0 stands and reads correctly as "never flown".
    AP("\"tscore\":%lu,", (unsigned long)scoreTotal);
    // Redeemed blueprints (3c-2). Flat key, absent on every earlier save -> 0.
    AP("\"bp\":%u,", (unsigned)blueprints);
    AP("\"tm\":[%d,%d,%d,%d,%d],", tTemp, tBuilder, tNeedWood, tFireCool, tPop);
    AP("\"nev\":%lu,", (unsigned long)nextEventAt);
    AP("\"echo\":[%d,%ld,%lu],", echoRes, (long)echoAmt,
       (unsigned long)echoDueEpoch);
    AP("\"seen\":%lu,\"cshow\":%lu,\"perks\":%lu,",
       (unsigned long)seen, (unsigned long)craftShown, (unsigned long)perks);
    AP("\"dcool\":%lu,", (unsigned long)deathAt);
    // Remembered Path outfit (sparse [idx,val,...] pairs — only the non-zero
    // keep-slots): compact because a returned outfit is a handful of entries.
    AP("\"oftr\":[");
    { bool f = true; for (int i = 0; i < RES_COUNT; i++) if (savedOutfitRes[i] > 0)
        { AP("%s%d,%d", f ? "" : ",", i, savedOutfitRes[i]); f = false; } }
    AP("],\"ofti\":[");
    { bool f = true; for (int i = 0; i < ITEM_COUNT; i++) if (savedOutfitItem[i] > 0)
        { AP("%s%d,%d", f ? "" : ",", i, savedOutfitItem[i]); f = false; } }
    AP("],");
    AP("\"stores\":[");
    for (int i = 0; i < RES_COUNT; i++) AP("%s%ld", i ? "," : "", (long)stores[i]);
    AP("],\"bld\":[");
    for (int i = 0; i < BLD_COUNT; i++) AP("%s%d", i ? "," : "", buildings[i]);
    AP("],\"itm\":[");
    for (int i = 0; i < ITEM_COUNT; i++) AP("%s%d", i ? "," : "", items[i]);
    AP("],\"wrk\":[");
    for (int i = 0; i < JOB_COUNT; i++) AP("%s%u", i ? "," : "", workers[i]);
    // log entries: ["key",arg,hasArg,count]. count is a v0.3.1 addition,
    // always written from here on; fromJson() treats it as OPTIONAL on read
    // (defaults to 1) so saves written by pre-0.3.1 firmware still load.
    AP("],\"log\":[");
    for (int i = 0; i < logCount; i++) {
        if (i) AP(",");
        AP("[");
        apStr(out, cap, o, log[i].enKey);
        AP(",%ld,%d,%d]", (long)log[i].arg, log[i].hasArg ? 1 : 0, log[i].count);
    }
    AP("]}");
#undef AP
    return o;
}

bool GameState::fromJson(const char* j) {
    if (!j) return false;
    long v = readLong(afterKey(j, "v"));
    if (v < 1 || v > 5) return false;    // accept v1 (pre-events) .. v5 saves
    init();                              // defaults, then overwrite
    lastSettleTs = (uint32_t)readLong(afterKey(j, "ts"));
    rng          = (uint32_t)readLong(afterKey(j, "rng"));
    fire         = (uint8_t)readLong(afterKey(j, "fire"));
    temp         = (uint8_t)readLong(afterKey(j, "temp"));
    builderLevel = (int8_t)readLong(afterKey(j, "bl"));
    population   = (uint16_t)readLong(afterKey(j, "pop"));
    int flags    = (int)readLong(afterKey(j, "fl"));
    outsideUnlocked    = flags & 1;
    craftablesUnlocked = flags & 2;
    woodSeen           = flags & 4;
    seenForest         = flags & 8;
    needWoodActive     = flags & 16;
    shipUnlocked       = flags & 32;     // v4; clear on v1..v3 -> no ship page
    shipSeenWarning    = flags & 64;     // v4; clear on v1..v3 -> warning still due
    spaceWon           = flags & 128;    // v5; clear on v1..v4 -> never reached space
    execEntered        = flags & 256;    // 3c-2; clear on an older save -> prologue due
    wingEngineering    = flags & 512;
    wingMartial        = flags & 1024;
    wingMedical        = flags & 2048;
    blueprints = (uint8_t)readLong(afterKey(j, "bp"));   // absent -> none redeemed
    int32_t cd[3];  readIntArr(afterKey(j, "cd"), cd, 3);
    cdFire = (uint32_t)cd[0]; cdGather = (uint32_t)cd[1]; cdTraps = (uint32_t)cd[2];
    // v4 starship. PRESENCE-CHECKED, not readLong'd blind: shipThrusters defaults
    // to 1, not 0, so on a v1..v3 save the missing key must leave init()'s value
    // standing rather than zero it. (hull and the cooldown default to 0 either
    // way, but they go through the same guard so the three read as one block.)
    { const char* p = afterKey(j, "shiph"); if (p) shipHull      = (int16_t)readLong(p); }
    { const char* p = afterKey(j, "shipt"); if (p) shipThrusters = (int16_t)readLong(p); }
    cdLiftoff = (uint32_t)readLong(afterKey(j, "cdlift"));   // absent -> 0 (ready)
    scoreTotal = (uint32_t)readLong(afterKey(j, "tscore"));  // absent -> 0 (v5)
    int32_t tm[5];  readIntArr(afterKey(j, "tm"), tm, 5);
    tTemp = tm[0]; tBuilder = tm[1]; tNeedWood = tm[2];
    tFireCool = tm[3]; tPop = tm[4];
    // v2 event fields — absent in v1 saves, where init()'s defaults stand
    // (nextEventAt=0 => reroll on load; echo slot empty).
    nextEventAt = (uint32_t)readLong(afterKey(j, "nev"));   // nullptr -> 0
    const char* echoP = afterKey(j, "echo");
    if (echoP) {
        int32_t e[3]; readIntArr(echoP, e, 3);
        echoRes = (uint8_t)e[0]; echoAmt = e[1]; echoDueEpoch = (uint32_t)e[2];
    }
    readIntArr(afterKey(j, "stores"), stores, RES_COUNT);
    // v3 craft-unlock bitsets. Absent in v1/v2 saves: derive `seen` from the
    // loaded stores (any resource currently >0 counts as seen — the best
    // available proxy for "ever owned"), and leave craftShown=0 so already-
    // eligible craftables re-emit their availableMsg once on the first post-
    // upgrade load (harmless, self-limiting via the latch).
    // perks bitfield — absent in pre-2.4 saves, init()'s 0 stands (readLong of a
    // nullptr key is 0, so this is safe unconditionally).
    perks = (uint32_t)readLong(afterKey(j, "perks"));
    deathAt = (uint32_t)readLong(afterKey(j, "dcool"));   // absent -> 0 (no lockout)
    // Remembered Path outfit — sparse [idx,val,...] pairs; absent on pre-0.9 saves,
    // where init()'s zeroed arrays stand (empty outfit). Zero-padding from readIntArr
    // scatters as (0,0) no-ops; bounds-guarded on the way in.
    {
        int32_t pr[2 * RES_COUNT]; readIntArr(afterKey(j, "oftr"), pr, 2 * RES_COUNT);
        for (int k = 0; k + 1 < 2 * RES_COUNT; k += 2)
            if (pr[k] >= 0 && pr[k] < RES_COUNT && pr[k + 1] > 0)
                savedOutfitRes[pr[k]] = (int16_t)pr[k + 1];
        int32_t pi[2 * ITEM_COUNT]; readIntArr(afterKey(j, "ofti"), pi, 2 * ITEM_COUNT);
        for (int k = 0; k + 1 < 2 * ITEM_COUNT; k += 2)
            if (pi[k] >= 0 && pi[k] < ITEM_COUNT && pi[k + 1] > 0)
                savedOutfitItem[pi[k]] = (int16_t)pi[k + 1];
    }
    const char* seenP = afterKey(j, "seen");
    if (seenP) {
        seen = (uint32_t)readLong(seenP);
        craftShown = (uint32_t)readLong(afterKey(j, "cshow"));   // nullptr -> 0
    } else {
        seen = 0;
        for (int i = 0; i < RES_COUNT; i++) if (stores[i] > 0) seen |= (1u << i);
        craftShown = 0;
    }
    int32_t tmp[32];   // >= max(BLD_COUNT, ITEM_COUNT, JOB_COUNT)
    readIntArr(afterKey(j, "bld"), tmp, BLD_COUNT);
    for (int i = 0; i < BLD_COUNT; i++) buildings[i] = (uint8_t)tmp[i];
    readIntArr(afterKey(j, "itm"), tmp, ITEM_COUNT);
    for (int i = 0; i < ITEM_COUNT; i++) items[i] = (uint8_t)tmp[i];
    readIntArr(afterKey(j, "wrk"), tmp, JOB_COUNT);
    for (int i = 0; i < JOB_COUNT; i++) workers[i] = (uint16_t)tmp[i];

    // log: array of ["key",arg,h] (pre-0.3.1) or ["key",arg,h,count] (0.3.1+).
    // count is OPTIONAL on read: after hasArg, strtol leaves p at whatever
    // follows the digit(s) — ']' for the old 3-field form (default count=1,
    // lossless read of an already-published v2 save), ',' when a 4th field
    // follows.
    logCount = 0;
    const char* p = afterKey(j, "log");
    if (p) {
        p = strchr(p, '[');
        if (p) {
            p++;                          // into the outer array
            while (logCount < LOG_CAP) {
                const char* open = strchr(p, '[');
                const char* close = strchr(p, ']');   // end of outer array?
                if (!open || (close && close < open)) break;
                p = open + 1;
                LogEntry& e = log[logCount];
                if (!readStr(p, e.enKey, LOG_KEY_MAX)) break;
                while (*p && *p != ',') p++; if (*p == ',') p++;
                e.arg = (int32_t)strtol(p, (char**)&p, 10);
                while (*p && *p != ',') p++; if (*p == ',') p++;
                e.hasArg = strtol(p, (char**)&p, 10) != 0;
                e.count = 1;
                while (*p == ' ') p++;
                if (*p == ',') {
                    p++;
                    long c = strtol(p, (char**)&p, 10);
                    if (c >= 1 && c <= 255) e.count = (uint8_t)c;
                }
                logCount++;
                const char* nb = strchr(p, ']');   // close this entry
                if (nb) p = nb + 1;
            }
        }
    }
    return true;
}

// ===================== persistence (platform) =============================

#ifdef ARDUINO
bool GameState::save() const {
    char buf[4096];
    size_t n = toJson(buf, sizeof buf);
    if (n >= sizeof buf) return false;              // truncated -> refuse
    const char* tmp = ADR_SAVE_PATH ".tmp";
    File f = SD.open(tmp, FILE_WRITE);
    if (!f) return false;
    f.write((const uint8_t*)buf, n);
    f.close();
    SD.remove(ADR_SAVE_PATH);                        // atomic tmp+rename
    return SD.rename(tmp, ADR_SAVE_PATH);
}
bool GameState::load() {
    File f = SD.open(ADR_SAVE_PATH, FILE_READ);
    if (!f) return false;
    size_t len = f.size();
    if (len == 0 || len >= 4096) { f.close(); return false; }
    char buf[4096];
    size_t rd = f.read((uint8_t*)buf, len);
    f.close();
    buf[rd] = 0;
    return fromJson(buf);
}
#else   // host build (smoke test): plain stdio
bool GameState::save() const {
    char buf[4096];
    size_t n = toJson(buf, sizeof buf);
    if (n >= sizeof buf) return false;
    const char* tmp = ADR_SAVE_PATH ".tmp";
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    fwrite(buf, 1, n, f);
    fclose(f);
    remove(ADR_SAVE_PATH);
    return rename(tmp, ADR_SAVE_PATH) == 0;
}
bool GameState::load() {
    FILE* f = fopen(ADR_SAVE_PATH, "rb");
    if (!f) return false;
    char buf[4096];
    size_t rd = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[rd] = 0;
    return fromJson(buf);
}
#endif

}  // namespace adr
