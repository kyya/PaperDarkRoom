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
//
// Zero source intrusion: every RNG the statistics need is already injectable —
// generateMap(seed) for the map stream, embark(trekSeed) / the public ex.rng for
// the fight+loot stream — so no rng-exposure hack was needed.
//
// Build (clang++ is the host toolchain on this box):
//   clang++ -std=c++17 -I src tools/mechanics_test.cpp src/world_state.cpp \
//           src/game_state.cpp src/setpiece_engine.cpp \
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
    /* SP_EXECUTIONER */ { 0, 0, 0 },
    /* SP_CACHE       */ { 0, 0, 0 },
};

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
            // narrative loot slots
            for (int i = 0; i < sc.lootN; i++)
                if (!lootSlotLegal(sc.loot[i])) lootOk = false;
            for (int b = 0; b < sc.btnCount; b++) {
                const SpButton& btn = def.btns[sc.btnStart + b];
                // cost slot legal
                if (btn.costSlot != SP_NO_COST) {
                    if (btn.costIsItem) { if (btn.costSlot >= ITEM_COUNT) costOk = false; }
                    else                { if (btn.costSlot >= RES_COUNT)  costOk = false; }
                }
                if (btn.next == SP_SCENE_END) continue;
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

    printf("\n==== %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
