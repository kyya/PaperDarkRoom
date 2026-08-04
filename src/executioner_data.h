// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room (Doublespeak Games, MPL-2.0) — Phase 3c-2: the Executioner, the
// ravaged battleship at the X tile. Transcribed field-for-field from upstream
// script/events/executioner.js (2343 lines): scene text / branch probabilities /
// costs / loot / enemy stat blocks ARE the port, so this file is a derivative of
// the MPL game and carries the MPL header.
//
// NOT A STANDALONE HEADER: it is the tail of setpieces_data.h (which defines
// SpScene/SpButton/SpProb/SpDef and the LOOT_END/NOLOOT shorthands) and is
// included from exactly one place, just above the SETPIECES[] master table.
// Splitting it out is purely about size — six setpieces, 103 scenes, 203 buttons.
//
// SHAPE (upstream -> port, research-phase3.md §3 / §10.3):
//   * Upstream is FIVE `Events.Executioner[*]` events plus a front hall that hops
//     between them with `nextEvent`. Here that is six SpDefs and the front hall's
//     SP_SCENE_EVENT buttons (the target SetpieceId rides in `probStart`).
//   * The four repeated machines (guard / quadruped / medic / turret) are spread
//     into 20+ scenes upstream with `...Enemies.Executioner.guard`. Here ONE
//     exec_enemies[] table is shared by all six SpDefs (SpDef::enemies is a bare
//     pointer, so there is nothing to duplicate) and every scene indexes into it.
//     The four rows at the tail are the same machines carrying a scene's OWN
//     notification — upstream's `{...spread, notification: X}` override.
//   * Sub-second attackDelays (0.25s bugs, the 2.5s turret) keep their UPSTREAM
//     numbers in attackDelayCS and are folded to the 1s tick by the engine
//     (combat_data.h foldAttack), so these rows stay diffable against upstream.
//   * The six blueprints are 100%-chance LOOT lines upstream; here they are the
//     scene's `bp` bit (game_data.h Blueprint). glowstone's is absent by ruling
//     (§12 Q8) — which is why medical `8`'s automaton drops nothing at all.
//   * Nothing here calls markVisited: the X tile is deliberately RE-ENTRABLE
//     (world.js doSpace picks intro-vs-antechamber off World.state.executioner),
//     and only the command deck's clearDungeon finally spends it.
//
// UPSTREAM BUGS COPIED ON PURPOSE (the values ARE the port):
//   * quadruped's loot object repeats the 'alien alloy' key, so JS keeps only the
//     LAST one: 2-4 @ 0.2, and the four-legs drop nothing 80% of the time
//     (§12 Q5 — reproduce observed behaviour, not authorial intent).
//   * engineering `1-3` has NO leave button: water 5 or hp 10, pick one. See the
//     note on that scene.
//   * three key strings carry upstream's own typos/curly quote and are copied
//     BYTE-FOR-BYTE because they are also the tr() lookup keys: `ship’s` (U+2019),
//     `what appears the be`, and (in the Fabricator) `somtimes`.
#pragma once
#ifndef ADR_SETPIECE_TYPES_DEFINED
#error "executioner_data.h is a continuation of setpieces_data.h — include that"
#endif
// (No `namespace adr {` here: the include site is already inside it.)

// ===========================================================================
// Shared enemy table — every Executioner SpDef points at this one array, so a
// scene's `enemy` field is an index into THIS list, not a per-wing one.
// ===========================================================================
static const SetpieceEnemy exec_enemies[] = {
    /*  0 guard — shared */
    { 'G', "tripped a motion sensor.",
      60, 10, 800, 2, true,
      { {false,R_ENERGY_CELL,1,5,800}, {true,I_LASER_RIFLE,1,1,800}, {false,R_ALIEN_ALLOY,1,1,200}, LOOT_END, LOOT_END, LOOT_END }, 3,
      0, SK_NONE, 0, 0, ST_NONE, 0 },
    /*  1 quadruped — shared */
    { 'Q', "a mobile defence platform trundles around the corner.",
      70, 8, 800, 1, false,
      { {false,R_ALIEN_ALLOY,2,4,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 1,
      0, SK_NONE, 0, 0, ST_NONE, 0 },
    /*  2 medic — shared */
    { 'M', "a medical drone wheels out of control.",
      80, 15, 800, 3, false,
      { {false,R_ALIEN_ALLOY,1,2,1000}, {false,R_HYPO,1,4,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      0, SK_NONE, 0, 40, ST_VENOMOUS, 0 },
    /*  3 turret — shared */
    { 'T', "one of the defence turrets still works.",
      50, 25, 800, 4, true,
      { {false,R_ENERGY_CELL,1,5,800}, {false,R_ALIEN_ALLOY,1,1,800}, {true,I_LASER_RIFLE,1,1,200}, LOOT_END, LOOT_END, LOOT_END }, 3,
      0, SK_NONE, 0, 0, ST_NONE, 0 },
    /*  4 chitinous horror — intro 3-1 */
    { 'H', "a huge arthropod lunges from the shadows, its mandibles thrashing.",
      60, 1, 700, 0, false,
      { {false,R_MEAT,5,10,800}, {false,R_SCALES,5,10,500}, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      25, SK_NONE, 0, 0, ST_NONE, 0 },
    /*  5 chitinous queen — intro 4-1 */
    { 'Q', "the webs part, and a grotesque insect lurches forward.",
      70, 1, 700, 0, false,
      { {false,R_MEAT,8,12,800}, {false,R_SCALES,8,12,500}, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      25, SK_NONE, 0, 0, ST_NONE, 0 },
    /*  6 operative — intro 2-2 */
    { 'O', "an operative waits in ambush around the corner.",
      60, 8, 800, 2, false,
      { {true,I_BAYONET,1,1,500}, {false,R_BULLETS,1,5,800}, {false,R_CURED_MEAT,1,5,800}, LOOT_END, LOOT_END, LOOT_END }, 3,
      0, SK_NONE, 0, 0, ST_NONE, 0 },
    /*  7 researcher — intro 4-2 */
    { 'R', "a dusty researcher clumsily hides in the shadows.",
      20, 1, 800, 2, false,
      { {true,I_TORCH,1,3,800}, {false,R_CLOTH,1,5,800}, {false,R_CURED_MEAT,1,5,800}, LOOT_END, LOOT_END, LOOT_END }, 3,
      0, SK_NONE, 0, 0, ST_NONE, 0 },
    /*  8 ancient beast — intro 4-3 */
    { 'A', "an ancient beast has made these ruins its home.",
      60, 6, 800, 1, false,
      { {false,R_FUR,5,10,1000}, {false,R_MEAT,5,10,1000}, {false,R_TEETH,5,10,800}, LOOT_END, LOOT_END, LOOT_END }, 3,
      0, SK_NONE, 0, 0, ST_NONE, 0 },
    /*  9 automated turret — intro 6 */
    { 'T', "as the lights come online, so too do the defence systems.",
      60, 10, 800, 0, true,
      { {false,R_ENERGY_CELL,1,5,800}, {true,I_LASER_RIFLE,1,1,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      250, SK_NONE, 0, 0, ST_NONE, 0 },
    /* 10 unruly welder — engineering 2-1a */
    { 'W', "assembly arms spin wildly out of control.",
      50, 13, 800, 2, false,
      { {false,R_ENERGY_CELL,1,5,800}, {false,R_ALIEN_ALLOY,1,1,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      0, SK_NONE, 0, 0, ST_NONE, 0 },
    /* 11 unstable prototype — engineering 7 (shield every 5s) */
    { 'P', "an unfinished automaton whirs to life.",
      150, 5, 800, 2, false,
      { {false,R_ALIEN_ALLOY,1,3,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 1,
      0, SK_SHIELD, 5, 0, ST_NONE, 0 },
    /* 12 murderous robot — martial 12 (energised every 13s) */
    { 'M', "the machine attacks, blades whirling.",
      250, 10, 800, 3, false,
      { {false,R_ALIEN_ALLOY,1,3,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 1,
      0, SK_ENERGISED, 13, 0, ST_NONE, 0 },
    /* 13 unstable automaton — medical 8 (self-destructs for 30) */
    { 'A', "something's wrong with this robot.",
      100, 10, 700, 2, false,
      { LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 0,
      0, SK_NONE, 0, 0, ST_NONE, 30 },
    /* 14 malformed experiment — medical 16 (enraged every 16s) */
    { 'E', "a mutated beast leaps from its cell.",
      200, 5, 800, 2, false,
      { LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 0,
      0, SK_ENRAGED, 16, 0, ST_NONE, 0 },
    /* 15 immortal wanderer — command 6 (BOSS: a new status every 7s) */
    { '@', "the immortal wanderer attacks.",
      500, 12, 800, 2, false,
      { {false,R_FLEET_BEACON,1,1,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 1,
      0, SK_RANDOM3, 7, 0, ST_NONE, 0 },
    /* 16 guard, martial 8-1a's own notification */
    { 'G', "drew some attention with all that noise.",
      60, 10, 800, 2, true,
      { {false,R_ENERGY_CELL,1,5,800}, {true,I_LASER_RIFLE,1,1,800}, {false,R_ALIEN_ALLOY,1,1,200}, LOOT_END, LOOT_END, LOOT_END }, 3,
      0, SK_NONE, 0, 0, ST_NONE, 0 },
    /* 17 guard, martial 9-1's own notification */
    { 'G', "ran straight into another one.",
      60, 10, 800, 2, true,
      { {false,R_ENERGY_CELL,1,5,800}, {true,I_LASER_RIFLE,1,1,800}, {false,R_ALIEN_ALLOY,1,1,200}, LOOT_END, LOOT_END, LOOT_END }, 3,
      0, SK_NONE, 0, 0, ST_NONE, 0 },
    /* 18 medic, medical 6-1a's own notification */
    { 'M', "it had friends.",
      80, 15, 800, 3, false,
      { {false,R_ALIEN_ALLOY,1,2,1000}, {false,R_HYPO,1,4,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      0, SK_NONE, 0, 40, ST_VENOMOUS, 0 },
    /* 19 medic, medical 6-2a's own notification */
    { 'M', "the noise draws attention.",
      80, 15, 800, 3, false,
      { {false,R_ALIEN_ALLOY,1,2,1000}, {false,R_HYPO,1,4,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      0, SK_NONE, 0, 40, ST_VENOMOUS, 0 },
};

// ===========================================================================
// executioner-intro — 14 scenes
// ===========================================================================
static const SpButton xi_btns[] = {
    /*   0 start       */ { "enter",                 I_TORCH,         true,  1,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   1 start       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   2 1           */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 0,             3, 0, SPA_ALWAYS, SPE_NONE },
    /*   3 1           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   4 2-1         */ { "continue",              SP_NO_COST,      false, 3,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   5 2-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   6 3-1         */ { "continue",              SP_NO_COST,      false, 4,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   7 3-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   8 4-1         */ { "continue",              SP_NO_COST,      false, 11,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   9 4-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  10 2-2         */ { "continue",              SP_NO_COST,      false, 6,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  11 2-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  12 3-2         */ { "continue",              SP_NO_COST,      false, 7,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  13 3-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  14 4-2         */ { "continue",              SP_NO_COST,      false, 11,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  15 4-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  16 2-3         */ { "continue",              SP_NO_COST,      false, 9,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  17 2-3         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  18 3-3         */ { "continue",              SP_NO_COST,      false, 10,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  19 3-3         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  20 4-3         */ { "continue",              SP_NO_COST,      false, 11,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  21 4-3         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  22 5           */ { "power cycle",           SP_NO_COST,      false, 12,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  23 5           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  24 6           */ { "continue",              SP_NO_COST,      false, 13,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  25 6           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  26 7           */ { "take device and leave", SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
};
static const SpProb xi_probs[] = {
    /*  0 */ {  400,  2 },   // '2-1'
    /*  1 */ {  800,  5 },   // '2-2'
    /*  2 */ { 1000,  8 },   // '2-3'
};
static const SpScene xi_scenes[] = {
    /*  0 'start' */
    { { "the remains of a massive battleship lie here, like a silent sealed city.",
        "it lists to the side in a deep crevasse, cut when it fell from the sky.",
        "the hatches are all sealed, but the hull is blown out just above the dirt, providing an entrance.",
        nullptr,
      }, "the remains of a huge ship are embedded in the earth.", SPE_NONE, false, 0,
      NOLOOT,
      0, 2, 1, false },
    /*  1 '1' */
    { { "the interior of the ship is cold and dark. what little light there is only accentuates its harsh angles.",
        "the walls hum faintly.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      2, 2, 1, false },
    /*  2 '2-1' */
    { { "thick, sticky webbing covers the walls of the corridor.",
        "deeper into the ship, the darkness seems almost to writhe.",
        "a small knapsack hangs from a cluster of webs, a few feet from the floor.",
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_CURED_MEAT,1,5,800}, {false,R_BULLETS,1,5,500}, {false,R_ENERGY_CELL,1,5,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 3,
      4, 2, 1, false },
    /*  3 '3-1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 4, NOLOOT,
      6, 2, 1, false },
    /*  4 '4-1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 5, NOLOOT,
      8, 2, 1, false },
    /*  5 '2-2' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 6, NOLOOT,
      10, 2, 1, false },
    /*  6 '3-2' */
    { { "the military has set up a small camp just inside the ship.",
        "crude attempts have been made to cut into the walls.",
        "scraps of copper wire litter the floor.",
        "two bedrolls are wedged into a corner.",
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_CURED_MEAT,1,5,1000}, {true,I_TORCH,1,3,800}, {false,R_BULLETS,1,5,500}, {false,R_ALIEN_ALLOY,1,2,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 4,
      12, 2, 1, false },
    /*  7 '4-2' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 7, NOLOOT,
      14, 2, 1, false },
    /*  8 '2-3' */
    { { "debris is stacked in the corridor, forming a low barricade.",
        "the walls are scorched and melted.",
        "behind the barricade, a few weapons lay abandoned.",
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {true,I_LASER_RIFLE,1,3,1000}, {false,R_ENERGY_CELL,1,5,800}, {true,I_PLASMA_RIFLE,1,1,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 3,
      16, 2, 1, false },
    /*  9 '3-3' */
    { { "the partially devoured remains of several wanderers are piled before a dark corridor.",
        "shuffling noises can be heard from within.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_ENERGY_CELL,1,5,500}, {false,R_CLOTH,1,5,800}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      18, 2, 1, false },
    /* 10 '4-3' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 8, NOLOOT,
      20, 2, 1, false },
    /* 11 '5' */
    { { "a maintenance panel is embedded in the wall next to a large sealed door.",
        "perhaps the ship’s systems are still operational.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      22, 2, 1, false },
    /* 12 '6' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 9, NOLOOT,
      24, 2, 1, false },
    /* 13 '7' */
    { { "beyond the bulkhead is a small antechamber, seemingly untouched by scavengers.",
        "a large hatch grinds open, and the wind rushes in.",
        "a strange device sits on the floor. looks important.",
        nullptr,
      }, nullptr, SPE_CLEAR_EXEC, false, 0,
      NOLOOT,
      26, 1, 0, false },
};

// ===========================================================================
// executioner-antechamber — 1 scenes
// ===========================================================================
static const SpButton xa_btns[] = {
    /*   0 start       */ { "engineering",           SP_NO_COST,      false, SP_SCENE_EVENT, SP_EXEC_ENG,   0, 0, SPA_NOT_ENGINEERING, SPE_NONE },
    /*   1 start       */ { "medical",               SP_NO_COST,      false, SP_SCENE_EVENT, SP_EXEC_MED,   0, 0, SPA_NOT_MEDICAL, SPE_NONE },
    /*   2 start       */ { "martial",               SP_NO_COST,      false, SP_SCENE_EVENT, SP_EXEC_MAR,   0, 0, SPA_NOT_MARTIAL, SPE_NONE },
    /*   3 start       */ { "command deck",          SP_NO_COST,      false, SP_SCENE_EVENT, SP_EXEC_CMD,   0, 0, SPA_ALL_WINGS, SPE_NONE },
    /*   4 start       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
};
static const SpScene xa_scenes[] = {
    /*  0 'start' */
    { { "a large hatch opens into a wide corridor.",
        "the corridor leads to a bank of elevators, which appear to be functional.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      0, 5, 4, false },
};

// ===========================================================================
// executioner-engineering — 21 scenes
// ===========================================================================
static const SpButton xe_btns[] = {
    /*   0 start       */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 0,             3, 0, SPA_ALWAYS, SPE_NONE },
    /*   1 start       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   2 1-1         */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 3,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*   3 1-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   4 2-1a        */ { "continue",              SP_NO_COST,      false, 4,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   5 2-1a        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   6 2-1b        */ { "continue",              SP_NO_COST,      false, 4,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   7 2-1b        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   8 3-1         */ { "continue",              SP_NO_COST,      false, 13,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   9 3-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  10 1-2         */ { "continue",              SP_NO_COST,      false, 6,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  11 1-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  12 2-2         */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 5,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*  13 2-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  14 3-2a        */ { "continue",              SP_NO_COST,      false, 13,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  15 3-2a        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  16 3-2b        */ { "continue",              SP_NO_COST,      false, 13,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  17 3-2b        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  18 1-3         */ { "extinguish",            SP_COST_WATER,   false, SP_SCENE_PROB, 7,             2, 5, SPA_ALWAYS, SPE_NONE },
    /*  19 1-3         */ { "rush through",          SP_COST_HP,      false, SP_SCENE_PROB, 9,             2, 10, SPA_ALWAYS, SPE_NONE },
    /*  20 2-3a        */ { "continue",              SP_NO_COST,      false, 12,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  21 2-3a        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  22 2-3b        */ { "continue",              SP_NO_COST,      false, 12,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  23 2-3b        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  24 3-3         */ { "continue",              SP_NO_COST,      false, 13,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  25 3-3         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  26 4           */ { "use machine",           R_ALIEN_ALLOY,   false, 14,            0,             0, 0, SPA_ALWAYS, SPE_HEAL_FULL },
    /*  27 4           */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 11,            2, 0, SPA_ALWAYS, SPE_NONE },
    /*  28 4           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  29 4-heal      */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 13,            2, 0, SPA_ALWAYS, SPE_NONE },
    /*  30 4-heal      */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  31 5-1         */ { "continue",              SP_NO_COST,      false, 17,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  32 5-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  33 5-2         */ { "continue",              SP_NO_COST,      false, 17,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  34 5-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  35 6           */ { "continue",              SP_NO_COST,      false, 18,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  36 6           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  37 7-intro     */ { "fight",                 SP_NO_COST,      false, 19,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  38 7           */ { "continue",              SP_NO_COST,      false, 20,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  39 7           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  40 8           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
};
static const SpProb xe_probs[] = {
    /*  0 */ {  300,  1 },   // '1-1'
    /*  1 */ {  700,  5 },   // '1-2'
    /*  2 */ { 1000,  9 },   // '1-3'
    /*  3 */ {  500,  2 },   // '2-1a'
    /*  4 */ { 1000,  3 },   // '2-1b'
    /*  5 */ {  500,  7 },   // '3-2a'
    /*  6 */ { 1000,  8 },   // '3-2b'
    /*  7 */ {  500, 10 },   // '2-3a'
    /*  8 */ { 1000, 11 },   // '2-3b'
    /*  9 */ {  500, 10 },   // '2-3a'
    /* 10 */ { 1000, 11 },   // '2-3b'
    /* 11 */ {  500, 15 },   // '5-1'
    /* 12 */ { 1000, 16 },   // '5-2'
    /* 13 */ {  500, 15 },   // '5-1'
    /* 14 */ { 1000, 16 },   // '5-2'
};
static const SpScene xe_scenes[] = {
    /*  0 'start' */
    { { "elevator doors open to a blasted corridor. debris covers the floor, piled into makeshift defences.",
        "emergency lighting flickers.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      0, 2, 1, false },
    /*  1 '1-1' */
    { { "an automated assembly line performs its empty routines, long since deprived of materials.",
        "its final works lie forgotten, covered by a thin layer of dust.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_ENERGY_CELL,1,5,800}, {true,I_LASER_RIFLE,1,1,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      2, 2, 1, false },
    /*  2 '2-1a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 10, NOLOOT,
      4, 2, 1, false },
    /*  3 '2-1b' */
    { { "assembly arms spark and jitter.",
        "a cacophony of decrepit machinery fills the room.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      6, 2, 1, false },
    /*  4 '3-1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT,
      8, 2, 1, false },
    /*  5 '1-2' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 3, NOLOOT,
      10, 2, 1, false },
    /*  6 '2-2' */
    { { "must have been the engine room, once. the massive machines now stand inert, twisted and scorched by explosions.",
        "the destruction is uniform and precise.",
        "bits of them can be scavenged.",
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_ALIEN_ALLOY,2,5,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 1,
      12, 2, 1, false },
    /*  7 '3-2a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT,
      14, 2, 1, false },
    /*  8 '3-2b' */
    { { "none of the ship's engines escaped the destruction.",
        "it's no mystery why she no longer flies.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      16, 2, 1, false },
    // THE burning corridor — the ONLY scene in the whole game with no way
    // back out, and the only user of the two non-inventory currencies. Copied
    // verbatim after checking upstream (executioner.js:786-803 + events.js
    // updateButtons/buttonClick/getQuantity):
    //   * `cost:{water:5}` and `cost:{hp:10}`, and NO 'leave' button — upstream
    //     really does dead-end here. events.js:1184-1202 only greys a button out
    //     when `num < cost`, so with water<5 AND hp<10 both bands are dead and the
    //     upstream panel has no exit at all (allowLeave only ever wires the loot
    //     'take everything and leave' affordance, and this scene has no loot).
    //   * The affordability test is therefore `>=`, NOT `>`: at exactly 10 HP,
    //     `rush through` is enabled and World.setHp(0) clamps to zero WITHOUT
    //     dying — upstream checks death in doSpace/combat, never in setHp. The
    //     port matches (setpiece_engine btnAvailable/choose), so the wanderer can
    //     walk out of the fire on 0 HP, one hit from the end.
    //   * The dead end does not brick the DEVICE the way it bricks a browser tab:
    //     setpiece state is RAM-only and the trek is committed every scene, so a
    //     sleep or a power-off drops the panel and resumes the trip standing on
    //     the X tile (see setpiece_modal endForSleep / WorldState::restore).
    /*  9 '1-3' */
    { { "sparks cascade from a reactivated power junction, and catch.",
        "the flames fill the corridor.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      18, 2, 0, false },
    /* 10 '2-3a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT,
      20, 2, 1, false },
    /* 11 '2-3b' */
    { { "rows of inert security robots hang suspended from the ceiling.",
        "wires run overhead, corroded and useless.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      22, 2, 1, false },
    /* 12 '3-3' */
    { { "more signs of past combat down the hall. guard post is ransacked.",
        "still, some things can be found.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_ENERGY_CELL,1,5,800}, {true,I_LASER_RIFLE,1,1,700}, {true,I_GRENADE,1,3,600}, {true,I_PLASMA_RIFLE,1,1,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 4,
      24, 2, 1, false },
    /* 13 '4' */
    { { "marks on the door read 'research and development.' everything seems mostly untouched, but dead.",
        "one machine thrums with power, and might still work.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      26, 3, 2, false },
    /* 14 '4-heal' */
    { { "step inside, and the machine whirs. muscle and bone reknit. good as new.",
        nullptr,
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      29, 2, 1, false },
    /* 15 '5-1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 3, NOLOOT,
      31, 2, 1, false },
    /* 16 '5-2' */
    { { "the machines here look unfinished, abandoned by their creator. wires and other scrap are scattered about the work benches.",
        nullptr,
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      33, 2, 1, false },
    /* 17 '6' */
    { { "experimental plans cover one wall, held by an unseen force.",
        "this one looks useful.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      35, 2, 1, false, BP_HYPO + 1 },
    /* 18 '7-intro' */
    { { "clattering metal and old servos. something is coming...",
        nullptr,
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      37, 1, 0, false },
    /* 19 '7' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 11, NOLOOT,
      38, 2, 1, false, BP_KINETIC_ARMOUR + 1 },
    /* 20 '8' */
    { { "at the back of the workshop, elevator doors twitch and buzz.",
        "looks like a way out of here.",
        nullptr,
        nullptr,
      }, nullptr, SPE_MARK_ENGINEERING, false, 0,
      NOLOOT,
      40, 1, 0, false },
};

// ===========================================================================
// executioner-martial — 27 scenes
// ===========================================================================
static const SpButton xm_btns[] = {
    /*   0 start       */ { "continue",              SP_NO_COST,      false, 1,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   1 start       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   2 1           */ { "blow it down",          I_GRENADE,       true,  2,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   3 1           */ { "continue right",        SP_NO_COST,      false, SP_SCENE_PROB, 0,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*   4 1           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   5 2-1         */ { "continue",              SP_NO_COST,      false, 3,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   6 2-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   7 3-1         */ { "continue",              SP_NO_COST,      false, 4,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   8 3-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   9 4-1         */ { "continue",              SP_NO_COST,      false, 13,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  10 4-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  11 2-2         */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 2,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*  12 2-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  13 3-2a        */ { "continue",              SP_NO_COST,      false, 8,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  14 3-2a        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  15 3-2b        */ { "continue",              SP_NO_COST,      false, 8,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  16 3-2b        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  17 4-2         */ { "continue",              SP_NO_COST,      false, 13,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  18 4-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  19 2-3         */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 4,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*  20 2-3         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  21 3-3a        */ { "continue",              SP_NO_COST,      false, 12,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  22 3-3a        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  23 3-3b        */ { "continue",              SP_NO_COST,      false, 12,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  24 3-3b        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  25 4-3         */ { "continue",              SP_NO_COST,      false, 13,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  26 4-3         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  27 5           */ { "continue",              SP_NO_COST,      false, 14,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  28 5           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  29 6           */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 6,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*  30 6           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  31 7-1         */ { "scavenge maps",         SP_NO_COST,      false, 16,            0,             0, 0, SPA_ALWAYS, SPE_REVEAL_MAP3 },
    /*  32 7-1         */ { "continue",              SP_NO_COST,      false, 17,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  33 7-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  34 8-1a        */ { "continue",              SP_NO_COST,      false, 18,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  35 8-1a        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  36 8-1b        */ { "continue",              SP_NO_COST,      false, 18,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  37 8-1b        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  38 9-1         */ { "continue",              SP_NO_COST,      false, 23,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  39 9-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  40 7-2         */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 8,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*  41 7-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  42 8-2a        */ { "continue",              SP_NO_COST,      false, 22,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  43 8-2a        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  44 8-2b        */ { "continue",              SP_NO_COST,      false, 22,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  45 8-2b        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  46 9-2         */ { "continue",              SP_NO_COST,      false, 23,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  47 9-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  48 10          */ { "use machine",           R_ALIEN_ALLOY,   false, 24,            0,             0, 0, SPA_ALWAYS, SPE_HEAL_FULL },
    /*  49 10          */ { "continue",              SP_NO_COST,      false, 24,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  50 10          */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  51 11          */ { "engage",                SP_NO_COST,      false, 25,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  52 12          */ { "continue",              SP_NO_COST,      false, 26,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  53 13          */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
};
static const SpProb xm_probs[] = {
    /*  0 */ {  500,  5 },   // '2-2'
    /*  1 */ { 1000,  9 },   // '2-3'
    /*  2 */ {  500,  6 },   // '3-2a'
    /*  3 */ { 1000,  7 },   // '3-2b'
    /*  4 */ {  500, 10 },   // '3-3a'
    /*  5 */ { 1000, 11 },   // '3-3b'
    /*  6 */ {  500, 15 },   // '7-1'
    /*  7 */ { 1000, 19 },   // '7-2'
    /*  8 */ {  500, 20 },   // '8-2a'
    /*  9 */ { 1000, 21 },   // '8-2b'
};
static const SpScene xm_scenes[] = {
    /*  0 'start' */
    { { "metal grinds, and the elevator doors open halfway. beyond is a brightly lit battlefield. remains litter the corridor, undisturbed by scavengers.",
        "looks like they tried to barricade the elevators.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      0, 2, 1, false },
    /*  1 '1' */
    { { "further along, the corridor branches.",
        "the door to the left is sealed and refuses to open.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      2, 3, 2, false },
    /*  2 '2-1' */
    { { "the blast throws the door inwards.",
        "through the bulkhead is a large room, walls lined with weapon racks. fighting seems to have passed it by.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {true,I_ENERGY_BLADE,2,5,1000}, {true,I_LASER_RIFLE,2,5,1000}, {false,R_ENERGY_CELL,5,20,1000}, {true,I_GRENADE,1,5,800}, {true,I_PLASMA_RIFLE,1,1,200}, LOOT_END, LOOT_END, LOOT_END }, 5,
      5, 2, 1, false },
    /*  3 '3-1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 3, NOLOOT,
      7, 2, 1, false },
    /*  4 '4-1' */
    { { "another door at the end of the hall, sealed from this side.",
        "should be able to open it.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      9, 2, 1, false },
    /*  5 '2-2' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 3, NOLOOT,
      11, 2, 1, false },
    /*  6 '3-2a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 1, NOLOOT,
      13, 2, 1, false },
    /*  7 '3-2b' */
    { { "the corridor is eerily silent.",
        nullptr,
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      15, 2, 1, false },
    /*  8 '4-2' */
    { { "crew cabins flank the hall, devoid of life.",
        "a few useful items can be scavenged.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_ENERGY_CELL,1,5,1000}, {true,I_ENERGY_BLADE,1,1,200}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      17, 2, 1, false },
    /*  9 '2-3' */
    { { "ruined defence turrets flank the corridor.",
        "could put the scrap to good use.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_ALIEN_ALLOY,1,3,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 1,
      19, 2, 1, false },
    /* 10 '3-3a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT,
      21, 2, 1, false },
    /* 11 '3-3b' */
    { { "small sensors in the walls still look to be operational.",
        "easily avoided.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      23, 2, 1, false },
    /* 12 '4-3' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 1, NOLOOT,
      25, 2, 1, false },
    /* 13 '5' */
    { { "large barricades bisect the corridor, scorched by weapons fire.",
        "bodies litter the ground on either side.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      27, 2, 1, false },
    /* 14 '6' */
    { { "documents are scattered down the hall, most charred and curled.",
        "this one looks interesting.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      29, 2, 1, false, BP_PLASMA_RIFLE + 1 },
    /* 15 '7-1' */
    { { "the next door leads to a ransacked planning room.",
        "maps of the surface can still be found amongst the debris.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      31, 3, 2, false },
    /* 16 '8-1a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 16, NOLOOT,
      34, 2, 1, false },
    /* 17 '8-1b' */
    { { "slipped past an automated sentry.",
        "if only they'd been destroyed along with everything else.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      36, 2, 1, false },
    /* 18 '9-1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 17, NOLOOT,
      38, 2, 1, false },
    /* 19 '7-2' */
    { { "the corridor passes through a security checkpoint. the defences are blown apart, ragged edges scorched by laser fire.",
        "past the checkpoint, banks of containment cells can be seen.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      40, 2, 1, false },
    /* 20 '8-2a' */
    { { "the cells are all empty.",
        "power cables running across the ceiling are split in several places, sparking occasionally.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      42, 2, 1, false },
    /* 21 '8-2b' */
    { { "the guards died at their posts, shot through with superheated plasma.",
        "their weapons lie on the floor beside them.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {true,I_LASER_RIFLE,2,2,1000}, {false,R_ENERGY_CELL,5,10,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      44, 2, 1, false },
    /* 22 '9-2' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 1, NOLOOT,
      46, 2, 1, false },
    /* 23 '10' */
    { { "the corridor opens onto a vast training complex, obstacles and features blackened by real combat.",
        "a regenerative machine hums uncannily by one of the courses.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      48, 3, 2, false },
    /* 24 '11' */
    { { "motion from the centre of the yard.",
        "a sparring automaton, still fully function and crusted with timeworn blood, lunges forward.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      51, 1, 0, false },
    /* 25 '12' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 12, NOLOOT,
      52, 1, 0, false, BP_DISRUPTOR + 1 },
    /* 26 '13' */
    { { "the ruins of the sparring machine clatter to the ground.",
        "picked this deck clean.",
        nullptr,
        nullptr,
      }, nullptr, SPE_MARK_MARTIAL, false, 0,
      NOLOOT,
      53, 1, 0, false },
};

// ===========================================================================
// executioner-medical — 31 scenes
// ===========================================================================
static const SpButton xd_btns[] = {
    /*   0 start       */ { "continue",              SP_NO_COST,      false, 1,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   1 start       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   2 1           */ { "continue",              SP_NO_COST,      false, 2,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   3 1           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   4 2           */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 0,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*   5 2           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   6 3a          */ { "continue",              SP_NO_COST,      false, 5,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   7 3a          */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   8 3b          */ { "continue",              SP_NO_COST,      false, 5,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   9 3b          */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  10 4           */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 2,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*  11 4           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  12 5-1         */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 4,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*  13 5-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  14 6-1a        */ { "continue",              SP_NO_COST,      false, 9,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  15 6-1a        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  16 6-1b        */ { "continue",              SP_NO_COST,      false, 9,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  17 6-1b        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  18 7-1         */ { "continue",              SP_NO_COST,      false, 15,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  19 7-1         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  20 5-2         */ { "force locker",          SP_NO_COST,      false, 11,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  21 5-2         */ { "continue",              SP_NO_COST,      false, 13,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  22 5-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  23 6-2a-intro  */ { "continue",              SP_NO_COST,      false, 12,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  24 6-2a-intro  */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  25 6-2a        */ { "continue",              SP_NO_COST,      false, 14,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  26 6-2a        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  27 6-2b        */ { "continue",              SP_NO_COST,      false, 14,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  28 6-2b        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  29 7-2         */ { "continue",              SP_NO_COST,      false, 15,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  30 7-2         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  31 8           */ { "continue",              SP_NO_COST,      false, 16,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  32 8           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  33 9           */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 6,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*  34 9           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  35 10a         */ { "continue",              SP_NO_COST,      false, 19,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  36 10a         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  37 10b         */ { "continue",              SP_NO_COST,      false, 19,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  38 10b         */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  39 11          */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 8,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*  40 11          */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  41 12-1        */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 10,            2, 0, SPA_ALWAYS, SPE_NONE },
    /*  42 12-1        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  43 13-1a       */ { "continue",              SP_NO_COST,      false, 23,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  44 13-1a       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  45 13-1b       */ { "continue",              SP_NO_COST,      false, 23,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  46 13-1b       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  47 14-1        */ { "continue",              SP_NO_COST,      false, 28,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  48 14-1        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  49 12-2        */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 12,            2, 0, SPA_ALWAYS, SPE_NONE },
    /*  50 12-2        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  51 13-2a       */ { "continue",              SP_NO_COST,      false, 27,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  52 13-2a       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  53 13-2b       */ { "continue",              SP_NO_COST,      false, 27,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  54 13-2b       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  55 14-2        */ { "continue",              SP_NO_COST,      false, 28,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  56 14-2        */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  57 15          */ { "continue",              SP_NO_COST,      false, 29,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  58 15          */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  59 16          */ { "continue",              SP_NO_COST,      false, 30,            0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  60 17          */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
};
static const SpProb xd_probs[] = {
    /*  0 */ {  500,  3 },   // '3a'
    /*  1 */ { 1000,  4 },   // '3b'
    /*  2 */ {  500,  6 },   // '5-1'
    /*  3 */ { 1000, 10 },   // '5-2'
    /*  4 */ {  500,  7 },   // '6-1a'
    /*  5 */ { 1000,  8 },   // '6-1b'
    /*  6 */ {  500, 17 },   // '10a'
    /*  7 */ { 1000, 18 },   // '10b'
    /*  8 */ {  500, 20 },   // '12-1'
    /*  9 */ { 1000, 24 },   // '12-2'
    /* 10 */ {  500, 21 },   // '13-1a'
    /* 11 */ { 1000, 22 },   // '13-1b'
    /* 12 */ {  500, 25 },   // '13-2a'
    /* 13 */ { 1000, 26 },   // '13-2b'
};
static const SpScene xd_scenes[] = {
    /*  0 'start' */
    { { "elevator doors open to an empty corridor.",
        "a few dusty corpses can be seen further down, but this deck appears to have been spared most of the combat.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      0, 2, 1, false },
    /*  1 '1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 3, NOLOOT,
      2, 2, 1, false },
    /*  2 '2' */
    { { "past the checkpoint, the corridor is undamaged save for sporadic graffiti.",
        "there was no fighting here.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      4, 2, 1, false },
    /*  3 '3a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 1, NOLOOT,
      6, 2, 1, false },
    /*  4 '3b' */
    { { "automated guardians still stalk the halls, unaware that their masters have long gone.",
        "clumsy machines, and easily avoided.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      8, 2, 1, false },
    /*  5 '4' */
    { { "medical gurneys are fixed to grooves running down the corridor walls.",
        "the automated patient transport system now sits motionless.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      10, 2, 1, false },
    /*  6 '5-1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 2, NOLOOT,
      12, 2, 1, false },
    /*  7 '6-1a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 18, NOLOOT,
      14, 2, 1, false },
    /*  8 '6-1b' */
    { { "more medical robots stand frozen, attached by a network of wires.",
        "they take no notice of the intrusion.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      16, 2, 1, false },
    /*  9 '7-1' */
    { { "weapons are strewn about the medical dispatch bay. must have been used as a muster point.",
        "more strange graffiti adorns the walls.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {true,I_LASER_RIFLE,1,1,1000}, {false,R_ENERGY_CELL,3,10,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      18, 2, 1, false },
    /* 10 '5-2' */
    { { "this ward has been converted to a makeshift strategy room, maps scrawled hastily on any flat surface.",
        "a secure locker is set into one wall.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      20, 3, 2, false },
    /* 11 '6-2a-intro' */
    { { "hinges rusted through. no challenge.",
        nullptr,
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_ENERGY_CELL,5,10,1000}, {false,R_HYPO,1,3,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      23, 2, 1, false },
    /* 12 '6-2a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 19, NOLOOT,
      25, 2, 1, false },
    /* 13 '6-2b' */
    { { "better to move without drawing attention.",
        "noises can be heard from the corridor outside.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      27, 2, 1, false },
    /* 14 '7-2' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 1, NOLOOT,
      29, 2, 1, false },
    /* 15 '8' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 13, NOLOOT,
      31, 2, 1, false },
    /* 16 '9' */
    { { "another checkpoint ahead, fitted with heavy doors.",
        "security is even tighter here.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      33, 2, 1, false },
    /* 17 '10a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT,
      35, 2, 1, false },
    /* 18 '10b' */
    { { "slipped through unnoticed.",
        "air whistles as the doors open. this section must have lower pressure than the rest of the ship.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      37, 2, 1, false },
    /* 19 '11' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 2, NOLOOT,
      39, 2, 1, false },
    /* 20 '12-1' */
    { { "the air is cooler here. low cabinets ring the room, doors dusted with frost.",
        "samples of something biological inside.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_CURED_MEAT,5,10,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 1,
      41, 2, 1, false },
    /* 21 '13-1a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT,
      43, 2, 1, false },
    /* 22 '13-1b' */
    { { "security drones still patrol the hallways.",
        "predictable paths.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      45, 2, 1, false },
    /* 23 '14-1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 2, NOLOOT,
      47, 2, 1, false },
    /* 24 '12-2' */
    { { "surgical tools are scattered on the floor, near what appears the be the remains of a fire.",
        "strange.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      49, 2, 1, false },
    /* 25 '13-2a' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 2, NOLOOT,
      51, 2, 1, false },
    /* 26 '13-2b' */
    { { "the air in this room has a metallic tinge. floor is covered in dark powder.",
        "some completed explosives in the corner.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {true,I_GRENADE,3,8,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 1,
      53, 2, 1, false },
    /* 27 '14-2' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 2, NOLOOT,
      55, 2, 1, false },
    /* 28 '15' */
    { { "containment cells arranged at the back of the room, all open.",
        "something moving up ahead.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      57, 2, 1, false },
    /* 29 '16' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 14, NOLOOT,
      59, 1, 0, false, BP_STIM + 1 },
    /* 30 '17' */
    { { "the creature's tortured breathing ceases.",
        "nothing more here.",
        nullptr,
        nullptr,
      }, nullptr, SPE_MARK_MEDICAL, false, 0,
      NOLOOT,
      60, 1, 0, false },
};

// ===========================================================================
// executioner-command — 9 scenes
// ===========================================================================
static const SpButton xc_btns[] = {
    /*   0 start       */ { "continue",              SP_NO_COST,      false, 1,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   1 start       */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   2 1           */ { "continue",              SP_NO_COST,      false, 2,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   3 1           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   4 2           */ { "continue",              SP_NO_COST,      false, SP_SCENE_PROB, 0,             2, 0, SPA_ALWAYS, SPE_NONE },
    /*   5 2           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   6 3a          */ { "continue",              SP_NO_COST,      false, 5,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   7 3a          */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   8 3b          */ { "continue",              SP_NO_COST,      false, 5,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*   9 3b          */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  10 4           */ { "approach",              SP_NO_COST,      false, 6,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  11 4           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  12 5           */ { "observe",               SP_NO_COST,      false, 7,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  13 6           */ { "continue",              SP_NO_COST,      false, 8,             0,             0, 0, SPA_ALWAYS, SPE_NONE },
    /*  14 7           */ { "leave",                 SP_NO_COST,      false, SP_SCENE_END,  0,             0, 0, SPA_ALWAYS, SPE_NONE },
};
static const SpProb xc_probs[] = {
    /*  0 */ {  500,  3 },   // '3a'
    /*  1 */ { 1000,  4 },   // '3b'
};
static const SpScene xc_scenes[] = {
    /*  0 'start' */
    { { "the path to the command bridge is wide, walls adorned with decorative shields.",
        "fighting hadn't reached here, it seems.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      0, 2, 1, false },
    /*  1 '1' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 0, NOLOOT,
      2, 2, 1, false },
    /*  2 '2' */
    { { "detour through the officer's lounge.",
        "might be something useful here.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      4, 2, 1, false },
    /*  3 '3a' */
    { { "small weapons cache in a cabinet.",
        "lucky.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_ENERGY_CELL,3,10,1000}, {true,I_GRENADE,1,5,800}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 2,
      6, 2, 1, false },
    /*  4 '3b' */
    { { "found some medical supplies in a discarded bag.",
        nullptr,
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      { {false,R_HYPO,1,3,1000}, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END, LOOT_END }, 1,
      8, 2, 1, false },
    /*  5 '4' */
    { { "the command deck is empty, save for a squat figure sitting motionless in the centre of the room.",
        "in a flash, the figure is standing.",
        nullptr,
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      10, 2, 1, false },
    /*  6 '5' */
    { { "wanderer form, but not quite flesh. not quite metal either. a crystal set into its chest pulses with light.",
        "it says it saw the rebellion coming. said it made arrangements.",
        "says it can't die.",
        nullptr,
      }, nullptr, SPE_NONE, false, 0,
      NOLOOT,
      12, 1, 0, false },
    /*  7 '6' */
    { { nullptr, nullptr, nullptr, nullptr }, nullptr, SPE_NONE, true, 15, NOLOOT,
      13, 1, 0, false },
    /*  8 '7' */
    { { "the crystal pulses brightly, then goes dark. the assailant shimmers as its shape becomes less defined.",
        "then it is gone.",
        "time to get out of here.",
        nullptr,
      }, nullptr, SPE_CLEAR_DUNGEON, false, 0,
      NOLOOT,
      14, 1, 0, false },
};

// (namespace adr is closed by setpieces_data.h, the includer.)
