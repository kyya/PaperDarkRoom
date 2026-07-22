// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room (Doublespeak Games, MPL-2.0) — Phase 3 (v0.3.0) random-event
// data tables. Every string, cost, reward, probability and side effect here is
// transcribed from the upstream script/events/room.js & outside.js — the values
// ARE the port, so this file is a derivative of the MPL game and carries the MPL
// header. Pure data, no Arduino/M5 dependency, so event_engine can be
// host-compiled for the smoke test. All en_key strings double as tr() keys
// (strings_zh.h) exactly as upstream keys them.
//
// Roster: all 13 no-combat events. The three medicine events (The Sick Man /
// Sickness / Plague, v0.4.9) are included — medicine is a trading-post good
// (buy medicine), so these gate on stores.medicine>0 exactly as upstream and
// finally give medicine a use. NOTE (upstream-faithful, unchanged): medicine has
// no natural P1 source — traps/income never yield it, and the trading-post buy
// button only appears once medicine has been SEEN (buyOfferable), which itself
// needs medicine from somewhere. So in strict P1 these three fire only after
// medicine is seeded via GM adr:give or Phase-2 World content — matching
// upstream, where medicine is a World-gated resource. Only compass remains cut
// (Phase-2 World content):
// The Nomad still drops its buyCompass button. alien alloy / energy cell appear
// ONLY as Sick-Man rewards (given, never costed) — no P1 economy depends on
// them, they just accumulate as trophies until Phase-2 gives them a sink.
//   - The Nomad          KEPT, buyCompass button CUT (reward = compass)
//   - Noises (outside)   KEPT
//   - Noises (inside)    KEPT
//   - The Beggar         KEPT
//   - The Shady Builder  KEPT
//   - Mysterious Wanderer (wood)  KEPT  (delayed echo)
//   - Mysterious Wanderer (fur)   KEPT  (delayed echo)
//   - A Ruined Trap      KEPT
//   - Fire               KEPT
//   - A Beast Attack     KEPT
//   - The Sick Man       KEPT (v0.4.9)  trigger medicine>0; rewards alloy/cells/scales
//   - Sickness           KEPT (v0.4.9)  heal costs medicine, else villagers die
//   - Plague             KEPT (v0.4.9)  buy/heal with medicine, else villagers die
#pragma once
#include <stdint.h>
#include "game_data.h"

namespace adr {

// ---- Scene-transition sentinels (a button's `next` field) -----------------
// Real scene indices are 0..SCENE_COUNT-1, well below these markers.
constexpr uint8_t SCENE_PROB = 0xFD;  // resolve via the button's PROBS[] table
constexpr uint8_t SCENE_STAY = 0xFE;  // remain on the current scene (repeat trade)
constexpr uint8_t SCENE_END  = 0xFF;  // close the event

// ---- Event availability predicates (isAvailable, ported 1:1) --------------
// The upstream "Engine.activeModule == Room/Outside" gate is decoupled from the
// currently-shown page (the engine is UI-independent, research.md §5.4): Room
// events are eligible whenever the game is running; Outside events require the
// Outside module to exist (outsideUnlocked). The resource/building conditions
// are the real gate and match upstream exactly.
enum AvailCond : uint8_t {
    AV_ROOM_FUR = 0,    // fur > 0
    AV_ROOM_WOOD,       // wood > 0
    AV_ROOM_HUT_RANGE,  // hut >= arg1 && hut < arg2
    AV_OUT_TRAP,        // outsideUnlocked && trap > 0
    AV_OUT_HUT_POP,     // outsideUnlocked && hut > 0 && population > arg1
    AV_OUT_POP,         // outsideUnlocked && population > arg1
    AV_ROOM_MED,        // medicine > 0                                (Sick Man)
    AV_OUT_POP_RANGE_MED, // outsideUnlocked && arg1<pop<arg2 && medicine>0 (Sickness)
    AV_OUT_POP_MED,     // outsideUnlocked && pop>arg1 && medicine>0   (Plague)
};

// ---- Scene onLoad side effects (effect code + param, no lambdas in data) --
enum Effect : uint8_t {
    EFF_NONE = 0,
    EFF_WOOD_PCT,        // arg=Res: take floor(10% wood, min 1) wood,
                         //          give floor(that/5, min 1) of arg (Noises Inside)
    EFF_ADD_HUT,         // hut += 1 while hut < 20 (Shady Builder build scene)
    EFF_WRECK_TRAPS,     // destroy rand[1..trapCount] traps (Ruined Trap start)
    EFF_DESTROY_HUT,     // destroyHuts(arg) — razes huts + kills residents (Fire)
    EFF_KILL_VILLAGERS,  // kill rand[1..arg] villagers (Beast Attack start)
    EFF_KILL_POP_HALF,   // kill rand[1..floor(pop/2)] villagers (Sickness death)
    EFF_KILL_RANGE,      // kill rand[base..base+span-1]; arg=(base<<16)|span
                         //   (Plague healed 2..6 = (2<<16)|5, death 10..89 = (10<<16)|80)
};

// ---- Delayed echo (Mysterious Wanderer) -----------------------------------
// On scene load, with probability probMilli/1000, arm a single persistent
// payout of `amt` whole units of `res`, due 60s later; redeemed in
// GameState::redeemDelayedEcho (offline too). probMilli == 0 => no echo.
struct EchoSpec { int16_t probMilli; uint8_t res; int32_t amt; };

// ---- Probability branch (nextScene {0.5:'a',0.8:'b',1:'c'} semantics) ------
// roll = rand[0,1000); pick the FIRST branch (ascending) with roll <
// thresholdMilli — the upstream "lowest key i with r<i" rule.
struct ProbBranch { int16_t thresholdMilli; uint8_t scene; };

// ---- Button --------------------------------------------------------------
struct BtnDef {
    const char* textKey;
    ResAmt      cost[3];     // RA_END-terminated (whole units)
    ResAmt      reward[3];   // RA_END-terminated (whole units)
    const char* notifyKey;   // pushed to the game log on click; nullptr = none
    uint8_t     next;        // scene idx | SCENE_PROB | SCENE_STAY | SCENE_END
    uint8_t     probStart;   // into PROBS[] when next == SCENE_PROB
    uint8_t     probCount;
};

// ---- Scene ---------------------------------------------------------------
struct SceneDef {
    const char* textKeys[4]; // panel body lines, nullptr-terminated
    const char* notifyKey;   // pushed to the game log on load; nullptr = none
    uint8_t     effect;      // Effect
    int32_t     effectArg;
    ResAmt      reward[3];    // scene-level reward, RA_END-terminated
    EchoSpec    echo;         // probMilli 0 = none
    uint8_t     btnStart;     // into BTNS[]
    uint8_t     btnCount;
    uint8_t     defaultBtn;   // LOCAL index of the no-cost safe-exit button
};

// ---- Event ---------------------------------------------------------------
struct EventDef {
    const char* titleKey;
    uint8_t     avail;        // AvailCond
    int32_t     availArg1;
    int32_t     availArg2;
    uint8_t     sceneStart;   // global index of this event's 'start' scene
};

// Stable event ids (pool index).
enum EventId : uint8_t {
    EV_NOMAD = 0, EV_NOISES_OUT, EV_NOISES_IN, EV_BEGGAR, EV_SHADY,
    EV_WANDER_WOOD, EV_WANDER_FUR, EV_RUINED_TRAP, EV_FIRE, EV_BEAST,
    EV_SICK_MAN, EV_SICKNESS, EV_PLAGUE,          // v0.4.9 medicine events
    EVENT_COUNT
};

// ===========================================================================
// Probability tables (referenced by button.probStart/probCount)
// ===========================================================================
enum {
    PB_NOISES_OUT = 0,   // {0.3: stuff, 1: nothing}
    PB_NOISES_IN  = 2,   // {0.5: scales, 0.8: teeth, 1: cloth}
    PB_BEGGAR_50  = 5,   // {0.5: scales, 0.8: teeth, 1: cloth}
    PB_BEGGAR_100 = 8,   // {0.5: teeth, 0.8: scales, 1: cloth}
    PB_SHADY      = 11,   // {0.6: steal, 1: build}
    PB_RUINED     = 13,   // {0.5: nothing, 1: catch}
    PB_SICK_HELP  = 15,   // {0.1: alloy, 0.3: cells, 0.5: scales, 1: nothing}
};

// Scene indices (kept local to this file for the PROBS/BTN wiring below).
enum {
    S_NOMAD_START = 0,
    S_NO_START, S_NO_NOTHING, S_NO_STUFF,             // Noises Outside
    S_NI_START, S_NI_SCALES, S_NI_TEETH, S_NI_CLOTH,  // Noises Inside
    S_BG_START, S_BG_SCALES, S_BG_TEETH, S_BG_CLOTH,  // Beggar
    S_SH_START, S_SH_STEAL, S_SH_BUILD,               // Shady Builder
    S_WW_START, S_WW_100, S_WW_500,                   // Wanderer wood
    S_WF_START, S_WF_100, S_WF_500,                   // Wanderer fur
    S_RT_START, S_RT_NOTHING, S_RT_CATCH,             // Ruined Trap
    S_FIRE_START,                                     // Fire
    S_BEAST_START,                                    // Beast Attack
    S_SICK_START, S_SICK_ALLOY, S_SICK_CELLS, S_SICK_SCALES, S_SICK_NOTHING,  // Sick Man
    S_SICKNESS_START, S_SICKNESS_HEALED, S_SICKNESS_DEATH,                    // Sickness
    S_PLAGUE_START, S_PLAGUE_HEALED, S_PLAGUE_DEATH,                          // Plague
    SCENE_COUNT
};

static const ProbBranch PROBS[] = {
    /* PB_NOISES_OUT */ { 300, S_NO_STUFF }, { 1000, S_NO_NOTHING },
    /* PB_NOISES_IN  */ { 500, S_NI_SCALES }, { 800, S_NI_TEETH }, { 1000, S_NI_CLOTH },
    /* PB_BEGGAR_50  */ { 500, S_BG_SCALES }, { 800, S_BG_TEETH }, { 1000, S_BG_CLOTH },
    /* PB_BEGGAR_100 */ { 500, S_BG_TEETH }, { 800, S_BG_SCALES }, { 1000, S_BG_CLOTH },
    /* PB_SHADY      */ { 600, S_SH_STEAL }, { 1000, S_SH_BUILD },
    /* PB_RUINED     */ { 500, S_RT_NOTHING }, { 1000, S_RT_CATCH },
    /* PB_SICK_HELP  */ { 100, S_SICK_ALLOY }, { 300, S_SICK_CELLS },
                        { 500, S_SICK_SCALES }, { 1000, S_SICK_NOTHING },
};

// ===========================================================================
// Buttons (referenced by scene.btnStart/btnCount)
// ===========================================================================
enum {
    B_NOMAD = 0,          // buyScales, buyTeeth, buyBait, goodbye
    B_NO_START = 4,       // investigate, ignore
    B_NO_NOTHING = 6,     // go back inside
    B_NO_STUFF = 7,
    B_NI_START = 8,       // investigate, ignore
    B_NI_SCALES = 10, B_NI_TEETH = 11, B_NI_CLOTH = 12,
    B_BG_START = 13,      // give 50, give 100, deny
    B_BG_SCALES = 16, B_BG_TEETH = 17, B_BG_CLOTH = 18,
    B_SH_START = 19,      // 300 wood, deny
    B_SH_STEAL = 21, B_SH_BUILD = 22,
    B_WW_START = 23,      // give 100, give 500, deny
    B_WW_100 = 26, B_WW_500 = 27,
    B_WF_START = 28,      // give 100, give 500, deny
    B_WF_100 = 31, B_WF_500 = 32,
    B_RT_START = 33,      // track, ignore
    B_RT_NOTHING = 35, B_RT_CATCH = 36,
    B_FIRE = 37,          // mourn
    B_BEAST = 38,         // go home
    B_SICK_START = 39,    // help (give 1 medicine), ignore
    B_SICK_ALLOY = 41, B_SICK_CELLS = 42, B_SICK_SCALES = 43, B_SICK_NOTHING = 44,
    B_SICKNESS_START = 45,   // heal (1 medicine), ignore
    B_SICKNESS_HEALED = 47, B_SICKNESS_DEATH = 48,
    B_PLAGUE_START = 49,     // buy medicine, heal (5 medicine), ignore
    B_PLAGUE_HEALED = 52, B_PLAGUE_DEATH = 53,
    BTN_COUNT = 54
};

#define RA1 {RA_END,0}
#define RAEND {RA_END,0},{RA_END,0},{RA_END,0}

static const BtnDef BTNS[BTN_COUNT] = {
    // --- The Nomad (S_NOMAD_START) ---
    { "buy scales", { {R_FUR,100}, RA1, RA1 }, { {R_SCALES,1}, RA1, RA1 }, nullptr, SCENE_STAY, 0, 0 },
    { "buy teeth",  { {R_FUR,200}, RA1, RA1 }, { {R_TEETH,1},  RA1, RA1 }, nullptr, SCENE_STAY, 0, 0 },
    { "buy bait",   { {R_FUR,5},   RA1, RA1 }, { {R_BAIT,1},   RA1, RA1 },
      "traps are more effective with bait.", SCENE_STAY, 0, 0 },
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },

    // --- Noises Outside ---
    { "investigate", { RAEND }, { RAEND }, nullptr, SCENE_PROB, PB_NOISES_OUT, 2 },
    { "ignore them", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },
    { "go back inside", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // nothing scene
    { "go back inside", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // stuff scene

    // --- Noises Inside ---
    { "investigate", { RAEND }, { RAEND }, nullptr, SCENE_PROB, PB_NOISES_IN, 3 },
    { "ignore them", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },
    { "leave", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // scales
    { "leave", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // teeth
    { "leave", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // cloth

    // --- The Beggar ---
    { "give 50",  { {R_FUR,50},  RA1, RA1 }, { RAEND }, nullptr, SCENE_PROB, PB_BEGGAR_50,  3 },
    { "give 100", { {R_FUR,100}, RA1, RA1 }, { RAEND }, nullptr, SCENE_PROB, PB_BEGGAR_100, 3 },
    { "turn him away", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // scales
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // teeth
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // cloth

    // --- The Shady Builder ---
    { "300 wood", { {R_WOOD,300}, RA1, RA1 }, { RAEND }, nullptr, SCENE_PROB, PB_SHADY, 2 },
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },
    { "go home", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // steal
    { "go home", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // build

    // --- Mysterious Wanderer (wood) ---
    { "give 100", { {R_WOOD,100}, RA1, RA1 }, { RAEND }, nullptr, S_WW_100, 0, 0 },
    { "give 500", { {R_WOOD,500}, RA1, RA1 }, { RAEND }, nullptr, S_WW_500, 0, 0 },
    { "turn him away", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // wood100
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // wood500

    // --- Mysterious Wanderer (fur) ---
    { "give 100", { {R_FUR,100}, RA1, RA1 }, { RAEND }, nullptr, S_WF_100, 0, 0 },
    { "give 500", { {R_FUR,500}, RA1, RA1 }, { RAEND }, nullptr, S_WF_500, 0, 0 },
    { "turn her away", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // fur100
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // fur500

    // --- A Ruined Trap ---
    { "track them", { RAEND }, { RAEND }, nullptr, SCENE_PROB, PB_RUINED, 2 },
    { "ignore them", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },
    { "go home", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // nothing
    { "go home", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // catch

    // --- Fire ---
    { "mourn", { RAEND }, { RAEND }, "some villagers have died", SCENE_END, 0, 0 },

    // --- A Beast Attack ---
    { "go home", { RAEND }, { RAEND }, "predators become prey. price is unfair", SCENE_END, 0, 0 },

    // --- The Sick Man (S_SICK_START) ---
    { "give 1 medicine", { {R_MEDICINE,1}, RA1, RA1 }, { RAEND },
      "the man swallows the medicine eagerly", SCENE_PROB, PB_SICK_HELP, 4 },
    { "tell him to leave", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // alloy
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // cells
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // scales
    { "say goodbye", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // nothing

    // --- Sickness ---
    { "1 medicine", { {R_MEDICINE,1}, RA1, RA1 }, { RAEND }, nullptr, S_SICKNESS_HEALED, 0, 0 },
    { "ignore it",  { RAEND }, { RAEND }, nullptr, S_SICKNESS_DEATH, 0, 0 },
    { "go home", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // healed
    { "go home", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // death

    // --- Plague ---
    { "buy medicine", { {R_SCALES,70}, {R_TEETH,50}, RA1 }, { {R_MEDICINE,1}, RA1, RA1 },
      nullptr, SCENE_STAY, 0, 0 },
    { "5 medicine", { {R_MEDICINE,5}, RA1, RA1 }, { RAEND }, nullptr, S_PLAGUE_HEALED, 0, 0 },
    { "do nothing", { RAEND }, { RAEND }, nullptr, S_PLAGUE_DEATH, 0, 0 },
    { "go home", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // healed
    { "go home", { RAEND }, { RAEND }, nullptr, SCENE_END, 0, 0 },  // death
};

// ===========================================================================
// Scenes
// ===========================================================================
static const SceneDef SCENES[SCENE_COUNT] = {
    // ---- S_NOMAD_START ----
    { { "a nomad shuffles into view, laden with makeshift bags bound with rough twine.",
        "won't say from where he came, but it's clear that he's not staying.", nullptr, nullptr },
      "a nomad arrives, looking to trade", EFF_NONE, 0, { RAEND }, {0,0,0},
      B_NOMAD, 4, 3 },

    // ---- Noises Outside ----
    { { "through the walls, shuffling noises can be heard.",
        "can't tell what they're up to.", nullptr, nullptr },
      "strange noises can be heard through the walls", EFF_NONE, 0, { RAEND }, {0,0,0},
      B_NO_START, 2, 1 },
    { { "vague shapes move, just out of sight.", "the sounds stop.", nullptr, nullptr },
      nullptr, EFF_NONE, 0, { RAEND }, {0,0,0}, B_NO_NOTHING, 1, 0 },
    { { "a bundle of sticks lies just beyond the threshold, wrapped in coarse furs.",
        "the night is silent.", nullptr, nullptr },
      nullptr, EFF_NONE, 0, { {R_WOOD,100}, {R_FUR,10}, {RA_END,0} }, {0,0,0},
      B_NO_STUFF, 1, 0 },

    // ---- Noises Inside ----
    { { "scratching noises can be heard from the store room.",
        "something's in there.", nullptr, nullptr },
      "something's in the store room", EFF_NONE, 0, { RAEND }, {0,0,0}, B_NI_START, 2, 1 },
    { { "some wood is missing.", "the ground is littered with small scales", nullptr, nullptr },
      nullptr, EFF_WOOD_PCT, R_SCALES, { RAEND }, {0,0,0}, B_NI_SCALES, 1, 0 },
    { { "some wood is missing.", "the ground is littered with small teeth", nullptr, nullptr },
      nullptr, EFF_WOOD_PCT, R_TEETH, { RAEND }, {0,0,0}, B_NI_TEETH, 1, 0 },
    { { "some wood is missing.", "the ground is littered with scraps of cloth", nullptr, nullptr },
      nullptr, EFF_WOOD_PCT, R_CLOTH, { RAEND }, {0,0,0}, B_NI_CLOTH, 1, 0 },

    // ---- The Beggar ----
    { { "a beggar arrives.", "asks for any spare furs to keep him warm at night.", nullptr, nullptr },
      "a beggar arrives", EFF_NONE, 0, { RAEND }, {0,0,0}, B_BG_START, 3, 2 },
    { { "the beggar expresses his thanks.", "leaves a pile of small scales behind.", nullptr, nullptr },
      nullptr, EFF_NONE, 0, { {R_SCALES,20}, {RA_END,0}, {RA_END,0} }, {0,0,0}, B_BG_SCALES, 1, 0 },
    { { "the beggar expresses his thanks.", "leaves a pile of small teeth behind.", nullptr, nullptr },
      nullptr, EFF_NONE, 0, { {R_TEETH,20}, {RA_END,0}, {RA_END,0} }, {0,0,0}, B_BG_TEETH, 1, 0 },
    { { "the beggar expresses his thanks.", "leaves some scraps of cloth behind.", nullptr, nullptr },
      nullptr, EFF_NONE, 0, { {R_CLOTH,20}, {RA_END,0}, {RA_END,0} }, {0,0,0}, B_BG_CLOTH, 1, 0 },

    // ---- The Shady Builder ----
    { { "a shady builder passes through", "says he can build you a hut for less wood", nullptr, nullptr },
      "a shady builder passes through", EFF_NONE, 0, { RAEND }, {0,0,0}, B_SH_START, 2, 1 },
    { { "the shady builder has made off with your wood", nullptr, nullptr, nullptr },
      "the shady builder has made off with your wood", EFF_NONE, 0, { RAEND }, {0,0,0}, B_SH_STEAL, 1, 0 },
    { { "the shady builder builds a hut", nullptr, nullptr, nullptr },
      "the shady builder builds a hut", EFF_ADD_HUT, 0, { RAEND }, {0,0,0}, B_SH_BUILD, 1, 0 },

    // ---- Mysterious Wanderer (wood) ----
    { { "a wanderer arrives with an empty cart. says if he leaves with wood, he'll be back with more.",
        "builder's not sure he's to be trusted.", nullptr, nullptr },
      "a mysterious wanderer arrives", EFF_NONE, 0, { RAEND }, {0,0,0}, B_WW_START, 3, 2 },
    { { "the wanderer leaves, cart loaded with wood", nullptr, nullptr, nullptr },
      nullptr, EFF_NONE, 0, { RAEND }, { 500, R_WOOD, 300 }, B_WW_100, 1, 0 },
    { { "the wanderer leaves, cart loaded with wood", nullptr, nullptr, nullptr },
      nullptr, EFF_NONE, 0, { RAEND }, { 300, R_WOOD, 1500 }, B_WW_500, 1, 0 },

    // ---- Mysterious Wanderer (fur) ----
    { { "a wanderer arrives with an empty cart. says if she leaves with furs, she'll be back with more.",
        "builder's not sure she's to be trusted.", nullptr, nullptr },
      "a mysterious wanderer arrives", EFF_NONE, 0, { RAEND }, {0,0,0}, B_WF_START, 3, 2 },
    { { "the wanderer leaves, cart loaded with furs", nullptr, nullptr, nullptr },
      nullptr, EFF_NONE, 0, { RAEND }, { 500, R_FUR, 300 }, B_WF_100, 1, 0 },
    { { "the wanderer leaves, cart loaded with furs", nullptr, nullptr, nullptr },
      nullptr, EFF_NONE, 0, { RAEND }, { 300, R_FUR, 1500 }, B_WF_500, 1, 0 },

    // ---- A Ruined Trap ----
    { { "some of the traps have been torn apart.", "large prints lead away, into the forest.", nullptr, nullptr },
      "some traps have been destroyed", EFF_WRECK_TRAPS, 0, { RAEND }, {0,0,0}, B_RT_START, 2, 1 },
    { { "the tracks disappear after just a few minutes.", "the forest is silent.", nullptr, nullptr },
      "nothing was found", EFF_NONE, 0, { RAEND }, {0,0,0}, B_RT_NOTHING, 1, 0 },
    { { "not far from the village lies a large beast, its fur matted with blood.",
        "it puts up little resistance before the knife.", nullptr, nullptr },
      "there was a beast. it's dead now", EFF_NONE, 0,
      { {R_FUR,100}, {R_MEAT,100}, {R_TEETH,10} }, {0,0,0}, B_RT_CATCH, 1, 0 },

    // ---- Fire ----
    { { "a fire rampages through one of the huts, destroying it.",
        "all residents in the hut perished in the fire.", nullptr, nullptr },
      "a fire has started", EFF_DESTROY_HUT, 1, { RAEND }, {0,0,0}, B_FIRE, 1, 0 },

    // ---- A Beast Attack ----
    { { "a pack of snarling beasts pours out of the trees.",
        "the fight is short and bloody, but the beasts are repelled.",
        "the villagers retreat to mourn the dead.", nullptr },
      "wild beasts attack the villagers", EFF_KILL_VILLAGERS, 10,
      { {R_FUR,100}, {R_MEAT,100}, {R_TEETH,10} }, {0,0,0}, B_BEAST, 1, 0 },

    // ---- The Sick Man (Room; medicine>0) ----
    { { "a man hobbles up, coughing.", "he begs for medicine.", nullptr, nullptr },
      "a sick man hobbles up", EFF_NONE, 0, { RAEND }, {0,0,0}, B_SICK_START, 2, 1 },
    { { "the man is thankful.", "he leaves a reward.",
        "some weird metal he picked up on his travels.", nullptr },
      nullptr, EFF_NONE, 0, { {R_ALIEN_ALLOY,1}, {RA_END,0}, {RA_END,0} }, {0,0,0},
      B_SICK_ALLOY, 1, 0 },
    { { "the man is thankful.", "he leaves a reward.",
        "some weird glowing boxes he picked up on his travels.", nullptr },
      nullptr, EFF_NONE, 0, { {R_ENERGY_CELL,3}, {RA_END,0}, {RA_END,0} }, {0,0,0},
      B_SICK_CELLS, 1, 0 },
    { { "the man is thankful.", "he leaves a reward.",
        "all he has are some scales.", nullptr },
      nullptr, EFF_NONE, 0, { {R_SCALES,5}, {RA_END,0}, {RA_END,0} }, {0,0,0},
      B_SICK_SCALES, 1, 0 },
    { { "the man expresses his thanks and hobbles off.", nullptr, nullptr, nullptr },
      nullptr, EFF_NONE, 0, { RAEND }, {0,0,0}, B_SICK_NOTHING, 1, 0 },

    // ---- Sickness (Outside; 10<pop<50, medicine>0) ----
    { { "a sickness is spreading through the village.",
        "medicine is needed immediately.", nullptr, nullptr },
      "some villagers are ill", EFF_NONE, 0, { RAEND }, {0,0,0}, B_SICKNESS_START, 2, 1 },
    { { "the sickness is cured in time.", nullptr, nullptr, nullptr },
      "sufferers are healed", EFF_NONE, 0, { RAEND }, {0,0,0}, B_SICKNESS_HEALED, 1, 0 },
    { { "the sickness spreads through the village.",
        "the days are spent with burials.",
        "the nights are rent with screams.", nullptr },
      "sufferers are left to die", EFF_KILL_POP_HALF, 0, { RAEND }, {0,0,0},
      B_SICKNESS_DEATH, 1, 0 },

    // ---- Plague (Outside; pop>50, medicine>0) ----
    { { "a terrible plague is fast spreading through the village.",
        "medicine is needed immediately.", nullptr, nullptr },
      "a plague afflicts the village", EFF_NONE, 0, { RAEND }, {0,0,0}, B_PLAGUE_START, 3, 2 },
    { { "the plague is kept from spreading.", "only a few die.",
        "the rest bury them.", nullptr },
      "epidemic is eradicated eventually", EFF_KILL_RANGE, (2 << 16) | 5, { RAEND }, {0,0,0},
      B_PLAGUE_HEALED, 1, 0 },
    { { "the plague rips through the village.",
        "the nights are rent with screams.",
        "the only hope is a quick death.", nullptr },
      "population is almost exterminated", EFF_KILL_RANGE, (10 << 16) | 80, { RAEND }, {0,0,0},
      B_PLAGUE_DEATH, 1, 0 },
};

#undef RA1
#undef RAEND

// ===========================================================================
// Events
// ===========================================================================
static const EventDef EVENTS[EVENT_COUNT] = {
    { "The Nomad",               AV_ROOM_FUR,       0,  0, S_NOMAD_START },
    { "Noises",                  AV_ROOM_WOOD,      0,  0, S_NO_START },
    { "Noises",                  AV_ROOM_WOOD,      0,  0, S_NI_START },
    { "The Beggar",              AV_ROOM_FUR,       0,  0, S_BG_START },
    { "The Shady Builder",       AV_ROOM_HUT_RANGE, 5, 20, S_SH_START },
    { "The Mysterious Wanderer", AV_ROOM_WOOD,      0,  0, S_WW_START },
    { "The Mysterious Wanderer", AV_ROOM_FUR,       0,  0, S_WF_START },
    { "A Ruined Trap",           AV_OUT_TRAP,       0,  0, S_RT_START },
    { "Fire",                    AV_OUT_HUT_POP,   50,  0, S_FIRE_START },
    { "A Beast Attack",          AV_OUT_POP,        0,  0, S_BEAST_START },
    { "The Sick Man",            AV_ROOM_MED,          0,  0, S_SICK_START },
    { "Sickness",                AV_OUT_POP_RANGE_MED, 10, 50, S_SICKNESS_START },
    { "Plague",                  AV_OUT_POP_MED,      50,  0, S_PLAGUE_START },
};

}  // namespace adr
