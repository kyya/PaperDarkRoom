// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// World (荒芜世界) map-exploration page — Phase 2 milestone 2.2. The upstream
// "A Barren World" screen: a player-following viewport onto the 61x61 map with a
// survival HUD (water / cured meat / hp / compass). Reached by embarking from
// the Path page (path_page doEmbark jumps to ringIndexByName("world")); left by
// walking home (goHome commits the trip) or dying. The whole engine (map,
// move()/doSpace upkeep, goHome/die, trek.bin persistence) lives in
// world_state.h — this page is a thin renderer + input mapper over it.
//
// Unlike Room/Outside/Trade/Assign/Path, World is NOT a village location, so it
// draws its OWN 36px title (荒芜世界) instead of the shared page_tabs header, and
// resetHitCache()s the header so a tab-less page can't inherit a prior village
// page's stale tab hit-spans (handleTouch calls page_tabs::hitTab for ANY
// y<TAB_H tap). Every string routes through tr() (strings_zh.h) so only the
// official Simplified-Chinese translation reaches the sparse 12px CJK face — the
// §8.3 glyph-closure iron law. Tile glyphs (';' ',' '.' '#' 'A'..'@') are baked
// ASCII (font covers 0x20..0x7E), not tr() text.
//
// Layout (540x960): 36px title (16..52) · HUD line A 水/熏肉/生命 (68) · HUD line
// B 罗盘指向 + 消息位 (96) · a 19-col x 33-row viewport of 24px cells (map
// 42..498 x 124..916), clearing the 32px status bar (< 928). The viewport is a
// freeze/recenter camera (world_page.cpp updateCamera, m_camX/m_camY): held still
// between recenters so a plain step moves only the '@' — the e-ink throttle that
// keeps a step from full-refreshing the whole scrolling map. The map area is ONE
// touch region; a press resolves to N/S/E/W by its dominant axis relative to the
// wanderer's live on-screen cell (pressRect returns w=0 — no flash, the map's own
// redraw is the feedback). A death frame replaces the map on STEP_DIED (any press
// returns to the village); the expedition itself was already discarded by die().
//
// wantsAwake stays false (Page default, not overridden): trek.bin is written
// EVERY step, so a power-off (the device fully powers down to sleep) is always a
// safe pause the next cold boot resumes — no reason to burn battery staying awake
// mid-expedition. The village economy is timestamp-catch-up via settle(), redeemed
// on the next page/boot that settles, so this page needs no per-second tick work.
#pragma once
#include "page.h"

namespace world_page {
// Raise the shared death frame from OUTSIDE the World page's own move loop — the
// fight overlay calls this after g_world.die() kills the wanderer in combat
// (research decision 4: death reuses the World page's existing die + death-screen
// flow). Sets the RAM-only death latch and repaints the World page so the frame
// shows; a press then dismisses it to the village (WorldPage::onLocalAction).
void enterDeath();
bool inDeath();
}  // namespace world_page

class WorldPage : public pages::Page {
public:
    const char* name() const override { return "world"; }
    bool draw(m5gfx::M5Canvas& canvas) override;   // false unless expedition/death
    bool available() const override;               // trek active OR death frame up
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;
    // tick() left as the Page no-op: nothing on this screen changes on the time
    // axis (the HUD reflects only expedition state, which moves on a step, not a
    // second). See the header note on the village economy.

private:
    // One region only — the map area (movement) OR, in the death frame, a
    // full-screen dismiss band. Sized 2 for headroom.
    static constexpr int MAX_REGIONS = 2;

    // Region param sentinels (this page's single region is always one of these).
    static constexpr uint8_t PARAM_MAP   = 0x01;   // map viewport (a step)
    static constexpr uint8_t PARAM_DEATH = 0x02;   // death frame (dismiss)

    // The death frame latch is a world_page:: namespace static (world_page.cpp) so
    // the fight overlay can raise it from outside the move loop (enterDeath); set on
    // STEP_DIED / combat death, cleared on dismiss or when a fresh expedition
    // supersedes it. Keeps the page available() to show the death text after die()
    // has already dropped the trek (so ex.active is false).

    // The HUD message-slot tr() key for the step just taken: a StepResult.notice
    // one-shot (meat/water just ran out, a danger-zone crossing, terrain-change
    // narration — §3.1/§3.3/§7.3) when the engine set one, else the landmark's own
    // name on a STEP_LANDMARK with no overlay (a 2.4 hook, name only, no event).
    // nullptr = no message. Cleared on any step that carries neither.
    const char* m_msgKey = nullptr;

    mutable pages::Region m_regions[MAX_REGIONS];
    mutable int           m_regionCount = 0;

    // Viewport camera — the world coord of the top-left visible cell. Held STILL
    // between recenters so an ordinary step moves only the '@' glyph (two cells
    // change, not the whole scrolling map); updateCamera() recenters on the
    // wanderer ONLY when it reaches the look-ahead margin (or on embark/resume,
    // when the player lands outside the stale window). This is the e-ink throttle
    // the "全屏刷新太多" report needed: the EPD per-pixel diff then drives just the
    // two changed cells on a normal step and the whole viewport only on the
    // ~1-in-N recenter, instead of re-driving all 456x792 every step. mutable:
    // written from the const draw path (drawMapAndHud), read from const resolveDir.
    mutable int16_t m_camX = 0, m_camY = 0;
    mutable bool    m_camInit = false;

    void drawMapAndHud(m5gfx::M5Canvas& c) const;   // the per-step repaint body
    void updateCamera() const;                      // hold-still / recenter-on-margin
    uint8_t resolveDir(int x, int y) const;         // press -> N/S/E/W (dominant axis)
};
