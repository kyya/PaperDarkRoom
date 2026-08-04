// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Host MECHANICS test for the A Dark Room Phase-2 port. A companion to
// tools/world_smoke.cpp (which stays a fast behavioural smoke): this suite is
// SYSTEMATIC, cross-checking the ported engines against docs/research-phase2.md
// — the only correctness baseline this repo has (upstream A Dark Room ships no
// tests). Four layers, ordered by value:
//   Layer 1  static data-table validation
//            (a) every SETPIECES[] graph: BFS reachability, legal next/prob/enemy
//                /loot/cost indices, monotone prob thresholds ending at 1000, no
//                dead-end (every scene can reach SP_SCENE_END);
//            (b) double-entry numeric checks: the §4.2 enemy tiers, §4.3 weapon
//                table, §2.4 LANDMARKS table and §2.1 constants are transcribed a
//                SECOND time here and diffed field-by-field against the headers,
//                so a future edit to a header can't silently drift a value.
//   Layer 2  seeded end-to-end economy loop (embark -> walk to iron mine ->
//            setpiece fight -> clear -> goHome -> miner job + banked loot).
//   Layer 3  Monte-Carlo statistics (fixed seeds, reproducible): map generation
//            over 200+ seeds, >=5000 combat hit rolls, >=3000 loot draws, >=5000
//            rollable encounter steps.
//   Layer 4  save robustness: world.bin v1->v2 migration / truncation / bad
//            version / v2 round-trip; trek.bin truncation / round-trip; game.json
//            missing "dcool" -> deathAt==0.
//   Layer 5  the Space level (Phase 3b).
//   Layer 6  the Executioner combat MECHANICS (Phase 3c-1) — the six statuses,
//            scene specials, atHealth, explosion, the three new weapons.
//   Layer 7  the Executioner CONTENT (Phase 3c-2) — the 20 enemy stat blocks and
//            38 combat scenes transcribed a second time from upstream
//            executioner.js, the wing bookkeeping, the blueprint drops, the front
//            hall's availability gates and the burning corridor.
//   Layer 8  the Fabricator (Phase 3c-3) — the FABRICATE table, fabricate()'s
//            blueprint/cost/cap rules, and the bag / water / armour tiers its
//            three upgrades finally switch on.
//
// Zero source intrusion: every RNG the statistics need is already injectable —
// generateMap(seed) for the map stream, embark(trekSeed) / the public ex.rng for
// the fight+loot stream — so no rng-exposure hack was needed.
//
// Build (clang++ is the host toolchain on this box):
//   clang++ -std=c++17 -I src tools/mechanics_test.cpp src/world_state.cpp \
//           src/game_state.cpp src/setpiece_engine.cpp src/space_game.cpp \
//           -DADR_SAVE_PATH='"mechanics_game.json"' \
//           -DADR_WORLD_PATH='"mechanics_world.bin"' \
//           -DADR_TREK_PATH='"mechanics_trek.bin"' \
//           -o mechanics_test.exe
#include "world_state.h"
#include "game_state.h"
#include "setpiece_engine.h"
#include "setpieces_data.h"
#include "combat_data.h"
#include "world_data.h"
#include "game_data.h"
#include "space_game.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

using namespace adr;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; printf("  [PASS] %s\n", msg); } \
    else      { g_fail++; printf("  [FAIL] %s\n", msg); } } while (0)

// ---- shared helpers -------------------------------------------------------

static int manhattan(int x, int y) {
    int dx = x - VILLAGE_X, dy = y - VILLAGE_Y;
    return (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
}
static int landmarkDist(const WorldState& w, uint8_t tile) {
    for (int y = 0; y < WORLD_DIM; y++)
        for (int x = 0; x < WORLD_DIM; x++)
            if (w.tileAt(x, y) == tile) return manhattan(x, y);
    return -1;
}
static bool findTile(const WorldState& w, uint8_t tile, int& ox, int& oy) {
    for (int y = 0; y < WORLD_DIM; y++)
        for (int x = 0; x < WORLD_DIM; x++)
            if (w.tileAt(x, y) == tile) { ox = x; oy = y; return true; }
    return false;
}

// Raw little-endian writers for hand-crafting save files (Layer 4).
static void rawU32(uint8_t* b, size_t& o, uint32_t v) {
    b[o++] = (uint8_t)v; b[o++] = (uint8_t)(v >> 8);
    b[o++] = (uint8_t)(v >> 16); b[o++] = (uint8_t)(v >> 24);
}
static bool writeRaw(const char* path, const uint8_t* d, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fwrite(d, 1, n, f);
    fclose(f);
    return true;
}

// ===========================================================================
// Layer 1a — SETPIECES[] graph static validation
// ===========================================================================

// SpDef carries no array-length fields (btns/probs/enemies are bare pointers),
// so the exact button/prob/enemy counts are transcribed here from
// setpieces_data.h — a second-source bound so an index can be checked for real.
struct SpMeta { uint8_t btnN, probN, enemyN; };
static const SpMeta SP_META[] = {
    /* SP_NONE        */ { 0, 0, 0 },
    /* SP_OUTPOST     */ { 1, 0, 0 },
    /* SP_IRONMINE    */ { 4, 0, 1 },
    /* SP_COALMINE    */ { 8, 0, 2 },
    /* SP_SULPHURMINE */ { 8, 0, 3 },
    /* SP_HOUSE       */ { 5, 3, 1 },
    /* SP_CAVE        */ { 21, 13, 5 },
    /* SP_TOWN        */ { 7, 2, 1 },
    /* SP_CITY        */ { 5, 2, 1 },
    /* SP_SHIP        */ { 1, 0, 0 },
    /* SP_BOREHOLE    */ { 1, 0, 0 },
    /* SP_BATTLEFIELD */ { 1, 0, 0 },
    /* SP_SWAMP       */ { 5, 0, 0 },
    /* SP_EXEC_INTRO  */ { 27, 3, 20 },
    /* SP_CACHE       */ { 0, 0, 0 },
    // The six Executioner tables share ONE 20-row enemy array, so every wing
    // declares the same enemyN (executioner_data.h).
    /* SP_EXEC_ANTE   */ { 5, 0, 20 },
    /* SP_EXEC_ENG    */ { 41, 15, 20 },
    /* SP_EXEC_MAR    */ { 54, 10, 20 },
    /* SP_EXEC_MED    */ { 61, 14, 20 },
    /* SP_EXEC_CMD    */ { 15, 2, 20 },
};
static_assert(sizeof(SP_META) / sizeof(SP_META[0]) == SETPIECE_COUNT,
              "SP_META must describe every SETPIECES[] row");

static bool lootSlotLegal(const LootDrop& d) {
    return d.isItem ? (d.slot < ITEM_COUNT) : (d.slot < RES_COUNT);
}

// Forward: can scene `s` reach SP_SCENE_END along SOME button path? Memoized via
// a 3-state color array (0 unknown, 1 in-progress, 2 yes, 3 no).
static bool sceneCanEnd(const SpDef& def, uint8_t sceneN, const SpMeta& m,
                        int s, uint8_t* color) {
    if (s < 0 || s >= sceneN) return false;
    if (color[s] == 2) return true;
    if (color[s] == 1) return false;   // cycle: this path doesn't itself end
    color[s] = 1;
    const SpScene& sc = def.scenes[s];
    bool canEnd = false;
    for (int b = 0; b < sc.btnCount && !canEnd; b++) {
        const SpButton& btn = def.btns[sc.btnStart + b];
        if (btn.next == SP_SCENE_END) { canEnd = true; break; }
        // A cross-event hop leaves THIS graph for another setpiece, which is just
        // as good an exit as `end` for "can the player get out of this scene".
        if (btn.next == SP_SCENE_EVENT) { canEnd = true; break; }
        if (btn.next == SP_SCENE_PROB) {
            for (int i = 0; i < btn.probCount; i++) {
                uint8_t tgt = def.probs[btn.probStart + i].scene;
                if (sceneCanEnd(def, sceneN, m, tgt, color)) { canEnd = true; break; }
            }
        } else {
            if (sceneCanEnd(def, sceneN, m, btn.next, color)) canEnd = true;
        }
    }
    color[s] = canEnd ? 2 : 3;
    // A '3' (no-end found on this recursion) may be a false negative caused by an
    // in-progress ancestor; leave it unknown for a later independent query.
    if (!canEnd) color[s] = 0;
    return canEnd;
}

static void layer1_setpieces() {
    printf("== [L1a] SETPIECES[] graph: reachability / indices / probs / no dead-end ==\n");
    for (int id = 0; id < SETPIECE_COUNT; id++) {
        if (!setpieceExists(id)) continue;
        const SpDef& def = SETPIECES[id];
        const SpMeta& m = SP_META[id];
        uint8_t sceneN = def.sceneN;
        char tag[96];

        // (a) BFS from scene 0: every scene reachable; every transition legal.
        bool reached[64] = { false };
        int stack[64], sp = 0;
        stack[sp++] = 0; reached[0] = true;
        bool idxOk = true, probOk = true, enemyOk = true, lootOk = true, costOk = true;
        bool btnRangeOk = true;
        while (sp > 0) {
            int s = stack[--sp];
            const SpScene& sc = def.scenes[s];
            // scene's button window inside btns[]
            if (sc.btnStart + sc.btnCount > m.btnN) btnRangeOk = false;
            // combat scene enemy index
            if (sc.combat && sc.enemy >= m.enemyN) enemyOk = false;
            // narrative loot slots + the Executioner's blueprint bit
            for (int i = 0; i < sc.lootN; i++)
                if (!lootSlotLegal(sc.loot[i])) lootOk = false;
            if (sc.bp > BP_COUNT) lootOk = false;
            for (int b = 0; b < sc.btnCount; b++) {
                const SpButton& btn = def.btns[sc.btnStart + b];
                // cost slot legal. The two non-inventory currencies (the
                // Executioner's water/hp prices) are SENTINEL slots that carry an
                // amount instead of indexing a table — they must have one, and a
                // bag cost must have none (SpButton::costAmt is unread there).
                if (btn.costSlot == SP_COST_WATER || btn.costSlot == SP_COST_HP) {
                    if (btn.costAmt <= 0) costOk = false;
                } else if (btn.costSlot != SP_NO_COST) {
                    if (btn.costAmt != 0) costOk = false;
                    if (btn.costIsItem) { if (btn.costSlot >= ITEM_COUNT) costOk = false; }
                    else                { if (btn.costSlot >= RES_COUNT)  costOk = false; }
                }
                if (btn.next == SP_SCENE_END) continue;
                if (btn.next == SP_SCENE_EVENT) {
                    // probStart is the TARGET SetpieceId here, not a prob index.
                    if (!setpieceExists(btn.probStart)) idxOk = false;
                    continue;
                }
                if (btn.next == SP_SCENE_PROB) {
                    // prob window in bounds + thresholds monotone ending at 1000
                    if (btn.probStart + btn.probCount > m.probN) { probOk = false; continue; }
                    int prev = -1;
                    for (int i = 0; i < btn.probCount; i++) {
                        const SpProb& p = def.probs[btn.probStart + i];
                        if (p.thresholdMilli <= prev) probOk = false;   // strictly increasing
                        prev = p.thresholdMilli;
                        if (p.scene >= sceneN) idxOk = false;
                        else if (!reached[p.scene]) { reached[p.scene] = true; stack[sp++] = p.scene; }
                    }
                    if (btn.probCount > 0 &&
                        def.probs[btn.probStart + btn.probCount - 1].thresholdMilli != 1000)
                        probOk = false;
                } else {
                    if (btn.next >= sceneN) idxOk = false;
                    else if (!reached[btn.next]) { reached[btn.next] = true; stack[sp++] = btn.next; }
                }
            }
        }
        int reachN = 0;
        for (int s = 0; s < sceneN; s++) if (reached[s]) reachN++;
        snprintf(tag, sizeof tag, "sp#%d: all %d scenes reachable from start", id, sceneN);
        CHECK(reachN == sceneN, tag);
        snprintf(tag, sizeof tag, "sp#%d: every button.next is a legal scene index", id);
        CHECK(idxOk, tag);
        snprintf(tag, sizeof tag, "sp#%d: prob windows in-bounds, monotone, end at 1000", id);
        CHECK(probOk, tag);
        snprintf(tag, sizeof tag, "sp#%d: every button window inside btns[]", id);
        CHECK(btnRangeOk, tag);
        snprintf(tag, sizeof tag, "sp#%d: every combat enemy index inside enemies[]", id);
        CHECK(enemyOk, tag);
        snprintf(tag, sizeof tag, "sp#%d: every narrative loot slot is a legal Res/Item", id);
        CHECK(lootOk, tag);
        snprintf(tag, sizeof tag, "sp#%d: every button cost slot legal (SP_NO_COST or in range)", id);
        CHECK(costOk, tag);

        // (b) enemy loot slots legal
        bool enemyLootOk = true;
        for (int e = 0; e < m.enemyN; e++) {
            const SetpieceEnemy& en = def.enemies[e];
            for (int i = 0; i < en.lootN; i++)
                if (!lootSlotLegal(en.loot[i])) enemyLootOk = false;
        }
        snprintf(tag, sizeof tag, "sp#%d: every combat-enemy loot slot is a legal Res/Item", id);
        CHECK(enemyLootOk, tag);

        // (c) no dead-end: every scene can reach SP_SCENE_END
        bool allEnd = true;
        for (int s = 0; s < sceneN; s++) {
            uint8_t color[64] = { 0 };
            if (!sceneCanEnd(def, sceneN, m, s, color)) allEnd = false;
        }
        snprintf(tag, sizeof tag, "sp#%d: no dead-end (every scene can reach SP_SCENE_END)", id);
        CHECK(allEnd, tag);
    }
}

// ===========================================================================
// Layer 1b — double-entry numeric transcription (spec §2.1/§2.4/§4.2/§4.3)
// ===========================================================================

static void layer1_constants() {
    printf("== [L1b] §2.1 world constants match world_data.h ==\n");
    CHECK(WORLD_RADIUS == 30,        "RADIUS 30");
    CHECK(WORLD_DIM == 61,           "dim 61x61");
    CHECK(VILLAGE_X == 30 && VILLAGE_Y == 30, "village [30,30]");
    CHECK(STICKINESS_PM == 500,      "stickiness 0.5");
    CHECK(LIGHT_RADIUS == 2,         "light radius 2");
    CHECK(BASE_WATER == 10,          "base water 10");
    CHECK(BASE_HEALTH == 10,         "base health 10");
    CHECK(MOVES_PER_FOOD == 2,       "moves per food 2");
    CHECK(MOVES_PER_WATER == 1,      "moves per water 1");
    CHECK(DEATH_COOLDOWN_S == 120,   "death cooldown 120s");
    CHECK(FIGHT_CHANCE_PM == 200,    "fight chance 0.20");
    CHECK(FIGHT_DELAY == 3,          "fight delay 3");
    CHECK(BASE_HIT_CHANCE_PM == 800, "base hit chance 0.8");
    CHECK(MEAT_HEAL == 8,            "meat heal 8");
    CHECK(MEDS_HEAL == 20,           "meds heal 20");
    CHECK(HYPO_HEAL == 30,           "hypo heal 30 (P3)");
    // terrain probs §2.3
    CHECK(TILE_PROBS_PM[TI_FOREST] == 150 && TILE_PROBS_PM[TI_FIELD] == 350 &&
          TILE_PROBS_PM[TI_BARRENS] == 500, "terrain probs 0.15/0.35/0.50");
    // water / health / bag tiers §1.1/§3.2/§4.4
    CHECK(WATER_BASE == 10 && WATER_WATERSKIN == 20 && WATER_CASK == 30 &&
          WATER_TANK == 60, "water tiers 10/20/30/60");
    CHECK(HEALTH_BASE == 10 && HEALTH_L_ARMOUR == 15 && HEALTH_I_ARMOUR == 25 &&
          HEALTH_S_ARMOUR == 45, "health tiers 10/15/25/45");
    CHECK(BAG_BASE_CENTI == 1000 && BAG_RUCKSACK == 2000 && BAG_WAGON == 4000 &&
          BAG_CONVOY == 7000, "bag tiers 10/20/40/70");
    // combat cooldown constants §4.5/§4.6
    CHECK(FIGHT_EAT_COOLDOWN_S == 5 && FIGHT_MEDS_COOLDOWN_S == 7 &&
          FIGHT_STUN_S == 4, "eat/meds/stun cd 5/7/4");
}

static void layer1_landmarks() {
    printf("== [L1b] §2.4 LANDMARKS table matches world_data.h ==\n");
    // tile, num, minR, maxR — transcribed from research-phase2.md §2.4.
    struct L { uint8_t tile; int num, minR, maxR; };
    static const L exp[] = {
        { T_OUTPOST,       0,  0,  0 },
        { T_IRON_MINE,     1,  5,  5 },
        { T_COAL_MINE,     1, 10, 10 },
        { T_SULPHUR_MINE,  1, 20, 20 },
        { T_HOUSE,        10,  0, 45 },
        { T_CAVE,          5,  3, 10 },
        { T_TOWN,         10, 10, 20 },
        { T_CITY,         20, 20, 45 },
        { T_SHIP,          1, 28, 28 },
        { T_BOREHOLE,     10, 15, 45 },
        { T_BATTLEFIELD,   5, 18, 45 },
        { T_SWAMP,         1, 15, 45 },
        { T_EXECUTIONER,   1, 28, 28 },
        { T_CACHE,         1, 10, 45 },
    };
    int n = (int)(sizeof(exp) / sizeof(exp[0]));
    CHECK(n == LANDMARK_ROWS, "landmark row count matches");
    bool ok = true;
    for (int i = 0; i < n && i < LANDMARK_ROWS; i++) {
        const LandmarkDef& g = LANDMARKS[i];
        if (g.tile != exp[i].tile || g.num != exp[i].num ||
            g.minR != exp[i].minR || g.maxR != exp[i].maxR) {
            ok = false;
            printf("     mismatch row %d: tile=%d num=%d minR=%d maxR=%d\n",
                   i, g.tile, g.num, g.minR, g.maxR);
        }
    }
    CHECK(ok, "every LANDMARKS row (tile/num/minR/maxR) matches the spec");
}

static void layer1_weapons() {
    printf("== [L1b] §4.3 weapon table matches combat_data.h ==\n");
    // key, damage, cooldownS — DMG_STUN for bolas. From research-phase2.md §4.3.
    struct W { WeaponId id; int16_t dmg; int16_t cd; };
    static const W exp[] = {
        { WEAPON_FISTS,      1,  2 },
        { WEAPON_BONE_SPEAR, 2,  2 },
        { WEAPON_IRON_SWORD, 4,  2 },
        { WEAPON_STEEL_SWORD,6,  2 },
        { WEAPON_BAYONET,    8,  2 },
        { WEAPON_RIFLE,      5,  1 },
        { WEAPON_LASER_RIFLE,8,  1 },
        { WEAPON_GRENADE,   15,  5 },
        { WEAPON_BOLAS, DMG_STUN, 15 },
    };
    bool ok = true;
    for (int i = 0; i < (int)(sizeof(exp)/sizeof(exp[0])); i++) {
        const WeaponDef& g = WEAPONS[exp[i].id];
        if (g.damage != exp[i].dmg || g.cooldownS != exp[i].cd) {
            ok = false;
            printf("     weapon %s dmg=%d cd=%d\n", g.key, g.damage, g.cooldownS);
        }
    }
    CHECK(ok, "every weapon (damage/cooldown) matches the spec");
    // ammo wiring: rifle->bullets, laser->energy cell, grenade/bolas self-ammo
    CHECK(WEAPONS[WEAPON_RIFLE].ammoRes == R_BULLETS &&
          WEAPONS[WEAPON_LASER_RIFLE].ammoRes == R_ENERGY_CELL, "ranged ammo res wired");
    CHECK(WEAPONS[WEAPON_GRENADE].selfAmmo && WEAPONS[WEAPON_BOLAS].selfAmmo,
          "grenade + bolas spend themselves");
}

static void layer1_enemies() {
    printf("== [L1b] §4.2 enemy tiers match combat_data.h ==\n");
    // tier, terrain, HP, dmg, hit, delay, ranged — from research-phase2.md §4.2.
    struct E {
        EncounterId id; uint8_t tier, terrain;
        int16_t hp, dmg, hit, delay; bool ranged;
    };
    static const E exp[] = {
        { E_SNARLING_BEAST, 1, T_FOREST,   5,  1, 800, 1, false },
        { E_GAUNT_MAN,      1, T_BARRENS,  6,  2, 800, 2, false },
        { E_STRANGE_BIRD,   1, T_FIELD,    4,  3, 800, 2, false },
        { E_TWO_HEADED,     1, T_FIELD,   10,  2, 500, 3, false },
        { E_SHIVERING_MAN,  2, T_BARRENS, 20,  5, 500, 1, false },
        { E_MAN_EATER,      2, T_FOREST,  25,  3, 800, 1, false },
        { E_SCAVENGER,      2, T_BARRENS, 30,  4, 800, 2, false },
        { E_LIZARD,         2, T_FIELD,   20,  5, 800, 2, false },
        { E_FERAL_TERROR,   3, T_FOREST,  45,  6, 800, 1, false },
        { E_SOLDIER,        3, T_BARRENS, 50,  8, 800, 2, true  },
        { E_SNIPER,         3, T_FIELD,   30, 15, 800, 4, true  },
    };
    int n = (int)(sizeof(exp)/sizeof(exp[0]));
    CHECK(n == ENCOUNTER_COUNT, "encounter count matches (11)");
    bool ok = true;
    for (int i = 0; i < n && i < ENCOUNTER_COUNT; i++) {
        const Encounter& g = ENCOUNTERS[exp[i].id];
        if (g.tier != exp[i].tier || g.terrain != exp[i].terrain ||
            g.health != exp[i].hp || g.damage != exp[i].dmg ||
            g.hitPM != exp[i].hit || g.attackDelayS != exp[i].delay ||
            g.ranged != exp[i].ranged) {
            ok = false;
            printf("     enemy #%d hp=%d dmg=%d hit=%d delay=%d ranged=%d tier=%d terr=%d\n",
                   exp[i].id, g.health, g.damage, g.hitPM, g.attackDelayS,
                   g.ranged, g.tier, g.terrain);
        }
    }
    CHECK(ok, "every enemy (tier/terrain/HP/dmg/hit/delay/ranged) matches the spec");

    // representative loot lines (min/max/chance) — spot-check the three tiers.
    // snarling beast: fur 1-3@1000, meat 1-3@1000, teeth 1-3@800
    const Encounter& sb = ENCOUNTERS[E_SNARLING_BEAST];
    bool sbOk = sb.lootN == 3 &&
        sb.loot[0].slot == R_FUR   && sb.loot[0].mn == 1 && sb.loot[0].mx == 3 && sb.loot[0].chancePM == 1000 &&
        sb.loot[1].slot == R_MEAT  && sb.loot[1].mn == 1 && sb.loot[1].mx == 3 && sb.loot[1].chancePM == 1000 &&
        sb.loot[2].slot == R_TEETH && sb.loot[2].mn == 1 && sb.loot[2].mx == 3 && sb.loot[2].chancePM == 800;
    CHECK(sbOk, "snarling beast loot table matches (fur/meat/teeth min-max@chance)");
    // soldier (T3, ranged): cloth 5-10@800, bullets 1-5@500, rifle 1@200, medicine 1-2@100
    const Encounter& so = ENCOUNTERS[E_SOLDIER];
    bool soOk = so.lootN == 4 &&
        so.loot[0].slot == R_CLOTH    && so.loot[0].mn == 5 && so.loot[0].mx == 10 && so.loot[0].chancePM == 800 &&
        so.loot[1].slot == R_BULLETS  && so.loot[1].mn == 1 && so.loot[1].mx == 5  && so.loot[1].chancePM == 500 &&
        so.loot[2].isItem && so.loot[2].slot == I_RIFLE && so.loot[2].mn == 1 && so.loot[2].mx == 1 && so.loot[2].chancePM == 200 &&
        so.loot[3].slot == R_MEDICINE && so.loot[3].mn == 1 && so.loot[3].mx == 2  && so.loot[3].chancePM == 100;
    CHECK(soOk, "soldier loot table matches (cloth/bullets/rifle/medicine)");
}

// ===========================================================================
// Layer 2 — seeded end-to-end economy loop
// ===========================================================================

// Drive an armed setpiece fight to victory without letting the enemy swing (no
// fightTick, cooldown zeroed each swing) — a controlled kill.
static void simWin(WorldState& w, GameState& gs) {
    for (int guard = 0; guard < 800 && w.cx.active && !w.cx.won; guard++) {
        w.cx.weaponCool[0] = 0;
        w.fightAttack(gs, 0);
    }
}

static void layer2_economy() {
    printf("== [L2] seeded economy loop: embark -> iron mine -> clear -> goHome ==\n");
    GameState gs; gs.init();
    WorldState w; w.init();
    w.generateMap(0xBEEF01);

    // Stock the village: compass (Path key), cured meat (embark gate), torch (mine
    // gate) + bayonet (a weapon that one-shots the matriarch's 10 hp). A wagon
    // stays HOME — bag capacity is derived from gs.items (bagCapacityCenti), so it
    // widens the expedition's carry room without occupying a bag slot; the base
    // 10-unit bag would otherwise be full of cured meat and clamp all loot away.
    gs.stores[R_COMPASS] = 1 * FP;
    gs.stores[R_CURED_MEAT] = 5 * FP;
    gs.items[I_TORCH] = 1;
    gs.items[I_BAYONET] = 1;
    gs.items[I_WAGON] = 1;                  // capacity 40 units (kept at home)

    int16_t outfitRes[RES_COUNT] = { 0 };  outfitRes[R_CURED_MEAT] = 5;
    int16_t outfitItem[ITEM_COUNT] = { 0 }; outfitItem[I_TORCH] = 1; outfitItem[I_BAYONET] = 1;

    CHECK(w.embark(gs, outfitRes, outfitItem, 0x2233), "embark accepted (cured meat present)");
    CHECK(w.ex.active, "expedition active");
    CHECK(w.ex.outfitItem[I_TORCH] == 1 && w.ex.outfitItem[I_BAYONET] == 1,
          "torch + bayonet packed into the bag");
    CHECK(gs.items[I_TORCH] == 0 && gs.items[I_BAYONET] == 0, "gear deducted from the village");

    // Locate the iron mine on the working map and walk onto it (setPos next to it,
    // one real move so doSpace() fires the landmark hook — mechanism, not pathing).
    int mx = -1, my = -1;
    CHECK(findTile(w, T_IRON_MINE, mx, my), "iron mine exists on the map");
    CHECK(manhattan(mx, my) == 5, "iron mine at Manhattan 5 (spec §2.4)");
    w.ex.x = (int16_t)(mx - 1); w.ex.y = (int16_t)my;   // stand one cell west
    StepResult r = w.move(gs, DIR_EAST);
    CHECK(r.kind == STEP_LANDMARK && r.scene == SP_IRONMINE,
          "stepping onto the iron mine fires the ironmine setpiece");

    // Run the setpiece: go inside (spends torch) -> matriarch fight -> cleared.
    setpiece::bind(&w, &gs);
    CHECK(setpiece::begin(SP_IRONMINE), "iron-mine setpiece begins");
    CHECK(setpiece::btnAvailable(0), "go inside affordable (torch in bag)");
    CHECK(setpiece::choose(0) == RC_OK, "go inside accepted");
    CHECK(w.ex.outfitItem[I_TORCH] == 0, "torch spent by go inside");
    CHECK(setpiece::awaitingCombat() && w.fightActive() && w.combat().setpiece,
          "combat scene armed a setpiece fight");
    simWin(w, gs);
    CHECK(w.fightWon(), "matriarch defeated");
    setpiece::resolveCombat(true);
    CHECK(setpiece::choose(0) == RC_OK, "advance to cleared scene");
    CHECK(w.ex.clearedIron, "cleared scene set the iron-mine flag");
    CHECK(setpiece::choose(0) == RC_OK, "leave the cleared mine");
    CHECK(!setpiece::active(), "setpiece ended");
    // matriarch loot: teeth 5-10@1000 (always), scales 5-10@800, cloth 5-10@500.
    CHECK(w.ex.outfitRes[R_TEETH] >= 5, "matriarch loot (teeth) banked into the bag");

    // Walk home -> goHome commits the trip.
    w.ex.x = VILLAGE_X + 1; w.ex.y = VILLAGE_Y;
    int teethBag = w.ex.outfitRes[R_TEETH];
    r = w.move(gs, DIR_WEST);
    CHECK(r.kind == STEP_HOME, "stepping onto the village triggers goHome");
    CHECK(!w.ex.active, "expedition ended at home");
    CHECK(gs.buildings[B_IRON_MINE] == 1, "iron mine building unlocked in game state");
    CHECK(gs.hasUnlockedJob(), "a job is now staffable (miner unlocked)");
    {
        uint8_t jobs[JOB_COUNT]; int nj = gs.unlockedJobs(jobs, JOB_COUNT);
        bool sawIron = false;
        for (int i = 0; i < nj; i++) if (jobs[i] == J_IRON_MINER) sawIron = true;
        CHECK(sawIron, "iron miner appears in unlockedJobs()");
    }
    CHECK(gs.whole(R_TEETH) == teethBag, "banked teeth landed in the village stores");
    CHECK(gs.hasSeen(R_TEETH), "banked loot latched the seen bit");
    CHECK(w.countTiles(T_IRON_MINE) == 1, "iron mine tile survived (mine kept, road drawn)");
}

// ===========================================================================
// Layer 3 — Monte-Carlo statistics
// ===========================================================================

static void layer3_mapgen() {
    printf("== [L3] map generation over 250 seeds: counts / distances / village ==\n");
    // Expected landmark counts from the LANDMARKS table (prestige-only cache is
    // skipped by generateMap, so 0 on the map; outpost num=0).
    const int nSeeds = 250;
    bool countsOk = true, fixedOk = true, villageOk = true, ringOk = true;
    int firstBadSeed = -1;
    for (int s = 0; s < nSeeds; s++) {
        WorldState w; w.init();
        w.generateMap(0x1000u + (uint32_t)s * 2654435761u);  // spread the seeds
        // village + forest ring
        if (w.tileAt(VILLAGE_X, VILLAGE_Y) != T_VILLAGE) villageOk = false;
        const int nx[4] = { VILLAGE_X, VILLAGE_X, VILLAGE_X - 1, VILLAGE_X + 1 };
        const int ny[4] = { VILLAGE_Y - 1, VILLAGE_Y + 1, VILLAGE_Y, VILLAGE_Y };
        for (int i = 0; i < 4; i++) {
            uint8_t t = w.tileAt(nx[i], ny[i]);
            if (t != T_FOREST && !isLandmark(t)) ringOk = false;
        }
        // per-table counts
        for (int i = 0; i < LANDMARK_ROWS; i++) {
            const LandmarkDef& l = LANDMARKS[i];
            int want = l.prestigeOnly ? 0 : l.num;   // cache never generated
            if (w.countTiles(l.tile) != want) { countsOk = false; if (firstBadSeed < 0) firstBadSeed = s; }
        }
        // fixed-distance landmarks pinned to exact Manhattan distance
        if (landmarkDist(w, T_IRON_MINE) != 5)    fixedOk = false;
        if (landmarkDist(w, T_COAL_MINE) != 10)   fixedOk = false;
        if (landmarkDist(w, T_SULPHUR_MINE) != 20) fixedOk = false;
        if (landmarkDist(w, T_SHIP) != 28)        fixedOk = false;
        if (landmarkDist(w, T_EXECUTIONER) != 28) fixedOk = false;
    }
    CHECK(villageOk, "village at center on all 250 seeds");
    CHECK(ringOk,    "village neighbours forest-or-landmark (never field/barrens) on all seeds");
    CHECK(countsOk,  "landmark counts match the LANDMARKS table on all 250 seeds");
    CHECK(fixedOk,   "iron5/coal10/sulphur20/ship28/executioner28 exact on all 250 seeds");
    (void)firstBadSeed;
}

static void layer3_combat_hitrate() {
    printf("== [L3] combat hit rate over >=5000 rolls ~ 0.8 ==\n");
    GameState gs; gs.init();
    WorldState w; w.init(); w.generateMap(555);
    int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 3;
    gs.stores[R_CURED_MEAT] = 3 * FP;
    w.embark(gs, out, nullptr, 0xABCDEF01);
    // Fists only (no weapons packed). Huge-HP dummy so no swing ever wins.
    w.beginFight(E_FERAL_TERROR);
    w.cx.enemyHp = w.cx.enemyMaxHp = 30000;   // fists dmg 1: survives all rolls
    const int N = 6000;
    int hits = 0;
    for (int i = 0; i < N; i++) {
        w.cx.weaponCool[0] = 0;
        w.cx.enemyHp = 30000;                  // keep it alive
        w.fightAttack(gs, 0);                  // one hit roll off ex.rng
        if (!w.cx.lastMiss) hits++;
    }
    double rate = (double)hits / N;
    printf("     hit rate = %.4f over %d rolls\n", rate, N);
    CHECK(rate > 0.77 && rate < 0.83, "player hit rate within 0.8 +/- 0.03");
}

static void layer3_loot() {
    printf("== [L3] loot count draw over >=3000 draws: in [min,max-1], non-degenerate ==\n");
    GameState gs; gs.init();
    WorldState w; w.init(); w.generateMap(777);
    gs.items[I_CONVOY] = 1;                    // roomy bag: no capacity clamp
    int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 3;
    gs.stores[R_CURED_MEAT] = 3 * FP;
    w.embark(gs, out, nullptr, 0x0FF1CE55);

    // (a) max>min: fur 5-10 always hits -> num in [5,9], both ends seen.
    const LootDrop d1 = { false, R_FUR, 5, 10, 1000 };
    const int N = 4000;
    bool rangeOk = true; bool saw5 = false, saw9 = false;
    for (int i = 0; i < N; i++) {
        memset(w.ex.outfitRes, 0, sizeof w.ex.outfitRes);   // avoid accumulation clamp
        memset(w.ex.outfitItem, 0, sizeof w.ex.outfitItem);
        LootLine ln[1];
        int n = w.bankLootTable(gs, &d1, 1, ln, 1);
        if (n != 1) { rangeOk = false; continue; }
        int g = ln[0].got;
        if (g < 5 || g > 9) rangeOk = false;
        if (g == 5) saw5 = true;
        if (g == 9) saw9 = true;
    }
    CHECK(rangeOk, "fur 5-10 draws all land in [5,9] (max-min, exclusive of max)");
    CHECK(saw5 && saw9, "distribution non-degenerate (both 5 and 9 occur)");

    // (b) max==min: alien alloy 1-1 always exactly 1 (still consumes a random).
    const LootDrop d2 = { false, R_ALIEN_ALLOY, 1, 1, 1000 };
    bool fixedOk = true;
    for (int i = 0; i < 1000; i++) {
        memset(w.ex.outfitRes, 0, sizeof w.ex.outfitRes);
        LootLine ln[1];
        int n = w.bankLootTable(gs, &d2, 1, ln, 1);
        if (n != 1 || ln[0].got != 1) fixedOk = false;
    }
    CHECK(fixedOk, "alien alloy 1-1 always draws exactly 1 (max==min)");

    // (c) chance gate: 0-chance never banks, 1000-chance always banks.
    const LootDrop d0 = { false, R_TEETH, 1, 2, 0 };
    memset(w.ex.outfitRes, 0, sizeof w.ex.outfitRes);
    int got0 = 0;
    for (int i = 0; i < 500; i++) { LootLine ln[1]; got0 += w.bankLootTable(gs, &d0, 1, ln, 1); }
    CHECK(got0 == 0, "chance 0 never drops");
}

static void layer3_encounter_rate() {
    printf("== [L3] encounter rate over >=5000 rollable steps ~ 0.20, zero within FIGHT_DELAY ==\n");
    GameState gs; gs.init();
    WorldState w; w.init(); w.generateMap(31415);
    int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 3;
    gs.stores[R_CURED_MEAT] = 3 * FP;
    w.embark(gs, out, nullptr, 0xF00DBEEF);
    // Featureless plain: all forest (so every roll has an available encounter),
    // infinite supplies so nothing dies and no goHome/landmark ever fires.
    for (int i = 0; i < WORLD_CELLS; i++) w.ex.tiles[i] = T_FOREST;
    w.ex.outfitRes[R_CURED_MEAT] = 30000;
    w.ex.maxWater = 30000; w.ex.water = 30000;

    long rollable = 0, fights = 0, withinDelayFights = 0;
    long steps = 0;
    while (rollable < 8000 && steps < 400000) {
        int f0 = w.ex.fightMove;               // pre-step counter
        bool rollableStep = (f0 + 1) > FIGHT_DELAY;
        StepResult r = w.move(gs, DIR_EAST);   // clamps at the wall, still a full step
        steps++;
        if (rollableStep) {
            rollable++;
            if (r.kind == STEP_FIGHT) fights++;
        } else {
            if (r.kind == STEP_FIGHT) withinDelayFights++;
        }
    }
    double rate = (double)fights / (double)rollable;
    printf("     rollable=%ld fights=%ld rate=%.4f  within-delay fights=%ld\n",
           rollable, fights, rate, withinDelayFights);
    CHECK(rollable >= 5000, "collected >=5000 rollable steps");
    CHECK(withinDelayFights == 0, "no encounter ever fires within FIGHT_DELAY (guaranteed grace)");
    CHECK(rate > 0.18 && rate < 0.22, "encounter rate within 0.20 +/- 0.02");
}

// ===========================================================================
// Layer 4 — save robustness
// ===========================================================================

static void layer4_worldbin() {
    printf("== [L4] world.bin: v1 migration / truncation / bad version / v2 round-trip ==\n");
    const size_t MASK = WORLD_MASK_BYTES;
    const size_t V1SZ = 12 + WORLD_CELLS + 2 * MASK;   // 4665
    const size_t V2SZ = 12 + WORLD_CELLS + 3 * MASK;   // 5131

    // Source map to migrate.
    WorldState src; src.init(); src.generateMap(0x5A5A5A);

    // -- v1 file (no usedOutpost mask): hand-build + load + assert migration --
    {
        uint8_t* buf = (uint8_t*)malloc(V1SZ);
        size_t o = 0;
        rawU32(buf, o, WORLD_MAGIC);
        buf[o++] = 1; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;   // ver 1
        rawU32(buf, o, src.seed);
        memcpy(buf + o, src.tiles, WORLD_CELLS);    o += WORLD_CELLS;
        memcpy(buf + o, src.revealed, MASK);        o += MASK;
        memcpy(buf + o, src.visited, MASK);         o += MASK;
        CHECK(o == V1SZ, "v1 buffer is exactly the legacy size (4665)");
        writeRaw(ADR_WORLD_PATH, buf, o);
        free(buf);

        WorldState ld; ld.init();
        CHECK(ld.loadWorld(), "v1 world.bin loads (migrated)");
        CHECK(ld.seed == src.seed, "v1: seed preserved");
        CHECK(memcmp(ld.tiles, src.tiles, WORLD_CELLS) == 0, "v1: tiles preserved");
        CHECK(memcmp(ld.revealed, src.revealed, MASK) == 0, "v1: revealed preserved");
        CHECK(memcmp(ld.visited, src.visited, MASK) == 0, "v1: visited preserved");
        bool allZero = true;
        for (size_t i = 0; i < MASK; i++) if (ld.usedOutpost[i] != 0) allZero = false;
        CHECK(allZero, "v1: usedOutpost mask migrated to empty");
    }

    // -- truncated file rejected, no crash --
    {
        uint8_t small[100];
        size_t o = 0;
        rawU32(small, o, WORLD_MAGIC);
        small[o++] = WORLD_VER; small[o++] = 0; small[o++] = 0; small[o++] = 0;
        for (; o < sizeof small; o++) small[o] = 0;
        writeRaw(ADR_WORLD_PATH, small, sizeof small);
        WorldState ld; ld.init();
        CHECK(!ld.loadWorld(), "truncated world.bin rejected (no crash)");
    }

    // -- bad version rejected --
    {
        uint8_t* buf = (uint8_t*)malloc(V2SZ);
        memset(buf, 0, V2SZ);
        size_t o = 0;
        rawU32(buf, o, WORLD_MAGIC);
        buf[o++] = 99; buf[o++] = 0; buf[o++] = 0; buf[o++] = 0;   // bogus ver
        writeRaw(ADR_WORLD_PATH, buf, V2SZ);
        free(buf);
        WorldState ld; ld.init();
        CHECK(!ld.loadWorld(), "bad-version world.bin rejected");
    }

    // -- bad magic rejected --
    {
        uint8_t* buf = (uint8_t*)malloc(V2SZ);
        memset(buf, 0, V2SZ);
        size_t o = 0;
        rawU32(buf, o, 0xDEADBEEF);
        buf[o++] = WORLD_VER;
        writeRaw(ADR_WORLD_PATH, buf, V2SZ);
        free(buf);
        WorldState ld; ld.init();
        CHECK(!ld.loadWorld(), "bad-magic world.bin rejected");
    }

    // -- v2 round-trip incl. usedOutpost mask --
    {
        WorldState w; w.init(); w.generateMap(0x0B0B0B);
        w.usedOutpost[0] = 0xA5; w.usedOutpost[10] = 0x3C;   // some used-outpost bits
        CHECK(w.saveWorld(), "v2 saveWorld ok");
        WorldState ld; ld.init();
        CHECK(ld.loadWorld(), "v2 loadWorld ok");
        CHECK(ld.seed == w.seed, "v2: seed round-trips");
        CHECK(memcmp(ld.tiles, w.tiles, WORLD_CELLS) == 0, "v2: tiles round-trip");
        CHECK(memcmp(ld.revealed, w.revealed, MASK) == 0, "v2: revealed round-trips");
        CHECK(memcmp(ld.usedOutpost, w.usedOutpost, MASK) == 0, "v2: usedOutpost mask round-trips");
    }
}

static void layer4_trekbin() {
    printf("== [L4] trek.bin: truncation / corrupt / round-trip ==\n");
    // -- normal round-trip --
    {
        GameState gs; gs.init();
        WorldState w; w.init(); w.generateMap(0x246);
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 8; out[R_MEDICINE] = 2;
        gs.stores[R_CURED_MEAT] = 8 * FP; gs.stores[R_MEDICINE] = 2 * FP;
        gs.items[I_RIFLE] = 1;
        int16_t outi[ITEM_COUNT] = { 0 }; outi[I_RIFLE] = 1;
        w.embark(gs, out, outi, 0x99);
        w.move(gs, DIR_EAST); w.move(gs, DIR_EAST);   // a couple of steps, saveTrek each
        WorldState ld; ld.init();
        CHECK(ld.loadTrek(), "trek.bin loads");
        CHECK(ld.ex.active, "restored expedition active");
        CHECK(ld.ex.x == w.ex.x && ld.ex.y == w.ex.y, "position round-trips");
        CHECK(ld.ex.hp == w.ex.hp && ld.ex.water == w.ex.water, "hp/water round-trip");
        CHECK(ld.ex.rng == w.ex.rng, "expedition rng round-trips");
        CHECK(ld.ex.outfitRes[R_CURED_MEAT] == w.ex.outfitRes[R_CURED_MEAT] &&
              ld.ex.outfitItem[I_RIFLE] == 1, "bag (res+items) round-trips");
        CHECK(memcmp(ld.ex.tiles, w.ex.tiles, WORLD_CELLS) == 0, "working map round-trips");
    }
    // -- truncated trek rejected --
    {
        uint8_t small[40];
        size_t o = 0; rawU32(small, o, TREK_MAGIC);
        small[o++] = TREK_VER;
        for (; o < sizeof small; o++) small[o] = 0;
        writeRaw(ADR_TREK_PATH, small, sizeof small);
        WorldState ld; ld.init();
        CHECK(!ld.loadTrek(), "truncated trek.bin rejected (no crash)");
        CHECK(!ld.ex.active, "rejected trek leaves no active expedition");
    }
    // -- corrupt magic rejected (full-size buffer, wrong magic) --
    {
        // full-size so the length gate passes; only the magic is wrong.
        const size_t TSZ = 68 + RES_COUNT * 2 + ITEM_COUNT * 2 +
                           WORLD_CELLS + 2 * WORLD_MASK_BYTES;
        uint8_t* buf = (uint8_t*)malloc(TSZ);
        memset(buf, 0, TSZ);
        size_t o = 0; rawU32(buf, o, 0x00000000);   // bad magic
        buf[o] = TREK_VER;
        writeRaw(ADR_TREK_PATH, buf, TSZ);
        free(buf);
        WorldState ld; ld.init();
        CHECK(!ld.loadTrek(), "corrupt-magic trek.bin rejected");
    }
}

static void layer4_gamejson() {
    printf("== [L4] game.json: missing \"dcool\" -> deathAt == 0 ==\n");
    GameState gs; gs.init();
    gs.deathAt = 987654;                       // a lockout in the source save
    gs.stores[R_WOOD] = 42 * FP;
    static char buf[8192];
    gs.toJson(buf, sizeof buf);

    // Strip the "dcool":N, token to simulate a pre-2.5 save that never had it.
    char stripped[8192];
    const char* key = "\"dcool\":";
    char* pos = strstr(buf, key);
    CHECK(pos != nullptr, "reference save contains a dcool field to strip");
    if (pos) {
        size_t head = (size_t)(pos - buf);
        const char* comma = strchr(pos, ',');        // token ends at the next comma
        const char* rest = comma ? comma + 1 : "";
        memcpy(stripped, buf, head);
        strcpy(stripped + head, rest);
    } else {
        strcpy(stripped, buf);
    }
    CHECK(strstr(stripped, "dcool") == nullptr, "stripped save has no dcool key");

    GameState gs2; gs2.init(); gs2.deathAt = 555;    // dirty, must be overwritten
    CHECK(gs2.fromJson(stripped), "dcool-less save still parses");
    CHECK(gs2.deathAt == 0, "missing dcool loads as deathAt == 0 (no phantom lockout)");
    CHECK(gs2.whole(R_WOOD) == 42, "other fields still load correctly");
}


// ===========================================================================
// Layer 5: the Space level (Phase 3b) — research-phase3.md §2 (upstream) and
// §8 (the e-ink re-derivation). This is the only layer whose subject is an
// ACTION game, so the shape is different from the four above: every rule is
// pure (space_game.cpp has no Arduino in it), the PRNG is seeded, and a whole
// 60-second flight is 679 deterministic calls to step(). What that buys is a
// golden frame count for the victory clock, which is the one acceptance
// criterion (§11.3b.5) that nobody can eyeball on the glass.
// ===========================================================================

namespace sp = adr::space;

static void layer5_space_constants() {
    printf("== [L5] space: geometry + tempo constants (§8.2/§8.4/§8.5) ==\n");
    // Double entry, same discipline as layer1_constants: the numbers are
    // transcribed a SECOND time here straight out of the spec, so an edit to
    // space_game.h that drifts one of them fails here rather than on the glass.
    CHECK(sp::FRAME_MS == 92,            "logic frame is 92 ms (4 scans, §8.2)");
    CHECK(sp::GAME_SECONDS == 60,        "the flight is 60 s of game time (§2.6)");
    CHECK(sp::PF_TOP == 84 && sp::PF_BOT == 788, "playfield is y 84..788 (§8.5)");
    CHECK(sp::PF_BOT - sp::PF_TOP == 704, "playfield is 704 px tall (§8.5)");
    CHECK(sp::CTRL_TOP == 792,           "control band starts at y=792 (§8.5)");
    CHECK(sp::SHIP_W == 48 && sp::SHIP_H == 36, "ship is 48x36 (§8.4 d5)");
    CHECK(sp::SHIP_Y == 740,             "ship y is fixed at 740 (§8.3, 1-D)");
    CHECK(sp::SHIP_X_MIN == 24 && sp::SHIP_X_MAX == 516,
          "ship x range is [24,516] (§11.3b.2)");
    CHECK(sp::MAX_ASTEROIDS == 16,       "at most 16 asteroids alive (§8.4 d4)");
    CHECK(sp::AST_SPEED_MIN == 24 && sp::AST_SPEED_MAX == 48,
          "asteroid speed is 24..48 px/frame = 260..521 px/s (§8.4 d3/d5)");
    CHECK(sp::AST_DIAM_SMALL == 40 && sp::AST_DIAM_MID == 48 &&
          sp::AST_DIAM_BIG == 56, "asteroid diameters are 40/48/56 (§8.4 d1)");
    CHECK(sp::AST_DIAM_SMALL >= 40, "40 px is the hard readability floor (§8.4 d2)");
    CHECK(sp::XITION_FRAMES == 3,        "layer transition holds 3 frames = 276 ms (§9.3)");
}

static void layer5_space_curves() {
    printf("== [L5] space: thrusters, spawn curve, layers, tones ==\n");
    // §8.3: SHIP_MAX_STEP = min(72, 32 + 8*thrusters).
    CHECK(sp::shipMaxStep(1) == 40, "thrusters 1 -> 40 px/frame (434 px/s)");
    CHECK(sp::shipMaxStep(2) == 48, "thrusters 2 -> 48 px/frame");
    CHECK(sp::shipMaxStep(3) == 56, "thrusters 3 -> 56 px/frame");
    CHECK(sp::shipMaxStep(4) == 64, "thrusters 4 -> 64 px/frame");
    CHECK(sp::shipMaxStep(5) == 72, "thrusters 5 -> 72 px/frame (the cap)");
    CHECK(sp::shipMaxStep(9) == 72, "thrusters past 5 buy nothing (§12 Q12)");
    CHECK(sp::shipMaxStep(0) == 40, "a corrupt thrusters=0 still flies (floor at 1)");

    // §8.4 d4's table, band by band, at both ends of every band.
    CHECK(sp::spawnIntervalFrames(0)  == 10 && sp::waveCount(0)  == 1, "0 km:  1 / 10 frames");
    CHECK(sp::spawnIntervalFrames(10) == 10 && sp::waveCount(10) == 1, "10 km: 1 / 10 frames");
    CHECK(sp::spawnIntervalFrames(11) == 7  && sp::waveCount(11) == 1, "11 km: 1 / 7 frames");
    CHECK(sp::spawnIntervalFrames(20) == 7  && sp::waveCount(20) == 1, "20 km: 1 / 7 frames");
    CHECK(sp::spawnIntervalFrames(21) == 7  && sp::waveCount(21) == 2, "21 km: 2 / 7 frames");
    CHECK(sp::spawnIntervalFrames(30) == 7  && sp::waveCount(30) == 2, "30 km: 2 / 7 frames");
    CHECK(sp::spawnIntervalFrames(31) == 5  && sp::waveCount(31) == 2, "31 km: 2 / 5 frames");
    CHECK(sp::spawnIntervalFrames(45) == 5  && sp::waveCount(45) == 2, "45 km: 2 / 5 frames");
    CHECK(sp::spawnIntervalFrames(46) == 5  && sp::waveCount(46) == 3, "46 km: 3 / 5 frames");
    CHECK(sp::spawnIntervalFrames(60) == 5  && sp::waveCount(60) == 3, "60 km: 3 / 5 frames");

    // §2.6's thresholds, WITHOUT upstream's `altitude % 10` staleness (§12 Q4).
    CHECK(sp::layerOf(0) == 0 && sp::layerOf(9) == 0,   "0..9 km is the troposphere");
    CHECK(sp::layerOf(10) == 1 && sp::layerOf(19) == 1, "10..19 km is the stratosphere");
    CHECK(sp::layerOf(20) == 2 && sp::layerOf(29) == 2, "20..29 km is the mesosphere");
    CHECK(sp::layerOf(30) == 3 && sp::layerOf(44) == 3, "30..44 km is the thermosphere");
    CHECK(sp::layerOf(45) == 4, "45 km IS the exosphere — upstream's %10 bug is not ported");
    CHECK(sp::layerOf(49) == 4 && sp::layerOf(59) == 4, "45..59 km stays the exosphere");
    CHECK(sp::layerOf(60) == 5, "60 km is space");
    CHECK(sp::isLayerEdge(10) && sp::isLayerEdge(20) && sp::isLayerEdge(30) &&
          sp::isLayerEdge(45) && sp::isLayerEdge(60), "the five transition altitudes");
    CHECK(!sp::isLayerEdge(0) && !sp::isLayerEdge(40) && !sp::isLayerEdge(50),
          "and nothing else fires a transition");

    // §2.5's three bands, three pitches (§8.4).
    CHECK(sp::hitToneHz(0) == 880 && sp::hitToneHz(20) == 880, "<=20 km hit tone 880 Hz");
    CHECK(sp::hitToneHz(21) == 1200 && sp::hitToneHz(40) == 1200, "21..40 km hit tone 1200 Hz");
    CHECK(sp::hitToneHz(41) == 1600 && sp::hitToneHz(60) == 1600, ">40 km hit tone 1600 Hz");
}

static void layer5_space_control() {
    printf("== [L5] space: the absolute control band (§8.3) ==\n");
    sp::Game g;
    sp::reset(g, 5, 1, 12345);                       // thrusters 1 -> 40 px/frame
    CHECK(g.shipX == sp::UI_W / 2, "the ship starts centred (space.js:64)");
    CHECK(g.maxStep == 40, "maxStep comes from thrusters at reset");

    // A finger at the far right: the ship closes at most maxStep a frame and
    // stops exactly at the clamp, never past it.
    int start = g.shipX;
    sp::step(g, 539);
    CHECK(g.shipX == start + 40, "one frame closes at most maxStep on the finger");
    for (int i = 0; i < 40; i++) sp::step(g, 539);
    CHECK(g.shipX == sp::SHIP_X_MAX, "a finger at the right edge reaches x=516 and stops");

    // Lifting the finger holds position — no re-centring, no drift (§8.3).
    int held = g.shipX;
    for (int i = 0; i < 10; i++) sp::step(g, -1);
    CHECK(g.shipX == held, "a lifted finger leaves the ship exactly where it was");

    for (int i = 0; i < 40; i++) sp::step(g, 0);
    CHECK(g.shipX == sp::SHIP_X_MIN, "a finger at the left edge reaches x=24 and stops");

    // A finger closer than maxStep lands the ship exactly on it (1:1 absolute).
    sp::step(g, 40);
    CHECK(g.shipX == 40, "a finger within maxStep is matched exactly, not overshot");
}

// Drop one asteroid onto the field by hand. The generator is random by design,
// so every collision assertion below places its own.
static void placeAsteroid(sp::Game& g, int x, int y, int r) {
    for (int i = 0; i < sp::MAX_ASTEROIDS; i++) {
        if (g.ast[i].alive) continue;
        g.ast[i].x = (int16_t)x; g.ast[i].y = (int16_t)y;
        g.ast[i].r = (uint8_t)r; g.ast[i].vy = 0; g.ast[i].alive = true;
        return;
    }
}

static void layer5_space_collision() {
    printf("== [L5] space: AABB collisions and the hull (§2.5/§8.4 d5) ==\n");
    sp::Game g;
    sp::reset(g, 3, 1, 999);
    int x = g.shipX;

    // Dead centre: a hit, one hull, and the asteroid is consumed.
    placeAsteroid(g, x, sp::SHIP_Y, 24);
    sp::FrameOut ev = sp::step(g, -1);
    CHECK(ev.hit, "an asteroid on the ship is a hit");
    CHECK(g.hull == 2, "a hit costs exactly 1 hull");
    CHECK(sp::aliveCount(g) == 0, "the asteroid that hit is removed (§2.5)");
    CHECK(g.hitFlash > 0, "the hit arms the reversed-ship feedback");

    // One pixel outside the AABB on each axis: no hit. vy is 0 so the placement
    // is exactly what is tested.
    sp::reset(g, 3, 1, 999);
    x = g.shipX;
    placeAsteroid(g, x + sp::SHIP_W / 2 + 24 + 1, sp::SHIP_Y, 24);
    ev = sp::step(g, -1);
    CHECK(!ev.hit && g.hull == 3, "one px clear on x is a miss");
    sp::reset(g, 3, 1, 999);
    placeAsteroid(g, g.shipX, sp::SHIP_Y - sp::SHIP_H / 2 - 24 - 1, 24);
    ev = sp::step(g, -1);
    CHECK(!ev.hit && g.hull == 3, "one px clear on y is a miss");

    // Two asteroids in one frame cost two hull.
    sp::reset(g, 3, 1, 999);
    placeAsteroid(g, g.shipX - 10, sp::SHIP_Y, 24);
    placeAsteroid(g, g.shipX + 10, sp::SHIP_Y, 24);
    sp::step(g, -1);
    CHECK(g.hull == 1, "two asteroids in one frame cost two hull");

    // An asteroid past the floor is retired, not counted.
    sp::reset(g, 3, 1, 999);
    placeAsteroid(g, g.shipX, sp::PF_BOT + 25, 24);
    sp::step(g, -1);
    CHECK(sp::aliveCount(g) == 0 && g.hull == 3,
          "an asteroid past y=788 is removed without a hit");

    // The §8.4 d4 cap: pile on far more than 16 and the field never exceeds it.
    sp::reset(g, 99, 1, 4242);
    for (int i = 0; i < 400; i++) sp::step(g, -1);
    CHECK(sp::aliveCount(g) <= sp::MAX_ASTEROIDS,
          "the field never holds more than 16 asteroids (§8.4 d4)");
}

static void layer5_space_crash() {
    printf("== [L5] space: the crash sequence (§8.6) ==\n");
    sp::Game g;
    sp::reset(g, 1, 1, 77);
    placeAsteroid(g, g.shipX, sp::SHIP_Y, 24);
    sp::FrameOut ev = sp::step(g, -1);
    CHECK(ev.crashed && g.crashed, "the last hull point starts the crash");
    CHECK(g.hull == 0, "hull is 0 at the crash");
    CHECK(g.phase == sp::PH_CRASH_POP, "the crash opens on the 96x96 burst frame");

    int frames = 0;
    while (!sp::done(g) && frames < 100) { sp::step(g, -1); frames++; }
    CHECK(sp::done(g), "the crash sequence terminates");
    // 1 burst + 3 black + 1 text, each counted by the step that LEAVES it.
    CHECK(frames == sp::CRASH_POP_FRAMES + sp::CRASH_BLACK_FRAMES + sp::CRASH_TEXT_FRAMES,
          "the crash 演出 is 1 + 3 + 1 logic frames (§8.6)");
    CHECK(!g.won, "a crash is not a win");
}

static void layer5_space_victory() {
    printf("== [L5] space: the victory clock (§11.3b.5) ==\n");
    // A hull nothing can empty, so the flight is decided by the clock alone.
    sp::Game g;
    sp::reset(g, 30000, 1, 20260804u);
    long winFrame = -1;
    long frames = 0;
    while (!sp::done(g) && frames < 5000) {
        sp::FrameOut ev = sp::step(g, -1);
        frames++;
        if (ev.won) winFrame = frames;
        if (ev.crashed) break;
    }
    CHECK(g.won && !g.crashed, "an untouchable ship reaches space");
    CHECK(g.altitude == 60, "the flight ends at 60 km (§2.6)");
    CHECK(g.xitions == 5, "five layer transitions play: 10/20/30/45/60 (§9.3)");
    CHECK(g.layer == 5, "the last layer announced is 太空 / Space");
    // 653 PLAY frames carry gameMs past 60 000, and the five transitions add 3
    // frames each while the clock is FROZEN — §9.3's whole point.
    CHECK(winFrame == 653 + 5 * sp::XITION_FRAMES,
          "victory fires on logic frame 668 (653 played + 15 paused)");
    long winMs = winFrame * (long)sp::FRAME_MS;
    CHECK(winMs >= 60400 && winMs <= 62400,
          "victory lands at 61.4 +/- 1 s of wall clock (§11.3b.5)");
    CHECK(g.gameMs >= 60000 && g.gameMs < 60000 + (uint32_t)sp::FRAME_MS,
          "exactly 60 s of GAME time was played; the transitions cost none of it");

    // The 演出 that follows: 3 rise frames, then 8 of empty sky.
    CHECK(frames == winFrame + sp::WIN_RISE_FRAMES + sp::WIN_WHITE_FRAMES,
          "the victory 演出 is 3 + 8 logic frames (§8.6)");
}

static void layer5_space_state() {
    printf("== [L5] space: the outcome lands in GameState (§2.7/§11.3b.4) ==\n");
    GameState gs; gs.init();
    gs.shipUnlocked = true;
    gs.shipHull = 4; gs.shipThrusters = 3;
    gs.stores[R_ALIEN_ALLOY] = 7 * FP;
    gs.stores[R_WOOD] = 100 * FP;
    gs.items[I_IRON_SWORD] = 1;

    CHECK(gs.startLiftoff(1000) == RC_OK, "liftoff is allowed with hull > 0");
    CHECK(!gs.spacePending, "startLiftoff alone does not launch");
    gs.liftOff();
    CHECK(gs.spacePending, "liftOff raises the pending flag for the app loop");

    // A crash costs the cooldown and NOTHING else (§2.7 / §12 Q14).
    uint32_t score0 = gs.scoreTotal;
    gs.onSpaceCrash(2000);
    CHECK(!gs.spacePending, "the crash clears the pending flag");
    CHECK(gs.shipHull == 4 && gs.shipThrusters == 3, "a crash spends no hull or engine");
    CHECK(gs.whole(R_ALIEN_ALLOY) == 7 && gs.whole(R_WOOD) == 100,
          "a crash spends no inventory");
    CHECK(gs.items[I_IRON_SWORD] == 1, "a crash spends no items");
    CHECK(gs.liftoffCooldownLeft(2000) == LIFTOFF_COOLDOWN_S,
          "a crash restamps the full 120 s liftoff cooldown (space.js:376)");
    CHECK(gs.liftoffCooldownLeft(2000 + LIFTOFF_COOLDOWN_S) == 0,
          "and it expires 120 s later");
    CHECK(!gs.spaceWon && gs.scoreTotal == score0, "a crash banks nothing");

    // A win banks the score and latches the flag; it does NOT delete the save
    // (upstream does — §12 Q1 parks that in 3d).
    gs.liftOff();
    uint32_t s = gs.score();
    gs.onSpaceVictory(3000, s);
    CHECK(gs.spaceWon, "a win latches spaceWon");
    CHECK(gs.scoreTotal == s, "a win banks its score into the running total");
    CHECK(gs.shipHull == 4, "a win spends no hull either");
    gs.onSpaceVictory(4000, s);
    CHECK(gs.scoreTotal == 2 * s, "a second win adds to the total");
}

static void layer5_space_score() {
    printf("== [L5] space: scoring.js §2.8, against a hand-computed save ==\n");
    GameState gs; gs.init();
    // Hand computation, item by item, exactly as scoring.js would:
    //   wood      10 x   1 =   10
    //   fur        4 x 1.5 =    6
    //   iron       3 x   2 =    6
    //   sulphur    2 x   3 =    6
    //   bait       3 x 1.5 =  4.5
    //   bullets    5 x   3 =   15
    //   torch      2 x   1 =    2
    //   bone spear 1 x  10 =   10
    //   rifle      1 x 150 =  150
    //   alloy      2 x  10 =   20
    //   hull       3 x  50 =  150
    //                       ------
    //                        379.5  -> floored to 379
    gs.stores[R_WOOD]        = 10 * FP;
    gs.stores[R_FUR]         =  4 * FP;
    gs.stores[R_IRON]        =  3 * FP;
    gs.stores[R_SULPHUR]     =  2 * FP;
    gs.stores[R_BAIT]        =  3 * FP;
    gs.stores[R_BULLETS]     =  5 * FP;
    gs.stores[R_ALIEN_ALLOY] =  2 * FP;
    gs.items[I_TORCH]        = 2;
    gs.items[I_BONE_SPEAR]   = 1;
    gs.items[I_RIFLE]        = 1;
    gs.shipHull              = 3;
    CHECK(gs.score() == 379, "the golden save scores 379 (379.5 floored)");

    GameState empty; empty.init();
    CHECK(empty.score() == 0, "a fresh save scores 0 (hull is 0 too)");

    // The two heaviest weights, isolated, so a table typo cannot hide in a sum.
    GameState one; one.init();
    one.items[I_LASER_RIFLE] = 1;
    CHECK(one.score() == 150, "laser rifle is worth 150");
    one.init(); one.items[I_BAYONET] = 1;
    CHECK(one.score() == 100, "bayonet is worth 100");
    one.init(); one.shipHull = 1;
    CHECK(one.score() == 50, "each hull point is worth 50 (scoring.js:23)");
    // scoring.js:22 — the command deck's beacon, the heaviest line in the table.
    one.init(); one.stores[R_FLEET_BEACON] = 1 * FP;
    CHECK(one.score() == 500, "one fleet beacon is worth 500");
    one.stores[R_FLEET_BEACON] = 2 * FP;
    CHECK(one.score() == 1000, "and it scales linearly like every other store");
    // The same golden save, now carrying the beacon: 379.5 + 500 -> 879.
    gs.stores[R_FLEET_BEACON] = 1 * FP;
    CHECK(gs.score() == 879, "the golden save + a beacon scores 879");
    // A half-weight on an ODD count is where an integer port goes wrong: 1 fur
    // is 1.5 and must floor to 1, not round to 2 and not truncate to 0.
    one.init(); one.stores[R_FUR] = 1 * FP;
    CHECK(one.score() == 1, "one fur (1.5) floors to 1");
    one.stores[R_FUR] = 3 * FP;
    CHECK(one.score() == 4, "three fur (4.5) floors to 4");
}

static void layer5_space_save() {
    printf("== [L5] space: save round-trip and v4 -> v5 compatibility ==\n");
    GameState gs; gs.init();
    gs.shipUnlocked = true; gs.shipSeenWarning = true;
    gs.shipHull = 6; gs.shipThrusters = 4;
    gs.spaceWon = true; gs.scoreTotal = 123456;
    gs.spacePending = true;                       // RAM-only: must NOT survive
    static char buf[8192];
    gs.toJson(buf, sizeof buf);
    CHECK(strstr(buf, "\"v\":5") != nullptr, "a fresh save is written as v5");
    CHECK(strstr(buf, "\"tscore\":123456") != nullptr, "the running total is a flat key");

    GameState r; r.init();
    CHECK(r.fromJson(buf), "a v5 save parses");
    CHECK(r.spaceWon && r.scoreTotal == 123456, "spaceWon + total survive a round trip");
    CHECK(r.shipHull == 6 && r.shipThrusters == 4, "the v4 starship fields still survive");
    CHECK(!r.spacePending, "spacePending is RAM-only and never persisted");

    // A v4 save: no "tscore" key and no bit 128 in "fl". It must load, and it
    // must read as "never flown" rather than as garbage.
    char v4[8192];
    { // strip tscore, relabel the version
        const char* key = "\"tscore\":";
        char* pos = strstr(buf, key);
        CHECK(pos != nullptr, "reference save contains a tscore field to strip");
        size_t head = (size_t)(pos - buf);
        const char* comma = strchr(pos, ',');
        memcpy(v4, buf, head);
        strcpy(v4 + head, comma ? comma + 1 : "");
        char* vp = strstr(v4, "\"v\":5");
        if (vp) vp[4] = '4';
        // clear bit 128 out of "fl"
        char* fp = strstr(v4, "\"fl\":");
        if (fp) {
            long fl = strtol(fp + 5, nullptr, 10);
            char* end = strchr(fp, ',');
            char tail[8192]; strcpy(tail, end ? end : "");
            snprintf(fp, sizeof(v4) - (size_t)(fp - v4), "\"fl\":%ld%s",
                     fl & ~128L, tail);
        }
    }
    CHECK(strstr(v4, "tscore") == nullptr, "the v4 save has no tscore key");
    GameState old; old.init();
    old.spaceWon = true; old.scoreTotal = 999;    // dirty, must be overwritten
    CHECK(old.fromJson(v4), "a v4 save still loads under v5 firmware");
    CHECK(!old.spaceWon, "a v4 save reads as never having reached space");
    CHECK(old.scoreTotal == 0, "a v4 save reads as a zero running total");
    CHECK(old.shipHull == 6 && old.shipThrusters == 4,
          "and its v4 starship fields are untouched by the upgrade");
}

// ===========================================================================
// Layer 6 — the Executioner combat mechanics (Phase 3c-1)
//
// docs/research-phase3.md §3.3 lists six statuses, a periodic scene `specials`
// hook, an `atHealth` blood line and a death `explosion`; §10.4 pins how each maps
// onto the port's 1s tick. Every mechanic below gets the same three shapes the 3c-1
// brief asks for — it TRIGGERS, it CLEARS, and its BOUNDARY behaves — driven off a
// throwaway SetpieceEnemy rather than any shipped table, so none of this depends on
// the Executioner data landing (3c-2).
// ===========================================================================

// A bare battlefield: a live expedition parked on plain terrain with a known rng,
// so combat rolls are reproducible. Mirrors world_smoke's `plant`.
static void plantEx(WorldState& w, GameState& gs, int hp, uint32_t rng) {
    gs.init();
    w.init();
    w.generateMap(0x3C1A11);
    gs.stores[R_CURED_MEAT] = 5 * FP;
    int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;
    w.embark(gs, out, nullptr, rng);
    w.ex.maxHp = (int16_t)hp; w.ex.hp = (int16_t)hp;
}

// A setpiece enemy that never lands a blow on its own (hitPM 0, delay huge), so a
// test only sees the mechanic it is actually exercising. Callers overwrite the
// mechanic fields they care about.
static SetpieceEnemy inertEnemy(int health, int damage) {
    SetpieceEnemy e{};
    e.chara = 'Z'; e.notif = "a test construct";
    e.health = (int16_t)health; e.damage = (int16_t)damage;
    e.hitPM = 1000; e.attackDelayS = 999; e.ranged = false;
    e.lootN = 0;
    return e;
}

// Swing weapon slot 0 until one CONNECTS, ignoring the cooldown. Both the cooldown
// and the 0.8 base hit chance are already covered by their own tests; retrying here
// is what makes "one landed hit does exactly X" assertable without pinning an rng
// stream. Returns the status of the landing swing (or the rejection that stopped it).
static uint8_t swing(WorldState& w, GameState& gs) {
    uint8_t st = FIGHT_NOOP;
    for (int guard = 0; guard < 200; guard++) {
        w.cx.weaponCool[0] = 0;
        st = w.fightAttack(gs, 0);
        if (st == FIGHT_NOOP || !w.cx.lastMiss) break;
    }
    return st;
}

static void layer6_fold() {
    printf("== [L6] sub-second attackDelay folds to a DPS-equivalent tick ==\n");
    for (int i = 0; i < SUBSECOND_FOLD_ROWS; i++) {
        const SubsecondFold& f = SUBSECOND_FOLDS[i];
        AttackFold got = foldAttack(f.dmg, f.delayCS);
        char msg[160];
        snprintf(msg, sizeof msg, "fold(%d dmg / %d cs) = %d dmg / %d s — %s",
                 f.dmg, f.delayCS, (int)got.damage, (int)got.delayS, f.who);
        CHECK(got.damage == f.expDamage && got.delayS == f.expDelayS, msg);
    }
    // The general 0.5s rule the enraged status rides on: damage doubles, delay 1.
    bool halfSecOk = true;
    for (int d = 1; d <= 20; d++) {
        AttackFold f = foldAttack(d, 50);
        if (f.delayS != 1 || f.damage != 2 * d) halfSecOk = false;
    }
    CHECK(halfSecOk, "attackDelay 0.5s folds to (2 x damage, 1 tick) for every damage");
    // A whole-second delay is a no-op fold (the invariant that lets a data row
    // express EITHER form without the engine double-converting).
    CHECK(foldAttack(7, 100).damage == 7 && foldAttack(7, 100).delayS == 1,
          "a 1.00s delay folds to itself");
    CHECK(foldAttack(9, 0).delayS == 1, "a zero delay degrades to one tick, not a div-by-0");

    // The engine arms the folded pair, and the data row keeps upstream's numbers.
    GameState gs; WorldState w; plantEx(w, gs, 40, 0x5151);
    SetpieceEnemy horror = inertEnemy(60, 1);      // chitinous horror: 1 dmg @ 0.25s
    horror.attackDelayS = 0; horror.attackDelayCS = 25;
    w.beginFightSetpiece(horror);
    CHECK(w.cx.enemyDamage == 4 && w.cx.enemyDelayS == 1,
          "beginFightSetpiece folds 1 dmg / 0.25s into 4 dmg / 1 tick");
    CHECK(w.cx.enemyDelayLeft == 1, "the first swing is scheduled on the folded delay");
}

static void layer6_shield() {
    printf("== [L6] shield: damage becomes healing, and one hit breaks it ==\n");
    // -- on the ENEMY (a scene special / boss roll) --
    {
        GameState gs; WorldState w; plantEx(w, gs, 40, 0x1111);
        w.beginFightSetpiece(inertEnemy(30, 3));
        w.cx.enemyHp = 20;                            // wounded, so healing is visible
        w.cx.enemyStatus = ST_SHIELD; w.cx.enemyStatusLeft = STATUS_FOREVER;
        swing(w, gs);                                 // fists, 1 damage
        CHECK(w.cx.enemyHp == 21, "a shielded enemy HEALS by the blocked damage");
        CHECK(w.cx.enemyStatus == ST_NONE, "and the shield breaks on that one hit");
        int before = w.cx.enemyHp;
        swing(w, gs);
        CHECK(w.cx.enemyHp == before - 1, "the next hit lands normally");
        // boundary: healing never exceeds max HP
        w.cx.enemyHp = w.cx.enemyMaxHp;
        w.cx.enemyStatus = ST_SHIELD; w.cx.enemyStatusLeft = STATUS_FOREVER;
        swing(w, gs);
        CHECK(w.cx.enemyHp == w.cx.enemyMaxHp, "shield healing is capped at max HP");
    }
    // -- on the PLAYER (the kinetic-armour button) --
    {
        GameState gs; WorldState w; plantEx(w, gs, 40, 0x2222);
        gs.items[I_KINETIC_ARMOUR] = 1;
        SetpieceEnemy e = inertEnemy(30, 6);
        e.attackDelayS = 1;                           // swings every tick
        w.beginFightSetpiece(e);
        w.ex.hp = 20;
        CHECK(w.fightShield() == FIGHT_ONGOING, "shield button accepted");
        CHECK(w.cx.playerStatus == ST_SHIELD, "player carries the shield status");
        CHECK(w.cx.shieldCool == FIGHT_SHIELD_COOLDOWN_S, "shield starts its 10s cooldown");
        CHECK(w.fightShield() == FIGHT_NOOP, "shielding again while cooling is a no-op");
        w.fightTick(gs);                              // the enemy's swing is blocked
        CHECK(w.ex.hp == 26, "the blocked 6-damage blow HEALS the wanderer instead");
        CHECK(w.cx.playerStatus == ST_NONE, "and the player's shield broke on it");
        w.fightTick(gs);
        CHECK(w.ex.hp == 20, "the following swing hurts again (26 - 6)");
    }
}

static void layer6_enraged() {
    printf("== [L6] enraged: attack interval drops to 1 tick, restored after 4 ==\n");
    GameState gs; WorldState w; plantEx(w, gs, 60, 0x3333);
    SetpieceEnemy e = inertEnemy(40, 5);
    e.attackDelayS = 5;                               // a slow enemy, so the drop shows
    e.specialKind = SK_ENRAGED; e.specialDelayS = 2;
    w.beginFightSetpiece(e);
    CHECK(w.cx.enemyDelayS == 5 && w.cx.enemyDelayBaseS == 5, "armed at its own 5s interval");
    CHECK(w.cx.specialDelay == 2 && w.cx.specialLeft == 2, "the specials clock is armed");

    w.fightTick(gs);
    CHECK(w.cx.enemyStatus == ST_NONE, "no special before its delay elapses");
    w.fightTick(gs);                                   // specials fire on tick 2
    CHECK(w.cx.enemyStatus == ST_ENRAGED, "the special inflicts enraged");
    CHECK(w.cx.enemyStatusLeft == ENRAGE_TICKS, "enrage runs for 4 ticks");
    CHECK(w.cx.enemyDelayS == ENRAGED_DELAY_S, "the attack interval is forced to 1 tick");
    CHECK(w.cx.enemyDelayLeft <= 1, "and the pending swing is pulled in to match");

    // Boundary: it expires exactly on the 4th tick and restores the ARMED interval.
    // The specials clock is silenced first — it is PERIODIC, so left running it would
    // re-inflict enrage every 2 ticks and the expiry could never be observed (which
    // is itself the correct upstream behaviour, just not what this case is about).
    w.cx.specialDelay = 0; w.cx.specialLeft = 0;
    for (int i = 0; i < 3; i++) w.fightTick(gs);
    CHECK(w.cx.enemyStatus == ST_ENRAGED, "still enraged after 3 of its 4 ticks");
    w.fightTick(gs);
    CHECK(w.cx.enemyStatus == ST_NONE, "enrage clears on the 4th tick");
    CHECK(w.cx.enemyDelayS == 5, "and the original 5s interval is restored, not lost");
}

static void layer6_energised() {
    printf("== [L6] energised: the next enemy hit is x4, then it clears ==\n");
    GameState gs; WorldState w; plantEx(w, gs, 60, 0x4444);
    SetpieceEnemy e = inertEnemy(40, 5);
    e.attackDelayS = 1;
    e.specialKind = SK_ENERGISED; e.specialDelayS = 1;
    w.beginFightSetpiece(e);
    CHECK(w.cx.enemyStatus == ST_NONE, "no status at the start of the fight");
    int hp0 = w.ex.hp;
    w.fightTick(gs);            // special fires (tick 1), then the swing lands x4
    CHECK(w.ex.hp == hp0 - 5 * ENERGISE_MULT, "the energised swing deals 4x damage (20)");
    CHECK(w.cx.enemyStatus == ST_NONE, "energised is spent on that one hit");
    // Boundary: energised never lingers into a second swing (the special re-arms on
    // its own clock, so what is asserted here is the SPENDING, not the absence).
    w.cx.specialDelay = 0; w.cx.specialLeft = 0;       // stop re-arming
    int hp1 = w.ex.hp;
    w.fightTick(gs);
    CHECK(w.ex.hp == hp1 - 5, "the following swing is back to base damage");
}

static void layer6_venomous() {
    printf("== [L6] venomous: a landed hit hangs floor(dmg/2) per tick ==\n");
    GameState gs; WorldState w; plantEx(w, gs, 60, 0x5555);
    SetpieceEnemy e = inertEnemy(40, 7);               // floor(7/2) = 3 per tick
    e.attackDelayS = 1;
    e.atHealthThreshold = 20; e.atHealthStatus = ST_VENOMOUS;
    w.beginFightSetpiece(e);
    CHECK(w.cx.atHealthThreshold == 20 && w.cx.atHealthStatus == ST_VENOMOUS,
          "the medic's atHealth blood line is armed off the data row");

    // atHealth triggers only on the hit that CROSSES the line.
    w.cx.enemyHp = 22;
    swing(w, gs);                                      // 22 -> 21, still above
    CHECK(w.cx.enemyStatus == ST_NONE, "a hit that stays above the line does nothing");
    swing(w, gs);                                      // 21 -> 20, crosses
    CHECK(w.cx.enemyStatus == ST_VENOMOUS, "the crossing hit turns the medic venomous");
    swing(w, gs);                                      // 20 -> 19, already below
    CHECK(w.cx.enemyStatus == ST_VENOMOUS, "and it does not re-fire below the line");

    int hp0 = w.ex.hp;
    w.fightTick(gs);                                   // the venomous swing lands
    CHECK(w.ex.hp == hp0 - 7, "the swing itself deals its normal 7");
    CHECK(w.cx.dotDamage == 3 && w.cx.dotTicksLeft == STATUS_FOREVER,
          "and hangs floor(7/2)=3 damage per tick, for the rest of the fight");
    w.cx.enemyDelayS = 999; w.cx.enemyDelayLeft = 999;  // silence the swings
    int hp1 = w.ex.hp;
    w.fightTick(gs);
    CHECK(w.ex.hp == hp1 - 3, "the DoT bites once per tick");
    w.fightTick(gs);
    CHECK(w.ex.hp == hp1 - 6, "and keeps biting");
    // Boundary: a shielded hit hangs NO venom (upstream: blocked -> no DoT).
    {
        GameState g2; WorldState w2; plantEx(w2, g2, 60, 0x5656);
        g2.items[I_KINETIC_ARMOUR] = 1;
        SetpieceEnemy v = inertEnemy(40, 7);
        v.attackDelayS = 1;
        w2.beginFightSetpiece(v);
        w2.cx.enemyStatus = ST_VENOMOUS; w2.cx.enemyStatusLeft = STATUS_FOREVER;
        w2.fightShield();
        w2.fightTick(g2);
        CHECK(w2.cx.dotDamage == 0, "a shield-blocked venomous hit hangs no DoT");
    }
}

static void layer6_meditation() {
    printf("== [L6] meditation: 5 ticks of absorption, then one reflected blow ==\n");
    GameState gs; WorldState w; plantEx(w, gs, 60, 0x6666);
    SetpieceEnemy e = inertEnemy(40, 5);
    e.attackDelayS = 1;
    w.beginFightSetpiece(e);
    w.cx.enemyHp = 30;
    w.cx.enemyStatus = ST_MEDITATION; w.cx.enemyStatusLeft = MEDITATE_TICKS;
    w.cx.meditateAccum = 0;

    swing(w, gs); swing(w, gs); swing(w, gs);           // three fists, 1 damage each
    CHECK(w.cx.enemyHp == 30, "a meditating enemy takes no HP damage");
    CHECK(w.cx.meditateAccum == 3, "the damage is banked into meditateAccum");

    int hp0 = w.ex.hp;
    for (int i = 0; i < MEDITATE_TICKS - 1; i++) w.fightTick(gs);
    CHECK(w.cx.enemyStatus == ST_MEDITATION, "still meditating after 4 of its 5 ticks");
    CHECK(w.ex.hp == hp0, "and it does not swing while meditating");
    w.fightTick(gs);                                   // the 5th tick ends it AND swings
    CHECK(w.cx.enemyStatus == ST_NONE, "meditation clears on the 5th tick");
    CHECK(w.ex.hp == hp0 - 3, "the banked 3 damage comes back in one unavoidable blow");
    CHECK(w.cx.meditateAccum == 0, "and the accumulator is emptied");
    // Boundary: with nothing banked, the swing is an ordinary attack.
    int hp1 = w.ex.hp;
    w.fightTick(gs);
    CHECK(w.ex.hp == hp1 - 5, "with an empty accumulator it swings normally again");
}

static void layer6_boost() {
    printf("== [L6] boost (stim): x2 weapon damage, 3 ticks, 10 self-harm, 1 stim ==\n");
    GameState gs; WorldState w; plantEx(w, gs, 60, 0x7777);
    w.beginFightSetpiece(inertEnemy(80, 5));
    w.ex.outfitRes[R_STIM] = 2;
    int hp0 = w.ex.hp;
    CHECK(w.fightStim() == FIGHT_ONGOING, "stim accepted");
    CHECK(w.ex.hp == hp0 - BOOST_DAMAGE, "the stim costs 10 HP (upstream's dotDamage)");
    CHECK(w.ex.outfitRes[R_STIM] == 1, "and SPENDS a stim (the fixed upstream leak)");
    CHECK(w.cx.playerStatus == ST_BOOST && w.cx.playerStatusLeft == BOOST_TICKS,
          "boost armed for 3 ticks");
    CHECK(w.fightStim() == FIGHT_NOOP, "a second stim while cooling is a no-op");

    int e0 = w.cx.enemyHp;
    swing(w, gs);
    CHECK(w.cx.enemyHp == e0 - 1 * BOOST_MULT, "boosted fists deal double damage");
    // Boundary: it lasts exactly BOOST_TICKS, and damage returns to base after.
    w.cx.enemyDelayS = 999; w.cx.enemyDelayLeft = 999;
    for (int i = 0; i < BOOST_TICKS - 1; i++) w.fightTick(gs);
    CHECK(w.cx.playerStatus == ST_BOOST, "still boosted after 2 of its 3 ticks");
    w.fightTick(gs);
    CHECK(w.cx.playerStatus == ST_NONE, "boost clears on the 3rd tick");
    int e1 = w.cx.enemyHp;
    swing(w, gs);
    CHECK(w.cx.enemyHp == e1 - 1, "and the next swing is back to base damage");
    // Boundary: the self-harm is genuinely lethal at low HP (upstream has no floor).
    {
        GameState g2; WorldState w2; plantEx(w2, g2, 60, 0x7878);
        w2.beginFightSetpiece(inertEnemy(80, 5));
        w2.ex.outfitRes[R_STIM] = 1;
        w2.ex.hp = BOOST_DAMAGE;
        CHECK(w2.fightStim() == FIGHT_LOST, "a stim at exactly 10 HP kills the wanderer");
        CHECK(w2.ex.dead && !w2.ex.active, "and routes through die()");
    }
}

static void layer6_explosion() {
    printf("== [L6] explosion: 3 ticks of wind-up, then the blast decides the fight ==\n");
    // -- survived: the win lands on the blast tick, with the loot banked --
    {
        GameState gs; WorldState w; plantEx(w, gs, 60, 0x8888);
        SetpieceEnemy e = inertEnemy(3, 5);
        e.attackDelayS = 1;
        e.explosionDamage = 30;
        e.loot[0] = { false, R_ALIEN_ALLOY, 1, 1, 1000 }; e.lootN = 1;
        w.beginFightSetpiece(e);
        CHECK(w.cx.explosionDamage == 30, "the scene's explosion payload is armed");
        uint8_t st = FIGHT_ONGOING;
        for (int i = 0; i < 40 && w.cx.enemyHp > 0; i++) st = swing(w, gs);
        CHECK(st == FIGHT_ONGOING && w.cx.enemyHp == 0,
              "HP hits 0 WITHOUT winning — the corpse is winding up");
        CHECK(w.cx.exploding && w.cx.explodeTicksLeft == EXPLOSION_TICKS,
              "the 3-tick self-destruct countdown is running");
        CHECK(w.fightAttack(gs, 0) == FIGHT_NOOP, "the exploding corpse cannot be hit");
        int hp0 = w.ex.hp;
        CHECK(w.fightTick(gs) == FIGHT_ONGOING && w.ex.hp == hp0,
              "tick 1 of the wind-up: no swing, no blast");
        CHECK(w.fightTick(gs) == FIGHT_ONGOING && w.ex.hp == hp0, "tick 2: still winding up");
        uint8_t last = w.fightTick(gs);
        CHECK(last == FIGHT_WON, "tick 3 detonates and the survived blast is the victory");
        CHECK(w.ex.hp == hp0 - 30, "the blast deals its full 30 (no hit roll)");
        CHECK(w.cx.enemyChara == '*', "and the glyph swaps to '*' like upstream");
        CHECK(w.ex.outfitRes[R_ALIEN_ALLOY] == 1, "loot banks on the deferred victory");
    }
    // -- boundary: a blast that kills is a LOSS, not a win --
    {
        GameState gs; WorldState w; plantEx(w, gs, 60, 0x8989);
        SetpieceEnemy e = inertEnemy(3, 5);
        e.attackDelayS = 999;
        e.explosionDamage = 30;
        w.beginFightSetpiece(e);
        for (int i = 0; i < 40 && w.cx.enemyHp > 0; i++) swing(w, gs);
        w.ex.hp = 30;                                  // exactly lethal
        w.fightTick(gs); w.fightTick(gs);
        CHECK(w.fightTick(gs) == FIGHT_LOST, "a blast that empties the HP bar loses the fight");
        CHECK(w.ex.dead && !w.ex.active, "and routes through die()");
    }
    // -- no explosion armed: the kill is the win, immediately (Phase-2 behaviour) --
    {
        GameState gs; WorldState w; plantEx(w, gs, 60, 0x8a8a);
        w.beginFightSetpiece(inertEnemy(3, 5));
        uint8_t st = FIGHT_ONGOING;
        for (int i = 0; i < 40 && !w.cx.won; i++) st = swing(w, gs);
        CHECK(st == FIGHT_WON && !w.cx.exploding,
              "an enemy with no explosion still wins on the killing blow");
    }
}

static void layer6_specials_random3() {
    printf("== [L6] SK_RANDOM3: rolls the boss trio, never twice in a row ==\n");
    GameState gs; WorldState w; plantEx(w, gs, 200, 0x9999);
    SetpieceEnemy e = inertEnemy(500, 1);
    e.attackDelayS = 999;
    e.specialKind = SK_RANDOM3; e.specialDelayS = 1;
    w.beginFightSetpiece(e);
    bool inPool = true, neverRepeats = true;
    uint8_t prev = ST_NONE;
    int seen[8] = { 0 };
    for (int i = 0; i < 60; i++) {
        w.cx.enemyStatus = ST_NONE; w.cx.enemyStatusLeft = 0;   // clear the last roll
        w.fightTick(gs);
        uint8_t st = w.cx.enemyStatus;
        if (st != ST_SHIELD && st != ST_ENRAGED && st != ST_MEDITATION) inPool = false;
        if (st == prev) neverRepeats = false;
        if (st < 8) seen[st]++;
        prev = st;
    }
    CHECK(inPool, "every roll is one of shield / enraged / meditation");
    CHECK(neverRepeats, "and never the same status twice in a row (lastSpecial memory)");
    CHECK(seen[ST_SHIELD] > 0 && seen[ST_ENRAGED] > 0 && seen[ST_MEDITATION] > 0,
          "all three appear over 60 rolls");
}

static void layer6_new_gear() {
    printf("== [L6] the three Phase-3c weapons + kinetic armour ==\n");
    // Double-entry against docs/research-phase3.md §4.3.
    struct WRow { uint8_t id; const char* key; const char* verb; int16_t dmg, cd;
                  uint8_t slot, ammo; bool self; };
    static const WRow EXPECT[] = {
        { WEAPON_PLASMA_RIFLE, "plasma rifle", "disintegrate", 12, 1,
          I_PLASMA_RIFLE, R_ENERGY_CELL, false },
        { WEAPON_ENERGY_BLADE, "energy blade", "slice",        10, 2,
          I_ENERGY_BLADE, RES_NONE,      false },
        { WEAPON_DISRUPTOR,    "disruptor",    "stun",   DMG_STUN, 15,
          I_DISRUPTOR,    RES_NONE,      false },
    };
    for (const WRow& r : EXPECT) {
        const WeaponDef& w = WEAPONS[r.id];
        char msg[96]; snprintf(msg, sizeof msg, "%s: %d dmg / %d s, matches §4.3",
                               r.key, (int)r.dmg, (int)r.cd);
        CHECK(strcmp(w.key, r.key) == 0 && strcmp(w.verb, r.verb) == 0 &&
              w.damage == r.dmg && w.cooldownS == r.cd && w.itemSlot == r.slot &&
              w.ammoRes == r.ammo && w.selfAmmo == r.self, msg);
    }
    CHECK(weightCenti("plasma rifle") == 500, "plasma rifle weighs 5 (Path.Weight)");
    CHECK(weightCenti("energy blade") == DEFAULT_WEIGHT_CENTI &&
          weightCenti("disruptor") == DEFAULT_WEIGHT_CENTI,
          "energy blade / disruptor are absent from Path.Weight -> weigh 1");

    // The disruptor stuns without spending itself — the bolas' whole drawback gone.
    {
        GameState gs; WorldState w; plantEx(w, gs, 40, 0xaaaa);
        w.ex.outfitItem[I_DISRUPTOR] = 1;
        w.beginFightSetpiece(inertEnemy(40, 5));
        CHECK(w.fightWeaponCount() == 1 &&
              w.fightWeaponId(0) == WEAPON_DISRUPTOR, "the disruptor arms as a weapon");
        int hp = w.cx.enemyHp;
        swing(w, gs);
        CHECK(w.cx.enemyHp == hp && w.cx.enemyStunLeft == FIGHT_STUN_S,
              "it stuns for 4s and deals no HP damage");
        CHECK(w.ex.outfitItem[I_DISRUPTOR] == 1, "and does NOT consume itself (unlike bolas)");
    }
    // The plasma rifle spends an energy cell per shot.
    {
        GameState gs; WorldState w; plantEx(w, gs, 40, 0xabab);
        w.ex.outfitItem[I_PLASMA_RIFLE] = 1;
        w.ex.outfitRes[R_ENERGY_CELL] = 1;
        w.beginFightSetpiece(inertEnemy(40, 5));
        w.cx.weaponCool[0] = 0;
        w.fightAttack(gs, 0);        // ammo is spent on the SWING, hit or miss
        CHECK(w.ex.outfitRes[R_ENERGY_CELL] == 0, "a plasma shot spends one energy cell");
        CHECK(!w.fightWeaponEnabled(0), "and reads disabled once the cells run out");
    }
    // kinetic armour raises the HP ceiling above steel (world.js getMaxHealth).
    {
        GameState gs; gs.init();
        CHECK(WorldState::maxHealth(gs) == HEALTH_BASE, "no armour -> 10 HP");
        gs.items[I_S_ARMOUR] = 1;
        CHECK(WorldState::maxHealth(gs) == HEALTH_S_ARMOUR, "steel armour -> 45 HP");
        gs.items[I_KINETIC_ARMOUR] = 1;
        CHECK(WorldState::maxHealth(gs) == HEALTH_KINETIC,
              "kinetic armour wins the tier ladder -> 85 HP");
    }
    // hypo heals 30, on its own 7s cooldown, and both consumables stay in the bag.
    {
        GameState gs; WorldState w; plantEx(w, gs, 60, 0xacac);
        w.beginFightSetpiece(inertEnemy(40, 5));
        w.ex.hp = 10; w.ex.outfitRes[R_HYPO] = 2;
        CHECK(w.fightHypo() == FIGHT_ONGOING, "hypo accepted");
        CHECK(w.ex.hp == 40 && w.ex.outfitRes[R_HYPO] == 1, "hypo heals 30, spends one");
        CHECK(w.cx.hypoCool == FIGHT_HYPO_COOLDOWN_S, "hypo starts its 7s cooldown");
        CHECK(w.fightHypo() == FIGHT_NOOP, "a second hypo while cooling is a no-op");
        w.cx.hypoCool = 0; w.ex.hp = w.ex.maxHp;
        CHECK(w.fightHypo() == FIGHT_NOOP, "and no hypo at full HP");
    }
}

static void layer6_leave_at_home() {
    printf("== [L6] hypo / stim / the new weapons ride home in the bag ==\n");
    GameState gs; gs.init();
    WorldState w; w.init(); w.generateMap(0xADADAD);
    gs.stores[R_CURED_MEAT] = 5 * FP;
    gs.stores[R_HYPO] = 2 * FP; gs.stores[R_STIM] = 2 * FP;
    gs.stores[R_ALIEN_ALLOY] = 3 * FP;
    gs.items[I_ENERGY_BLADE] = 1;
    gs.items[I_WAGON] = 1;
    int16_t out[RES_COUNT] = { 0 };
    out[R_CURED_MEAT] = 5; out[R_HYPO] = 2; out[R_STIM] = 2; out[R_ALIEN_ALLOY] = 3;
    int16_t outi[ITEM_COUNT] = { 0 }; outi[I_ENERGY_BLADE] = 1;
    CHECK(w.embark(gs, out, outi, 0x4242), "embark with hypo/stim/alloy/energy blade");
    w.goHome(gs);
    CHECK(gs.savedOutfitRes[R_HYPO] == 2 && gs.savedOutfitRes[R_STIM] == 2,
          "hypo + stim stay packed for the next trip (world.js leaveItAtHome)");
    CHECK(gs.savedOutfitRes[R_ALIEN_ALLOY] == 0,
          "alien alloy is left at home for the ship / fabricator to spend");
    CHECK(gs.savedOutfitItem[I_ENERGY_BLADE] == 1, "a World weapon stays packed");
    CHECK(gs.whole(R_HYPO) == 2 && gs.whole(R_STIM) == 2,
          "and everything is still banked into the village stores");
}

// The fight grid's arithmetic, transcribed a SECOND time from fight_modal.cpp's
// layout constants (they live in an anonymous namespace inside an Arduino
// translation unit, so this is the only way to bind them to a test). §11's 3c-1
// acceptance is "the worst case must not overflow, must not overlap, and every band
// stays >= 80px" — with 12 weapons and 6 fixed actions that is 18 candidate cells,
// which is exactly why §12 Q13's answer had to be pagination.
static void layer6_fight_layout() {
    printf("== [L6] fight_modal grid: the 18-button worst case fits by paging ==\n");
    const int BTN_H = 80, BTN_GAP = 10, BTN_BOTTOM = 912;
    const int SUP_Y = 308, GLYPH = 24;         // last line of the player block
    const int GRID_MAX_CELLS = 12, GRID_MAX_ROWS = GRID_MAX_CELLS / 2;
    const int MAX_FIXED = 6;                   // eat/meds/hypo/stim/shield/run

    CHECK(BTN_H >= 80, "every band keeps the 80px long-press floor (§9.3)");
    int rows = GRID_MAX_ROWS;
    int top  = BTN_BOTTOM - (rows * BTN_H + (rows - 1) * BTN_GAP);
    CHECK(top == 382, "a full 6-row grid starts at y=382");
    CHECK(top > SUP_Y + GLYPH,
          "which clears the player supply line at 332 — no overlap in the worst case");

    // Worst case: every weapon in the game packed at once, with all six actions.
    const int totalWeapons = WEAPON_COUNT;     // 12 as of Phase 3c
    CHECK(totalWeapons == 12, "WEAPON_COUNT is 12 (9 Phase-2 + the three new)");
    CHECK(totalWeapons + MAX_FIXED > GRID_MAX_CELLS,
          "18 candidate cells genuinely exceed the 12-cell grid — paging is required");

    int maxWpn  = GRID_MAX_CELLS - MAX_FIXED;
    int perPage = maxWpn - 1;                  // one cell buys the 更多 band
    int numPages = (totalWeapons + perPage - 1) / perPage;
    CHECK(maxWpn == 6 && perPage == 5 && numPages == 3,
          "12 weapons split into 3 pages of 5 behind a 更多 cell");
    // Every page's cell count, and every weapon's reachability.
    bool fits = true, covered = true;
    int seenW = 0;
    for (int pg = 0; pg < numPages; pg++) {
        int start = pg * perPage;
        int take = totalWeapons - start; if (take > perPage) take = perPage;
        int cells = take + 1 /*更多*/ + MAX_FIXED;
        if (cells > GRID_MAX_CELLS) fits = false;
        if ((cells + 1) / 2 > GRID_MAX_ROWS) fits = false;
        seenW += take;
    }
    if (seenW != totalWeapons) covered = false;
    CHECK(fits, "no page overflows 12 cells / 6 rows");
    CHECK(covered, "and the three pages between them reach all 12 weapons");

    // The no-paging path: the common case must NOT spend a cell on 更多.
    CHECK(GRID_MAX_CELLS - MAX_FIXED >= 6,
          "up to 6 weapons still fit alongside all six actions with no 更多 cell");
}

// ---- a throwaway setpiece table, built to exercise the engine seams ---------
// research-phase3.md §11 explicitly allows this for 3c-1: the graph lives here, not
// in setpieces_data.h, so nothing ships that the Executioner data (3c-2) will own.
static const SpButton tp_btns[] = {
    /* 0 start.drink   */ { "drink",   SP_COST_WATER, false, 1, 0, 0, 5 },
    /* 1 start.bleed   */ { "bleed",   SP_COST_HP,    false, 2, 0, 0, 10 },
    /* 2 start.jump    */ { "jump",    SP_NO_COST,    false, SP_SCENE_EVENT,
                            SP_OUTPOST, 0, 0 },
    /* 3 start.maps    */ { "maps",    SP_NO_COST,    false, 3, 0, 0, 0 },
    /* 4 paid.leave    */ { "leave",   SP_NO_COST,    false, SP_SCENE_END, 0, 0, 0 },
};
static const SpScene tp_scenes[] = {
    /* 0 start */ { { "a test chamber.", nullptr, nullptr, nullptr }, nullptr,
                    SPE_NONE, false, 0,
                    { {false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},
                      {false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0} }, 0,
                    0, 4, 3 },
    /* 1 drank */ { { "drank.", nullptr, nullptr, nullptr }, nullptr, SPE_NONE, false, 0,
                    { {false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},
                      {false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0} }, 0,
                    4, 1, 0 },
    /* 2 bled  */ { { "bled.", nullptr, nullptr, nullptr }, nullptr, SPE_NONE, false, 0,
                    { {false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},
                      {false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0} }, 0,
                    4, 1, 0 },
    /* 3 maps  */ { { "scavenged maps.", nullptr, nullptr, nullptr }, nullptr,
                    SPE_REVEAL_MAP3, false, 0,
                    { {false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},
                      {false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0},{false,0,0,0,0} }, 0,
                    4, 1, 0 },
};
static const SpDef TEST_SP = { "A Test Chamber", tp_scenes,
                               (uint8_t)(sizeof(tp_scenes) / sizeof(tp_scenes[0])),
                               tp_btns, nullptr, nullptr };

static int revealedCount(const uint8_t* mask) {
    int n = 0;
    for (int i = 0; i < WORLD_CELLS; i++)
        if (mask[i >> 3] & (uint8_t)(1 << (i & 7))) n++;
    return n;
}

static void layer6_setpiece_seams() {
    printf("== [L6] setpiece seams: water/hp costs, nextEvent, applyMap ==\n");
    // -- water cost --
    {
        GameState gs; WorldState w; plantEx(w, gs, 40, 0xb1b1);
        setpiece::bind(&w, &gs);
        w.ex.water = 4;
        CHECK(setpiece::beginTable(&TEST_SP), "beginTable starts the throwaway setpiece");
        CHECK(!setpiece::btnAvailable(0), "5 water is unaffordable at 4");
        CHECK(setpiece::choose(0) == RC_ERR_COST, "and choosing it is refused");
        w.ex.water = 5;
        CHECK(setpiece::btnAvailable(0), "exactly 5 water affords a 5-water price");
        CHECK(setpiece::choose(0) == RC_OK, "the drink is paid for");
        CHECK(w.ex.water == 0, "and 5 water is deducted");
        setpiece::end();
    }
    // -- hp cost: upstream's `num < cost` gate, so exactly-enough is affordable and
    // the wanderer can walk out of engineering `1-3` on precisely 0 HP (events.js
    // updateButtons/buttonClick + World.setHp, which clamps and never kills). --
    {
        GameState gs; WorldState w; plantEx(w, gs, 40, 0xb2b2);
        setpiece::bind(&w, &gs);
        setpiece::beginTable(&TEST_SP);
        w.ex.hp = 9;
        CHECK(!setpiece::btnAvailable(1), "a 10-HP price is refused at 9 HP");
        CHECK(setpiece::choose(1) == RC_ERR_COST, "and pressing it anyway is rejected");
        w.ex.hp = 10;
        CHECK(setpiece::btnAvailable(1), "exactly 10 HP affords a 10-HP price");
        CHECK(setpiece::choose(1) == RC_OK && w.ex.hp == 0,
              "paying it lands on 0 HP — alive, one hit from the end (upstream parity)");
        CHECK(w.ex.active && !w.ex.dead, "0 HP is NOT death: setHp never kills, doSpace does");
        setpiece::end();
    }
    // -- nextEvent: hop to another setpiece, keeping the expedition --
    {
        GameState gs; WorldState w; plantEx(w, gs, 40, 0xb3b3);
        setpiece::bind(&w, &gs);
        setpiece::beginTable(&TEST_SP);
        w.ex.water = 0;
        int bagBefore = w.ex.outfitRes[R_CURED_MEAT];
        CHECK(strcmp(setpiece::titleKey(), "A Test Chamber") == 0, "the test table is running");
        CHECK(setpiece::choose(2) == RC_OK, "the nextEvent button resolves");
        CHECK(setpiece::active(), "the engine is still running a setpiece");
        CHECK(strcmp(setpiece::titleKey(), "An Outpost") == 0,
              "and it is now the TARGET setpiece, not the source");
        CHECK(w.ex.active && w.ex.outfitRes[R_CURED_MEAT] >= bagBefore,
              "the expedition and its bag survive the hop (World state untouched)");
        setpiece::end();
    }
    // -- applyMap x3, on the WORKING fog, surviving goHome and lost to die() --
    {
        GameState gs; WorldState w; plantEx(w, gs, 40, 0xb4b4);
        setpiece::bind(&w, &gs);
        setpiece::beginTable(&TEST_SP);
        int before   = revealedCount(w.ex.revealed);
        int committed = revealedCount(w.revealed);
        CHECK(setpiece::choose(3) == RC_OK, "the scavenge-maps button resolves");
        int after = revealedCount(w.ex.revealed);
        CHECK(after > before, "applyMap x3 uncovers new cells on the WORKING fog");
        CHECK(revealedCount(w.revealed) == committed,
              "and does NOT touch the committed layer mid-trip (the §3.7 bug fixed)");
        setpiece::end();
        w.goHome(gs);
        CHECK(revealedCount(w.revealed) == after,
              "goHome commits the scavenged map — it is kept for good");
    }
    {
        GameState gs; WorldState w; plantEx(w, gs, 40, 0xb5b5);
        setpiece::bind(&w, &gs);
        int committed = revealedCount(w.revealed);
        setpiece::beginTable(&TEST_SP);
        setpiece::choose(3);
        CHECK(revealedCount(w.ex.revealed) > committed, "the reveal landed this trip");
        setpiece::end();
        w.die();
        CHECK(revealedCount(w.revealed) == committed,
              "dying on the way home forfeits the scavenged map (working layer dropped)");
    }
    setpiece::bind(nullptr, nullptr);        // leave the engine unbound for later layers
}

static void layer6_trek_v2() {
    printf("== [L6] trek.bin v1/v2/v3 -> v4 migration (three rounds of enum growth) ==\n");
    // Produce a real v4 file, then hand-fold it back into the v1 layout (19 Res /
    // 18 Item, no tail), the v2 layout (21 / 22, no tail) and the v3 layout
    // (22 / 22, WITH the tail) and prove all three migrations read losslessly.
    GameState gs; gs.init();
    WorldState w; w.init(); w.generateMap(0xC0DEC0);
    gs.stores[R_CURED_MEAT] = 8 * FP; gs.stores[R_MEDICINE] = 2 * FP;
    gs.items[I_RIFLE] = 1;
    int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 8; out[R_MEDICINE] = 2;
    int16_t outi[ITEM_COUNT] = { 0 }; outi[I_RIFLE] = 1;
    w.embark(gs, out, outi, 0xC0DE);
    w.move(gs, DIR_EAST); w.move(gs, DIR_EAST);
    w.ex.wingMartial = true; w.ex.bpFound = 1 << BP_STIM;
    w.saveTrek();

    const size_t HDR  = 68;
    const size_t MAP  = WORLD_CELLS + 2 * WORLD_MASK_BYTES;
    const size_t TAIL = 2;                        // wing flags + bpFound (v3 onward)
    const size_t BODY = HDR + RES_COUNT * 2 + ITEM_COUNT * 2 + MAP;
    const size_t V4SZ = BODY + TAIL;
    const size_t V3SZ = HDR + 22 * 2 + 22 * 2 + MAP + TAIL;
    const size_t V2SZ = HDR + 21 * 2 + 22 * 2 + MAP;
    const size_t V1SZ = HDR + 19 * 2 + 18 * 2 + MAP;
    uint8_t* v4 = (uint8_t*)malloc(V4SZ);
    FILE* f = fopen(ADR_TREK_PATH, "rb");
    CHECK(f != nullptr, "the freshly saved trek.bin is readable");
    size_t got = f ? fread(v4, 1, V4SZ, f) : 0;
    if (f) fclose(f);
    CHECK(got == V4SZ, "and it is exactly the v4 size (body + 2-byte tail)");
    CHECK(v4[4] == 4, "written as TREK_VER 4");

    // v4 round-trips its own tail.
    {
        WorldState ld; ld.init();
        CHECK(ld.loadTrek(), "a v4 trek.bin loads");
        CHECK(ld.ex.wingMartial && !ld.ex.wingEngineering && !ld.ex.wingMedical,
              "the wing flags survive a power-cut mid-wing");
        CHECK(ld.ex.bpFound == (1 << BP_STIM), "so does the blueprint found this trip");
    }

    // Fold down to v3: same Res array, the Item array minus the two 3c-3 upgrade
    // slots, tail intact (v3 is the first version that HAS one).
    uint8_t* v3 = (uint8_t*)malloc(V3SZ);
    memcpy(v3, v4, HDR);
    v3[4] = 3;
    memcpy(v3 + HDR, v4 + HDR, 22 * 2);
    memcpy(v3 + HDR + 22 * 2, v4 + HDR + RES_COUNT * 2, 22 * 2);
    memcpy(v3 + HDR + 22 * 2 + 22 * 2,
           v4 + HDR + RES_COUNT * 2 + ITEM_COUNT * 2, MAP + TAIL);
    writeRaw(ADR_TREK_PATH, v3, V3SZ);
    {
        WorldState ld; ld.init();
        CHECK(ld.loadTrek(), "a v3 trek.bin still loads under v4 firmware");
        CHECK(ld.ex.active && ld.ex.x == w.ex.x && ld.ex.y == w.ex.y,
              "the interrupted expedition is NOT silently dropped");
        CHECK(ld.ex.outfitRes[R_MEDICINE] == 2 && ld.ex.outfitItem[I_RIFLE] == 1,
              "the v3-era bag survives the wider Item array");
        CHECK(ld.ex.wingMartial && ld.ex.bpFound == (1 << BP_STIM),
              "and the v3 tail is still found at ITS offset, not the v4 one");
        CHECK(ld.ex.outfitItem[I_CARGO_DRONE] == 0 &&
              ld.ex.outfitItem[I_FLUID_RECYCLER] == 0,
              "the two slots v3 never had read as empty");
    }

    // Fold down to v2: same arrays minus the fleet-beacon slot, and no tail.
    uint8_t* v2 = (uint8_t*)malloc(V2SZ);
    memcpy(v2, v4, HDR);
    v2[4] = 2;
    memcpy(v2 + HDR, v4 + HDR, 21 * 2);
    memcpy(v2 + HDR + 21 * 2, v4 + HDR + RES_COUNT * 2, 22 * 2);
    memcpy(v2 + HDR + 21 * 2 + 22 * 2, v4 + HDR + RES_COUNT * 2 + ITEM_COUNT * 2, MAP);
    writeRaw(ADR_TREK_PATH, v2, V2SZ);
    {
        WorldState ld; ld.init();
        CHECK(ld.loadTrek(), "a v2 trek.bin still loads under v4 firmware");
        CHECK(ld.ex.active && ld.ex.x == w.ex.x && ld.ex.y == w.ex.y,
              "the interrupted expedition is NOT silently dropped");
        CHECK(ld.ex.outfitRes[R_MEDICINE] == 2 && ld.ex.outfitItem[I_RIFLE] == 1,
              "the v2-era bag survives the wider Res array");
        CHECK(ld.ex.outfitRes[R_FLEET_BEACON] == 0 && ld.ex.bpFound == 0 &&
              !ld.ex.wingMartial,
              "and everything v2 never had reads as empty");
    }

    uint8_t* v1 = (uint8_t*)malloc(V1SZ);
    memcpy(v1, v4, HDR);
    v1[4] = 1;                                        // relabel as v1
    memcpy(v1 + HDR, v4 + HDR, 19 * 2);               // the old 19 Res slots
    memcpy(v1 + HDR + 19 * 2, v4 + HDR + RES_COUNT * 2, 18 * 2);   // old 18 Items
    memcpy(v1 + HDR + 19 * 2 + 18 * 2,
           v4 + HDR + RES_COUNT * 2 + ITEM_COUNT * 2, MAP);        // map, unchanged
    writeRaw(ADR_TREK_PATH, v1, V1SZ);

    WorldState ld; ld.init();
    CHECK(ld.loadTrek(), "a v1 trek.bin still loads under v4 firmware");
    CHECK(ld.ex.active, "the interrupted expedition is NOT silently dropped");
    CHECK(ld.ex.x == w.ex.x && ld.ex.y == w.ex.y, "position survives the migration");
    CHECK(ld.ex.hp == w.ex.hp && ld.ex.water == w.ex.water && ld.ex.rng == w.ex.rng,
          "hp / water / rng survive");
    CHECK(ld.ex.outfitRes[R_CURED_MEAT] == w.ex.outfitRes[R_CURED_MEAT] &&
          ld.ex.outfitRes[R_MEDICINE] == 2 && ld.ex.outfitItem[I_RIFLE] == 1,
          "the whole v1-era bag survives");
    CHECK(ld.ex.outfitRes[R_HYPO] == 0 && ld.ex.outfitRes[R_STIM] == 0 &&
          ld.ex.outfitItem[I_PLASMA_RIFLE] == 0 &&
          ld.ex.outfitItem[I_KINETIC_ARMOUR] == 0,
          "and the slots v1 never had read as empty");
    CHECK(memcmp(ld.ex.tiles, w.ex.tiles, WORLD_CELLS) == 0, "the working map survives");
    free(v1); free(v2); free(v3); free(v4);

    // A v1 file that is one byte short of even the v1 layout is still rejected.
    {
        uint8_t small[64] = { 0 };
        size_t o = 0; rawU32(small, o, TREK_MAGIC); small[o] = 1;
        writeRaw(ADR_TREK_PATH, small, sizeof small);
        WorldState bad; bad.init();
        CHECK(!bad.loadTrek(), "a truncated v1 trek.bin is still rejected");
    }
}

// Shrink the JSON array introduced by `prefix` down to its first `keep` entries,
// in place — the fixture for "a save written before the enum grew". Returns false
// if the array isn't there or is already short.
static bool truncJsonArray(char* json, const char* prefix, int keep) {
    char* arr = strstr(json, prefix);
    if (!arr) return false;
    char* p = arr + strlen(prefix);
    for (int n = 0; n < keep; n++) {
        p = strchr(p, ',');
        if (!p) return false;
        p++;
    }
    char* close = strchr(p, ']');
    if (!close) return false;
    memmove(p - 1, close, strlen(close) + 1);   // drop the separating comma too
    return true;
}

static void layer6_save_roundtrip() {
    printf("== [L6] game.json: the new Res/Item slots round-trip, v5 saves still load ==\n");
    GameState gs; gs.init();
    gs.stores[R_HYPO] = 5 * FP; gs.stores[R_STIM] = 3 * FP;
    gs.stores[R_WOOD] = 42 * FP;
    gs.items[I_PLASMA_RIFLE] = 1; gs.items[I_ENERGY_BLADE] = 2;
    gs.items[I_DISRUPTOR] = 1;    gs.items[I_KINETIC_ARMOUR] = 1;
    gs.markSeen(R_HYPO); gs.markSeen(R_STIM);
    static char buf[8192];
    gs.toJson(buf, sizeof buf);
    GameState r; r.init();
    CHECK(r.fromJson(buf), "a save carrying the new slots parses");
    CHECK(r.whole(R_HYPO) == 5 && r.whole(R_STIM) == 3, "hypo / stim round-trip");
    CHECK(r.items[I_PLASMA_RIFLE] == 1 && r.items[I_ENERGY_BLADE] == 2 &&
          r.items[I_DISRUPTOR] == 1 && r.items[I_KINETIC_ARMOUR] == 1,
          "the four new items round-trip");
    CHECK(r.hasSeen(R_HYPO) && r.hasSeen(R_STIM), "the seen bitset covers the new slots");

    // The tail-append claim: a save written before the enums grew (19 stores / 18
    // items in its positional arrays) must still load, with the new slots at 0 and
    // NOTHING shifted. Rebuild those two arrays short, in place.
    char old[8192];
    strcpy(old, buf);
    CHECK(truncJsonArray(old, "\"stores\":[", 19), "stores[] truncated to its v5 19 entries");
    CHECK(truncJsonArray(old, "\"itm\":[", 18), "itm[] truncated to its v5 18 entries");
    GameState o; o.init();
    o.stores[R_HYPO] = 99 * FP; o.items[I_DISRUPTOR] = 9;   // dirty, must be overwritten
    CHECK(o.fromJson(old), "a pre-growth save (19 stores / 18 items) still loads");
    CHECK(o.whole(R_WOOD) == 42, "and the slots it DID carry are unshifted");
    CHECK(o.whole(R_HYPO) == 0 && o.whole(R_STIM) == 0, "the new Res slots read as 0");
    CHECK(o.items[I_PLASMA_RIFLE] == 0 && o.items[I_KINETIC_ARMOUR] == 0,
          "and the new Item slots read as 0");
}

// ===========================================================================
// Layer 7 — the Executioner CONTENT (Phase 3c-2)
//
// Layer 1a already proves the six graphs are structurally sound (reachable,
// in-bounds, no dead end). This layer proves they are the RIGHT graphs: every
// enemy stat block transcribed a second time from upstream
// script/events/executioner.js, every combat scene's enemy assignment, the wing
// bookkeeping (clear -> mark -> the front hall greys out -> goHome banks it, die
// throws it away), the blueprint drops, and the two scenes that behave unlike
// anything else in the game (the front hall's cross-event hops, and the burning
// corridor with no way out).
// ===========================================================================

// Kit an expedition out for a full Executioner run: deep HP/water so a graph walk
// cannot starve, and the four things its buttons bill (torch / grenade / alloy).
static void plantExec(WorldState& w, GameState& gs, uint32_t rng) {
    gs.init();
    w.init();
    w.generateMap(0x3C2E7EC);
    gs.stores[R_CURED_MEAT] = 10 * FP;
    // A convoy, in the STORES only (embark would otherwise deduct it): the bag has
    // to have room left for the loot, or every drop silently fails the weight cap.
    gs.items[I_CONVOY] = 1;
    int16_t out[RES_COUNT] = { 0 };
    out[R_CURED_MEAT] = 10; out[R_ALIEN_ALLOY] = 6;
    int16_t outi[ITEM_COUNT] = { 0 };
    // A steel sword so there is always an unlimited swing available: the grenades
    // are here for martial `1`'s door, not for the fights, and a walk that spends
    // them all would otherwise stall in front of the next machine.
    outi[I_TORCH] = 5; outi[I_GRENADE] = 5; outi[I_STEEL_SWORD] = 1;
    gs.stores[R_ALIEN_ALLOY] = 10 * FP;
    gs.items[I_TORCH] = 5; gs.items[I_GRENADE] = 5; gs.items[I_STEEL_SWORD] = 1;
    w.embark(gs, out, outi, rng);
    w.ex.maxHp = 400; w.ex.hp = 400;
    w.ex.maxWater = 99; w.ex.water = 99;
    setpiece::bind(&w, &gs);
}

// Beat the armed setpiece fight without ever letting the enemy swing (the tick is
// only pumped for an explosion wind-up, which is the one thing a player cannot
// out-click), then hand the outcome back the way setpiece_modal does.
static void winSetpieceFight(WorldState& w, GameState& gs) {
    for (int g = 0; g < 20000 && w.cx.active && !w.cx.won; g++) {
        if (w.cx.exploding) { w.fightTick(gs); continue; }
        bool swung = false;
        for (int s = 0; s < w.fightWeaponCount(); s++) {
            w.cx.weaponCool[s] = 0;                       // ignore the cooldown
            if (!w.fightWeaponEnabled(s)) continue;       // spent its ammo/itself
            if (WEAPONS[w.fightWeaponId(s)].damage == DMG_STUN) continue;  // bolas-likes
            w.fightAttack(gs, s);
            swung = true;
            break;
        }
        if (!swung) break;                                // nothing left to swing
    }
    setpiece::resolveCombat(w.cx.won);
}

// Press the FIRST band of every scene until the setpiece closes. Button 0 is the
// forward move in every Executioner scene (continue / enter / power cycle / fight
// / engage / observe / use machine / blow it down / scavenge maps / take device),
// so this walks a whole wing end to end on any branch roll.
static int driveWing(WorldState& w, GameState& gs) {
    int steps = 0;
    while (setpiece::active() && steps++ < 300) {
        if (setpiece::awaitingCombat()) { winSetpieceFight(w, gs); continue; }
        if (setpiece::choose(0) != RC_OK) break;
    }
    return steps;
}

static void layer7_enemies() {
    printf("== [L7] exec_enemies[]: 20 stat blocks, transcribed a second time ==\n");
    // Second source: executioner.js:1-115 (the four shared machines) plus every
    // inline `combat: true` scene. delayCS is upstream's sub-second attackDelay in
    // centiseconds (0 == the whole-second delayS is used verbatim).
    struct ERow { const char* who; char chara; int16_t hp, dmg, hit, delayS, delayCS;
                  bool ranged; uint8_t sk; int16_t sd; int16_t ath; int16_t exp;
                  int lootN; };
    static const ERow EXPECT[] = {
        { "guard",              'G',  60, 10, 800, 2,   0, true,  SK_NONE,      0,  0,  0, 3 },
        { "quadruped",          'Q',  70,  8, 800, 1,   0, false, SK_NONE,      0,  0,  0, 1 },
        { "medic",              'M',  80, 15, 800, 3,   0, false, SK_NONE,      0, 40,  0, 2 },
        { "turret",             'T',  50, 25, 800, 4,   0, true,  SK_NONE,      0,  0,  0, 3 },
        { "chitinous horror",   'H',  60,  1, 700, 0,  25, false, SK_NONE,      0,  0,  0, 2 },
        { "chitinous queen",    'Q',  70,  1, 700, 0,  25, false, SK_NONE,      0,  0,  0, 2 },
        { "operative",          'O',  60,  8, 800, 2,   0, false, SK_NONE,      0,  0,  0, 3 },
        { "researcher",         'R',  20,  1, 800, 2,   0, false, SK_NONE,      0,  0,  0, 3 },
        { "ancient beast",      'A',  60,  6, 800, 1,   0, false, SK_NONE,      0,  0,  0, 3 },
        { "automated turret",   'T',  60, 10, 800, 0, 250, true,  SK_NONE,      0,  0,  0, 2 },
        { "unruly welder",      'W',  50, 13, 800, 2,   0, false, SK_NONE,      0,  0,  0, 2 },
        { "unstable prototype", 'P', 150,  5, 800, 2,   0, false, SK_SHIELD,    5,  0,  0, 1 },
        { "murderous robot",    'M', 250, 10, 800, 3,   0, false, SK_ENERGISED,13,  0,  0, 1 },
        { "unstable automaton", 'A', 100, 10, 700, 2,   0, false, SK_NONE,      0,  0, 30, 0 },
        { "malformed experiment",'E',200,  5, 800, 2,   0, false, SK_ENRAGED,  16,  0,  0, 0 },
        { "immortal wanderer",  '@', 500, 12, 800, 2,   0, false, SK_RANDOM3,   7,  0,  0, 1 },
        { "guard (8-1a)",       'G',  60, 10, 800, 2,   0, true,  SK_NONE,      0,  0,  0, 3 },
        { "guard (9-1)",        'G',  60, 10, 800, 2,   0, true,  SK_NONE,      0,  0,  0, 3 },
        { "medic (6-1a)",       'M',  80, 15, 800, 3,   0, false, SK_NONE,      0, 40,  0, 2 },
        { "medic (6-2a)",       'M',  80, 15, 800, 3,   0, false, SK_NONE,      0, 40,  0, 2 },
    };
    const int N = (int)(sizeof(EXPECT) / sizeof(EXPECT[0]));
    CHECK(N == 20, "the second source covers all 20 rows");
    for (int i = 0; i < N; i++) {
        const ERow& x = EXPECT[i];
        const SetpieceEnemy& e = exec_enemies[i];
        char m[128];
        snprintf(m, sizeof m, "%s: %d hp / %d dmg / %d pm / delay %d s %d cs",
                 x.who, (int)x.hp, (int)x.dmg, (int)x.hit, (int)x.delayS, (int)x.delayCS);
        CHECK(e.chara == x.chara && e.health == x.hp && e.damage == x.dmg &&
              e.hitPM == x.hit && e.attackDelayS == x.delayS &&
              e.attackDelayCS == x.delayCS && e.ranged == x.ranged, m);
        snprintf(m, sizeof m, "%s: special %d/%ds, atHealth %d, explosion %d, %d loot lines",
                 x.who, (int)x.sk, (int)x.sd, (int)x.ath, (int)x.exp, x.lootN);
        CHECK(e.specialKind == x.sk && e.specialDelayS == x.sd &&
              e.atHealthThreshold == x.ath && e.explosionDamage == x.exp &&
              e.lootN == x.lootN, m);
        // The data-row invariant combat_data.h states: one delay form or the other.
        CHECK((e.attackDelayCS != 0) == (e.attackDelayS == 0),
              (snprintf(m, sizeof m, "%s: exactly one attackDelay form is set", x.who), m));
    }
    // The medic's blood line, and the two overrides that only change a line of text.
    CHECK(exec_enemies[2].atHealthStatus == ST_VENOMOUS &&
          exec_enemies[18].atHealthStatus == ST_VENOMOUS,
          "every medic turns venomous at 40 hp, overrides included");
    CHECK(strcmp(exec_enemies[16].notif, "drew some attention with all that noise.") == 0 &&
          strcmp(exec_enemies[17].notif, "ran straight into another one.") == 0 &&
          strcmp(exec_enemies[18].notif, "it had friends.") == 0 &&
          strcmp(exec_enemies[19].notif, "the noise draws attention.") == 0,
          "the four scene-specific notifications are on their own rows");
    // §12 Q5: upstream's duplicate 'alien alloy' key means only 2-4 @ 0.2 survives.
    CHECK(exec_enemies[1].lootN == 1 && exec_enemies[1].loot[0].mn == 2 &&
          exec_enemies[1].loot[0].mx == 4 && exec_enemies[1].loot[0].chancePM == 200,
          "quadruped keeps ONLY the second 'alien alloy' line (upstream's dup-key bug)");
    // The boss's drop is the whole reason R_FLEET_BEACON exists.
    CHECK(!exec_enemies[15].loot[0].isItem &&
          exec_enemies[15].loot[0].slot == R_FLEET_BEACON &&
          exec_enemies[15].loot[0].chancePM == 1000,
          "the immortal wanderer always drops the fleet beacon");
    // The folds §10.4 works out by hand, now applied to the REAL rows.
    CHECK(foldAttack(exec_enemies[4].damage, exec_enemies[4].attackDelayCS).damage == 4,
          "chitinous horror folds to 4 damage a tick");
    CHECK(foldAttack(exec_enemies[9].damage, exec_enemies[9].attackDelayCS).delayS == 3 &&
          foldAttack(exec_enemies[9].damage, exec_enemies[9].attackDelayCS).damage == 12,
          "the automated turret folds to 12 damage every 3 ticks");
}

static void layer7_combat_scenes() {
    printf("== [L7] all 38 combat scenes point at the enemy upstream spreads in ==\n");
    // Second source: every `combat: true` / `...Enemies.Executioner.x` scene, in
    // the scene order executioner_data.h declares.
    struct CRow { uint8_t sp; uint8_t scene; uint8_t enemy; const char* sid; };
    static const CRow EXPECT[] = {
        { SP_EXEC_INTRO,  3,  4, "3-1" }, { SP_EXEC_INTRO,  4,  5, "4-1" },
        { SP_EXEC_INTRO,  5,  6, "2-2" }, { SP_EXEC_INTRO,  7,  7, "4-2" },
        { SP_EXEC_INTRO, 10,  8, "4-3" }, { SP_EXEC_INTRO, 12,  9, "6"   },
        { SP_EXEC_ENG,    2, 10, "2-1a" }, { SP_EXEC_ENG,   4,  0, "3-1" },
        { SP_EXEC_ENG,    5,  3, "1-2"  }, { SP_EXEC_ENG,   7,  0, "3-2a" },
        { SP_EXEC_ENG,   10,  0, "2-3a" }, { SP_EXEC_ENG,  15,  3, "5-1" },
        { SP_EXEC_ENG,   19, 11, "7"    },
        { SP_EXEC_MAR,    3,  3, "3-1"  }, { SP_EXEC_MAR,   5,  3, "2-2" },
        { SP_EXEC_MAR,    6,  1, "3-2a" }, { SP_EXEC_MAR,  10,  0, "3-3a" },
        { SP_EXEC_MAR,   12,  1, "4-3"  }, { SP_EXEC_MAR,  16, 16, "8-1a" },
        { SP_EXEC_MAR,   18, 17, "9-1"  }, { SP_EXEC_MAR,  22,  1, "9-2" },
        { SP_EXEC_MAR,   25, 12, "12"   },
        { SP_EXEC_MED,    1,  3, "1"    }, { SP_EXEC_MED,   3,  1, "3a"  },
        { SP_EXEC_MED,    6,  2, "5-1"  }, { SP_EXEC_MED,   7, 18, "6-1a" },
        { SP_EXEC_MED,   12, 19, "6-2a" }, { SP_EXEC_MED,  14,  1, "7-2" },
        { SP_EXEC_MED,   15, 13, "8"    }, { SP_EXEC_MED,  17,  0, "10a" },
        { SP_EXEC_MED,   19,  2, "11"   }, { SP_EXEC_MED,  21,  0, "13-1a" },
        { SP_EXEC_MED,   23,  2, "14-1" }, { SP_EXEC_MED,  25,  2, "13-2a" },
        { SP_EXEC_MED,   27,  2, "14-2" }, { SP_EXEC_MED,  29, 14, "16"  },
        { SP_EXEC_CMD,    1,  0, "1"    }, { SP_EXEC_CMD,   7, 15, "6"   },
    };
    const int N = (int)(sizeof(EXPECT) / sizeof(EXPECT[0]));
    bool ok = true;
    for (int i = 0; i < N; i++) {
        const SpDef& d = SETPIECES[EXPECT[i].sp];
        const SpScene& s = d.scenes[EXPECT[i].scene];
        if (!s.combat || s.enemy != EXPECT[i].enemy) {
            ok = false;
            printf("       (scene '%s' of sp#%d: combat=%d enemy=%d, expected %d)\n",
                   EXPECT[i].sid, EXPECT[i].sp, (int)s.combat, (int)s.enemy,
                   (int)EXPECT[i].enemy);
        }
    }
    CHECK(ok, "every listed scene is a combat scene against the listed enemy");
    // And nothing ELSE is a fight: the count has to match exactly, or a scene was
    // silently turned into (or out of) combat.
    int found = 0;
    const uint8_t IDS[6] = { SP_EXEC_INTRO, SP_EXEC_ANTE, SP_EXEC_ENG,
                             SP_EXEC_MAR, SP_EXEC_MED, SP_EXEC_CMD };
    for (uint8_t id : IDS) {
        const SpDef& d = SETPIECES[id];
        for (int s = 0; s < d.sceneN; s++) if (d.scenes[s].combat) found++;
    }
    char m[96];
    snprintf(m, sizeof m, "exactly %d combat scenes across the six tables (found %d)", N, found);
    CHECK(found == N, m);
    // Scene totals, second-sourced from the upstream scene keys.
    CHECK(SETPIECES[SP_EXEC_INTRO].sceneN == 14 && SETPIECES[SP_EXEC_ANTE].sceneN == 1 &&
          SETPIECES[SP_EXEC_ENG].sceneN == 21 && SETPIECES[SP_EXEC_MAR].sceneN == 27 &&
          SETPIECES[SP_EXEC_MED].sceneN == 31 && SETPIECES[SP_EXEC_CMD].sceneN == 9,
          "scene counts: 14 / 1 / 21 / 27 / 31 / 9 (103 total)");
}

static void layer7_blueprints() {
    printf("== [L7] the five blueprints drop where upstream drops them ==\n");
    struct BRow { uint8_t sp; uint8_t scene; uint8_t bp; bool combat; const char* sid; };
    static const BRow EXPECT[] = {
        { SP_EXEC_ENG, 17, BP_HYPO,           false, "engineering 6"  },
        { SP_EXEC_ENG, 19, BP_KINETIC_ARMOUR, true,  "engineering 7"  },
        { SP_EXEC_MAR, 14, BP_PLASMA_RIFLE,   false, "martial 6"      },
        { SP_EXEC_MAR, 25, BP_DISRUPTOR,      true,  "martial 12"     },
        { SP_EXEC_MED, 29, BP_STIM,           true,  "medical 16"     },
    };
    for (const BRow& r : EXPECT) {
        const SpScene& s = SETPIECES[r.sp].scenes[r.scene];
        char m[96];
        snprintf(m, sizeof m, "%s carries %s", r.sid, BLUEPRINT_KEY[r.bp]);
        CHECK(s.bp == (uint8_t)(r.bp + 1) && s.combat == r.combat, m);
    }
    // Nothing else carries one, and the count is five, not six: §12 Q8 cut the
    // glow stone, so medical `8` — upstream's glowstone-blueprint drop — is bare.
    int n = 0;
    const uint8_t IDS[6] = { SP_EXEC_INTRO, SP_EXEC_ANTE, SP_EXEC_ENG,
                             SP_EXEC_MAR, SP_EXEC_MED, SP_EXEC_CMD };
    for (uint8_t id : IDS) {
        const SpDef& d = SETPIECES[id];
        for (int s = 0; s < d.sceneN; s++) if (d.scenes[s].bp) n++;
    }
    CHECK(n == BP_COUNT && BP_COUNT == 5, "exactly five blueprint drops in the ship");
    CHECK(SETPIECES[SP_EXEC_MED].scenes[15].bp == 0 &&
          exec_enemies[13].lootN == 0,
          "medical 8 drops nothing — its only loot was the cut glowstone blueprint");
}

static void layer7_intro_and_landmark() {
    printf("== [L7] the X tile: prologue once, then the elevator hall, forever ==\n");
    GameState gs; WorldState w; plantExec(w, gs, 0xE1E1);
    // Stand ON the battleship: drawRoad runs from wherever the wanderer is.
    for (int y = 0; y < WORLD_DIM; y++)
        for (int x = 0; x < WORLD_DIM; x++)
            if (w.exTileAt(x, y) == T_EXECUTIONER) { w.ex.x = (int16_t)x; w.ex.y = (int16_t)y; }
    CHECK(landmarkScene(T_EXECUTIONER) == SP_EXEC_INTRO,
          "a fresh world routes the battleship tile to the prologue");
    CHECK(setpiece::begin(SP_EXEC_INTRO), "the prologue opens");
    CHECK(strcmp(setpiece::titleKey(), "A Ravaged Battleship") == 0, "titled upstream's way");
    CHECK(setpiece::btnCount() == 2 && setpiece::btnCostSlot(0) == I_TORCH &&
          setpiece::btnCostIsItem(0), "`enter` bills one torch");
    w.ex.outfitItem[I_TORCH] = 0;
    CHECK(!setpiece::btnAvailable(0), "with no torch the entrance is a dead band");
    w.ex.outfitItem[I_TORCH] = 5;
    CHECK(setpiece::btnAvailable(0), "a torch re-arms it");

    driveWing(w, gs);
    CHECK(!setpiece::active(), "the prologue plays to its end");
    CHECK(w.ex.clearedExec, "and scene 7 records the battleship as boarded");
    // drawRoad ran: the X tile is still X, but it is now connected.
    int roads = 0;
    for (int y = 0; y < WORLD_DIM; y++)
        for (int x = 0; x < WORLD_DIM; x++)
            if (w.exTileAt(x, y) == T_ROAD) roads++;
    CHECK(roads > 0, "clearMine drew the road home");

    CHECK(!gs.execEntered,
          "nothing is banked mid-trip: die here and the prologue is due again");
    w.goHome(gs);
    CHECK(gs.execEntered, "walking home banks it for good");

    // Second trip: the same tile now opens the elevator hall.
    int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 10;
    gs.stores[R_CURED_MEAT] = 10 * FP;
    CHECK(w.embark(gs, out, nullptr, 0xE2E2), "a second expedition sets out");
    CHECK(w.ex.clearedExec, "which starts already knowing the battleship is open");
}

static void layer7_antechamber() {
    printf("== [L7] the front hall: four hops, greying out as the wings fall ==\n");
    GameState gs; WorldState w; plantExec(w, gs, 0xA1A1);
    w.ex.clearedExec = true;
    CHECK(setpiece::begin(SP_EXEC_ANTE), "the elevator hall opens");
    CHECK(setpiece::btnCount() == 5, "five bands: three wings, the deck, and out");
    CHECK(setpiece::btnAvailable(0) && setpiece::btnAvailable(1) &&
          setpiece::btnAvailable(2), "all three wings start live");
    CHECK(!setpiece::btnAvailable(3), "the command deck starts greyed out");
    CHECK(setpiece::choose(3) == RC_ERR_LOCKED, "and pressing it anyway is refused");

    w.ex.wingEngineering = true;
    CHECK(!setpiece::btnAvailable(0), "a cleared wing greys out its own band");
    CHECK(setpiece::choose(0) == RC_ERR_LOCKED, "which is not merely cosmetic");
    CHECK(setpiece::btnAvailable(1) && setpiece::btnAvailable(2),
          "the other two are untouched");
    w.ex.wingMedical = true; w.ex.wingMartial = true;
    CHECK(setpiece::btnAvailable(3), "all three cleared -> the command deck arms");

    CHECK(setpiece::choose(3) == RC_OK, "the hop resolves");
    CHECK(setpiece::active() && strcmp(setpiece::titleKey(), "Command Deck") == 0,
          "and lands INSIDE the command deck, not back on the map");
    CHECK(w.ex.active && w.ex.outfitRes[R_CURED_MEAT] > 0,
          "the expedition and its bag ride through the hop untouched");
    setpiece::end();

    // Each wing button hops to its own event.
    struct HRow { int btn; const char* title; bool eng, mar, med; };
    static const HRow H[] = {
        { 0, "Engineering Wing", false, false, false },
        { 1, "Medical Wing",     false, false, false },
        { 2, "Martial Wing",     false, false, false },
    };
    for (const HRow& h : H) {
        GameState g2; WorldState w2; plantExec(w2, g2, 0xA2A2);
        w2.ex.clearedExec = true;
        setpiece::begin(SP_EXEC_ANTE);
        char m[80]; snprintf(m, sizeof m, "band %d opens the %s", h.btn, h.title);
        CHECK(setpiece::choose(h.btn) == RC_OK &&
              strcmp(setpiece::titleKey(), h.title) == 0, m);
        setpiece::end();
    }
    setpiece::bind(nullptr, nullptr);
}

// One wing, end to end: walk it, check the mark + blueprints landed on the trip,
// then prove goHome banks them and die() does not.
static void wingRun(uint8_t spId, const char* name, uint8_t wantBp,
                    bool Expedition::*trip, bool GameState::*banked) {
    char m[128];
    {
        GameState gs; WorldState w; plantExec(w, gs, 0x7001 + spId);
        w.ex.clearedExec = true;
        CHECK(setpiece::begin(spId),
              (snprintf(m, sizeof m, "%s opens", name), m));
        int steps = driveWing(w, gs);
        CHECK(!setpiece::active() && steps < 300,
              (snprintf(m, sizeof m, "%s plays to its terminal scene in %d steps",
                        name, steps), m));
        CHECK(w.ex.*trip,
              (snprintf(m, sizeof m, "%s's last scene marks the wing cleared", name), m));
        CHECK(w.ex.bpFound == wantBp,
              (snprintf(m, sizeof m, "%s yields blueprint bits 0x%02x (got 0x%02x)",
                        name, wantBp, w.ex.bpFound), m));
        // Dying throws the whole wing away — upstream's World.state discard.
        w.die();
        CHECK(!(gs.*banked) && gs.blueprints == 0,
              (snprintf(m, sizeof m, "%s: dying banks neither the wing nor its blueprints",
                        name), m));
    }
    {
        GameState gs; WorldState w; plantExec(w, gs, 0x7001 + spId);
        w.ex.clearedExec = true;
        setpiece::begin(spId);
        driveWing(w, gs);
        w.goHome(gs);
        CHECK(gs.*banked,
              (snprintf(m, sizeof m, "%s: walking home banks the wing", name), m));
        CHECK(gs.blueprints == wantBp,
              (snprintf(m, sizeof m, "%s: and redeems its blueprints into the save", name), m));
    }
    setpiece::bind(nullptr, nullptr);
}

static void layer7_wings() {
    printf("== [L7] each wing: entrance -> clear -> mark -> bank (or lose it all) ==\n");
    wingRun(SP_EXEC_ENG, "engineering",
            (1 << BP_HYPO) | (1 << BP_KINETIC_ARMOUR),
            &Expedition::wingEngineering, &GameState::wingEngineering);
    wingRun(SP_EXEC_MAR, "martial",
            (1 << BP_PLASMA_RIFLE) | (1 << BP_DISRUPTOR),
            &Expedition::wingMartial, &GameState::wingMartial);
    wingRun(SP_EXEC_MED, "medical", (1 << BP_STIM),
            &Expedition::wingMedical, &GameState::wingMedical);
}

static void layer7_command_deck() {
    printf("== [L7] the command deck: the boss, the beacon, and the cleared tile ==\n");
    GameState gs; WorldState w; plantExec(w, gs, 0xC0DE01);
    // Stand ON the battleship so clearDungeon has the right tile to convert.
    int bx = -1, by = -1;
    for (int y = 0; y < WORLD_DIM && bx < 0; y++)
        for (int x = 0; x < WORLD_DIM; x++)
            if (w.exTileAt(x, y) == T_EXECUTIONER) { bx = x; by = y; break; }
    CHECK(bx >= 0, "the map placed a battleship");
    w.ex.x = (int16_t)bx; w.ex.y = (int16_t)by;
    w.ex.clearedExec = true;
    w.ex.wingEngineering = w.ex.wingMartial = w.ex.wingMedical = true;

    setpiece::begin(SP_EXEC_CMD);
    int steps = driveWing(w, gs);
    CHECK(!setpiece::active() && steps < 300, "the deck plays through to scene 7");
    CHECK(w.ex.outfitRes[R_FLEET_BEACON] == 1, "the immortal wanderer left the beacon");
    CHECK(w.exTileAt(bx, by) == T_OUTPOST,
          "clearDungeon turned the battleship into an outpost — the X is spent");
    w.goHome(gs);
    CHECK(gs.whole(R_FLEET_BEACON) == 1, "the beacon is banked in the village stores");
    CHECK(gs.savedOutfitRes[R_FLEET_BEACON] == 0,
          "and STAYS there: it is not on World.leaveItAtHome's keep list");
    CHECK(w.tileAt(bx, by) == T_OUTPOST, "the cleared tile is committed to the map");
    setpiece::bind(nullptr, nullptr);
}

static void layer7_burning_corridor() {
    printf("== [L7] engineering 1-3: water 5 or blood 10, and no way back out ==\n");
    // Find the scene through the real graph rather than reaching into the table:
    // start's 30/40/30 roll has to actually land on 1-3 for this to be reachable.
    GameState gs; WorldState w;
    bool found = false;
    for (uint32_t seed = 1; seed <= 4000 && !found; seed++) {
        plantExec(w, gs, seed);
        w.ex.clearedExec = true;
        setpiece::begin(SP_EXEC_ENG);
        if (setpiece::choose(0) != RC_OK) { setpiece::end(); continue; }
        if (setpiece::btnCount() == 2 && setpiece::btnCostSlot(0) == SP_COST_WATER)
            found = true;
        else setpiece::end();
    }
    CHECK(found, "the 30% branch really does reach the burning corridor");
    if (!found) return;

    CHECK(setpiece::btnCostAmt(0) == 5 && setpiece::btnCostSlot(1) == SP_COST_HP &&
          setpiece::btnCostAmt(1) == 10, "the two prices are 5 water and 10 hp");
    // No third band: this is the one scene in the game with no exit.
    CHECK(setpiece::btnCount() == 2, "and there is no `leave` — upstream has none either");
    CHECK(setpiece::defaultBtnIndex() == 0,
          "the idle-timeout default is `extinguish`, not a leave that does not exist");

    w.ex.water = 4; w.ex.hp = 9;
    CHECK(!setpiece::btnAvailable(0) && !setpiece::btnAvailable(1),
          "too little of both -> both bands dead (the upstream dead end, copied)");
    // ... and the dead end is survivable on THIS device because the setpiece is
    // RAM-only: the trek was committed at every scene, so a sleep drops the panel.
    setpiece::end();
    CHECK(w.ex.active && w.ex.hp == 9,
          "abandoning the panel leaves the expedition standing where it was");

    // Paying in blood is allowed down to exactly zero, and does not kill.
    plantExec(w, gs, 0);
    setpiece::beginTable(&SETPIECES[SP_EXEC_ENG]);
    // Re-walk to the corridor is not needed: the affordability rule is the engine's,
    // and layer 6 pins it on the throwaway table. Here we only assert the DATA.
    setpiece::end();
    const SpDef& eng = SETPIECES[SP_EXEC_ENG];
    const SpScene& burn = eng.scenes[9];
    CHECK(burn.btnCount == 2, "scene 9 of the engineering table is the corridor");
    CHECK(eng.btns[burn.btnStart].costSlot == SP_COST_WATER &&
          eng.btns[burn.btnStart].costAmt == 5 &&
          eng.btns[burn.btnStart + 1].costSlot == SP_COST_HP &&
          eng.btns[burn.btnStart + 1].costAmt == 10,
          "its two bands are the game's ONLY water/hp prices");
    int water = 0, hp = 0;
    for (int id = 0; id < SETPIECE_COUNT; id++) {
        if (!setpieceExists(id)) continue;
        const SpDef& d = SETPIECES[id];
        for (int s = 0; s < d.sceneN; s++)
            for (int b = 0; b < d.scenes[s].btnCount; b++) {
                uint8_t c = d.btns[d.scenes[s].btnStart + b].costSlot;
                if (c == SP_COST_WATER) water++;
                if (c == SP_COST_HP) hp++;
            }
    }
    CHECK(water == 1 && hp == 1, "exactly one water price and one hp price in the game");
    setpiece::bind(nullptr, nullptr);
}

static void layer7_heal_and_maps() {
    printf("== [L7] the two `use machine` heals and `scavenge maps` ==\n");
    // Both are BUTTON onChoose hooks upstream, so they must fire on the press.
    struct HRow { uint8_t sp; uint8_t scene; uint8_t btn; const char* sid; };
    static const HRow HEAL[] = {
        { SP_EXEC_ENG, 13, 0, "engineering 4" },
        { SP_EXEC_MAR, 23, 0, "martial 10" },
    };
    for (const HRow& h : HEAL) {
        const SpDef& d = SETPIECES[h.sp];
        const SpButton& b = d.btns[d.scenes[h.scene].btnStart + h.btn];
        char m[96]; snprintf(m, sizeof m, "%s's `use machine` costs an alloy and heals", h.sid);
        CHECK(b.costSlot == R_ALIEN_ALLOY && !b.costIsItem && b.effect == SPE_HEAL_FULL, m);
    }
    {
        const SpDef& d = SETPIECES[SP_EXEC_MAR];
        const SpButton& b = d.btns[d.scenes[15].btnStart];      // 7-1 'scavenge maps'
        CHECK(b.effect == SPE_REVEAL_MAP3 && b.costSlot == SP_NO_COST,
              "martial 7-1's `scavenge maps` is free and reveals three map chunks");
    }
    // Live: the heal really refills, and the maps really uncover the WORKING fog.
    GameState gs; WorldState w; plantExec(w, gs, 0x4EA1);
    gs.items[I_S_ARMOUR] = 1;                       // a 45-hp ceiling to heal back to
    w.ex.maxHp = (int16_t)WorldState::maxHealth(gs);
    w.ex.hp = 3;
    w.spHealFull(gs);
    CHECK(w.ex.hp == HEALTH_S_ARMOUR && w.ex.maxHp == HEALTH_S_ARMOUR,
          "spHealFull refills to the armour ceiling, not to a stale snapshot");
    int before = revealedCount(w.ex.revealed);
    for (int i = 0; i < 3; i++) w.spApplyMap();
    CHECK(revealedCount(w.ex.revealed) > before, "three applyMap calls uncover the trip's fog");
    setpiece::bind(nullptr, nullptr);
}

// ===========================================================================
// Layer 8 — the Fabricator (Phase 3c-3): the FABRICATE table transcribed a
// second time from research-phase3.md §4.2, the fabricate() rules, and the two
// equipment tiers (§4.3) the products finally switch on.
// ===========================================================================

static void layer8_fabricate_table() {
    printf("== [L8] FABRICATE[] against §4.2, row by row ==\n");
    // Double entry: every column of §4.2's table, re-typed from the doc rather
    // than copied from game_data.h, so a future edit to either one shows up here.
    struct FRow { uint8_t id; const char* key; bool isItem; uint8_t slot;
                  int8_t bp; int16_t maximum, cost, qty; const char* msg; };
    static const FRow EXPECT[] = {
        { F_ENERGY_BLADE,   "energy blade",   true,  I_ENERGY_BLADE,   BP_NONE,
          -1, 1, 1, "the blade hums, charged particles sparking and fizzing." },
        { F_FLUID_RECYCLER, "fluid recycler", true,  I_FLUID_RECYCLER, BP_NONE,
           1, 2, 1, "water out, water in. waste not, want not." },
        { F_CARGO_DRONE,    "cargo drone",    true,  I_CARGO_DRONE,    BP_NONE,
           1, 2, 1, "the workhorse of the wanderer fleet." },
        { F_KINETIC_ARMOUR, "kinetic armour", true,  I_KINETIC_ARMOUR, BP_KINETIC_ARMOUR,
           1, 2, 1, "wanderer soldiers succeed by subverting the enemy's rage." },
        { F_DISRUPTOR,      "disruptor",      true,  I_DISRUPTOR,      BP_DISRUPTOR,
          -1, 1, 1, "somtimes it is best not to fight." },
        { F_HYPO,           "hypo",           false, R_HYPO,           BP_HYPO,
          -1, 1, 5, "a handful of hypos. life in a vial." },
        { F_STIM,           "stim",           false, R_STIM,           BP_STIM,
          -1, 1, 1, "sometimes it is best to fight without restraint." },
        { F_PLASMA_RIFLE,   "plasma rifle",   true,  I_PLASMA_RIFLE,   BP_PLASMA_RIFLE,
          -1, 1, 1, "the peak of wanderer weapons technology, sleek and deadly." },
    };
    CHECK((int)(sizeof(EXPECT) / sizeof(EXPECT[0])) == FAB_COUNT,
          "eight fabricatables — upstream's nine minus glow stone (§12 Q8)");
    for (const FRow& e : EXPECT) {
        const Fabricatable& f = FABRICATE[e.id];
        char msg[128];
        snprintf(msg, sizeof msg, "%s: %d alloy -> x%d, max %d, %s",
                 e.key, (int)e.cost, (int)e.qty, (int)e.maximum,
                 e.bp == BP_NONE ? "no blueprint" : "blueprint-gated");
        CHECK(strcmp(f.key, e.key) == 0 && f.isItem == e.isItem &&
              f.slot == e.slot && f.blueprintBit == e.bp &&
              f.maximum == e.maximum && f.alloyCost == e.cost &&
              f.quantity == e.qty && strcmp(f.buildMsg, e.msg) == 0, msg);
        // The key doubles as the store identity, so it MUST name the slot it fills.
        CHECK(strcmp(f.key, f.isItem ? ITEM_KEY[f.slot] : RES_KEY[f.slot]) == 0,
              "…and its key names the very slot it fills");
    }
    // The three no-blueprint rows are exactly the ones §4.2 calls out.
    int free = 0;
    for (int i = 0; i < FAB_COUNT; i++) if (FABRICATE[i].blueprintBit == BP_NONE) free++;
    CHECK(free == 3, "three rows need no blueprint (energy blade / recycler / drone)");
    // Every Blueprint bit has exactly one product, or a redeemed blueprint would
    // unlock nothing (or two things).
    for (uint8_t bp = 0; bp < BP_COUNT; bp++) {
        int hits = 0;
        for (int i = 0; i < FAB_COUNT; i++)
            if (FABRICATE[i].blueprintBit == (int8_t)bp) hits++;
        char msg[80];
        snprintf(msg, sizeof msg, "blueprint %s unlocks exactly one row", BLUEPRINT_KEY[bp]);
        CHECK(hits == 1, msg);
    }
}

static void layer8_fabricate_rules() {
    printf("== [L8] fabricate(): the blueprint gate, the alloy ledger, the caps ==\n");
    // Locked until goHome banked the prologue.
    {
        GameState gs; gs.init();
        gs.stores[R_ALIEN_ALLOY] = 10 * FP;
        CHECK(!gs.execEntered, "fresh game: no fabricator");
        CHECK(gs.fabricate(F_ENERGY_BLADE) == RC_ERR_LOCKED,
              "fabricating is refused before the page is unlocked");
        CHECK(gs.whole(R_ALIEN_ALLOY) == 10, "a refused press spends nothing");
        gs.unlockFabricator();
        CHECK(gs.execEntered, "unlockFabricator opens the page");
        bool sawNotice = false;
        for (int i = 0; i < gs.logCount; i++)
            if (strncmp(gs.log[i].enKey, "builder knows the strange device", 32) == 0)
                sawNotice = true;
        CHECK(sawNotice, "…and pushes builder's one-shot notice (world.js:971)");
        int before = gs.logCount;
        gs.unlockFabricator();
        CHECK(gs.logCount == before, "a second cleared trip re-notifies nothing");
    }
    // The blueprint gate: six of eight rows are invisible until redeemed.
    {
        GameState gs; gs.init(); gs.unlockFabricator();
        gs.stores[R_ALIEN_ALLOY] = 20 * FP;
        CHECK(gs.canFabricate(F_ENERGY_BLADE) && gs.canFabricate(F_FLUID_RECYCLER) &&
              gs.canFabricate(F_CARGO_DRONE),
              "the three blueprint-free rows are fabricable the moment the page opens");
        CHECK(!gs.canFabricate(F_HYPO) && !gs.canFabricate(F_PLASMA_RIFLE),
              "the blueprint-gated rows are not");
        CHECK(gs.fabricate(F_HYPO) == RC_ERR_LOCKED, "and pressing one is refused");
        CHECK(gs.whole(R_ALIEN_ALLOY) == 20, "a refused press spends nothing");
        gs.blueprints |= (uint8_t)(1u << BP_HYPO);
        CHECK(gs.canFabricate(F_HYPO), "redeeming the blueprint opens the row");
        CHECK(gs.fabricate(F_HYPO) == RC_OK, "and now it fabricates");
        // fabricator.js's only quantity > 1.
        CHECK(gs.whole(R_HYPO) == 5, "hypo comes five at a time (quantity: 5)");
        CHECK(gs.whole(R_ALIEN_ALLOY) == 19, "for exactly 1 alloy");
        CHECK(gs.hasSeen(R_HYPO), "a fabricated resource counts as seen");
        bool sawMsg = false;
        for (int i = 0; i < gs.logCount; i++)
            if (strcmp(gs.log[i].enKey, FABRICATE[F_HYPO].buildMsg) == 0) sawMsg = true;
        CHECK(sawMsg, "and its buildMsg reaches the log");
    }
    // The 2-alloy price and the per-upgrade cap.
    {
        GameState gs; gs.init(); gs.unlockFabricator();
        gs.stores[R_ALIEN_ALLOY] = 1 * FP;
        CHECK(gs.fabricate(F_CARGO_DRONE) == RC_ERR_COST, "1 alloy cannot buy a 2-alloy row");
        bool sawShort = false;
        for (int i = 0; i < gs.logCount; i++)
            if (strcmp(gs.log[i].enKey, "not enough alien alloy") == 0) sawShort = true;
        CHECK(sawShort, "a short press notifies 「外星合金不足」");
        CHECK(gs.whole(R_ALIEN_ALLOY) == 1 && gs.items[I_CARGO_DRONE] == 0,
              "and changes nothing");
        gs.stores[R_ALIEN_ALLOY] = 6 * FP;
        CHECK(gs.fabricate(F_CARGO_DRONE) == RC_OK, "with 6 alloy it builds");
        CHECK(gs.items[I_CARGO_DRONE] == 1 && gs.whole(R_ALIEN_ALLOY) == 4,
              "one drone, four alloy left");
        CHECK(gs.fabricate(F_CARGO_DRONE) == RC_ERR_MAX,
              "a second is refused: maximum 1 (the button goes dashed)");
        CHECK(gs.whole(R_ALIEN_ALLOY) == 4, "and the refusal is free");
        // Uncapped rows really are uncapped.
        gs.blueprints = 0xFF;
        gs.stores[R_ALIEN_ALLOY] = 3 * FP;
        CHECK(gs.fabricate(F_ENERGY_BLADE) == RC_OK &&
              gs.fabricate(F_ENERGY_BLADE) == RC_OK &&
              gs.fabricate(F_ENERGY_BLADE) == RC_OK,
              "an uncapped weapon can be built over and over");
        CHECK(gs.items[I_ENERGY_BLADE] == 3 && gs.whole(R_ALIEN_ALLOY) == 0,
              "three blades for three alloy");
        CHECK(gs.fabricate(FAB_COUNT) == RC_ERR_INVALID, "an out-of-range id is rejected");
    }
}

static void layer8_gear_tiers() {
    printf("== [L8] cargo drone / fluid recycler: highest tier wins, never additive ==\n");
    // §4.3 + research-phase2.md's tier tables, transcribed a second time.
    CHECK(BAG_CARGO_DRONE == 11000, "cargo drone bag = 110 (10 base + 100)");
    CHECK(WATER_RECYCLER == 110,    "fluid recycler water = 110 (10 base + 100)");
    {
        GameState gs; gs.init();
        CHECK(WorldState::bagCapacityCenti(gs) == BAG_BASE_CENTI, "bare hands carry 10");
        gs.items[I_CONVOY] = 1;
        CHECK(WorldState::bagCapacityCenti(gs) == BAG_CONVOY, "a convoy carries 70");
        gs.items[I_CARGO_DRONE] = 1;
        CHECK(WorldState::bagCapacityCenti(gs) == BAG_CARGO_DRONE,
              "convoy + drone carries 110, NOT 180 — the tiers are exclusive");
        gs.items[I_CONVOY] = 0; gs.items[I_WAGON] = 0; gs.items[I_RUCKSACK] = 0;
        CHECK(WorldState::bagCapacityCenti(gs) == BAG_CARGO_DRONE,
              "and the drone alone is still 110 (it does not stack ON the convoy)");
    }
    {
        GameState gs; gs.init();
        CHECK(WorldState::maxWater(gs) == WATER_BASE, "bare hands hold 10 water");
        gs.items[I_WATER_TANK] = 1;
        CHECK(WorldState::maxWater(gs) == WATER_TANK, "a water tank holds 60");
        gs.items[I_FLUID_RECYCLER] = 1;
        CHECK(WorldState::maxWater(gs) == WATER_RECYCLER,
              "tank + recycler holds 110, NOT 170 — the tiers are exclusive");
        gs.items[I_WATER_TANK] = 0; gs.items[I_CASK] = 0; gs.items[I_WATERSKIN] = 0;
        CHECK(WorldState::maxWater(gs) == WATER_RECYCLER, "and the recycler alone is 110");
    }
    // The armour ceiling 3c-1 already wired, re-checked here so all three tier
    // tables are asserted in one place (kinetic armour is the row that made
    // getMaxHealth read items[] at all).
    {
        GameState gs; gs.init();
        gs.items[I_S_ARMOUR] = 1;
        CHECK(WorldState::maxHealth(gs) == HEALTH_S_ARMOUR, "steel armour caps HP at 45");
        gs.items[I_KINETIC_ARMOUR] = 1;
        CHECK(WorldState::maxHealth(gs) == HEALTH_KINETIC,
              "kinetic armour caps HP at 85, not 45+75");
    }
    // The whole loop, live: fabricate the drone, embark, and the trip really does
    // get the bigger bag and canteen.
    {
        GameState gs; gs.init(); gs.unlockFabricator();
        WorldState w; w.init(); w.generateMap(0xFAB1);
        gs.stores[R_ALIEN_ALLOY] = 4 * FP;
        gs.stores[R_CURED_MEAT]  = 5 * FP;
        CHECK(gs.fabricate(F_CARGO_DRONE) == RC_OK &&
              gs.fabricate(F_FLUID_RECYCLER) == RC_OK, "build both upgrades");
        int16_t out[RES_COUNT] = { 0 }; out[R_CURED_MEAT] = 5;
        int16_t outi[ITEM_COUNT] = { 0 };
        CHECK(w.embark(gs, out, outi, 0xFAB2), "embark with them standing in the village");
        CHECK(w.ex.maxWater == WATER_RECYCLER && w.ex.water == WATER_RECYCLER,
              "the expedition sets out with a 110 canteen, full");
        CHECK(WorldState::bagCapacityCenti(gs) == BAG_CARGO_DRONE,
              "and a 110 bag to fill");
        // Neither upgrade rides the bag: they are `upgrade`-type, so Path never
        // renders a row for them and goHome never remembers one (§4.4).
        w.goHome(gs);
        CHECK(gs.savedOutfitItem[I_CARGO_DRONE] == 0 &&
              gs.savedOutfitItem[I_FLUID_RECYCLER] == 0,
              "and neither is ever packed — they work from the village stores");
    }
}

// ===========================================================================

int main() {
    printf("############ Layer 1: static data-table validation ############\n");
    layer1_setpieces();
    layer1_constants();
    layer1_landmarks();
    layer1_weapons();
    layer1_enemies();

    printf("\n############ Layer 2: seeded end-to-end economy ############\n");
    layer2_economy();

    printf("\n############ Layer 3: Monte-Carlo statistics ############\n");
    layer3_mapgen();
    layer3_combat_hitrate();
    layer3_loot();
    layer3_encounter_rate();

    printf("\n############ Layer 4: save robustness ############\n");
    layer4_worldbin();
    layer4_trekbin();
    layer4_gamejson();

    printf("\n############ Layer 5: the Space level (Phase 3b) ############\n");
    layer5_space_constants();
    layer5_space_curves();
    layer5_space_control();
    layer5_space_collision();
    layer5_space_crash();
    layer5_space_victory();
    layer5_space_state();
    layer5_space_score();
    layer5_space_save();

    printf("\n######## Layer 6: Executioner combat mechanics (Phase 3c-1) ########\n");
    layer6_fold();
    layer6_shield();
    layer6_enraged();
    layer6_energised();
    layer6_venomous();
    layer6_meditation();
    layer6_boost();
    layer6_explosion();
    layer6_specials_random3();
    layer6_new_gear();
    layer6_fight_layout();
    layer6_leave_at_home();
    layer6_setpiece_seams();
    layer6_trek_v2();
    layer6_save_roundtrip();

    printf("\n########## Layer 7: the Executioner content (Phase 3c-2) ##########\n");
    layer7_enemies();
    layer7_combat_scenes();
    layer7_blueprints();
    layer7_intro_and_landmark();
    layer7_antechamber();
    layer7_wings();
    layer7_command_deck();
    layer7_burning_corridor();
    layer7_heal_and_maps();

    printf("\n############ Layer 8: the Fabricator (Phase 3c-3) ############\n");
    layer8_fabricate_table();
    layer8_fabricate_rules();
    layer8_gear_tiers();

    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
