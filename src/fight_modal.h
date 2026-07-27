// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// World combat overlay (Phase 2 milestone 2.3) — the SECOND pager-scheduled
// full-screen state, after event_modal.cpp. A random encounter
// (world_state move() -> STEP_FIGHT) is interruptive real-time combat, so it
// earns a full-screen panel that the "no modal" 铁律 exempts exactly like the
// event modal. Mirrors event_modal's active() guard model: while up it intercepts
// pager::showPage / tickCurrent (background pushes + page ticks no-op) and
// pager::handleTouch (a press drives a weapon/heal/flee button; page turns are
// inert). RAM-only, never persisted — a power-off mid-fight is a flee (the trek
// was already saved at the fight-triggering step; combat state is not; research
// decision 7), so a cold-boot wake resumes the pre-fight tile with no combat.
//
// UI SIDE of world_state's combat engine: world_state owns all the numbers
// (enemy HP, hit/damage rolls, cooldowns, loot) via g_world.cx; this file only
// renders that state and routes a press into g_world.fightAttack/fightEat/
// fightMeds/fightFlee. The engine keeps combat pure + host-testable
// (tools/world_smoke.cpp); the modal keeps the drawing. Every string routes
// through tr() (strings_zh.h) — the §8.3 glyph-closure iron law.
#pragma once
#include <stdint.h>

namespace fight_modal {

// True while the combat panel owns the screen (the guard pager reads, same role
// as event_modal::active()). Set by begin(), cleared on win-dismiss / flee /
// death / forced-sleep.
bool active();

// Start combat against EncounterId `enemyId` (from STEP_FIGHT.scene) and bring
// the panel up: arm g_world's combat state, then one full-panel quality push +
// a short alert tone. Called once from WorldPage::onLocalAction. nowMs = millis().
void begin(uint8_t enemyId, uint32_t nowMs);

// Start combat for a SETPIECE combat scene. The setpiece engine has already
// armed g_world's combat (beginFightSetpiece); this just raises the panel. On the
// winning blow / a flee / a death the outcome is handed BACK to setpiece_modal
// (onFightResult / abort) instead of returning straight to the World page, so the
// setpiece can show its own victory buttons (continue / run) or end. nowMs =
// millis().
void beginSetpiece(uint32_t nowMs);

// One combat second (self-gated on nowMs): drive g_world.fightTick() so the enemy
// swings on its delay + player cooldowns drain, then FASTEST-repaint the dynamic
// band (HP bars + cooldown bars). Player death -> the shared World death frame;
// an idle victory panel auto-dismisses after the idle timeout. Call each loop pass
// while active(); no-op otherwise. nowMs = millis().
void tick(uint32_t nowMs);

// A press landed while active (pager::handleTouch routes it here): hit-test the
// button band, resolve the attack/heal/flee (or dismiss the victory panel),
// repaint, and — on flee/win-dismiss/death — restore the World page underneath.
// Always returns true (the touch is consumed).
bool handleHold(int x, int y);

// Forced-sleep cleanup (main.cpp sleepNow): release the guard so the World page
// can repaint before power-off. Combat is RAM-only + wasn't re-saved mid-fight,
// so this is a flee — a cold boot resumes the pre-fight tile. No-op if inactive.
void endForSleep();

}  // namespace fight_modal
