// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room — Phase 1 game state engine (Room + Outside core loop). Ports the
// upstream room.js / outside.js / state_manager.js logic: fire/temperature/
// builder state machines, the 10-job worker economy with partial-crew income
// (部分产出), craftables/buildings/trade, and — the deep-sleep centerpiece — a
// fixed-point, 10s-step offline settle() shared by the awake tick and the
// cold-boot wake (research.md §5). Stores are integer × adr::FP so fractional
// income (hunter +0.5/tick) never drifts across 8640 offline steps.
//
// Arduino independence: all game logic is pure; only save()/load() touch the
// platform. Under ARDUINO they use the SD atomic tmp+rename pattern
// (frame_store parity); on a host build (tools/adr_smoke.cpp) they use stdio.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "game_data.h"

namespace adr {

// ---- Perks (character.perks) ----------------------------------------------
// Permanent World-survival buffs, granted by setpieces and combat milestones
// (upstream $SM.addPerk). Persistent (survive death + future expeditions),
// stored as a bitfield. Phase 2 grants only gastronome (the swamp); the rest of
// the upstream roster (slow metabolism / desert rat / scout / precise / …) are
// reserved slots, wired as they gain a Phase-2/3 source.
enum Perk : uint8_t { PK_GASTRONOME = 0, PERK_COUNT };

// Result of an action API call.
enum Result : uint8_t {
    RC_OK = 0,
    RC_ERR_COOLDOWN,   // button still cooling
    RC_ERR_COST,       // not enough resources
    RC_ERR_LOCKED,     // not unlocked yet (builder/workshop/building/trading post)
    RC_ERR_MAX,        // already at maximum
    RC_ERR_COLD,       // room too cold — "builder just shivers"
    RC_ERR_INVALID,    // bad argument
};

struct BuyResult {
    Result status;
    int requested;
    int purchased;
};

// One log line: an en_key (rendered via tr() at draw time — never a baked
// sentence, keeping the §8.3 glyph closure complete) plus an optional integer
// argument (e.g. villagers arrived) the renderer may splice into a {0} slot.
// The key is copied into a fixed buffer, not held as a literal pointer, so the
// log survives deep sleep (literal addresses shift across a reflash/reboot).
// count (v0.3.1): pushLog() collapses a repeat of the newest entry (same
// enKey + arg/hasArg) into this counter instead of scrolling a duplicate line
// — e.g. repeatedly long-pressing a cost-disabled band no longer floods the
// log with "not enough wood", it becomes one line ticking up to "...x3".
constexpr int LOG_KEY_MAX = 96;
// Joiner for a COMPOUND key — one entry whose text is several en_keys strung
// together (game_state.cpp checkTraps: the "the traps contain " intro plus one
// key per drop caught this round). Storing the pieces rather than a baked
// sentence keeps the §8.3 glyph closure intact; room_page.cpp logText() splits
// here and tr()s each segment. '|' is the joiner because no en_key in
// strings_zh.h contains it and it needs no escaping in the toJson/fromJson
// string codec (which only escapes '"' and '\\').
constexpr char LOG_KEY_SEP = '|';
struct LogEntry {
    char    enKey[LOG_KEY_MAX];
    int32_t arg;
    bool    hasArg;
    uint8_t count;    // 1 for a fresh entry; capped at 99
};

// 8 -> 16 (v0.16): the bottom-anchored button grid gave the log band up to 23
// lines early game, so an 8-entry ring ran out of history long before it ran out
// of room — the opposite of upstream, whose notification column scrolls a deep
// backlog. No SAVE_VER bump: toJson writes the ring as a variable-length "log"
// array and fromJson reads entries until the array closes OR the cap is hit, so
// an 8-entry save loads into a 16-slot ring untouched (and a 16-entry save read
// by older firmware simply truncates at 8).
constexpr int   LOG_CAP    = 16;
constexpr int   SAVE_VER   = 4;    // v4 appends an FNV-1a checksum. v1–v3 still
                                   // load; missing optional fields keep defaults.
constexpr uint8_t ECHO_NONE = 0xFF;  // delayedEcho.res sentinel: slot empty
#ifndef ADR_SAVE_PATH
#define ADR_SAVE_PATH "/.darkroom/adr_save.json"
#endif
#ifndef ADR_SAVE_TMP_PATH
#define ADR_SAVE_TMP_PATH ADR_SAVE_PATH ".tmp"
#endif
#ifndef ADR_SAVE_BAK_PATH
#define ADR_SAVE_BAK_PATH ADR_SAVE_PATH ".bak"
#endif

// Compile switch: keep the fire frozen while the device deep-sleeps (default),
// or cool it by real elapsed time. Off (0) is the recommended default —
// research.md §5.3: a 15min wake cycle vs a 5min fire cool would extinguish the
// fire every single wake, turning "tend the fire" into pure punishment.
#ifndef ADR_FIRE_OFFLINE_DECAY
#define ADR_FIRE_OFFLINE_DECAY 0
#endif

class GameState {
public:
    // ---- persistent state ----
    int32_t  stores[RES_COUNT];      // fixed point × FP
    uint8_t  buildings[BLD_COUNT];
    uint8_t  items[ITEM_COUNT];
    uint16_t workers[JOB_COUNT];     // assigned (gatherer slot derived, unused)
    uint16_t population;
    uint8_t  fire;                   // Fire enum
    uint8_t  temp;                   // Temp enum
    int8_t   builderLevel;           // -1..4

    // feature flags
    bool outsideUnlocked;            // Outside module live (post unlockForest)
    bool craftablesUnlocked;         // builder Helping (level 4)
    bool woodSeen;                   // stores.wood is a real number (not "free")
    bool seenForest;                 // first Outside arrival notice shown

    // Craftable progressive-unlock bitsets (room.js craftUnlocked, v0.4.3).
    // seen: bit r set once stores[r] has ever been >0 — a craftable's non-wood
    // cost materials must all be "seen" before its button appears (RES_COUNT<=32).
    // craftShown: bit c set once craftable c's availableMsg has been pushed (==
    // upstream Room.buttons memo) so it unlocks — and notifies — exactly once
    // (CRAFT_COUNT<=32).
    uint32_t seen;
    uint32_t craftShown;

    // Permanent perks bitfield (Perk enum). Granted by World setpieces (gastronome
    // = swamp) — persists across death and expeditions. 0 for a fresh/old save.
    uint32_t perks;

    // Epoch of the wanderer's last death (0 = none / already expired). embark is
    // locked for World.DEATH_COOLDOWN_S seconds after it (§1.5/§3.4). Epoch-based
    // (same clock as settle) so a deep sleep simply expires it; recorded UI-side
    // at the death frame, enforced in WorldState::embark. Optional in game.json
    // (absent on pre-2.5 saves -> 0).
    uint32_t deathAt;

    // Persistent Path outfit (upstream $SM 'outfit'): the loadout remembered from
    // the last goHome so the Path panel pre-fills it next embark instead of making
    // the player re-pack from zero every trip (§3.5 leaveItAtHome). goHome writes
    // the RETAINED portion here (supplies/weapons/gear stay; raw loot drops off).
    // Sparse — only ever the ~handful of keep-slots are non-zero. Whole units,
    // indexed like stores/items. Optional in game.json (absent on pre-0.9 saves ->
    // all zero == empty outfit, no SAVE_VER bump).
    //
    // DESIGN DECISION — deliberate divergence from upstream: death does NOT clear
    // this. Upstream die() does $SM.remove('outfit'), wiping the memory too. We
    // keep it. Rationale: the death penalty IS the loss of the Expedition's PHYSICAL
    // bag (world_state die() empties ex.outfit — unchanged); savedOutfit is only the
    // next-trip pre-fill MEMORY. Wiping it adds no penalty, just re-packing tedium
    // (real playtest pain: dying one step from home). And prefillOutfit's
    // min(remembered, stock) clamp means a death can never conjure resources the
    // player no longer owns — so keeping the memory is free of exploit.
    int16_t  savedOutfitRes[RES_COUNT];
    int16_t  savedOutfitItem[ITEM_COUNT];

    // cooldown last-press epochs (0 = ready). Light & stoke share one button.
    uint32_t cdFire, cdGather, cdTraps;

    // settle step timers (seconds remaining to next event)
    int32_t tTemp, tBuilder, tNeedWood, tFireCool, tPop;
    bool     needWoodActive;         // NEED_WOOD countdown armed (level 1)

    uint32_t lastSettleTs;           // epoch of last settled 10s boundary
    uint32_t rng;                    // deterministic PRNG (traps / pop / events)

    // ---- random-event persistence (v0.3.0, SAVE_VER 2) ----
    // Epoch the next event is due (0 = unscheduled -> the engine rerolls on the
    // first tick; also the v1-migration default). Events run only while awake
    // (research.md §5.4); the on-screen scene machine itself is NOT persisted.
    uint32_t nextEventAt;
    // Mysterious Wanderer delayed echo: a single pending payout (res==ECHO_NONE
    // when empty), redeemed at dueEpoch in settle()/tick — offline too.
    uint8_t  echoRes;                // Res, or ECHO_NONE
    int32_t  echoAmt;                // whole units to grant
    uint32_t echoDueEpoch;           // epoch the payout is due

    LogEntry log[LOG_CAP];
    uint8_t  logCount;               // linear: [0..logCount), newest last,
                                     // oldest dropped on overflow

    // ---- runtime only — deliberately NOT persisted ----
    // Bit j set while job j has already been TOLD about (its inputs could not
    // feed the whole crew). The idle notice fires on TRANSITION only: a 10s
    // production tick that logged every idle tick would bury the log ring and
    // thrash the e-ink, and the idle HEADCOUNT changing while still short is
    // deliberately silent too (a wobbling supply chain must not re-announce).
    // Not written by toJson / not read by fromJson and no SAVE_VER bump — the
    // save is a hand-written field list, so a runtime member is simply absent
    // from it, and re-announcing a shortage that still stands once after a
    // reboot is the better behavior anyway. init() (which fromJson calls first)
    // clears it, so a loaded game always starts from "nobody told yet".
    uint16_t idleJobs;

    // ---- lifecycle ----
    void init();                         // fresh new game (dark room, fire dead)
    bool load();                         // read save; false -> caller inits new
    bool save() const;                   // atomic persist

    // Pure (de)serialization — used by save/load AND the host smoke test.
    size_t toJson(char* out, size_t cap) const;
    bool   fromJson(const char* json);

    // ---- simulation ----
    // Advance passive economy from lastSettleTs to nowEpoch in 10s steps.
    // Shared by the awake tick and the cold-boot wake. Caps a single call at
    // 24h. Returns the number of steps simulated.
    // offline=true marks a large deep-sleep catch-up (main.cpp wake restore):
    // the fire (and its cool timer tFireCool) is FROZEN across those steps
    // unless ADR_FIRE_OFFLINE_DECAY. Awake page ticks pass offline=false, so
    // the fire cools by real elapsed time (research.md §5.3).
    uint32_t settle(uint32_t nowEpoch, bool offline = false);

    // room.js craftUnlocked(thing): is craftable c offered now? Gates on builder
    // Helping, workshop (tools/weapons), >=50% of the wood cost, and every other
    // cost material "seen" (the seen bitset). The FIRST time all conditions hold
    // it pushes the craftable's availableMsg and latches craftShown, so it is
    // NOT const (mutates craftShown / the log). Cost is not re-checked here —
    // an unaffordable-but-unlocked button still shows (see room_page).
    bool craftUnlocked(uint8_t craftId);

    // room.js buyUnlocked(thing): is trade good `tradeId` offered now? Requires
    // the trading post built AND (the good is compass — always offered — OR its
    // product resource has been seen). Also hidden once a capped good (compass,
    // max 1) is owned (room.js goodsMax). Const: unlike craftUnlocked, upstream
    // trade goods carry no availableMsg/maxMsg, so nothing is pushed or latched.
    bool buyOfferable(uint8_t tradeId) const;

    // ---- action API (校验成本/冷却/解锁, writes log, returns Result) ----
    Result lightFire(uint32_t now);
    Result stokeFire(uint32_t now);
    Result gatherWood(uint32_t now);
    Result checkTraps(uint32_t now);
    Result build(uint8_t craftId);          // buildings
    Result craft(uint8_t craftId);          // tools/upgrades/weapons
    // How many of this good can be bought right now (0 if the post is down,
    // at maximum, or none of the cost lines cover even one). Used by the
    // Trade page for dashed/enabled state; buy() uses the same number.
    int maxBuyable(uint8_t tradeId) const;
    // Buy `requested` of a trading-post good, TRUNCATING to maxBuyable():
    // a ×10 press with only 7 affordable buys 7 and returns RC_OK with
    // purchased=7. RC_ERR_COST only when not even one is affordable.
    // Visibility (hasSeen) stays in buyOfferable — buy() itself does not
    // re-check seen, matching upstream room.js buy().
    BuyResult buy(uint8_t tradeId, int requested = 1);
    Result assignWorker(uint8_t job, int delta);  // +/- villagers to a job

    // ---- read helpers ----
    int32_t whole(uint8_t res) const { return stores[res] / FP; }  // display units
    // Has resource `res` ever been owned (>0)? The seen bitset latched in
    // markSeen — the shared "已见过" judgement behind both craftUnlocked (craft
    // material gate) and buyOfferable (trade good gate).
    bool hasSeen(uint8_t res) const { return res < RES_COUNT && (seen & (1u << res)); }
    // Latch the seen bit for a stores GAIN. Called at every internal gain site
    // and also from the event engine's reward path (addStores) and the GM
    // adr:give injection — anything that grants a resource means it's been
    // "owned" (upstream $SM.add defines the store key), which unlocks the
    // craftUnlocked / buyOfferable gates for it.
    void markSeen(uint8_t res) { if (res < RES_COUNT) seen |= (1u << res); }
    // Permanent perk query / grant (Perk enum). addPerk is idempotent.
    bool hasPerk(uint8_t p) const { return p < 32 && (perks & (1u << p)); }
    void addPerk(uint8_t p) { if (p < 32) perks |= (1u << p); }
    // Wipe the remembered Path outfit. Only a fresh game (init) clears it — NOT
    // death (see the savedOutfit design-decision note above: we deliberately keep
    // the pre-fill memory across a death, unlike upstream's die() $SM.remove).
    void clearSavedOutfit() {
        for (int i = 0; i < RES_COUNT; i++)  savedOutfitRes[i]  = 0;
        for (int i = 0; i < ITEM_COUNT; i++) savedOutfitItem[i] = 0;
    }
    uint16_t maxPopulation() const { return buildings[B_HUT] * HUT_ROOM; }
    int      numGatherers() const;
    // The P1-assignable jobs currently unlocked — a job is offerable once its
    // required building stands (JOB_REQ_BLD); miners (BLD_NONE) never appear.
    // Fills `out` with up to `cap` Job ids and returns the count. THE single
    // source of the "which jobs exist" filter: the Outside worker summary,
    // AssignPage's job bands, and the Outside 分工 entry gate all read it, so
    // none can drift. hasUnlockedJob() is the buffer-free count>0 shortcut the
    // 分工 gate uses.
    int      unlockedJobs(uint8_t* out, int cap) const;
    bool     hasUnlockedJob() const;
    // Seconds of cooldown left on a button (0 = ready). action: 0 fire, 1 gather,
    // 2 traps.
    int cooldownLeft(int action, uint32_t now) const;

    void pushLog(const char* enKey, int32_t arg = 0, bool hasArg = false);

    // Deterministic PRNG (public so event_engine draws from the same stream —
    // keeps event branches reproducible for a fixed seed, research §5.2).
    uint32_t nextRand();                     // xorshift, [0, 2^32)
    int      rand1000();                     // [0, 1000)

    // ---- random-event side effects (ported from outside.js) ----
    // Reduce population by num (clamped at 0), then strip assigned workers so
    // the derived gatherer count never goes negative (killVillagers parity).
    void killVillagers(int num);
    // Raze `num` occupied huts, killing their residents; returns victims total
    // (destroyHuts parity, allowEmpty=false).
    int  destroyHuts(int num);

    // ---- Mysterious Wanderer delayed echo ----
    // Arm the single echo slot (overwrites any pending one). res==ECHO_NONE to
    // clear. amtWhole is in whole units.
    void armDelayedEcho(uint8_t res, int32_t amtWhole, uint32_t dueEpoch);
    // If armed and nowEpoch >= dueEpoch, grant the payout, log the wanderer's
    // return, clear the slot, and return true. Called from settle() (offline)
    // and events::tick (awake).
    bool redeemDelayedEcho(uint32_t nowEpoch);

private:
    Result makeCraftable(uint8_t craftId);   // shared build/craft core
    void   onFireChange();
    void   stepOnce(bool offline);           // one 10s passive tick
    // offline: a deep-sleep catch-up runs this up to 8640 times, so it carries
    // the flag through purely to suppress the idle-crew log (see stepOnce).
    void   applyIncomeSource(uint8_t job, int count, bool offline);
    void   adjustTemp();
    void   advanceBuilder();
    void   increasePopulation();
    void   unlockForest();
};

}  // namespace adr
