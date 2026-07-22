// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room — random-event engine implementation. See event_engine.h. Pure
// logic (no Arduino/M5) so it host-compiles into the smoke test. All randomness
// draws from the bound GameState's deterministic RNG so branches are
// reproducible for a fixed seed.
#include "event_engine.h"

namespace events {

using namespace adr;

// ---- scheduler tuning ----
// Upstream _EVENT_TIME_RANGE = [3,6] min; draw = floor(rand*(6-3))+3 => 3..5 min.
static const int EVENT_MIN_MIN = 3;
static const int EVENT_MAX_MIN = 6;
// A gap larger than this between two ticks means we slept (awake ticks are ~1s
// apart; deep sleep is ~15 min). Used to apply the §5.4 wake grace.
static const uint32_t WAKE_GAP_S = 120;

// ---- runtime (non-persistent) state ----
static GameState* gs            = nullptr;
static int        s_activeEvent = -1;    // EventId, or -1
static int        s_scene       = -1;    // global scene index, or -1
static uint32_t   s_epoch       = 0;     // last epoch seen (for choose()/echo)
static uint32_t   s_lastTick    = 0;     // epoch of the previous tick (0 = fresh)

// ---- helpers -------------------------------------------------------------

// floor(rand[0,1) * n), matching upstream Math.floor(Math.random()*n).
static int randBelow(int n) {
    if (n <= 1) return 0;
    return (int)((gs->rand1000() * (uint32_t)n) / 1000u);
}

static bool isAvailable(int eventId) {
    const EventDef& e = EVENTS[eventId];
    switch (e.avail) {
        case AV_ROOM_FUR:  return gs->stores[R_FUR]  > 0;
        case AV_ROOM_WOOD: return gs->stores[R_WOOD] > 0;
        case AV_ROOM_HUT_RANGE:
            return gs->buildings[B_HUT] >= e.availArg1 &&
                   gs->buildings[B_HUT] <  e.availArg2;
        case AV_OUT_TRAP:
            return gs->outsideUnlocked && gs->buildings[B_TRAP] > 0;
        case AV_OUT_HUT_POP:
            return gs->outsideUnlocked && gs->buildings[B_HUT] > 0 &&
                   (int32_t)gs->population > e.availArg1;
        case AV_OUT_POP:
            return gs->outsideUnlocked && (int32_t)gs->population > e.availArg1;
        case AV_ROOM_MED:
            return gs->stores[R_MEDICINE] > 0;
        case AV_OUT_POP_RANGE_MED:
            return gs->outsideUnlocked &&
                   (int32_t)gs->population > e.availArg1 &&
                   (int32_t)gs->population < e.availArg2 &&
                   gs->stores[R_MEDICINE] > 0;
        case AV_OUT_POP_MED:
            return gs->outsideUnlocked &&
                   (int32_t)gs->population > e.availArg1 &&
                   gs->stores[R_MEDICINE] > 0;
    }
    return false;
}

// Run a scene's onLoad side effect (effect code + param).
static void applyEffect(const SceneDef& sc) {
    switch (sc.effect) {
        case EFF_WOOD_PCT: {
            int32_t woodWhole = gs->stores[R_WOOD] / FP;
            int numWood = (int)(woodWhole / 10);        // floor(10%)
            if (numWood == 0) numWood = 1;
            int numRes = numWood / 5;
            if (numRes == 0) numRes = 1;
            gs->stores[R_WOOD] -= (int32_t)numWood * FP;
            if (gs->stores[R_WOOD] < 0) gs->stores[R_WOOD] = 0;
            gs->stores[sc.effectArg] += (int32_t)numRes * FP;
            break;
        }
        case EFF_ADD_HUT:
            if (gs->buildings[B_HUT] < 20) gs->buildings[B_HUT]++;
            break;
        case EFF_WRECK_TRAPS: {
            int t = gs->buildings[B_TRAP];
            if (t > 0) {
                int n = randBelow(t) + 1;               // 1..t
                if (n > t) n = t;
                gs->buildings[B_TRAP] = (uint8_t)(t - n);
            }
            break;
        }
        case EFF_DESTROY_HUT:
            gs->destroyHuts((int)sc.effectArg);
            break;
        case EFF_KILL_VILLAGERS: {
            int n = randBelow((int)sc.effectArg) + 1;   // 1..arg
            gs->killVillagers(n);
            break;
        }
        case EFF_KILL_POP_HALF: {                        // Sickness death
            int half = (int)gs->population / 2;
            int n = randBelow(half) + 1;                 // 1..floor(pop/2)
            gs->killVillagers(n);
            break;
        }
        case EFF_KILL_RANGE: {                           // Plague healed/death
            int base = (int)((uint32_t)sc.effectArg >> 16);
            int span = (int)((uint32_t)sc.effectArg & 0xFFFFu);
            int n = randBelow(span) + base;              // base..base+span-1
            gs->killVillagers(n);
            break;
        }
        default: break;
    }
}

static void addStores(const ResAmt* list) {
    for (int i = 0; i < 3 && list[i].res != RA_END; i++) {
        gs->stores[list[i].res] += list[i].amt * FP;
        // An event reward means the resource has now been "owned" (upstream
        // $SM.add defines the store key), which unlocks its craft/buy gates —
        // e.g. the Sick Man's scales/alloy/cell, the Plague's bought medicine.
        if (list[i].amt > 0) gs->markSeen(list[i].res);
    }
}

// Enter a scene: onLoad effect, notification, reward, delayed-echo arm.
static void loadScene(int sceneIdx) {
    s_scene = sceneIdx;
    const SceneDef& sc = SCENES[sceneIdx];
    applyEffect(sc);
    if (sc.notifyKey) gs->pushLog(sc.notifyKey);
    addStores(sc.reward);
    if (sc.echo.probMilli > 0 && gs->rand1000() < sc.echo.probMilli)
        gs->armDelayedEcho(sc.echo.res, sc.echo.amt, s_epoch + 60);
}

static void endEvent() {
    s_activeEvent = -1;
    s_scene = -1;
    // Schedule the following event (upstream schedules right after a trigger).
    int mins = EVENT_MIN_MIN + randBelow(EVENT_MAX_MIN - EVENT_MIN_MIN);
    gs->nextEventAt = s_epoch + (uint32_t)mins * 60;
}

// ---- lifecycle -----------------------------------------------------------

void reset() {
    s_activeEvent = -1;
    s_scene = -1;
    s_lastTick = 0;
}

void bind(GameState* g) {
    gs = g;
    reset();
}

// ---- scheduler -----------------------------------------------------------

bool startEvent(int eventId, uint32_t epochNow) {
    if (!gs || eventId < 0 || eventId >= EVENT_COUNT) return false;
    if (!isAvailable(eventId)) return false;
    s_epoch = epochNow;
    s_activeEvent = eventId;
    loadScene(EVENTS[eventId].sceneStart);
    return true;
}

static bool tryTrigger(uint32_t epochNow) {
    uint8_t pool[EVENT_COUNT];
    int n = 0;
    for (int i = 0; i < EVENT_COUNT; i++)
        if (isAvailable(i)) pool[n++] = (uint8_t)i;
    if (n == 0) return false;
    int pick = randBelow(n);
    if (pick >= n) pick = n - 1;
    return startEvent(pool[pick], epochNow);
}

void tick(uint32_t nowMs, uint32_t epochNow) {
    (void)nowMs;
    if (!gs) return;

    bool wokeGap = (s_lastTick == 0) || (epochNow > s_lastTick + WAKE_GAP_S);
    s_lastTick = epochNow;
    s_epoch = epochNow;

    // Redeem a due delayed echo (the awake path; offline is caught in settle()).
    gs->redeemDelayedEcho(epochNow);

    if (s_activeEvent >= 0) return;         // event on screen: freeze scheduler

    if (gs->nextEventAt == 0) {             // fresh game / v1-migrated save: seed
        int mins = EVENT_MIN_MIN + randBelow(EVENT_MAX_MIN - EVENT_MIN_MIN);
        gs->nextEventAt = epochNow + (uint32_t)mins * 60;
        return;
    }

    if (epochNow >= gs->nextEventAt) {
        if (wokeGap) {
            // Slept past the scheduled time: don't fire on the instant of waking
            // (research.md §5.4). Grant 60..120s of quiet, then fire.
            gs->nextEventAt = epochNow + 60 + (gs->nextRand() % 61u);
            return;
        }
        if (!tryTrigger(epochNow)) {
            // No available event: retry sooner (upstream scale 0.5).
            int mins = EVENT_MIN_MIN + randBelow(EVENT_MAX_MIN - EVENT_MIN_MIN);
            uint32_t half = (uint32_t)(mins * 60) / 2;
            if (half < 30) half = 30;
            gs->nextEventAt = epochNow + half;
        }
        // If an event started, endEvent() will schedule the next one on exit.
    }
}

// ---- commands ------------------------------------------------------------

static bool affordable(const BtnDef& b) {
    for (int i = 0; i < 3 && b.cost[i].res != RA_END; i++)
        if (gs->stores[b.cost[i].res] < b.cost[i].amt * FP) return false;
    return true;
}

Result choose(int localBtn) {
    if (!gs || s_activeEvent < 0) return RC_ERR_INVALID;
    const SceneDef& sc = SCENES[s_scene];
    if (localBtn < 0 || localBtn >= sc.btnCount) return RC_ERR_INVALID;
    const BtnDef& b = BTNS[sc.btnStart + localBtn];

    if (!affordable(b)) return RC_ERR_COST;
    // cost
    for (int i = 0; i < 3 && b.cost[i].res != RA_END; i++)
        gs->stores[b.cost[i].res] -= b.cost[i].amt * FP;
    // reward
    addStores(b.reward);
    // notification
    if (b.notifyKey) gs->pushLog(b.notifyKey);

    // next scene
    if (b.next == SCENE_STAY) {
        return RC_OK;                        // repeat trade; stay put
    } else if (b.next == SCENE_END) {
        endEvent();
    } else if (b.next == SCENE_PROB) {
        int roll = gs->rand1000();
        uint8_t dest = PROBS[b.probStart + b.probCount - 1].scene;  // fallback
        for (int i = 0; i < b.probCount; i++) {
            if (roll < PROBS[b.probStart + i].thresholdMilli) {
                dest = PROBS[b.probStart + i].scene;
                break;
            }
        }
        loadScene(dest);
    } else {
        loadScene(b.next);                   // fixed scene index
    }
    return RC_OK;
}

void dismissDefault() {
    if (!gs || s_activeEvent < 0) return;
    choose(SCENES[s_scene].defaultBtn);      // no-cost safe exit -> ends event
}

// ---- queries -------------------------------------------------------------

bool active()             { return s_activeEvent >= 0; }
int  currentEventId()     { return s_activeEvent; }
int  currentScene()       { return s_scene; }
uint32_t nextEventAt()    { return gs ? gs->nextEventAt : 0; }

const char* eventTitleKey() {
    return s_activeEvent >= 0 ? EVENTS[s_activeEvent].titleKey : nullptr;
}

int sceneTextCount() {
    if (s_scene < 0) return 0;
    int n = 0;
    while (n < 4 && SCENES[s_scene].textKeys[n]) n++;
    return n;
}
const char* sceneTextKey(int i) {
    if (s_scene < 0 || i < 0 || i >= 4) return nullptr;
    return SCENES[s_scene].textKeys[i];
}

int btnCount() { return s_scene < 0 ? 0 : SCENES[s_scene].btnCount; }

const char* btnTextKey(int localBtn) {
    if (s_scene < 0 || localBtn < 0 || localBtn >= SCENES[s_scene].btnCount)
        return nullptr;
    return BTNS[SCENES[s_scene].btnStart + localBtn].textKey;
}

bool btnAvailable(int localBtn) {
    if (s_scene < 0 || localBtn < 0 || localBtn >= SCENES[s_scene].btnCount)
        return false;
    return affordable(BTNS[SCENES[s_scene].btnStart + localBtn]);
}

const ResAmt* btnCost(int localBtn) {
    if (s_scene < 0 || localBtn < 0 || localBtn >= SCENES[s_scene].btnCount)
        return nullptr;
    return BTNS[SCENES[s_scene].btnStart + localBtn].cost;
}

int defaultBtnIndex() {
    return s_scene < 0 ? -1 : (int)SCENES[s_scene].defaultBtn;
}

}  // namespace events
