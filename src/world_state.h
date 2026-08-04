// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room — Phase 2 World state engine (map + expedition). Ports the pure
// logic of upstream script/world.js: deterministic map generation (stickiness
// terrain + landmark annulus placement), diamond visibility, road drawing, the
// doSpace() move loop (food/water upkeep, starvation/thirst death, goHome
// commit / die discard). Combat encounters are LEFT AS A HOOK — move() reports
// STEP_FIGHT / STEP_LANDMARK and the setpiece + real-time combat engine lands in
// milestone 2.3.
//
// STATE LAYERING (milestone hard constraint): the 61x61 map NEVER enters the
// 4096-byte main JSON save. It lives in two independent SD binaries:
//   * world.bin — the COMMITTED map (seed + tiles + fog): generated once, only
//     rewritten by goHome. This is game.world in upstream ($SM 'game.world').
//   * trek.bin  — the VOLATILE expedition (position/hp/water/bag + a WORKING
//     copy of the map it is mutating): saved EVERY step, deleted on goHome/die.
//     Its presence at cold boot == "an expedition was interrupted, resume it"
//     (the device sleeps by fully powering off; a wake is a cold boot).
// goHome copies working -> committed (cleared dungeons + revealed fog persist);
// die discards the working copy entirely (this trip's map changes are lost) and
// empties the bag — matching upstream's World.state = null + Path.outfit = {}.
//
// Arduino independence: all logic is pure; only saveWorld/loadWorld/saveTrek/
// loadTrek/clearTrek touch the platform (SD under ARDUINO, stdio on host, same
// tmp+rename atomic pattern as frame_store / game_state).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "world_data.h"
#include "combat_data.h"
#include "game_state.h"

namespace adr {

#ifndef ADR_WORLD_PATH
#define ADR_WORLD_PATH "/.darkroom/world.bin"
#endif
#ifndef ADR_TREK_PATH
#define ADR_TREK_PATH  "/.darkroom/trek.bin"
#endif

constexpr uint32_t WORLD_MAGIC = 0x314C5257;  // "WRL1" LE — world.bin
constexpr uint32_t TREK_MAGIC  = 0x314B5254;  // "TRK1" LE — trek.bin
constexpr uint8_t  WORLD_VER   = 2;   // v2 adds the used-outpost mask (v1 migrates)
// v2 = the Phase-3c Res/Item growth. trek.bin stores the outfit as two FIXED-LENGTH
// arrays sized by RES_COUNT/ITEM_COUNT, so appending enum slots moves every byte
// after them; without the bump an in-progress expedition would be silently dropped
// across the OTA. loadTrek keeps a v1 branch that reads the old 19 Res / 18 Item
// arrays and leaves the new slots at 0 — the rest of the layout is unchanged.
// v3 = 3c-2: R_FLEET_BEACON grows the Res array again, and two bytes (the wing
// flags + the blueprints found this trip) are APPENDED PAST the map blob so every
// earlier offset stays put and the v1/v2 branches keep reading unchanged.
// v4 = 3c-3: I_CARGO_DRONE + I_FLUID_RECYCLER grow the ITEM array. Same story as
// v2/v3 — the outfit arrays sit ahead of the map blob, so the map and the v3 tail
// both move and the file needs its own version. The tail itself is unchanged, so
// v3 keeps reading it at the v3 offset.
constexpr uint8_t  TREK_VER    = 4;

// What one move() resolved to. Landmark is a hook for 2.4 (the setpiece engine);
// FIGHT is live as of 2.3 — the caller (World page) starts the fight overlay.
enum StepKind : uint8_t {
    STEP_BLOCKED = 0,   // no active expedition
    STEP_MOVED,         // walked plain ground, no event (fight rolled false)
    STEP_FIGHT,         // a random encounter triggers here; `scene` = EncounterId
    STEP_LANDMARK,      // stepped onto a landmark; `scene` = SetpieceId (2.4 hook)
    STEP_HOME,          // reached the village -> goHome() committed the trip
    STEP_DIED,          // starvation/thirst killed the wanderer this step
};
// `notice` is a one-shot tr() key (nullptr = nothing to say) for the events
// world.js fires as a plain Notifications.notify() alongside the step — meat/
// water just running out, an armour/distance danger-zone crossing, or a
// terrain-change narration (§3.1/§3.3/§7.3) — distinct from the PERSISTING
// starving/thirsty HUD state (world_page hudMessage reads those off ex directly).
// Only one fires per step; move() picks a priority when more than one lands on
// the same tick (see world_state.cpp).
struct StepResult { uint8_t kind; uint8_t scene; const char* notice; };

// Result of a combat command / tick (world.js events.js real-time loop, ported
// to a 1s discrete tick — see combat_data.h). NOOP = the command was rejected
// (cooling, no ammo, already full HP) and nothing changed; ONGOING = the fight
// continues after a landed action; WON/LOST/FLED end it.
enum FightStatus : uint8_t {
    FIGHT_ONGOING = 0, FIGHT_WON, FIGHT_LOST, FIGHT_FLED, FIGHT_NOOP,
};

// One banked loot line, filled on victory (rollLoot) for the fight_modal summary.
struct LootLine { bool isItem; uint8_t slot; int16_t got; };

// The volatile combat state — RAM-ONLY, never serialized (research decision 7:
// combat isn't persisted, so a power-off mid-fight == a flee; trek.bin was
// already saved at the fight-triggering step, so a cold boot resumes the
// pre-fight tile with no combat). Lives beside Expedition; beginFight arms it,
// fightTick/fightAttack/… drive it, fightFlee/fightEndVictory/die clear it.
struct Combat {
    bool     active;
    bool     won;                       // victory panel up (loot already banked)
    bool     setpiece;                  // armed from a setpiece (vs a random encounter):
                                        // fight_modal skips its own name/victory panel and
                                        // hands the win/flee back to setpiece_modal.
    uint8_t  enemyId;                   // EncounterId (random encounter) or 0xFF (setpiece)
    int16_t  enemyHp, enemyMaxHp;
    int16_t  enemyDelayLeft;            // seconds to the enemy's next swing
    int16_t  enemyStunLeft;             // seconds of stun remaining (bolas)
    bool     lastMiss;                  // last player swing missed (UI hint)
    // Enemy stats + display sourced ONCE at beginFight/beginFightSetpiece so the
    // tick/attack/loot/render paths are agnostic to where the fight came from
    // (no per-tick ENCOUNTERS[enemyId] re-read). nameKey/deathKey are null for a
    // setpiece enemy (its name isn't shown; the victory belongs to the setpiece).
    int16_t  enemyDamage, enemyHitPM, enemyDelayS;
    char     enemyChara;
    const char* enemyNameKey;
    const char* enemyNotifKey;
    const char* enemyDeathKey;
    const LootDrop* lootTbl;            // loot table banked on victory
    int16_t  lootTblN;
    // Attack buttons: the weapons the expedition actually packs (ammo-gated at
    // beginFight), fists as the sole fallback. Fixed for the fight; each slot's
    // live cooldown drains in fightTick.
    uint8_t  weapons[WEAPON_COUNT];
    int16_t  weaponCool[WEAPON_COUNT];
    int      weaponN;
    int16_t  eatCool, medsCool;
    LootLine loot[MAX_LOOT];
    int      lootN;

    // ---- Phase 3c: the Executioner status system (research-phase3.md §10.4) --
    // All zero == a plain Phase-2 fight, so nothing here changes an existing
    // encounter's behaviour; a setpiece enemy arms the fields it declares.
    uint8_t  playerStatus;      // ST_NONE / ST_SHIELD / ST_BOOST
    uint8_t  playerStatusLeft;  // ticks left, or STATUS_FOREVER (shield: until broken)
    uint8_t  enemyStatus;       // ST_NONE / SHIELD / ENRAGED / ENERGISED / VENOMOUS /
                                // MEDITATION
    uint8_t  enemyStatusLeft;   // ticks left, or STATUS_FOREVER
    int16_t  enemyDelayBaseS;   // the armed attackDelayS, restored when enrage expires
    int16_t  meditateAccum;     // damage the meditating enemy has swallowed
    int16_t  dotDamage;         // venom damage per tick on the PLAYER; 0 = none
    uint8_t  dotTicksLeft;      // STATUS_FOREVER for the rest of the fight (see below)
    int16_t  specialDelay;      // scene `specials` period in ticks; 0 = no specials
    int16_t  specialLeft;       // ticks to the next special
    uint8_t  specialKind;       // SpecialKind (SK_RANDOM3 = command-deck boss)
    uint8_t  lastSpecial;       // SK_RANDOM3's "never twice in a row" memory
    int16_t  atHealthThreshold; // enemy HP blood line; 0 = none
    uint8_t  atHealthStatus;    // CombatStatus applied when a hit crosses it
    int16_t  explosionDamage;   // death self-destruct payload; 0 = dies clean
    uint8_t  explodeTicksLeft;  // wind-up countdown once HP hit 0 (exploding == >0)
    bool     exploding;         // HP is 0 and the corpse is counting down
    int16_t  hypoCool, stimCool, shieldCool;
};

// The volatile expedition (trek.bin). Holds its OWN working copy of the map so
// die() can drop it without touching the committed layer.
struct Expedition {
    bool     active;
    bool     dead;
    int16_t  x, y;              // wanderer position (0..60)
    int16_t  hp,  maxHp;        // captured from armour at embark
    int16_t  water, maxWater;   // captured from water upgrades at embark
    int16_t  foodMove, waterMove, fightMove;
    bool     starving, thirsty; // warn-then-die latches (world.js)
    bool     gastronome;        // swamp perk snapshot: meat heals x2 (world.js meatHeal).
                                // Captured from gs at embark, set live if the swamp
                                // grants it mid-trip; packed into the trek cleared byte.
    bool     danger;            // checkDanger edge-trigger latch (world.js World.danger):
                                // true once far-enough-without-armour has been warned, so
                                // the notice only fires on the step that FLIPS the state
                                // (§3.1/§4.4). Packed into the trek cleared byte.
    uint32_t rng;               // expedition PRNG (fight/loot rolls, 2.3)
    // Working map — a copy of the committed map, mutated during the trip.
    uint8_t  tiles[WORLD_CELLS];
    uint8_t  revealed[WORLD_MASK_BYTES];
    uint8_t  visited[WORLD_MASK_BYTES];
    // Bag: whole-unit counts, mirrored onto game stores/items at goHome.
    int16_t  outfitRes[RES_COUNT];
    int16_t  outfitItem[ITEM_COUNT];
    // Cleared-this-trip flags, committed to game.buildings at goHome.
    // clearedExec is the odd one out: upstream's World.state.executioner is SEEDED
    // from the committed world at embark and only re-committed by goHome, so it
    // reads "the prologue has been cleared, as far as this trip knows" — which is
    // what picks the antechamber over the intro when the X tile is stepped on
    // again (world.js doSpace:573-576).
    bool     clearedIron, clearedCoal, clearedSulphur, clearedShip, clearedExec;
    // The three Executioner wings + the blueprints found this trip. Same two-layer
    // rule as everything else here and as upstream's World.state: seeded from the
    // save at embark, written by the wings' terminal scenes, committed by goHome,
    // DISCARDED by die() — clear a wing and starve on the way back and the wing is
    // open again (research-phase3.md §3.1's "移植警示", kept faithful; §12 Q6 (a)).
    bool     wingEngineering, wingMartial, wingMedical;
    uint8_t  bpFound;           // Blueprint bits (game_data.h), redeemed at goHome
    // Outposts used THIS trip (position-keyed) don't re-trigger; a small ring is
    // plenty for one expedition. Merged into the committed usedOutpost bitmap at
    // goHome, discarded at die(). outpostUsed() checks this ring AND that bitmap.
    uint8_t  usedOutpostX[16], usedOutpostY[16];
    uint8_t  usedOutpostN;
};

class WorldState {
public:
    // ---- committed layer (world.bin) ----
    uint32_t seed;                          // map RNG seed (same seed -> same map)
    bool     generated;                     // committed map exists
    uint8_t  tiles[WORLD_CELLS];
    uint8_t  revealed[WORLD_MASK_BYTES];    // committed fog-of-war
    uint8_t  visited[WORLD_MASK_BYTES];
    // Outposts consumed for good (position-keyed bitmap). An outpost is one-shot
    // GLOBALLY (upstream keys usedOutposts on the persistent game.world), so the
    // committed layer must remember it across expeditions. goHome merges the
    // trip's ex.usedOutpost* ring in here; die() discards the trip's uses (they
    // never reach this bitmap), matching upstream's World.state discard.
    uint8_t  usedOutpost[WORLD_MASK_BYTES];

    // ---- volatile layer (trek.bin) ----
    Expedition ex;
    // ---- volatile combat (RAM-only, NOT serialized to trek.bin) ----
    Combat cx;

    // ---- lifecycle ----
    void init();                            // empty: no map, no expedition
    // Generate the committed map from `s` (deterministic). Overwrites tiles/fog.
    void generateMap(uint32_t s);
    // Generate only if not already generated; returns true if a map now exists.
    bool ensureGenerated(uint32_t s);

    // ---- expedition ----
    // Deduct `outfitRes`/`outfitItem` (whole units) from gs, fill water/hp from
    // equipment, snapshot committed map -> working, start at the village.
    // Requires cured meat > 0 (path.js embark gate) and a generated map, else
    // returns false and touches nothing. `trekSeed` seeds the expedition PRNG.
    // Also refuses during the post-death embark lockout (World.DEATH_COOLDOWN_S,
    // §1.5/§3.4): if gs.deathAt is within nowEpoch of the window. nowEpoch==0
    // (no RTC) or a clock at/behind the death epoch fails open — never a
    // permanent lockout.
    bool embark(GameState& gs, const int16_t* outfitRes,
                const int16_t* outfitItem, uint32_t trekSeed,
                uint32_t nowEpoch = 0);
    // Walk one Dir. Runs the full world.js move() -> doSpace() step; may commit
    // (goHome) or discard (die) the expedition. See StepResult.
    StepResult move(GameState& gs, uint8_t dir);
    // Reached the village: commit working map -> committed, unlock cleared mines
    // in gs.buildings, bank the bag into gs, end the expedition, delete trek.bin.
    void goHome(GameState& gs);
    // Starvation/thirst/(combat, 2.3): drop the working map, empty the bag, end
    // the expedition, delete trek.bin. Committed map + gs are left untouched.
    void die();

    // setpiece hooks (2.4) operate on the WORKING map, committed at goHome:
    // clear a cave/town/city -> OUTPOST + road; clear a mine/ship -> flag + road.
    void clearDungeon(int x, int y);        // -> OUTPOST, then drawRoad
    void clearMine(int x, int y, uint8_t tile);   // set flag + drawRoad
    void drawRoad(int x, int y);            // world.js drawRoad (L to nearest road)

    // ---- combat (events.js real-time loop, discretised — see combat_data.h) --
    // Which random encounter fires on the CURRENT tile (isAvailable: distance
    // tier + terrain), drawn from ex.rng; -1 on a non-terrain tile (road/void).
    // Public for the smoke test; rollFight uses it inside move().
    int  chooseEncounter();
    // Arm combat against ENCOUNTERS[enemyId]: enemy HP/timer, and the attack-
    // button weapon list (ammo-gated at start; fists fallback). Idempotent init.
    void beginFight(uint8_t enemyId);
    // Arm combat against a setpiece's inline enemy (setpieces.js combat scene).
    // Same weapon-list rules as beginFight; cx.setpiece is set so fight_modal
    // hands the outcome back to setpiece_modal instead of running its own panel.
    void beginFightSetpiece(const SetpieceEnemy& e);
    // Advance one second, in this fixed order (research-phase3.md §10.4 / the 3c-1
    // brief): (1) player cooldowns, (2) player status expiry (boost), (3) enemy
    // timed-status expiry, (4) scene `specials`, (5) venom DoT, (6) explosion
    // countdown + payload, (7) the enemy's swing. Returns FIGHT_LOST (-> die()
    // already ran), FIGHT_WON (an explosion the player survived — loot banked) or
    // FIGHT_ONGOING. Needs gs for the explosion victory's loot capacity cap.
    uint8_t fightTick(GameState& gs);
    // Player swings weapon slot `s` (0..fightWeaponCount-1): rejects on cooldown/
    // no-ammo (FIGHT_NOOP), else spends ammo, rolls hit, deals damage/stun, and
    // may kill the enemy (FIGHT_WON -> rollLoot banked into the bag; or the
    // explosion wind-up, which defers the win to fightTick). Needs gs for the loot
    // capacity cap.
    uint8_t fightAttack(GameState& gs, int s);
    uint8_t fightEat();                     // eat 1 cured meat, heal MEAT_HEAL
    uint8_t fightMeds();                    // use 1 medicine, heal MEDS_HEAL
    uint8_t fightHypo();                    // use 1 hypo, heal HYPO_HEAL (P3)
    uint8_t fightStim();                    // use 1 stim: BOOST_TICKS of BOOST_MULT
                                            // weapon damage, at BOOST_DAMAGE self-harm
    uint8_t fightShield();                  // kinetic armour: the next incoming hit
                                            // heals instead of hurting (P3)
    void    fightFlee();                    // abandon (no loot, keep damage), saveTrek
    void    fightEndVictory();              // dismiss the victory panel, saveTrek

    // ---- setpiece effects (setpieces.js scene onLoad, operate on the trip) ----
    // Bank a loot table into the bag, capacity-capped (shared with rollLoot); the
    // banked lines land in `out` (up to outCap) for the panel to show. Uses ex.rng
    // so a scene's loot is deterministic for the trek seed. Returns the line count.
    int  bankLootTable(GameState& gs, const LootDrop* tbl, int n,
                       LootLine* out, int outCap);
    void spFillWater() { ex.water = ex.maxWater; }        // outpost / house well
    // World.setHp(World.getMaxHealth()) — the Executioner's two regeneration
    // machines. Recomputed from gs rather than reusing ex.maxHp so a mid-trip
    // armour change (there is none today) could not desync the ceiling.
    void spHealFull(const GameState& gs) {
        ex.maxHp = (int16_t)maxHealth(gs);
        ex.hp = ex.maxHp;
    }
    void spGrantGastronome(GameState& gs);                // swamp charm -> perk
    // world.js markVisited — mark the CURRENT landmark tile as consumed (upstream
    // appends '!' to the tile char). A visited landmark's doSpace LANDMARKS lookup
    // then misses, so move() treats it as plain terrain (no re-trigger). Written to
    // the WORKING visited mask (goHome commits, die discards) — the same layering
    // as revealed/tiles. Called from the setpiece scenes upstream marks visited.
    void spMarkVisited();
    // world.js applyMap (world.js:687-697) — pick a still-dark cell at random and
    // uncover its Manhattan-radius-5 diamond (research-phase2.md §2.5). The
    // martial wing's `scavenge maps` calls it three times.
    //
    // UPSTREAM BUG, DELIBERATELY FIXED (research-phase3.md §3.7 / §5.4 item 1):
    // upstream writes the PERSISTENT `game.world.mask` while the trip renders and
    // commits `World.state.mask`, so the reveal was invisible for the rest of the
    // expedition and then overwritten by goHome. We write ex.revealed — the same
    // working layer as lightMap — so goHome commits it and die() drops it, which
    // is the layering every other trip mutation already obeys.
    void spApplyMap();
    // Persist the trip after a setpiece scene mutates it (loot banked, water
    // filled, mine flagged): the World map changes commit at goHome, but the bag /
    // water / cleared flags must survive a power-off mid-setpiece like any step.
    void spCommitStep() { if (ex.active) saveTrek(); }

    // Draw [0,1000) off the expedition PRNG — the setpiece engine's branch rolls
    // share the trek stream so a setpiece plays deterministically for a trek seed.
    int     spRand1000();

    // Expedition-active accessor (world_page / event scheduler gating): true
    // while the wanderer is out on the World map (trek.bin loaded/active).
    bool    trekActive() const { return ex.active; }

    // combat read helpers (fight_modal renderer)
    bool    fightActive() const { return cx.active; }
    bool    fightWon() const { return cx.won; }
    int     fightWeaponCount() const { return cx.weaponN; }
    uint8_t fightWeaponId(int s) const;               // WEAPONS index, or WEAPON_FISTS
    int     fightWeaponCoolLeft(int s) const;         // seconds (0 = ready)
    bool    fightWeaponEnabled(int s) const;          // ready AND ammo present
    const Combat& combat() const { return cx; }

    // ---- read helpers ----
    uint8_t tileAt(int x, int y) const;               // committed
    uint8_t exTileAt(int x, int y) const;             // working (expedition)
    bool    isRevealed(int x, int y) const;           // committed fog
    bool    exRevealed(int x, int y) const;           // working fog
    bool    exVisited(int x, int y) const;
    // Count committed tiles of a given type (tests / renderer).
    int     countTiles(uint8_t tile) const;
    // world.js compassDir — the crashed starship's fixed 8-way direction FROM THE
    // VILLAGE on the COMMITTED map (§2.7), the "the compass points <dir>" tr()
    // key trade_page pushes once at the first compass purchase (§1.6). Distinct
    // from world_page's own shipCompassKey, which is relative to the wanderer's
    // LIVE position during a trek — the two answer different questions and both
    // are upstream-faithful for where they're used. False (out untouched) if the
    // committed map has no ship (shouldn't happen: LANDMARKS always places one).
    bool    compassFromVillage(char* out, size_t cap) const;

    // Equipment-derived caps (world.js getMaxWater/getMaxHealth, path.js
    // getCapacity) — read gs.items. Pure, exposed for the future Path panel.
    static int maxWater(const GameState& gs);
    static int maxHealth(const GameState& gs);
    static int bagCapacityCenti(const GameState& gs);

    // ---- SD persistence (platform: SD under ARDUINO, stdio on host) ----
    bool saveWorld() const;                 // committed -> world.bin (atomic)
    bool loadWorld();                       // world.bin -> committed
    bool saveTrek() const;                  // expedition -> trek.bin (atomic)
    bool loadTrek();                        // trek.bin -> expedition (active=1)
    void clearTrek();                       // delete trek.bin (goHome / die)
    // Cold-boot restore: load committed, then resume an interrupted expedition
    // if trek.bin exists. Returns true if an expedition is now active.
    bool restore();

private:
    uint32_t mapRand();                     // xorshift32 off a local gen stream
    uint8_t  chooseTile(int x, int y);      // world.js chooseTile (stickiness)
    void     placeLandmark(const LandmarkDef& l);
    bool     findClosestRoad(int sx, int sy, int& rx, int& ry) const;
    void     lightMap(int x, int y);        // reveal working diamond r=LIGHT_RADIUS
    // food/water tick; false -> died. `notice` is set to a one-shot tr() key
    // ("the meat has run out" / "there is no more water") on the exact step the
    // last unit is consumed — distinct from the starving/thirsty latches above,
    // which are the PERSISTING warn-then-die state (§3.3).
    bool     useSupplies(GameState& gs, const char*& notice);
    bool     rollFight(int& enemyOut);      // world.js checkFight + encounter pick
    // world.js checkDanger — edge-triggered armour/distance warning (§3.1/§4.4).
    // Reads gs.items the same way maxHealth() does. Only writes res.notice when
    // ex.danger FLIPS this step (and only if nothing higher-priority already
    // claimed the slot — see the move() call site).
    void     checkDanger(const GameState& gs, StepResult& res);
    int      exBagUsedCenti() const;        // total carried weight (loot cap)
    void     armWeapons();                  // build the attack-button weapon list
    void     rollLoot(GameState& gs);       // events.js drawLoot -> bank into bag
    // ---- Phase 3c status plumbing (events.js setStatus / damage / enemyAttack) --
    void     armMechanics(const SetpieceEnemy& e);  // fold delay + arm the scene rules
    void     setEnemyStatus(uint8_t st);    // apply + seed the status' own timer
    void     enemyDamaged(int dmg);         // route a landed player hit through
                                            // shield / meditation, else HP
    // The enemy's HP hit 0: either wind up the self-destruct or win outright.
    uint8_t  enemyDefeated(GameState& gs);
    bool     outpostUsed(int x, int y) const;
    void     markOutpostUsed(int x, int y);

    uint32_t genRng;                        // transient generation PRNG
};

}  // namespace adr
