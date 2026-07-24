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
//    cooldown seconds) is discretised to a 1s tick (fight_modal drives it). ALL
//    §4 values are already whole seconds — there are NO sub-second gameplay
//    numbers to round: _FIGHT_SPEED (100ms) is animation-only and is dropped in
//    the discrete model; STUN_DURATION (4000ms) is exactly 4s. So the discrete
//    port reproduces the second-scale numbers verbatim.
//  * Loot count draw (events.js drawLoot): on a chance hit, num =
//    floor(rand01*(max-min)) + min — range [min, max-1] when max>min, exactly min
//    when max==min (upstream still consumes one random even then; we mirror that,
//    see world_state.cpp rollLoot). This is why e.g. "bullets 1-5" yields 1..4.
//  * Phase 2 scope: random encounters only (§4.8 — these have NO upstream flee;
//    fight_modal adds a "leave" affordance for the slow e-ink panel, see the
//    fightFlee note in world_state). Setpiece combat (2.4) and the Phase-3
//    weapons (plasma rifle / energy blade / disruptor) / heals (hypo / boost /
//    shield) are out of scope and omitted here.
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
