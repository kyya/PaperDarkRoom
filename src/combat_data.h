// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room (Doublespeak Games, MPL-2.0) — Phase 2 World COMBAT numeric data.
// Transcribed from upstream script/events/encounters.js (the random-encounter
// enemy table) and script/world.js World.Weapons + script/events.js combat
// constants — the values ARE the port, so this file is a derivative of the MPL
// game and carries the MPL header. Pure data + tiny helpers, no Arduino/M5
// dependency, so world_state's combat logic can be host-compiled for the smoke
// test (tools/world_smoke.cpp). Every en_key string doubles as a tr() key
// (strings_zh.h, §8.3 glyph closure); combat verbs / enemy names / notification
// / death lines all route through it in fight_modal.
//
// PORTING NOTES (deviations from upstream, all deliberate, kept faithful to the
// observable game — see docs/research-phase2.md §4):
//  * Probabilities / hit chances are integer permille (e.g. 0.8 -> 800), matching
//    world_data.h's permille model, so combat reproduces bit-for-bit on host and
//    device (no cross-platform float drift).
//  * The upstream real-time loop (enemy setInterval(attackDelay*1000), per-weapon
//    cooldown seconds) is discretised to a 1s tick (fight_modal drives it). Every
//    Phase-2 §4 value is already a whole second, so those are verbatim:
//    _FIGHT_SPEED (100ms) is animation-only and is dropped in the discrete model;
//    STUN_DURATION (4000ms) is exactly 4s. The Phase-3 Executioner is the first
//    content with SUB-second numbers (attackDelay 0.25 / 0.5 / 2.5s), and those
//    fold to a DPS-equivalent whole-second pair — see foldAttack below.
//  * Loot count draw (events.js drawLoot): on a chance hit, num =
//    floor(rand01*(max-min)) + min — range [min, max-1] when max>min, exactly min
//    when max==min (upstream still consumes one random even then; we mirror that,
//    see world_state.cpp rollLoot). This is why e.g. "bullets 1-5" yields 1..4.
//  * Random encounters have NO upstream flee (§4.8); fight_modal adds a "leave"
//    affordance for the slow e-ink panel — see the fightFlee note in world_state.
//  * Phase 3c (docs/research-phase3.md §3.3 / §10.4) adds the Executioner status
//    system on top: six statuses, periodic scene `specials`, an `atHealth` blood
//    line, death `explosion`, the three late weapons, and the hypo/stim/shield
//    player actions. All of it is appended — nothing above it changed value.
#pragma once
#include <stdint.h>
#include "game_data.h"    // Res/Item enums, RES_KEY/ITEM_KEY, R_*/I_*
#include "world_data.h"   // Tile enums (T_FOREST/…), BASE_HIT_CHANCE_PM, MEAT_HEAL

namespace adr {

// ---- combat-only constants (events.js) ------------------------------------
// (BASE_HIT_CHANCE_PM / MEAT_HEAL / MEDS_HEAL live in world_data.h — shared with
// the survival layer.)
constexpr int FIGHT_EAT_COOLDOWN_S  = 5;   // events.js _EAT_COOLDOWN
constexpr int FIGHT_MEDS_COOLDOWN_S = 7;   // events.js _MEDS_COOLDOWN
constexpr int FIGHT_STUN_S          = 4;   // events.js STUN_DURATION 4000ms -> 4s

// ---- Phase 3c: the Executioner status system (events.js:15-22) -------------
// Every upstream millisecond duration divides evenly by the port's 1s tick
// (docs/research-phase3.md §10.4), so these are exact, not rounded.
constexpr int FIGHT_HYPO_COOLDOWN_S   = 7;    // events.js _HYPO_COOLDOWN
constexpr int FIGHT_STIM_COOLDOWN_S   = 10;   // events.js _STIM_COOLDOWN
constexpr int FIGHT_SHIELD_COOLDOWN_S = 10;   // events.js _SHIELD_COOLDOWN
constexpr int ENRAGE_TICKS    = 4;    // ENRAGE_DURATION   4000ms
constexpr int MEDITATE_TICKS  = 5;    // MEDITATE_DURATION 5000ms
constexpr int BOOST_TICKS     = 3;    // BOOST_DURATION    3000ms
constexpr int EXPLOSION_TICKS = 3;    // EXPLOSION_DURATION 3000ms (self-destruct wind-up)
constexpr int ENERGISE_MULT   = 4;    // ENERGISE_MULTIPLIER
constexpr int BOOST_DAMAGE    = 10;   // BOOST_DAMAGE — the stim's self-harm
// PORT DECISION (docs/research-phase3.md §12 Q7): upstream's `boost` sets a status
// and the self-harm but never reads it back in damage() — the `boosted:` callback
// is Button-layer visuals only, so the stim has NO upstream gameplay payoff to
// port. We define one: weapon damage x2 for BOOST_TICKS. (The same ruling also
// makes useStim SPEND a stim, which upstream forgets to do.)
constexpr int BOOST_MULT      = 2;
// ENRAGE forces attackDelay to 0.5s upstream; the 1s tick floors that at 1 (§10.4).
constexpr int ENRAGED_DELAY_S = 1;
// A status/DoT with no upstream timer: it runs until something clears it (shield
// breaks on the next hit, energised is spent on the next hit, venomous rides the
// attacker for the whole fight). Distinct from a 0 counter, which means "expired".
constexpr uint8_t STATUS_FOREVER = 0xFF;

// The six statuses (events.js setStatus). A fighter carries at most one.
enum CombatStatus : uint8_t {
    ST_NONE = 0,
    ST_SHIELD,       // next incoming hit HEALS instead of hurting, then breaks
    ST_ENRAGED,      // enemy only: attack interval forced to ENRAGED_DELAY_S
    ST_ENERGISED,    // next outgoing hit deals ENERGISE_MULT x damage, then clears
    ST_VENOMOUS,     // landed hits hang a damage-over-time on the target
    ST_MEDITATION,   // absorbs all damage into meditateAccum, reflects it in one go
    ST_BOOST,        // player only: BOOST_MULT weapon damage (see BOOST_MULT above)
};

// What a scene's `specials` entry inflicts (events.js:161-171 — a periodic
// `{delay, action}` that re-fires for the whole fight, not a one-shot).
enum SpecialKind : uint8_t {
    SK_NONE = 0, SK_SHIELD, SK_ENRAGED, SK_ENERGISED, SK_MEDITATION,
    SK_RANDOM3,      // command-deck boss: one of shield/enraged/meditation, never
                     // the same one twice in a row (upstream's lastSpecial memory)
};

// ---- sub-second attackDelay folding (docs/research-phase3.md §10.4) --------
// The Executioner ships enemies with `attackDelay` below the port's 1s tick
// (chitinous horror/queen at 0.25s, and `enraged` at 0.5s), which a discrete
// 1s loop cannot express. Fold them to the DPS-equivalent whole-second pair:
//   delayS  = max(1, round(delayCS/100))
//   damage  = round(dmg * delayS * 100 / delayCS)
// so `1 dmg every 0.25s` becomes `4 dmg every 1s` — same expected DPS, higher
// variance (the accepted cost, §12 Q15). Data rows keep the UPSTREAM numbers in
// attackDelayCS + damage and the engine folds at arm time, so the tables stay
// diffable against upstream. Invariant on a data row: attackDelayCS != 0
// (sub-second form) <=> attackDelayS == 0.
struct AttackFold { int16_t damage, delayS; };
inline AttackFold foldAttack(int dmg, int delayCS) {
    if (delayCS <= 0) return { (int16_t)dmg, (int16_t)1 };
    int delayS = (delayCS + 50) / 100;               // round to whole seconds
    if (delayS < 1) delayS = 1;                      // never faster than one tick
    int damage = (dmg * delayS * 100 + delayCS / 2) / delayCS;
    return { (int16_t)damage, (int16_t)delayS };
}

// Double-entry fold table: the three cases §10.4 works out by hand, transcribed a
// SECOND time so tools/mechanics_test.cpp can diff foldAttack against them (the
// same guard-rail layer-1 already puts on the weapon/enemy tables).
struct SubsecondFold { int16_t dmg, delayCS, expDamage, expDelayS; const char* who; };
static const SubsecondFold SUBSECOND_FOLDS[] = {
    {  1,  25,  4, 1, "chitinous horror / queen (executioner-intro 3-1 / 4-1)" },
    { 10, 250, 12, 3, "automated turret (executioner-intro 6)" },
    {  6,  50, 12, 1, "enraged: attackDelay 0.5 -> damage x2 on a 1s tick" },
};
constexpr int SUBSECOND_FOLD_ROWS =
    (int)(sizeof(SUBSECOND_FOLDS) / sizeof(SUBSECOND_FOLDS[0]));

// Distance tiers (encounters.js isAvailable, Manhattan getDistance): T1 d<=10,
// T2 10<d<=20, T3 d>20. No module-level constant upstream — hardcoded per
// isAvailable; centralised here.
inline int fightTier(int dist) { return dist <= 10 ? 1 : (dist <= 20 ? 2 : 3); }

// ---- Weapons (world.js World.Weapons, Phase-2 subset) ---------------------
// cooldown is SECONDS (the player's attack-button cool). A weapon is a carryable
// Item (path.js carryable ∪ Room.Craftables) the expedition packs; fists is the
// unarmed fallback (no item). Ranged weapons spend ammo per swing: rifle/laser
// spend a Res (bullets / energy cell); grenade/bolas spend one of THEMSELVES
// (selfAmmo). damage == DMG_STUN means "no HP damage, stun the enemy" (bolas).
constexpr int16_t DMG_STUN   = -1;
constexpr uint8_t WSLOT_NONE = 0xFF;   // fists: no backing Item slot
constexpr uint8_t RES_NONE   = 0xFF;   // no Res ammo

struct WeaponDef {
    const char* key;        // identity ("fists", "rifle", …)
    const char* verb;       // tr() key drawn on the attack button ("punch", …)
    int16_t     damage;     // HP damage, or DMG_STUN
    int16_t     cooldownS;  // attack cooldown, seconds
    uint8_t     itemSlot;   // Item slot the expedition carries (WSLOT_NONE=fists)
    uint8_t     ammoRes;    // Res spent per swing (RES_NONE = none / selfAmmo)
    bool        selfAmmo;   // spend one of itemSlot per swing (grenade / bolas)
};

// Order = upstream World.Weapons order (weakest first); fists index 0.
enum WeaponId : uint8_t {
    WEAPON_FISTS = 0, WEAPON_BONE_SPEAR, WEAPON_IRON_SWORD, WEAPON_STEEL_SWORD,
    WEAPON_BAYONET, WEAPON_RIFLE, WEAPON_LASER_RIFLE, WEAPON_GRENADE, WEAPON_BOLAS,
    // -- Phase 3c (world.js World.Weapons tail; §4.3). The disruptor is a bolas
    // that does NOT spend itself — a strictly better stun, which is the point of
    // it being a Fabricator/Executioner reward.
    WEAPON_PLASMA_RIFLE, WEAPON_ENERGY_BLADE, WEAPON_DISRUPTOR,
    WEAPON_COUNT
};
static const WeaponDef WEAPONS[WEAPON_COUNT] = {
    { "fists",       "punch",   1,  2, WSLOT_NONE,    RES_NONE,      false },
    { "bone spear",  "stab",    2,  2, I_BONE_SPEAR,  RES_NONE,      false },
    { "iron sword",  "swing",   4,  2, I_IRON_SWORD,  RES_NONE,      false },
    { "steel sword", "slash",   6,  2, I_STEEL_SWORD, RES_NONE,      false },
    { "bayonet",     "thrust",  8,  2, I_BAYONET,     RES_NONE,      false },
    { "rifle",       "shoot",   5,  1, I_RIFLE,       R_BULLETS,     false },
    { "laser rifle", "blast",   8,  1, I_LASER_RIFLE, R_ENERGY_CELL, false },
    { "grenade",     "lob",    15,  5, I_GRENADE,     RES_NONE,      true  },
    { "bolas",       "tangle", DMG_STUN, 15, I_BOLAS,  RES_NONE,     true  },
    { "plasma rifle","disintegrate", 12, 1, I_PLASMA_RIFLE, R_ENERGY_CELL, false },
    { "energy blade","slice",  10,  2, I_ENERGY_BLADE, RES_NONE,     false },
    { "disruptor",   "stun",   DMG_STUN, 15, I_DISRUPTOR, RES_NONE,  false },
};

// ---- Loot (encounters.js loot tables, drawLoot) ---------------------------
// One drop line: a Res or Item, a count range [min,max] (see the count-draw note
// at the top), and a permille chance. isItem picks outfitItem[slot] vs
// outfitRes[slot] and RES_KEY[slot] vs ITEM_KEY[slot] for the weight lookup.
struct LootDrop { bool isItem; uint8_t slot; int16_t mn, mx; int16_t chancePM; };

// ---- Encounters (encounters.js Events.Encounters, Phase-2 random set) ------
// tier + terrain gate availability (isAvailable: getDistance tier && getTerrain).
// name/notif/death are tr() keys (verbatim upstream _() strings). chara is the
// enemy's map-combat glyph (baked ASCII, render-only). health/damage/hitPM/
// attackDelayS/ranged are the combat stats.
constexpr int MAX_LOOT = 4;
struct Encounter {
    uint8_t     tier;         // 1 / 2 / 3
    uint8_t     terrain;      // T_FOREST / T_FIELD / T_BARRENS
    const char* name;         // tr() key (enemy name)
    const char* notif;        // tr() key (encounter notification)
    const char* death;        // tr() key (enemy-defeated line)
    char        chara;        // combat glyph (render-only)
    int16_t     health;
    int16_t     damage;
    int16_t     hitPM;        // enemy hit chance, permille
    int16_t     attackDelayS; // seconds between enemy swings
    bool        ranged;
    LootDrop    loot[MAX_LOOT];
    int         lootN;
};

// ---- Setpiece combat enemy (setpieces.js inline combat scenes) -------------
// A landmark setpiece's combat scene carries its OWN enemy stat block + loot,
// NOT drawn from the random ENCOUNTERS table (research §5). Same combat scalars
// as an Encounter minus the tier/terrain availability gate (a setpiece places
// the fight explicitly). The name is not surfaced (upstream's fight UI shows
// only the chara glyph + notification — script/events.js createFighterDiv), so
// no enemy-name translation is needed; `notif` is the fight-start line. Armed
// into WorldState::cx via beginFightSetpiece, then driven by the shared combat
// loop exactly like a random encounter.
constexpr int SP_ENEMY_LOOT_MAX = 6;
struct SetpieceEnemy {
    char        chara;         // combat glyph (render-only, baked ASCII)
    const char* notif;         // tr() key: fight-start notification
    int16_t     health;
    int16_t     damage;
    int16_t     hitPM;         // enemy hit chance, permille
    int16_t     attackDelayS;  // seconds between enemy swings
    bool        ranged;
    LootDrop    loot[SP_ENEMY_LOOT_MAX];   // banked on victory (rollLoot)
    int         lootN;
    // ---- Phase 3c scene mechanics (all APPENDED, all zero == "absent"), so the
    // Phase-2 rows above stay byte-for-byte unedited and aggregate-init the new
    // fields to the inert value. -------------------------------------------------
    int16_t     attackDelayCS; // upstream sub-second attackDelay, centiseconds. 0 =
                               // use attackDelayS verbatim. Non-zero REQUIRES
                               // attackDelayS == 0 (foldAttack owns the conversion).
    uint8_t     specialKind;   // SpecialKind periodically inflicted on this enemy
    int16_t     specialDelayS; // its period, in ticks (events.js specials[].delay)
    int16_t     atHealthThreshold;  // events.js atHealth key; 0 = no blood-line trigger
    uint8_t     atHealthStatus;     // CombatStatus applied when a hit crosses it
    int16_t     explosionDamage;    // events.js explosion: EXPLOSION_TICKS of wind-up
                                    // on death, then this much damage. 0 = dies clean.
};

// Stable ids (tests + STEP_FIGHT scene payload). Order = tier 1, 2, 3.
enum EncounterId : uint8_t {
    E_SNARLING_BEAST = 0, E_GAUNT_MAN, E_STRANGE_BIRD, E_TWO_HEADED,       // T1
    E_SHIVERING_MAN, E_MAN_EATER, E_SCAVENGER, E_LIZARD,                   // T2
    E_FERAL_TERROR, E_SOLDIER, E_SNIPER,                                   // T3
    ENCOUNTER_COUNT
};

// NOTE (tr() gaps): the two-headed creature's name / notification / death lines
// are the ONE Phase-2 encounter NOT in the official zh_cn set (research §7.2):
//   "two-headed creature", "a two-headed creature appears, the smaller head
//   trembling", "the two creatures are dead" — tr() degrades them to English
//   (ASCII is baked in the 12px face). No copy is self-authored (§ iron law).
static const Encounter ENCOUNTERS[ENCOUNTER_COUNT] = {
    // ---- Tier 1 (distance <= 10) ----
    { 1, T_FOREST, "snarling beast",
      "a snarling beast leaps out of the underbrush", "the snarling beast is dead",
      'R', 5, 1, 800, 1, false,
      { { false, R_FUR, 1, 3, 1000 }, { false, R_MEAT, 1, 3, 1000 },
        { false, R_TEETH, 1, 3, 800 } }, 3 },
    { 1, T_BARRENS, "gaunt man",
      "a gaunt man approaches, a crazed look in his eye", "the gaunt man is dead",
      'E', 6, 2, 800, 2, false,
      { { false, R_CLOTH, 1, 3, 800 }, { false, R_TEETH, 1, 2, 800 },
        { false, R_LEATHER, 1, 2, 500 } }, 3 },
    { 1, T_FIELD, "strange bird",
      "a strange looking bird speeds across the plains", "the strange bird is dead",
      'R', 4, 3, 800, 2, false,
      { { false, R_SCALES, 1, 3, 800 }, { false, R_TEETH, 1, 2, 500 },
        { false, R_MEAT, 1, 3, 800 } }, 3 },
    { 1, T_FIELD, "two-headed creature",
      "a two-headed creature appears, the smaller head trembling",
      "the two creatures are dead",
      'K', 10, 2, 500, 3, false,
      { { false, R_FUR, 2, 4, 1000 }, { false, R_TEETH, 2, 3, 800 },
        { false, R_MEAT, 2, 3, 800 } }, 3 },
    // ---- Tier 2 (10 < distance <= 20) ----
    { 2, T_BARRENS, "shivering man",
      "a shivering man approaches and attacks with surprising strength",
      "the shivering man is dead",
      'E', 20, 5, 500, 1, false,
      { { false, R_CLOTH, 1, 1, 200 }, { false, R_TEETH, 1, 2, 800 },
        { false, R_LEATHER, 1, 1, 200 }, { false, R_MEDICINE, 1, 3, 700 } }, 4 },
    { 2, T_FOREST, "man-eater",
      "a large creature attacks, claws freshly bloodied", "the man-eater is dead",
      'T', 25, 3, 800, 1, false,
      { { false, R_FUR, 5, 10, 1000 }, { false, R_MEAT, 5, 10, 1000 },
        { false, R_TEETH, 5, 10, 800 } }, 3 },
    { 2, T_BARRENS, "scavenger",
      "a scavenger draws close, hoping for an easy score", "the scavenger is dead",
      'E', 30, 4, 800, 2, false,
      { { false, R_CLOTH, 5, 10, 800 }, { false, R_LEATHER, 5, 10, 800 },
        { false, R_IRON, 1, 5, 500 }, { false, R_MEDICINE, 1, 2, 100 } }, 4 },
    { 2, T_FIELD, "lizard",
      "the grass thrashes wildly as a huge lizard pushes through",
      "the lizard is dead",
      'T', 20, 5, 800, 2, false,
      { { false, R_SCALES, 5, 10, 800 }, { false, R_TEETH, 5, 10, 500 },
        { false, R_MEAT, 5, 10, 800 } }, 3 },
    // ---- Tier 3 (distance > 20) ----
    { 3, T_FOREST, "feral terror",
      "a beast, wilder than imagining, erupts out of the foliage",
      "the feral terror is dead",
      'T', 45, 6, 800, 1, false,
      { { false, R_FUR, 5, 10, 1000 }, { false, R_MEAT, 5, 10, 1000 },
        { false, R_TEETH, 5, 10, 800 } }, 3 },
    { 3, T_BARRENS, "soldier",
      "a soldier opens fire from across the desert", "the soldier is dead",
      'D', 50, 8, 800, 2, true,
      { { false, R_CLOTH, 5, 10, 800 }, { false, R_BULLETS, 1, 5, 500 },
        { true,  I_RIFLE, 1, 1, 200 }, { false, R_MEDICINE, 1, 2, 100 } }, 4 },
    { 3, T_FIELD, "sniper",
      "a shot rings out, from somewhere in the long grass", "the sniper is dead",
      'D', 30, 15, 800, 4, true,
      { { false, R_CLOTH, 5, 10, 800 }, { false, R_BULLETS, 1, 5, 500 },
        { true,  I_RIFLE, 1, 1, 200 }, { false, R_MEDICINE, 1, 2, 100 } }, 4 },
};

}  // namespace adr
