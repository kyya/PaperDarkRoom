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
    cdFire = cdGather = cdTraps = 0;
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
    if (logCount >= LOG_CAP) {        // drop oldest, shift down
        memmove(&log[0], &log[1], sizeof(LogEntry) * (LOG_CAP - 1));
        logCount = LOG_CAP - 1;
    }
    LogEntry& e = log[logCount++];
    snprintf(e.enKey, LOG_KEY_MAX, "%s", enKey);
    e.arg = arg;
    e.hasArg = hasArg;
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

int GameState::cooldownLeft(int action, uint32_t now) const {
    uint32_t last = action == 0 ? cdFire : action == 1 ? cdGather : cdTraps;
    int cd = action == 0 ? STOKE_COOLDOWN_S
           : action == 1 ? GATHER_DELAY_S : TRAPS_DELAY_S;
    if (last == 0 || now < last) return 0;
    uint32_t el = now - last;
    return el >= (uint32_t)cd ? 0 : (cd - (int)el);
}

// ===================== settle (offline / awake economy) ===================

uint32_t GameState::settle(uint32_t nowEpoch) {
    if (lastSettleTs == 0) { lastSettleTs = nowEpoch; return 0; }
    if (nowEpoch <= lastSettleTs) return 0;
    uint32_t elapsed = nowEpoch - lastSettleTs;
    uint32_t steps = elapsed / INCOME_TICK_S;
    const uint32_t maxSteps = SETTLE_MAX_S / INCOME_TICK_S;   // 8640 (24h)
    bool capped = false;
    if (steps > maxSteps) { steps = maxSteps; capped = true; }
    for (uint32_t i = 0; i < steps; i++) stepOnce();
    if (capped) lastSettleTs = nowEpoch;
    else        lastSettleTs += steps * INCOME_TICK_S;
    // A Wanderer echo that came due while offline is redeemed on wake as a lump
    // (its payout does not retroactively feed the just-simulated economy).
    redeemDelayedEcho(nowEpoch);
    return steps;
}

void GameState::stepOnce() {
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

    // -- fire cooling: FROZEN offline by default (research.md §5.3) --
#if ADR_FIRE_OFFLINE_DECAY
    if (fire > FIRE_DEAD) {
        tFireCool -= INCOME_TICK_S;
        if (tFireCool <= 0) {
            // builder auto-stokes a low fire if wood remains (room.js coolFire)
            if (fire <= FIRE_FLICKERING && builderLevel > 3 &&
                stores[R_WOOD] > 0) {
                stores[R_WOOD] -= 1 * FP;
                if (fire < FIRE_ROARING) fire++;
            }
            if (fire > FIRE_DEAD) fire--;
            tFireCool = FIRE_COOL_S;
        }
    }
#endif

    // -- worker income (every 10s tick), all-or-nothing per source --
    applyIncomeSource(J_GATHERER, numGatherers());
    for (int j = J_HUNTER; j < JOB_COUNT; j++)
        applyIncomeSource(j, (int)workers[j]);
    if (builderLevel >= 4)             // builder Helping: +2 wood / tick
        stores[R_WOOD] += BUILDER_WOOD_DFP;

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
    for (int i = 0; i < d.n; i++)
        stores[d.items[i].res] += d.items[i].dfp * count;
}

void GameState::adjustTemp() {
    if (temp > TEMP_FREEZING && temp > fire) temp--;
    if (temp < TEMP_HOT && temp < fire) temp++;
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

    // cost (trap/hut scale wood by existing count)
    for (int i = 0; i < 3 && c.cost[i].res != RA_END; i++) {
        int need = c.cost[i].amt;
        if (c.cost[i].res == R_WOOD) need += (int)c.woodIncrPerN * count;
        if (stores[c.cost[i].res] < need * FP) return RC_ERR_COST;
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
    for (int i = 0; i < 3 && g.cost[i].res != RA_END; i++)
        if (stores[g.cost[i].res] < g.cost[i].amt * FP) return RC_ERR_COST;
    for (int i = 0; i < 3 && g.cost[i].res != RA_END; i++)
        stores[g.cost[i].res] -= g.cost[i].amt * FP;
    stores[g.product] += 1 * FP;
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
    int flags = (outsideUnlocked ? 1 : 0) | (craftablesUnlocked ? 2 : 0) |
                (woodSeen ? 4 : 0) | (seenForest ? 8 : 0) |
                (needWoodActive ? 16 : 0);
    AP("\"fl\":%d,", flags);
    AP("\"cd\":[%lu,%lu,%lu],", (unsigned long)cdFire,
       (unsigned long)cdGather, (unsigned long)cdTraps);
    AP("\"tm\":[%d,%d,%d,%d,%d],", tTemp, tBuilder, tNeedWood, tFireCool, tPop);
    AP("\"nev\":%lu,", (unsigned long)nextEventAt);
    AP("\"echo\":[%d,%ld,%lu],", echoRes, (long)echoAmt,
       (unsigned long)echoDueEpoch);
    AP("\"stores\":[");
    for (int i = 0; i < RES_COUNT; i++) AP("%s%ld", i ? "," : "", (long)stores[i]);
    AP("],\"bld\":[");
    for (int i = 0; i < BLD_COUNT; i++) AP("%s%d", i ? "," : "", buildings[i]);
    AP("],\"itm\":[");
    for (int i = 0; i < ITEM_COUNT; i++) AP("%s%d", i ? "," : "", items[i]);
    AP("],\"wrk\":[");
    for (int i = 0; i < JOB_COUNT; i++) AP("%s%u", i ? "," : "", workers[i]);
    AP("],\"log\":[");
    for (int i = 0; i < logCount; i++) {
        if (i) AP(",");
        AP("[");
        apStr(out, cap, o, log[i].enKey);
        AP(",%ld,%d]", (long)log[i].arg, log[i].hasArg ? 1 : 0);
    }
    AP("]}");
#undef AP
    return o;
}

bool GameState::fromJson(const char* j) {
    if (!j) return false;
    long v = readLong(afterKey(j, "v"));
    if (v != 1 && v != 2) return false;  // accept v1 (pre-events) AND v2 saves
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
    int32_t cd[3];  readIntArr(afterKey(j, "cd"), cd, 3);
    cdFire = (uint32_t)cd[0]; cdGather = (uint32_t)cd[1]; cdTraps = (uint32_t)cd[2];
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
    int32_t tmp[32];   // >= max(BLD_COUNT, ITEM_COUNT, JOB_COUNT)
    readIntArr(afterKey(j, "bld"), tmp, BLD_COUNT);
    for (int i = 0; i < BLD_COUNT; i++) buildings[i] = (uint8_t)tmp[i];
    readIntArr(afterKey(j, "itm"), tmp, ITEM_COUNT);
    for (int i = 0; i < ITEM_COUNT; i++) items[i] = (uint8_t)tmp[i];
    readIntArr(afterKey(j, "wrk"), tmp, JOB_COUNT);
    for (int i = 0; i < JOB_COUNT; i++) workers[i] = (uint16_t)tmp[i];

    // log: array of ["key",arg,h]
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
