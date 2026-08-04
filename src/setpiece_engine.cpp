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
        // executioner.js:1349 calls World.applyMap() three times in one onChoose.
        case SPE_REVEAL_MAP3:
            for (int i = 0; i < 3; i++) s_w->spApplyMap();
            break;
        // ---- Phase 3c-2: the Executioner --------------------------------------
        case SPE_HEAL_FULL:        s_w->spHealFull(*s_gs); break;
        case SPE_CLEAR_EXEC:       s_w->clearMine(s_w->ex.x, s_w->ex.y, T_EXECUTIONER); break;
        case SPE_MARK_ENGINEERING: s_w->ex.wingEngineering = true; break;
        case SPE_MARK_MARTIAL:     s_w->ex.wingMartial     = true; break;
        case SPE_MARK_MEDICAL:     s_w->ex.wingMedical     = true; break;
        default: break;
    }
}

// world.js redeemBlueprints' other half: a blueprint FOUND this trip. Upstream
// banks a weight-1 backpack item; the port sets the expedition's bit (goHome
// promotes it to GameState::blueprints, die() drops it — the same two-layer rule
// the bag itself obeys). The name goes to the log so the find is visible: the
// panel's loot list is Res/Item-shaped and a blueprint is neither.
void grantBlueprint(uint8_t bp1) {
    if (!bp1 || bp1 > BP_COUNT) return;
    uint8_t bit = (uint8_t)(1u << (bp1 - 1));
    if (s_w->ex.bpFound & bit) return;
    s_w->ex.bpFound |= bit;
    if (s_gs) s_gs->pushLog(BLUEPRINT_KEY[bp1 - 1]);
}

// events.js `available: fn` — the front hall's four wing buttons.
bool availCondMet(uint8_t cond) {
    const Expedition& e = s_w->ex;
    switch (cond) {
        case SPA_NOT_ENGINEERING: return !e.wingEngineering;
        case SPA_NOT_MEDICAL:     return !e.wingMedical;
        case SPA_NOT_MARTIAL:     return !e.wingMartial;
        case SPA_ALL_WINGS:       return e.wingEngineering && e.wingMedical && e.wingMartial;
        default: return true;
    }
}

// events.js updateButtons' cost half (getQuantity(store) < cost -> disabled).
// NOTE the `>=` on hp: upstream's test is `num < cost`, so a 10-HP wanderer CAN
// pay a 10-HP price and land on exactly 0 — World.setHp clamps at zero and never
// checks death (that lives in doSpace / the combat loop), so paying is survivable
// but leaves nothing in the tank. See the engineering `1-3` note in
// executioner_data.h: that is the one scene where it matters, and it is also the
// one scene with no way back out.
bool costAffordable(const SpButton& b) {
    if (b.costSlot == SP_NO_COST)    return true;
    if (b.costSlot == SP_COST_WATER) return s_w->ex.water >= b.costAmt;
    if (b.costSlot == SP_COST_HP)    return s_w->ex.hp    >= b.costAmt;
    return b.costIsItem ? s_w->ex.outfitItem[b.costSlot] > 0
                        : s_w->ex.outfitRes[b.costSlot] > 0;
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
    // A narrative scene's blueprint is part of its loot, so it lands on load; a
    // combat scene's lands on victory (resolveCombat), where its loot would.
    if (sc.bp && !sc.combat) grantBlueprint(sc.bp);
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

bool beginTable(const SpDef* def) {
    if (!s_w || !s_gs) return false;
    if (!def || !def->scenes || def->sceneN == 0) return false;
    s_spId = SP_NONE;                 // a raw table has no SetpieceId identity
    s_def  = def;
    s_active = true;
    loadScene(0);
    return true;
}

bool begin(uint8_t setpieceId) {
    if (!setpieceExists(setpieceId)) return false;
    if (!beginTable(&SETPIECES[setpieceId])) return false;
    s_spId = setpieceId;
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
    return availCondMet(b.availCond) && costAffordable(b);
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
int btnCostAmt(int localBtn) {
    const SpScene* sc = cur();
    if (!sc || localBtn < 0 || localBtn >= sc->btnCount) return 0;
    const SpButton& b = s_def->btns[sc->btnStart + localBtn];
    if (b.costSlot == SP_NO_COST) return 0;
    // A Res/Item setpiece price is always exactly one unit (torch / charm / a
    // grenade / an alien alloy); only water and hp carry an amount.
    return (b.costSlot == SP_COST_WATER || b.costSlot == SP_COST_HP) ? b.costAmt : 1;
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
    const SpScene* sc = cur();
    if (sc && sc->bp) grantBlueprint(sc->bp);      // it was the kill's loot line
    s_w->fightEndVictory();
    s_w->spCommitStep();
}

Result choose(int localBtn) {
    const SpScene* sc = cur();
    if (!sc || s_awaitCombat) return RC_ERR_INVALID;
    if (localBtn < 0 || localBtn >= sc->btnCount) return RC_ERR_INVALID;
    const SpButton& b = s_def->btns[sc->btnStart + localBtn];
    // events.js renders an `available: fn` button DISABLED rather than hiding it,
    // so a press has to be refused here too (a greyed band is still touchable).
    if (!availCondMet(b.availCond)) return RC_ERR_LOCKED;

    // Pay the cost: a single unit from the bag, or (Executioner) an amount of the
    // expedition's own water / blood. Same affordability rule as btnAvailable.
    if (b.costSlot == SP_COST_WATER) {
        if (s_w->ex.water < b.costAmt) return RC_ERR_COST;
        s_w->ex.water -= b.costAmt;
    } else if (b.costSlot == SP_COST_HP) {
        if (s_w->ex.hp < b.costAmt) return RC_ERR_COST;
        s_w->ex.hp -= b.costAmt;            // may land on exactly 0 — see above
    } else if (b.costSlot != SP_NO_COST) {
        if (b.costIsItem) {
            if (s_w->ex.outfitItem[b.costSlot] <= 0) return RC_ERR_COST;
            s_w->ex.outfitItem[b.costSlot]--;
        } else {
            if (s_w->ex.outfitRes[b.costSlot] <= 0) return RC_ERR_COST;
            s_w->ex.outfitRes[b.costSlot]--;
        }
    }

    // events.js buttonClick order: cost, THEN onChoose, then the transition.
    if (b.effect != SPE_NONE) applyEffect(b.effect);

    uint8_t nxt = b.next;
    if (nxt == SP_SCENE_END) { end(); return RC_OK; }
    // events.js switchEvent: tear this setpiece down and open the target one. The
    // World/expedition state is deliberately untouched — the front hall and its
    // four wings are one continuous trip through the same ship.
    if (nxt == SP_SCENE_EVENT) {
        uint8_t target = b.probStart;
        end();
        return begin(target) ? RC_OK : RC_ERR_INVALID;
    }
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
