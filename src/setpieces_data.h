// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room (Doublespeak Games, MPL-2.0) — Phase 2 milestone 2.4 landmark
// setpiece scene tables. Transcribed from upstream script/events/setpieces.js —
// the scene text / costs / loot / branch probabilities / combat enemy stats ARE
// the port, so this file is a derivative of the MPL game and carries the MPL
// header. Pure data, no Arduino/M5 dependency, so setpiece_engine can be
// host-compiled for the smoke test. Every en_key string doubles as a tr() key
// (strings_zh.h) exactly as upstream keys it.
//
// Model (mirrors events_data.h's scene machine, but backed by the World
// expedition bag instead of the village stores, and with combat scenes + map
// onLoad hooks the village event engine lacks — the reason setpieces get their
// OWN engine rather than reusing event_engine, see setpiece_engine.h):
//   * Each setpiece is SELF-CONTAINED: its scenes/buttons/probs/enemies live in
//     dedicated arrays and cross-reference by LOCAL index (scene 0 == 'start'),
//     so there is no fragile global-index bookkeeping across setpieces.
//   * A narrative scene: body text + an optional onLoad effect + optional loot
//     auto-banked into the bag on load (research decision 4: no take/drop menu).
//   * A combat scene: an inline SetpieceEnemy (setpiece_modal hands it to
//     fight_modal); its loot banks on victory; the scene's buttons are the
//     POST-victory choices (continue / run / leave), shown once the fight is won.
//
// SCOPE (milestone 2.4, faithful to research-phase2.md §5):
//   * FULL ports: outpost, iron/coal/sulphur mine, house, borehole, battlefield,
//     swamp, ship (discover-only, Phase-3 door), cave (the full 13-scene dungeon).
//   * COMPACT faithful ports: town, city — a coherent representative subgraph of
//     the upstream 23/52-scene dungeons (entrance -> real combats/branches -> a
//     real terminal reward scene -> clearDungeon), using only upstream scenes /
//     enemies / loot / text (all already in the official zh_cn set). The full
//     town/city graphs are a documented follow-up; every landmark still triggers
//     a real, playable, correctly-clearing setpiece.
//   * executioner (Phase 3) / cache (prestige-only, never generated) have no
//     table — the engine no-ops on them.
#pragma once
#include <stdint.h>
#include "game_data.h"
#include "world_data.h"      // SetpieceId, tile enums
#include "combat_data.h"     // LootDrop, SetpieceEnemy

namespace adr {

// ---- scene-transition sentinels (a button's `next` field) -----------------
constexpr uint8_t SP_SCENE_PROB = 0xFD;  // resolve via the button's prob table
constexpr uint8_t SP_SCENE_END  = 0xFF;  // close the setpiece (back to World)

// ---- scene onLoad side effects (operate on the working map / expedition) --
enum SpEffect : uint8_t {
    SPE_NONE = 0,
    SPE_FILL_WATER,        // World.setWater(getMaxWater()) — outpost / house well
    SPE_CLEAR_DUNGEON,     // cave/town/city -> OUTPOST + road (clearDungeon)
    SPE_CLEAR_IRON,        // clearMine(IRON): flag + road (goHome unlocks miner)
    SPE_CLEAR_COAL,
    SPE_CLEAR_SULPHUR,
    SPE_CLEAR_SHIP,        // record ship (Phase-3 door), draw road
    SPE_GRANT_GASTRONOME,  // swamp charm -> perk (x2 meat heal)
};

// ---- probability branch ({0.25:'a',0.5:'b',1:'c'} semantics) --------------
// roll = rand[0,1000); pick the FIRST branch (ascending) with roll < threshold —
// upstream's "lowest key i with r<i" rule (same as events_data.h ProbBranch).
struct SpProb { int16_t thresholdMilli; uint8_t scene; };  // scene = LOCAL idx

// ---- button --------------------------------------------------------------
// A setpiece cost is always a single unit of one thing (torch / charm). costSlot
// == 0xFF means free. next is a LOCAL scene idx | SP_SCENE_PROB | SP_SCENE_END.
constexpr uint8_t SP_NO_COST = 0xFF;
struct SpButton {
    const char* textKey;
    uint8_t     costSlot;     // Res or Item slot, or SP_NO_COST
    bool        costIsItem;   // torch -> Item; charm -> Res
    uint8_t     next;         // scene idx | SP_SCENE_PROB | SP_SCENE_END
    uint8_t     probStart;    // into this setpiece's probs[] when next==SP_SCENE_PROB
    uint8_t     probCount;
};

// ---- scene ---------------------------------------------------------------
constexpr int SP_SCENE_LOOT_MAX = 8;    // cave end2 has 7 drop lines
struct SpScene {
    const char* text[4];      // body lines, nullptr-terminated (empty for combat)
    const char* notify;       // pushed to the log on load; nullptr = none
    uint8_t     effect;       // SpEffect, run on load
    bool        combat;       // combat scene: launch the fight, then show buttons
    uint8_t     enemy;        // index into the setpiece's enemies[] (combat only)
    LootDrop    loot[SP_SCENE_LOOT_MAX];   // narrative loot auto-banked on load
    uint8_t     lootN;
    uint8_t     btnStart, btnCount, defaultBtn;   // into the setpiece's btns[]
};

// ---- setpiece ------------------------------------------------------------
struct SpDef {
    const char*          titleKey;
    const SpScene*       scenes;   // scenes[0] is 'start'
    uint8_t              sceneN;
    const SpButton*      btns;
    const SpProb*        probs;
    const SetpieceEnemy* enemies;
};

// Convenience terminators.
#define LOOT_END {false,0,0,0,0}
#define NOLOOT {LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END}, 0

// ===========================================================================
// outpost — a safe rest point (spawned by clearing a dungeon). useOutpost fills
// water; one-shot use is enforced by move()/usedOutpost before the setpiece.
// ===========================================================================
static const SpButton op_btns[] = {
    { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene op_scenes[] = {
    { { "a safe place in the wilds.", nullptr, nullptr, nullptr },
      "a safe place in the wilds.", SPE_FILL_WATER, false, 0,
      { {false,R_CURED_MEAT,5,10,1000}, LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 1,
      0, 1, 0 },
};

// ===========================================================================
// swamp — enter -> cabin -> talk (costs 1 charm) grants gastronome.
// ===========================================================================
static const SpButton sw_btns[] = {
    /* 0 start.enter  */ { "enter", SP_NO_COST, false, 1, 0, 0 },       // -> cabin
    /* 1 start.leave  */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 2 cabin.talk   */ { "talk", R_CHARM, false, 2, 0, 0 },           // cost charm -> talk
    /* 3 cabin.leave  */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 4 talk.leave   */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene sw_scenes[] = {
    /* 0 start */
    { { "rotting reeds rise out of the swampy earth.",
        "a lone frog sits in the muck, silently.", nullptr, nullptr },
      "a swamp festers in the stagnant air.", SPE_NONE, false, 0, NOLOOT, 0, 2, 1 },
    /* 1 cabin */
    { { "deep in the swamp is a moss-covered cabin.",
        "an old wanderer sits inside, in a seeming trance.", nullptr, nullptr },
      nullptr, SPE_NONE, false, 0, NOLOOT, 2, 2, 1 },
    /* 2 talk */
    { { "the wanderer takes the charm and nods slowly.",
        "he speaks of once leading the great fleets to fresh worlds.",
        "unfathomable destruction to fuel wanderer hungers.",
        "his time here, now, is his penance." },
      nullptr, SPE_GRANT_GASTRONOME, false, 0, NOLOOT, 4, 1, 0 },
};

// ===========================================================================
// house — go inside -> 25% medicine / 25% supplies / 50% occupied (squatter).
// ===========================================================================
static const SetpieceEnemy ho_enemies[] = {
    { 'E', "a man charges down the hall, a rusty blade in his hand", 10, 3, 800, 2, false,
      { {false,R_CURED_MEAT,1,10,800}, {false,R_LEATHER,1,10,200},
        {false,R_CLOTH,1,10,500}, LOOT_END,LOOT_END,LOOT_END }, 3 },
};
static const SpProb ho_probs[] = {
    { 250, 1 }, { 500, 2 }, { 1000, 3 },   // medicine / supplies / occupied
};
static const SpButton ho_btns[] = {
    /* 0 start.enter    */ { "go inside", SP_NO_COST, false, SP_SCENE_PROB, 0, 3 },
    /* 1 start.leave    */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 2 medicine.leave */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 3 supplies.leave */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 4 occupied.leave */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene ho_scenes[] = {
    /* 0 start */
    { { "an old house remains here, once white siding yellowed and peeling.",
        "the door hangs open.", nullptr, nullptr },
      "the remains of an old house stand as a monument to simpler times",
      SPE_NONE, false, 0, NOLOOT, 0, 2, 1 },
    /* 1 medicine */
    { { "the house has been ransacked.",
        "but there is a cache of medicine under the floorboards.", nullptr, nullptr },
      nullptr, SPE_NONE, false, 0,
      { {false,R_MEDICINE,2,5,1000}, LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 1, 2, 1, 0 },
    /* 2 supplies */
    { { "the house is abandoned, but not yet picked over.",
        "still a few drops of water in the old well.", nullptr, nullptr },
      "water replenished", SPE_FILL_WATER, false, 0,
      { {false,R_CURED_MEAT,1,10,800}, {false,R_LEATHER,1,10,200},
        {false,R_CLOTH,1,10,500}, LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 3, 3, 1, 0 },
    /* 3 occupied (combat) */
    { { nullptr, nullptr, nullptr, nullptr },
      nullptr, SPE_NONE, true, 0, NOLOOT, 4, 1, 0 },
};

// ===========================================================================
// battlefield — one text scene, dormant war tech to loot.
// ===========================================================================
static const SpButton bf_btns[] = {
    { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene bf_scenes[] = {
    { { "a battle was fought here, long ago.",
        "battered technology from both sides lays dormant on the blasted landscape.",
        nullptr, nullptr },
      nullptr, SPE_NONE, false, 0,
      { {true,I_RIFLE,1,3,500}, {false,R_BULLETS,5,20,800}, {true,I_LASER_RIFLE,1,3,300},
        {false,R_ENERGY_CELL,5,10,500}, {true,I_GRENADE,1,5,500},
        {false,R_ALIEN_ALLOY,1,1,300}, LOOT_END,LOOT_END }, 6,
      0, 1, 0 },
};

// ===========================================================================
// borehole — one text scene, alien alloy at the edge.
// ===========================================================================
static const SpButton bh_btns[] = {
    { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene bh_scenes[] = {
    { { "a huge hole is cut deep into the earth, evidence of the past harvest.",
        "they took what they came for, and left.",
        "castoff from the mammoth drills can still be found by the edges of the precipice.",
        nullptr },
      nullptr, SPE_NONE, false, 0,
      { {false,R_ALIEN_ALLOY,1,3,1000}, LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 1,
      0, 1, 0 },
};

// ===========================================================================
// ship — discover + record (drawRoad + state.ship); Phase-3 flight is deferred.
// ===========================================================================
static const SpButton sh_btns[] = {
    { "salvage", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene sh_scenes[] = {
    { { "the familiar curves of a wanderer vessel rise up out of the dust and ash. ",
        "lucky that the natives can't work the mechanisms.",
        "with a little effort, it might fly again.", nullptr },
      nullptr, SPE_CLEAR_SHIP, false, 0, NOLOOT, 0, 1, 0 },
};

// ===========================================================================
// iron mine — go inside (costs 1 torch) -> beastly matriarch -> cleared.
// ===========================================================================
static const SetpieceEnemy im_enemies[] = {
    { 'T', "a large creature lunges, muscles rippling in the torchlight", 10, 4, 800, 2, false,
      { {false,R_TEETH,5,10,1000}, {false,R_SCALES,5,10,800},
        {false,R_CLOTH,5,10,500}, LOOT_END,LOOT_END,LOOT_END }, 3 },
};
static const SpButton im_btns[] = {
    /* 0 start.enter   */ { "go inside", I_TORCH, true, 1, 0, 0 },   // -> enter (fight)
    /* 1 start.leave   */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 2 enter.leave   */ { "leave", SP_NO_COST, false, 2, 0, 0 },   // victory -> cleared
    /* 3 cleared.leave */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene im_scenes[] = {
    /* 0 start */
    { { "an old iron mine sits here, tools abandoned and left to rust.",
        "bleached bones are strewn about the entrance. many, deeply scored with jagged grooves.",
        "feral howls echo out of the darkness.", nullptr },
      "the path leads to an abandoned mine", SPE_NONE, false, 0, NOLOOT, 0, 2, 1 },
    /* 1 enter (combat) */
    { { nullptr, nullptr, nullptr, nullptr },
      nullptr, SPE_NONE, true, 0, NOLOOT, 2, 1, 0 },
    /* 2 cleared */
    { { "the beast is dead.", "the mine is now safe for workers.", nullptr, nullptr },
      "the iron mine is clear of dangers", SPE_CLEAR_IRON, false, 0, NOLOOT, 3, 1, 0 },
};

// ===========================================================================
// coal mine — attack -> man x2 -> chief -> cleared.
// ===========================================================================
static const SetpieceEnemy cm_enemies[] = {
    /* 0 man   */ { 'E', "a man joins the fight", 10, 3, 800, 2, false,
        { {false,R_CURED_MEAT,1,5,800}, {false,R_CLOTH,1,5,800}, LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 2 },
    /* 1 chief */ { 'D', "only the chief remains.", 20, 5, 800, 2, false,
        { {false,R_CURED_MEAT,5,10,1000}, {false,R_CLOTH,5,10,800},
          {false,R_IRON,1,5,800}, LOOT_END,LOOT_END,LOOT_END }, 3 },
};
static const SpButton cm_btns[] = {
    /* 0 start.attack  */ { "attack", SP_NO_COST, false, 1, 0, 0 },     // -> a1
    /* 1 start.leave   */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 2 a1.continue   */ { "continue", SP_NO_COST, false, 2, 0, 0 },   // -> a2
    /* 3 a1.run        */ { "run", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 4 a2.continue   */ { "continue", SP_NO_COST, false, 3, 0, 0 },   // -> a3
    /* 5 a2.run        */ { "run", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 6 a3.continue   */ { "continue", SP_NO_COST, false, 4, 0, 0 },   // -> cleared
    /* 7 cleared.leave */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene cm_scenes[] = {
    /* 0 start */
    { { "camp fires burn by the entrance to the mine.",
        "men mill about, weapons at the ready.", nullptr, nullptr },
      "this old mine is not abandoned", SPE_NONE, false, 0, NOLOOT, 0, 2, 1 },
    /* 1 a1 (man)  */ { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT, 2, 2, 1 },
    /* 2 a2 (man)  */ { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT, 4, 2, 1 },
    /* 3 a3 (chief)*/ { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 1, NOLOOT, 6, 1, 0 },
    /* 4 cleared */
    { { "the camp is still, save for the crackling of the fires.",
        "the mine is now safe for workers.", nullptr, nullptr },
      "the coal mine is clear of dangers", SPE_CLEAR_COAL, false, 0, NOLOOT, 7, 1, 0 },
};

// ===========================================================================
// sulphur mine — attack -> soldier x2 (ranged) -> veteran -> cleared.
// ===========================================================================
static const SetpieceEnemy sm_enemies[] = {
    /* 0 soldier */ { 'D', "a soldier, alerted, opens fire.", 50, 8, 800, 2, true,
        { {false,R_CURED_MEAT,1,5,800}, {false,R_BULLETS,1,5,500},
          {true,I_RIFLE,1,1,200}, LOOT_END,LOOT_END,LOOT_END }, 3 },
    /* 1 soldier2*/ { 'D', "a second soldier joins the fight.", 50, 8, 800, 2, true,
        { {false,R_CURED_MEAT,1,5,800}, {false,R_BULLETS,1,5,500},
          {true,I_RIFLE,1,1,200}, LOOT_END,LOOT_END,LOOT_END }, 3 },
    /* 2 veteran */ { 'D', "a grizzled soldier attacks, waving a bayonet.", 65, 10, 800, 2, false,
        { {true,I_BAYONET,1,1,500}, {false,R_CURED_MEAT,1,5,800}, LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 2 },
};
static const SpButton su_btns[] = {
    /* 0 start.attack  */ { "attack", SP_NO_COST, false, 1, 0, 0 },
    /* 1 start.leave   */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 2 a1.continue   */ { "continue", SP_NO_COST, false, 2, 0, 0 },
    /* 3 a1.run        */ { "run", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 4 a2.continue   */ { "continue", SP_NO_COST, false, 3, 0, 0 },
    /* 5 a2.run        */ { "run", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 6 a3.continue   */ { "continue", SP_NO_COST, false, 4, 0, 0 },
    /* 7 cleared.leave */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene su_scenes[] = {
    /* 0 start */
    { { "the military is already set up at the mine's entrance.",
        "soldiers patrol the perimeter, rifles slung over their shoulders.", nullptr, nullptr },
      "a military perimeter is set up around the mine.", SPE_NONE, false, 0, NOLOOT, 0, 2, 1 },
    /* 1 a1 (soldier)  */ { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT, 2, 2, 1 },
    /* 2 a2 (soldier)  */ { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 1, NOLOOT, 4, 2, 1 },
    /* 3 a3 (veteran)  */ { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 2, NOLOOT, 6, 1, 0 },
    /* 4 cleared */
    { { "the military presence has been cleared.",
        "the mine is now safe for workers.", nullptr, nullptr },
      "the sulphur mine is clear of dangers", SPE_CLEAR_SULPHUR, false, 0, NOLOOT, 7, 1, 0 },
};

// ===========================================================================
// cave — full 13-scene dungeon. enter (torch) -> branching beasts/lizards ->
// one of three terminal caches, each clearing the dungeon (OUTPOST + road).
// ===========================================================================
static const SetpieceEnemy cv_enemies[] = {
    /* 0 beast(small)  */ { 'R', "a startled beast defends its home", 5, 1, 800, 1, false,
        { {false,R_FUR,1,10,1000}, {false,R_TEETH,1,5,800}, LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 2 },
    /* 1 beast(small2) */ { 'R', "a startled beast defends its home", 5, 1, 800, 1, false,
        { {false,R_FUR,1,3,1000}, {false,R_TEETH,1,2,800}, LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 2 },
    /* 2 cave lizard   */ { 'R', "a cave lizard attacks", 6, 3, 800, 2, false,
        { {false,R_SCALES,1,3,1000}, {false,R_TEETH,1,2,800}, LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 2 },
    /* 3 large beast   */ { 'R', "a large beast charges out of the dark", 10, 3, 800, 2, false,
        { {false,R_FUR,1,3,1000}, {false,R_TEETH,1,3,1000}, LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 2 },
    /* 4 giant lizard  */ { 'T', "a giant lizard shambles forward", 10, 4, 800, 2, false,
        { {false,R_SCALES,1,3,1000}, {false,R_TEETH,1,3,1000}, LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 2 },
};
static const SpProb cv_probs[] = {
    /* 0 start.enter    */ { 300, 1 }, { 600, 2 }, { 1000, 3 },   // a1 / a2 / a3
    /* 3 a1.continue    */ { 500, 4 }, { 1000, 5 },               // b1 / b2
    /* 5 a2.squeeze     */ { 500, 5 }, { 1000, 6 },               // b2 / b3
    /* 7 a3.continue    */ { 500, 6 }, { 1000, 7 },               // b3 / b4
    /* 9 c1.continue    */ { 500, 10 }, { 1000, 11 },             // end1 / end2
    /* 11 c2.continue   */ { 700, 11 }, { 1000, 12 },             // end2 / end3
};
static const SpButton cv_btns[] = {
    /* 0 start.enter  */ { "go inside", I_TORCH, true, SP_SCENE_PROB, 0, 3 },
    /* 1 start.leave  */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 2 a1.continue  */ { "continue", SP_NO_COST, false, SP_SCENE_PROB, 3, 2 },
    /* 3 a1.leave     */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 4 a2.squeeze   */ { "squeeze", SP_NO_COST, false, SP_SCENE_PROB, 5, 2 },
    /* 5 a2.leave     */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 6 a3.continue  */ { "continue", SP_NO_COST, false, SP_SCENE_PROB, 7, 2 },
    /* 7 a3.leave     */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 8 b1.continue  */ { "continue", SP_NO_COST, false, 8, 0, 0 },    // -> c1
    /* 9 b1.leave     */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 10 b2.continue */ { "continue", I_TORCH, true, 8, 0, 0 },        // torch -> c1
    /* 11 b2.leave    */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 12 b3.continue */ { "continue", SP_NO_COST, false, 9, 0, 0 },    // -> c2
    /* 13 b3.leave    */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 14 b4.continue */ { "continue", SP_NO_COST, false, 9, 0, 0 },    // -> c2
    /* 15 b4.leave    */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 16 c1.continue */ { "continue", SP_NO_COST, false, SP_SCENE_PROB, 9, 2 },
    /* 17 c1.leave    */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 18 c2.continue */ { "continue", SP_NO_COST, false, SP_SCENE_PROB, 11, 2 },
    /* 19 c2.leave    */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 20 end.leave   */ { "leave cave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene cv_scenes[] = {
    /* 0 start */
    { { "the mouth of the cave is wide and dark.", "can't see what's inside.", nullptr, nullptr },
      "the earth here is split, as if bearing an ancient wound", SPE_NONE, false, 0, NOLOOT, 0, 2, 1 },
    /* 1 a1 (beast, small) */
    { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT, 2, 2, 1 },
    /* 2 a2 (narrow) */
    { { "the cave narrows a few feet in.", "the walls are moist and moss-covered", nullptr, nullptr },
      nullptr, SPE_NONE, false, 0, NOLOOT, 4, 2, 1 },
    /* 3 a3 (old camp) */
    { { "the remains of an old camp sits just inside the cave.",
        "bedrolls, torn and blackened, lay beneath a thin layer of dust.", nullptr, nullptr },
      nullptr, SPE_NONE, false, 0,
      { {false,R_CURED_MEAT,1,5,1000}, {true,I_TORCH,1,5,500},
        {false,R_LEATHER,1,5,300}, LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 3, 6, 2, 1 },
    /* 4 b1 (dead wanderer) */
    { { "the body of a wanderer lies in a small cavern.",
        "rot's been to work on it, and some of the pieces are missing.",
        "can't tell what left it here.", nullptr },
      nullptr, SPE_NONE, false, 0,
      { {true,I_IRON_SWORD,1,1,1000}, {false,R_CURED_MEAT,1,5,800},
        {true,I_TORCH,1,3,500}, {false,R_MEDICINE,1,2,100}, LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 4, 8, 2, 1 },
    /* 5 b2 (torch dies) */
    { { "the torch sputters and dies in the damp air", "the darkness is absolute", nullptr, nullptr },
      "the torch goes out", SPE_NONE, false, 0, NOLOOT, 10, 2, 1 },
    /* 6 b3 (beast, small) */
    { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 1, NOLOOT, 12, 2, 1 },
    /* 7 b4 (cave lizard) */
    { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 2, NOLOOT, 14, 2, 1 },
    /* 8 c1 (large beast) */
    { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 3, NOLOOT, 16, 2, 1 },
    /* 9 c2 (giant lizard) */
    { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 4, NOLOOT, 18, 2, 1 },
    /* 10 end1 (nest) */
    { { "the nest of a large animal lies at the back of the cave.", nullptr, nullptr, nullptr },
      nullptr, SPE_CLEAR_DUNGEON, false, 0,
      { {false,R_MEAT,5,10,1000}, {false,R_FUR,5,10,1000}, {false,R_SCALES,5,10,1000},
        {false,R_TEETH,5,10,1000}, {false,R_CLOTH,5,10,500}, LOOT_END,LOOT_END,LOOT_END }, 5, 20, 1, 0 },
    /* 11 end2 (supply cache) */
    { { "a small supply cache is hidden at the back of the cave.", nullptr, nullptr, nullptr },
      nullptr, SPE_CLEAR_DUNGEON, false, 0,
      { {false,R_CLOTH,5,10,1000}, {false,R_LEATHER,5,10,1000}, {false,R_IRON,5,10,1000},
        {false,R_CURED_MEAT,5,10,1000}, {false,R_STEEL,5,10,500},
        {true,I_BOLAS,1,3,300}, {false,R_MEDICINE,1,4,150}, LOOT_END }, 7, 20, 1, 0 },
    /* 12 end3 (old case) */
    { { "an old case is wedged behind a rock, covered in a thick layer of dust.", nullptr, nullptr, nullptr },
      nullptr, SPE_CLEAR_DUNGEON, false, 0,
      { {true,I_STEEL_SWORD,1,1,1000}, {true,I_BOLAS,1,3,500},
        {false,R_MEDICINE,1,3,300}, LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 3, 20, 1, 0 },
};

// ===========================================================================
// town — compact faithful dungeon. explore -> a street thug OR a clinic; each
// path ends in a real terminal reward + clearDungeon (see SCOPE note above).
// ===========================================================================
static const SetpieceEnemy tw_enemies[] = {
    /* 0 thug */ { 'E', "ambushed on the street.", 30, 4, 800, 2, false,
        { {false,R_CLOTH,5,10,800}, {false,R_LEATHER,5,10,800},
          {false,R_CURED_MEAT,1,5,500}, LOOT_END,LOOT_END,LOOT_END }, 3 },
};
static const SpProb tw_probs[] = {
    { 500, 1 }, { 1000, 2 },     // start.explore -> thug / clinic
};
static const SpButton tw_btns[] = {
    /* 0 start.explore */ { "explore", SP_NO_COST, false, SP_SCENE_PROB, 0, 2 },
    /* 1 start.leave   */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 2 thug.continue */ { "continue", SP_NO_COST, false, 3, 0, 0 },   // -> end_bones
    /* 3 thug.run      */ { "run", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 4 clinic.enter  */ { "enter", I_TORCH, true, 4, 0, 0 },          // -> end_meds
    /* 5 clinic.leave  */ { "leave town", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 6 end.leave      */ { "leave town", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene tw_scenes[] = {
    /* 0 start */
    { { "a small suburb lays ahead, empty houses scorched and peeling.",
        "broken streetlights stand, rusting. light hasn't graced this place in a long time.",
        nullptr, nullptr },
      "the town lies abandoned, its citizens long dead", SPE_NONE, false, 0, NOLOOT, 0, 2, 1 },
    /* 1 thug (combat) */
    { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT, 2, 2, 1 },
    /* 2 clinic */
    { { "a squat building up ahead.",
        "a green cross barely visible behind grimy windows.", nullptr, nullptr },
      nullptr, SPE_NONE, false, 0, NOLOOT, 4, 2, 1 },
    /* 3 end_bones (post-thug reward, clearDungeon) */
    { { "eye for an eye seems fair.", "always worked before, at least.",
        "picking the bones finds some useful trinkets.", nullptr },
      nullptr, SPE_CLEAR_DUNGEON, false, 0,
      { {false,R_CURED_MEAT,5,10,1000}, {false,R_IRON,5,10,1000}, {true,I_TORCH,1,5,1000},
        {true,I_BOLAS,1,5,500}, {false,R_MEDICINE,1,2,100}, LOOT_END,LOOT_END,LOOT_END }, 5, 6, 1, 0 },
    /* 4 end_meds (clinic reward, clearDungeon) */
    { { "some medicine abandoned in the drawers.", nullptr, nullptr, nullptr },
      nullptr, SPE_CLEAR_DUNGEON, false, 0,
      { {false,R_MEDICINE,2,5,1000}, LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 1, 6, 1, 0 },
};

// ===========================================================================
// city — compact faithful dungeon. explore -> a ranged soldier OR an old subway
// battle site; each ends in a marquee reward (grenade / laser rifle / energy
// cell / alien alloy) + clearDungeon (see SCOPE note above).
// ===========================================================================
static const SetpieceEnemy ci_enemies[] = {
    /* 0 soldier */ { 'D', "the soldier steps out from between the buildings, rifle raised.",
        50, 8, 800, 2, true,
        { {false,R_CURED_MEAT,1,5,800}, {false,R_BULLETS,1,5,500},
          {true,I_RIFLE,1,1,200}, LOOT_END,LOOT_END,LOOT_END }, 3 },
};
static const SpProb ci_probs[] = {
    { 500, 1 }, { 1000, 2 },     // start.explore -> soldier / subway
};
static const SpButton ci_btns[] = {
    /* 0 start.explore  */ { "explore", SP_NO_COST, false, SP_SCENE_PROB, 0, 2 },
    /* 1 start.leave    */ { "leave", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 2 soldier.cont   */ { "continue", SP_NO_COST, false, 3, 0, 0 },   // -> end_outpost
    /* 3 soldier.run    */ { "run", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
    /* 4 end.leave      */ { "leave city", SP_NO_COST, false, SP_SCENE_END, 0, 0 },
};
static const SpScene ci_scenes[] = {
    /* 0 start */
    { { "a battered highway sign stands guard at the entrance to this once-great city.",
        "the towers that haven't crumbled jut from the landscape like the ribcage of some ancient beast.",
        "might be things worth having still inside.", nullptr },
      "the towers of a decaying city dominate the skyline", SPE_NONE, false, 0, NOLOOT, 0, 2, 1 },
    /* 1 soldier (combat) */
    { { nullptr,nullptr,nullptr,nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT, 2, 2, 1 },
    /* 2 subway (cache, clearDungeon) */
    { { "the tunnel opens up at another platform.",
        "the walls are scorched from an old battle.",
        "bodies and supplies from both sides litter the ground.", nullptr },
      nullptr, SPE_CLEAR_DUNGEON, false, 0,
      { {true,I_RIFLE,1,1,800}, {false,R_BULLETS,1,5,800}, {true,I_LASER_RIFLE,1,1,300},
        {false,R_ENERGY_CELL,1,5,300}, {false,R_ALIEN_ALLOY,1,1,300}, LOOT_END,LOOT_END,LOOT_END }, 5, 4, 1, 0 },
    /* 3 end_outpost (post-soldier reward, clearDungeon) */
    { { "the small military outpost is well supplied.",
        "arms and munitions, relics from the war, are neatly arranged on the store-room floor.",
        "just as deadly now as they were then.", nullptr },
      nullptr, SPE_CLEAR_DUNGEON, false, 0,
      { {true,I_RIFLE,1,1,1000}, {false,R_BULLETS,1,10,1000}, {true,I_GRENADE,1,5,800},
        LOOT_END,LOOT_END,LOOT_END,LOOT_END,LOOT_END }, 3, 4, 1, 0 },
};

// ===========================================================================
// Master table — indexed by SetpieceId (world_data.h). Unimplemented ids
// (SP_NONE, SP_EXECUTIONER, SP_CACHE) carry a null scene pointer; the engine
// treats a null table as "no setpiece" (a no-op landmark step).
// ===========================================================================
#define SPN(a) (uint8_t)(sizeof(a)/sizeof((a)[0]))
static const SpDef SETPIECES[] = {
    /* SP_NONE        */ { nullptr, nullptr, 0, nullptr, nullptr, nullptr },
    /* SP_OUTPOST     */ { "An Outpost",  op_scenes, SPN(op_scenes), op_btns, nullptr,   nullptr },
    /* SP_IRONMINE    */ { "The Iron Mine",    im_scenes, SPN(im_scenes), im_btns, nullptr,   im_enemies },
    /* SP_COALMINE    */ { "The Coal Mine",    cm_scenes, SPN(cm_scenes), cm_btns, nullptr,   cm_enemies },
    /* SP_SULPHURMINE */ { "The Sulphur Mine", su_scenes, SPN(su_scenes), su_btns, nullptr,   sm_enemies },
    /* SP_HOUSE       */ { "An Old House",     ho_scenes, SPN(ho_scenes), ho_btns, ho_probs,  ho_enemies },
    /* SP_CAVE        */ { "A Damp Cave",      cv_scenes, SPN(cv_scenes), cv_btns, cv_probs,  cv_enemies },
    /* SP_TOWN        */ { "A Deserted Town",  tw_scenes, SPN(tw_scenes), tw_btns, tw_probs,  tw_enemies },
    /* SP_CITY        */ { "A Ruined City",    ci_scenes, SPN(ci_scenes), ci_btns, ci_probs,  ci_enemies },
    /* SP_SHIP        */ { "A Crashed Ship",   sh_scenes, SPN(sh_scenes), sh_btns, nullptr,   nullptr },
    /* SP_BOREHOLE    */ { "A Huge Borehole",  bh_scenes, SPN(bh_scenes), bh_btns, nullptr,   nullptr },
    /* SP_BATTLEFIELD */ { "A Forgotten Battlefield", bf_scenes, SPN(bf_scenes), bf_btns, nullptr, nullptr },
    /* SP_SWAMP       */ { "A Murky Swamp",    sw_scenes, SPN(sw_scenes), sw_btns, nullptr,   nullptr },
    /* SP_EXECUTIONER */ { nullptr, nullptr, 0, nullptr, nullptr, nullptr },   // Phase 3
    /* SP_CACHE       */ { nullptr, nullptr, 0, nullptr, nullptr, nullptr },   // prestige-only
};
constexpr int SETPIECE_COUNT = (int)(sizeof(SETPIECES) / sizeof(SETPIECES[0]));
#undef SPN
#undef LOOT_END
#undef NOLOOT

// Is there a real setpiece for this SetpieceId?
inline bool setpieceExists(uint8_t id) {
    return id < SETPIECE_COUNT && SETPIECES[id].scenes != nullptr;
}

}  // namespace adr
