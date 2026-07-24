// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Landmark setpiece modal (fw 0.6.x, milestone 2.4) — the FOURTH pager-scheduled
// full-screen overlay, after preview.cpp, event_modal.cpp and fight_modal.cpp. A
// landmark step (world_state move() -> STEP_LANDMARK) opens a narrative choice
// panel exactly like the event modal, but backed by the setpiece_engine (which
// drives the World EXPEDITION, not the village) and able to hand a combat SCENE
// to fight_modal and take the outcome back. A near-clone of event_modal rather
// than a reuse: event_modal is welded to event_engine/GameState stores, whereas
// setpieces read the expedition bag, show banked loot, and interleave combat —
// cloning the ~120-line shell keeps both engines' modals simple and independent.
//
// Guard model mirrors the other overlays: while active() it intercepts
// pager::showPage / tickCurrent / handleTouch. RAM-only, never persisted (the
// scene position is transient like a fight); the trek itself is saved each scene
// (spCommitStep), so a power-off mid-setpiece resumes the pre-setpiece tile with
// any already-banked loot / cleared flags kept.
#pragma once
#include <stdint.h>

namespace setpiece_modal {

// True while the setpiece panel owns the screen (the guard pager reads). Stays
// true across an interleaved fight (fight_modal is checked first, so it wins the
// touch while combat is live); cleared on the setpiece ending (a leave, a flee, a
// combat death, or the idle timeout).
bool active();

// Open the setpiece for SetpieceId `spId` at its 'start' scene and raise the
// panel. No-op if the id has no table (SP_NONE / executioner / cache) — the
// landmark step is then inert. nowMs = millis(). Called from WorldPage's
// STEP_LANDMARK branch.
void begin(uint8_t spId, uint32_t nowMs);

// A press landed while active (pager::handleTouch routes it here): hit-test the
// button bands, resolve the choice through setpiece::choose(), then launch a
// fight (combat scene), repaint the next scene, or restore the World page (the
// setpiece ended). Always returns true (the touch is consumed).
bool handleHold(int x, int y);

// Idle-timeout watchdog (2 min, matching the event modal): auto-click the scene's
// no-cost default button so a forgotten setpiece can sleep. No-op while a fight is
// live (that overlay owns the clock). Call each loop pass with nowMs = millis().
void checkTimeout(uint32_t nowMs);

// fight_modal hand-back after a setpiece combat scene: won -> show the scene's
// post-victory buttons (continue / run) + banked loot; !won (flee) -> the setpiece
// ends and the World page is restored.
void onFightResult(bool won);

// fight_modal calls this on a combat DEATH: tear down the setpiece engine (the
// trip is already discarded by die()); the shared World death frame is raised by
// the caller. Also the forced-sleep teardown path (endForSleep).
void abort();

// Forced-sleep cleanup (main.cpp sleepNow): release the guard + end the setpiece
// so the World page can repaint before power-off. A cold boot resumes the
// pre-setpiece tile. No-op if inactive.
void endForSleep();

}  // namespace setpiece_modal
