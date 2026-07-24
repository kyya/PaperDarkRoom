// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// A Dark Room — Phase 2 milestone 2.4 setpiece engine implementation. See
// setpiece_engine.h. Pure logic (no Arduino/M5): binds to WorldState + GameState
// and drives the setpieces_data.h scene machine over the expedition bag, so it
// host-compiles for tools/world_smoke.cpp.
#include "setpiece_engine.h"
#include <string.h>

using namespace adr;

namespace setpiece {
namespace {

WorldState* s_w  = nullptr;
GameState*  s_gs = nullptr;

bool        s_active = false;
uint8_t     s_spId   = SP_NONE;
const SpDef* s_def   = nullptr;
int         s_scene  = -1;          // LOCAL scene index within s_def
bool        s_awaitCombat = false;  // a combat scene loaded, fight not yet resolved
bool        s_combatWon   = false;  // current combat scene won (show its buttons)

// Loot banked by the current scene (narrative onLoad) or the just-won combat.
LootLine    s_disp[SP_SCENE_LOOT_MAX];
int         s_dispN = 0;

const SpScene* cur() {
    if (!s_active || !s_def || s_scene < 0 || s_scene >= s_def->sceneN) return nullptr;
    return &s_def->scenes[s_scene];
}

// Run a scene's onLoad map/expedition effect (setpieces.js onLoad).
void applyEffect(uint8_t effect) {
    switch (effect) {
        case SPE_FILL_WATER:    s_w->spFillWater(); break;
        case SPE_CLEAR_DUNGEON: s_w->clearDungeon(s_w->ex.x, s_w->ex.y); break;
        case SPE_CLEAR_IRON:    s_w->clearMine(s_w->ex.x, s_w->ex.y, T_IRON_MINE); break;
        case SPE_CLEAR_COAL:    s_w->clearMine(s_w->ex.x, s_w->ex.y, T_COAL_MINE); break;
        case SPE_CLEAR_SULPHUR: s_w->clearMine(s_w->ex.x, s_w->ex.y, T_SULPHUR_MINE); break;
        case SPE_CLEAR_SHIP:    s_w->clearMine(s_w->ex.x, s_w->ex.y, T_SHIP); break;
        case SPE_GRANT_GASTRONOME: s_w->spGrantGastronome(*s_gs); break;
        default: break;
    }
}

// Load LOCAL scene `idx`: run its effect + notification, then either arm the
// fight (combat scene) or auto-bank its loot (narrative scene).
void loadScene(int idx) {
    s_scene = idx;
    s_awaitCombat = false;
    s_combatWon = false;
    s_dispN = 0;
    const SpScene& sc = s_def->scenes[idx];
    applyEffect(sc.effect);
    if (sc.visit) s_w->spMarkVisited();               // setpieces.js World.markVisited
    if (sc.notify && s_gs) s_gs->pushLog(sc.notify);
    if (sc.combat) {
        s_w->beginFightSetpiece(s_def->enemies[sc.enemy]);
        s_awaitCombat = true;                     // modal hands it to fight_modal
    } else {
        if (sc.lootN > 0)
            s_dispN = s_w->bankLootTable(*s_gs, sc.loot, sc.lootN,
                                         s_disp, SP_SCENE_LOOT_MAX);
        s_w->spCommitStep();                      // persist bag/water/flags this step
    }
}

}  // namespace

void bind(WorldState* world, GameState* gs) {
    s_w = world; s_gs = gs;
    s_active = false; s_scene = -1;
    s_awaitCombat = s_combatWon = false; s_dispN = 0;
    s_def = nullptr; s_spId = SP_NONE;
}

bool begin(uint8_t setpieceId) {
    if (!s_w || !s_gs) return false;
    if (!setpieceExists(setpieceId)) return false;
    s_spId = setpieceId;
    s_def  = &SETPIECES[setpieceId];
    s_active = true;
    loadScene(0);
    return true;
}

bool active() { return s_active; }

void end() {
    s_active = false;
    s_scene = -1;
    s_awaitCombat = s_combatWon = false;
    s_dispN = 0;
}

const char* titleKey() { return s_active && s_def ? s_def->titleKey : nullptr; }

int sceneTextCount() {
    const SpScene* sc = cur();
    if (!sc) return 0;
    int n = 0;
    for (int i = 0; i < 4 && sc->text[i]; i++) n++;
    return n;
}
const char* sceneTextKey(int i) {
    const SpScene* sc = cur();
    if (!sc || i < 0 || i >= 4) return nullptr;
    return sc->text[i];
}

int  lootCount() { return s_active ? s_dispN : 0; }
bool lootIsItem(int i) { return (i >= 0 && i < s_dispN) && s_disp[i].isItem; }
uint8_t lootSlot(int i) { return (i >= 0 && i < s_dispN) ? s_disp[i].slot : 0; }
int  lootGot(int i) { return (i >= 0 && i < s_dispN) ? s_disp[i].got : 0; }

int btnCount() {
    const SpScene* sc = cur();
    if (!sc) return 0;
    if (sc->combat && s_awaitCombat) return 0;   // fight owns the screen
    return sc->btnCount;
}
const char* btnTextKey(int localBtn) {
    const SpScene* sc = cur();
    if (!sc || localBtn < 0 || localBtn >= sc->btnCount) return nullptr;
    return s_def->btns[sc->btnStart + localBtn].textKey;
}
bool btnAvailable(int localBtn) {
    const SpScene* sc = cur();
    if (!sc || localBtn < 0 || localBtn >= sc->btnCount) return false;
    const SpButton& b = s_def->btns[sc->btnStart + localBtn];
    if (b.costSlot == SP_NO_COST) return true;
    return b.costIsItem ? s_w->ex.outfitItem[b.costSlot] > 0
                        : s_w->ex.outfitRes[b.costSlot] > 0;
}
uint8_t btnCostSlot(int localBtn) {
    const SpScene* sc = cur();
    if (!sc || localBtn < 0 || localBtn >= sc->btnCount) return SP_NO_COST;
    return s_def->btns[sc->btnStart + localBtn].costSlot;
}
bool btnCostIsItem(int localBtn) {
    const SpScene* sc = cur();
    if (!sc || localBtn < 0 || localBtn >= sc->btnCount) return false;
    return s_def->btns[sc->btnStart + localBtn].costIsItem;
}
int defaultBtnIndex() {
    const SpScene* sc = cur();
    if (!sc || (sc->combat && s_awaitCombat)) return -1;
    return sc->defaultBtn;
}

bool awaitingCombat() { return s_active && s_awaitCombat; }

void resolveCombat(bool won) {
    if (!s_active) return;
    if (!won) { end(); return; }                 // fled -> leave the setpiece
    s_awaitCombat = false;
    s_combatWon = true;
    // Copy the banked combat loot (already in the bag) for the victory panel,
    // then release the combat state (loot stays banked).
    const Combat& cx = s_w->combat();
    s_dispN = cx.lootN > SP_SCENE_LOOT_MAX ? SP_SCENE_LOOT_MAX : cx.lootN;
    for (int i = 0; i < s_dispN; i++) s_disp[i] = cx.loot[i];
    s_w->fightEndVictory();
    s_w->spCommitStep();
}

Result choose(int localBtn) {
    const SpScene* sc = cur();
    if (!sc || s_awaitCombat) return RC_ERR_INVALID;
    if (localBtn < 0 || localBtn >= sc->btnCount) return RC_ERR_INVALID;
    const SpButton& b = s_def->btns[sc->btnStart + localBtn];

    // Pay the cost (single unit, from the bag).
    if (b.costSlot != SP_NO_COST) {
        if (b.costIsItem) {
            if (s_w->ex.outfitItem[b.costSlot] <= 0) return RC_ERR_COST;
            s_w->ex.outfitItem[b.costSlot]--;
        } else {
            if (s_w->ex.outfitRes[b.costSlot] <= 0) return RC_ERR_COST;
            s_w->ex.outfitRes[b.costSlot]--;
        }
    }

    uint8_t nxt = b.next;
    if (nxt == SP_SCENE_END) { end(); return RC_OK; }
    if (nxt == SP_SCENE_PROB) {
        int r = s_w->spRand1000();
        nxt = s_def->probs[b.probStart + b.probCount - 1].scene;   // fallback: last
        for (int i = 0; i < b.probCount; i++) {
            const SpProb& p = s_def->probs[b.probStart + i];
            if (r < p.thresholdMilli) { nxt = p.scene; break; }
        }
    }
    loadScene(nxt);
    return RC_OK;
}

}  // namespace setpiece
