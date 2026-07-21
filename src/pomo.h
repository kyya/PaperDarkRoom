// Firmware-local pomodoro SERVICE (fw 0.10.0): the WORK/BREAK state machine,
// alerts, and low-battery gate — page-independent, runs whatever page is
// shown. Its VIEW is the PomoPage client page (pomo_page.cpp); the bottom bar
// shows a minute-granularity mini-variant while a server page is up. Started
// by a long-press on a type=1 region (PomoPage's buttons, param = minutes);
// any sleep/power-off cancels by construction. See
// docs/superpowers/specs/2026-07-20-client-pages-design.md.
#pragma once
#include <stdint.h>

namespace pomo {

// Long-press on a type=1 region: IDLE -> start WORK of `minutes`; while
// WORK/BREAK runs, ANY type=1 long-press cancels (no restart shortcut).
void onLocalAction(uint8_t minutes);

// Service tick: phase transitions (WORK->BREAK->IDLE), alerts, and the
// off-page minute-granularity bar repaint. Call every loop() pass
// (self-throttled). Page-independent — the WORK->BREAK flip happens even
// while a server page is shown.
void tick();

bool     active();            // WORK or BREAK in progress
bool     inBreak();           // BREAK phase (ring disc) vs WORK (solid disc)
bool     wantsAwake();        // power axis == active() (client_pages aggregate)
int      remainingMinutes();  // ceil of the phase remainder; 0 idle
uint32_t remainingMs();       // exact ms left in the current phase; 0 idle
uint32_t spanMs();            // current phase length (for the drain fraction)
uint8_t  startedMinutes();    // WORK minutes that started this pomo (0 idle)

}  // namespace pomo
