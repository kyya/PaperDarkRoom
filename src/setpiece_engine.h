// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room — Phase 2 milestone 2.4 landmark setpiece engine. The scene
// machine for the World's landmark events (setpieces_data.h), a sibling of the
// village random-event engine (event_engine.h) but deliberately SEPARATE:
//   * it mutates the volatile World EXPEDITION bag (WorldState::ex outfit /
//     water / cleared flags), not the village stores the event engine drives;
//   * scenes carry COMBAT nodes (handed to fight_modal) and onLoad MAP hooks
//     (clearDungeon / clearMine / useOutpost / grant-perk) the village engine has
//     no concept of.
// Folding those into event_engine would thread a "which world am I in" mode
// through the tested Phase-1/2.0-2.3 core for no shared behaviour, so setpieces
// get their own small machine and event_engine is left untouched (regression
// safety). Like event_engine it is UI-INDEPENDENT and pure (host-testable in
// tools/world_smoke.cpp): it queries/commands only, drives no drawing, and
// launches no fight — the modal reads awaitingCombat() and orchestrates the
// fight_modal handoff, then reports the outcome back via resolveCombat().
#pragma once
#include <stdint.h>
#include "game_state.h"
#include "world_state.h"
#include "setpieces_data.h"

namespace setpiece {

// Bind to the World + game state a running setpiece reads/mutates. Call once at
// boot (after the world/game models exist). Also clears any active setpiece.
void bind(adr::WorldState* world, adr::GameState* gs);

// Start setpiece `setpieceId` (a SetpieceId) at its 'start' scene, running that
// scene's onLoad. Returns false (and stays inactive) if the id has no table
// (SP_NONE / executioner / cache) — the landmark step is then a no-op. If 'start'
// were a combat scene it would arm the fight here too, but no Phase-2 setpiece
// opens on combat, so begin() always lands on a narrative scene.
bool begin(uint8_t setpieceId);
// The same start, from a table pointer instead of a SetpieceId — the seam the host
// mechanics suite drives a throwaway test setpiece through, so a scene graph built
// purely to exercise the engine never has to be shipped in setpieces_data.h
// (research-phase3.md §11, 3c-1: "用一个临时测试 setpiece（不进最终版）"). begin()
// is this plus the id lookup.
bool beginTable(const adr::SpDef* def);

// A setpiece owns the (modal) screen.
bool active();
// Tear down (leave / a combat death aborts the trip): clears the active flag.
void end();

// ---- queries (safe when inactive) ----
const char* titleKey();                 // tr() key for the panel title, or nullptr
int         sceneTextCount();           // narrative body line count (0..4)
const char* sceneTextKey(int i);        // body line `i` tr() key, or nullptr

// Loot banked by the current scene (a narrative scene's onLoad, or the just-won
// combat), shown under the narrative. Empty when the scene banked nothing.
int         lootCount();
bool        lootIsItem(int i);
uint8_t     lootSlot(int i);            // Res or Item slot (per lootIsItem)
int         lootGot(int i);

int         btnCount();                 // buttons in the current scene
const char* btnTextKey(int localBtn);   // button label tr() key, or nullptr
bool        btnAvailable(int localBtn); // cost affordable from the bag (true if free)
uint8_t     btnCostSlot(int localBtn);  // cost slot, or SP_NO_COST
bool        btnCostIsItem(int localBtn);// cost slot is an Item (else a Res)
int         defaultBtnIndex();          // local index of the no-cost safe-exit button

// ---- combat handoff ----
// True the instant a combat scene loads: the modal must hand g_world's freshly
// armed fight (beginFightSetpiece ran here) to fight_modal. Cleared by
// resolveCombat. While true the scene's buttons are NOT choosable (the fight owns
// the screen).
bool        awaitingCombat();
// Report a setpiece fight's outcome. won -> the combat scene's post-victory
// buttons (continue / run) become live and the banked loot shows; !won (flee) ->
// the setpiece ends (back to World). Player death is handled by the caller
// (die() + death frame) then end().
void        resolveCombat(bool won);

// ---- command ----
// Resolve the current scene's button `localBtn`: pay its cost from the bag
// (RC_ERR_COST if unaffordable), then stay/end/branch to the next scene (running
// its onLoad). Loading a combat scene arms the fight and raises awaitingCombat().
// RC_ERR_INVALID when inactive / bad index / mid-combat.
adr::Result choose(int localBtn);

}  // namespace setpiece
