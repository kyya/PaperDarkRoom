// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Random-event modal (fw 0.3.0) — the FIRST pager-scheduled overlay state
// (research.md §4.1's modal exception). A random event is interruptive
// narrative + a forced choice, so it earns a full-screen panel
// that the铁律's "no modal" rule explicitly exempts. Sets the active()
// guard model: while up it intercepts pager::showPage / tickCurrent (background
// pushes and page ticks no-op) and pager::handleTouch (a long-press selects a
// button band; short taps and page turns are inert). Exit repaints the page
// underneath. RAM-only, never persisted — a deep-sleep wake returns to the
// normal page (the engine's persistent scheduler in GameState re-arms it).
//
// UI SIDE of events::active(): the engine (event_engine.*) owns scene/button
// state; this file only renders it and routes the long-press into events::
// choose(). One event is on screen exactly while active() is true.
#pragma once
#include <stdint.h>

namespace event_modal {

// True while the panel owns the screen (the guard pager reads). Set by show(),
// cleared on exit (a choose that ends the event, or the idle timeout).
bool active();

// Bring the panel up for the currently-active engine event: one epd_quality
// full-panel push (a deliberate flash that also clears ghosting) + a short
// alert tone. Records the open time for the idle-timeout clock. Call once, from
// the loop, when events::active() && !event_modal::active(). nowMs = millis().
void show(uint32_t nowMs);

// A long-press landed while active (pager::handleTouch routes it here): hit-test
// the button bands, resolve the choice through events::choose() (RC_ERR_COST ->
// low tone, no change; RC_OK -> settle tone + persist), then repaint the next
// scene or, if the event ended, restore the page underneath. Always returns true
// (the touch is consumed). Resets the idle-timeout clock.
bool handleHold(int x, int y);

// Idle-timeout watchdog (research.md §5.4): after 2 minutes with no interaction
// auto-click the event's no-cost default button (events::dismissDefault) and
// restore the page underneath, so a forgotten card can sleep instead of the
// keep-awake gate pinning it on. Call each loop pass with nowMs = millis().
void checkTimeout(uint32_t nowMs);

}  // namespace event_modal
