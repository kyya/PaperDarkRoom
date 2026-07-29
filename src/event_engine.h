// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room — random-event engine (v0.3.0). Two responsibilities:
//   1. Scheduler: a 3-6 min timer (nextEventAt, persisted in GameState) that,
//      while awake and with no event on screen, filters the event pool by
//      isAvailable and activates one at random from GameState's deterministic
//      RNG. Events fire ONLY while awake (research.md §5.4): deep sleep never
//      triggers, and a wake that slept past the scheduled time re-arms the timer
//      to now+60..120s instead of firing on the instant of waking.
//   2. Scene machine: current scene text/buttons, per-button affordability,
//      choose()/dismissDefault(), cost/reward/onLoad/notification resolution,
//      and the Mysterious Wanderer's delayed echo (armed here, redeemed in
//      GameState::redeemDelayedEcho — offline too).
//
// UI-independent: no drawing headers, pure query/command interface. The UI
// layer polls the queries to render the event panel and calls choose() /
// dismissDefault(). Bind it once to the game state (events::bind), drive it
// each awake second (events::tick), and call events::dismissDefault() before
// sleeping so an on-screen event exits safely instead of vanishing.
#pragma once
#include <stdint.h>
#include "game_state.h"
#include "events_data.h"

namespace events {

// ---- lifecycle ----
// Bind the engine to the game state it reads/mutates. Call once at boot (after
// GameState::load/settle). Also resets runtime state (no active event).
void bind(adr::GameState* gs);

// Clear runtime scene state (active event -> none). Persistent scheduler /
// echo state lives in GameState and is untouched. Called by bind(); exposed for
// tests. Does NOT reschedule.
void reset();

// ---- scheduler ----
// Drive once per awake second. nowMs is the millis() clock (reserved for future
// panel-blink timing); epochNow is the RTC epoch used for scheduling and echo
// redemption. Safe to call every loop pass — self-gated on epoch seconds.
// trekActive mirrors upstream's Engine.activeModule == Room/Outside gate: while
// an expedition is out on the World map, the "home event" pool is treated as
// having no available event (the same 0.5x retry path as a genuinely empty
// pool), so no home event triggers mid-trek.
void tick(uint32_t nowMs, uint32_t epochNow, bool trekActive);

// ---- commands ----
// Resolve the current scene's button `localBtn` (0..btnCount-1): pays cost,
// grants reward, logs notification, then stays / ends / branches to the next
// scene. RC_ERR_COST if unaffordable, RC_ERR_INVALID if no event / bad index.
adr::Result choose(int localBtn);

// Timeout-safe exit: click the current scene's no-cost default button, ending
// the event. No-op if no event is active. Call before sleeping.
void dismissDefault();

// Force-activate event `eventId` at `epochNow` if its isAvailable holds; returns
// false otherwise. Used by the scheduler and by tests/scripted triggers; bypasses
// the timer (does not consume nextEventAt).
bool startEvent(int eventId, uint32_t epochNow);

// ---- queries (all safe when no event is active) ----
bool        active();              // an event is on screen
int         currentEventId();      // EventId, or -1
int         currentScene();        // global scene index, or -1
const char* eventTitleKey();       // tr() key, or nullptr

int         sceneTextCount();      // panel body line count (0..4)
const char* sceneTextKey(int i);   // body line `i` tr() key, or nullptr

int         btnCount();            // buttons in the current scene (0..)
const char* btnTextKey(int localBtn);          // button label tr() key, or nullptr
bool        btnAvailable(int localBtn);        // cost affordable (true if free)
const adr::ResAmt* btnCost(int localBtn);      // RA_END-terminated cost list, or nullptr
int         defaultBtnIndex();     // local index of the no-cost safe-exit button, or -1

uint32_t    nextEventAt();         // scheduled epoch of the next event (0 = unscheduled)

}  // namespace events
