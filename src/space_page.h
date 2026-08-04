// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Space level — the FOURTH full-screen state, after event_modal, fight_modal and
// setpiece_modal, and the only one that is not a modal in the pager's sense.
//
// WHY IT IS A BLOCKING LOOP AND NOT A MODAL (research-phase3.md §8.2). The three
// modals are re-entrant: main.cpp's appLoop pumps them one pass at a time and
// they repaint when something changed. That model cannot carry a 10.85 fps action
// level, for two reasons the panel driver imposes:
//
//   1. THERE IS ONE msg_flip() WAITER SLOT (msg_bridge.h). A game frame is a
//      whole-frame render plus a present(), and if anything else on the app task
//      presents in the same window the second caller blocks forever. Owning the
//      task outright for the duration is the only shape that cannot race.
//   2. THE PRESENT CALL IS NOT A CLOCK. The scan governor parks when the picture
//      stops changing, and a parked scan makes present() return immediately
//      (msg.h's governor note) — so the crash freeze, the victory white and the
//      score screen would all free-run. The loop carries its own millis()
//      metronome; present() only aligns it to VSYNC.
//
// So run() is a subroutine that returns ~62 seconds later, the same shape
// pager::flashPressRect already uses at 120 ms scale. What that costs is stated
// in §11's acceptance item 8: BLE STATUS and OTA do not answer during a flight.
// They resume on the pass after run() returns, because appLoop simply continues.
//
// The rules live in space_game.h (host-compilable, no Arduino); this file is the
// panel, the finger and the buzzer.
#pragma once
#include <stdint.h>

namespace space_page {

// True while the level owns the panel. Read by pager::drawFrame (so a deghost
// composes the GAME's frame, not the page underneath), pager::showPage and
// pager::handleTouch — the same guard ladder the three modals sit in. It is only
// ever true from inside run(), so nothing outside can observe it mid-frame.
bool active();

// Compose the level's whole 540x960 frame into the shared canvas — no flip. The
// pager calls it for the deghost pushes; run() calls it every logic frame.
void renderFrame();

// Play one flight and return when it is over: entry deghost, ~653 logic frames,
// the crash or victory 演出, the exit deghost, and (on a win) the score screen.
// Reads hull/thrusters from g_game, writes the outcome back through
// GameState::onSpaceCrash / onSpaceVictory, saves, and leaves the Starship page
// on screen. Call ONLY from the app task, with no modal up.
void run();

}  // namespace space_page
