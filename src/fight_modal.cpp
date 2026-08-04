// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// World combat overlay — full-screen panel for a random encounter. See
// fight_modal.h for the role / guard model. world_state owns every number
// (g_world.cx + the combat API); this file only renders that state and routes a
// press. Every string routes through tr() (strings_zh.h) so only the official
// Simplified-Chinese translation reaches the sparse 12px CJK face (§8.3 glyph
// closure); enemy glyphs ('R'/'E'/'K'/'T'/'D') are baked ASCII, not tr() text.
//
// Layout (540x960, status bar owns [928,960]): a static enemy header up top
// (48px combat glyph · 24px enemy name · enemy HP bar · wrapped notification),
// a player block (生命 N/M + HP bar + 水/熏肉/药剂 counts), and a bottom-anchored
// two-column button grid (one attack band per packed weapon — verb label + a
// draining cooldown bar, drawn by the shared action_band like every other button
// in the firmware — then 吃肉 / 服药 / hypo / stim / shield when the wanderer has
// them, then 跑 to flee, always last). The grid is capped at 6 rows and the weapon
// bands paginate behind a 更多 cell once they no longer fit — see GRID_MAX_CELLS
// for the arithmetic. A victory
// panel replaces the body on a kill:
// the enemy's death line + the banked loot + a 离开 band. The per-second tick and
// each landed action FASTEST-repaint just the dynamic band [enemy HP .. buttons];
// entry and the victory transition take a deliberate QUALITY flash.
#include "fight_modal.h"
#include "action_band.h"        // the app-wide button band (shared with every page)
#include "world_state.h"        // g_world combat API + cx + ex
#include "world_page.h"         // world_page::enterDeath (shared death frame)
#include "setpiece_modal.h"     // setpiece combat hand-back (onFightResult / abort)
#include "cjk_text.h"           // cjk::drawText/drawWrapped/textWidth, tr()
#include "pomo_page.h"          // PAD (shared layout authority)
#include "status_bar.h"
#include "pager.h"
#include "beeper.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>

// main.cpp owns the models and the full-screen sprite.
extern adr::GameState  g_game;
extern adr::WorldState g_world;
extern M5Canvas canvas;

using namespace adr;

namespace fight_modal {

namespace {
constexpr int SCALE_BODY  = 2;                 // 12px grid x2 = 24px body
constexpr int GLYPH       = 12 * SCALE_BODY;   // 24px body line box
constexpr int CONTENT_W   = 540 - 2 * PAD;     // 492
// The band label's 36px scale lives in action_band's contract now (v0.12), not
// in a local copy — see action_band.h for why the copies had to go.

// ---- enemy header (static during a fight) ----
constexpr int CHARA_Y     = 14;                // combat glyph, 48px -> 14..62
constexpr int CHARA_SCALE = 4;
constexpr int NAME_Y      = 70;                // enemy name, 24px, centered
constexpr int EHP_Y       = 102;               // enemy HP bar top
constexpr int HP_BAR_H    = 22;
constexpr int NOTIF_Y     = 140;               // notification, wrapped 24px
constexpr int NOTIF_LINEH = 34;

// ---- player block ----
constexpr int PHP_LABEL_Y = 232;               // 生命 N/M
constexpr int PHP_BAR_Y   = 266;               // player HP bar
constexpr int PHP_BAR_H   = 24;
constexpr int SUP_Y       = 308;               // 水 / 熏肉 / 药剂 counts

// ---- button grid (2-col, bottom-anchored) ----
constexpr int BTN_H      = 80;                 // long-press band (§9.3 floor)
constexpr int BTN_GAP    = 10;
constexpr int BTN_BOTTOM = 912;                // bands stack UP; status bar [928,960]
constexpr int COL_GAP    = 12;
constexpr int COL_W      = (CONTENT_W - COL_GAP) / 2;        // 240
constexpr int COL_X0[2]  = { PAD, PAD + COL_W + COL_GAP };   // {24, 276}
constexpr int COL_MID    = 270;                // x < MID => left column
constexpr int VBTN_TOP   = BTN_BOTTOM - BTN_H; // victory 离开 band (832)
// Phase 3c can offer up to 12 weapons and 6 fixed actions — 17-18 bands, which at
// 80px would run off the top of the panel (research-phase3.md §10.4's layout risk,
// §12 Q13). Resolution: keep the 80px long-press floor and PAGINATE the weapons,
// exactly the way room_page / trade_page batch their action lists. Six rows is the
// ceiling that still clears the player supply line: 6*80 + 5*10 = 530, so the grid
// tops out at 912-530 = 382, comfortably below SUP_Y+GLYPH = 332.
constexpr int GRID_MAX_CELLS = 12;             // 6 rows x 2 columns
constexpr int GRID_MAX_ROWS  = GRID_MAX_CELLS / 2;
static_assert(BTN_BOTTOM - (GRID_MAX_ROWS * BTN_H + (GRID_MAX_ROWS - 1) * BTN_GAP)
              > SUP_Y + GLYPH,
              "the worst-case fight grid must not overlap the player supply line");

// Dynamic-repaint band: enemy HP bar down through the buttons (the static enemy
// glyph/name/notification above it never change during a fight).
constexpr int DYN_TOP    = EHP_Y - 4;          // 98

constexpr uint32_t TIMEOUT_MS      = 120u * 1000u;   // idle victory-panel dismiss
constexpr uint32_t VICTORY_GUARD_MS = 400u;          // ignore spam-through taps

// ---- overlay state (RAM-only) ----
bool     s_active     = false;
uint32_t s_lastMs     = 0;     // last interaction (idle-timeout clock)
uint32_t s_lastTickMs = 0;     // 1s combat-tick gate
uint32_t s_victoryMs  = 0;     // when the victory panel appeared (spam guard)
uint32_t s_lastPressMs = 0;    // press debounce (e-ink double-tap bounce, pager.cpp)
int      s_wpnPage    = 0;     // weapon-batch page (only >0 once paging is needed)

// ---- button model, rebuilt each render ----
enum : uint8_t { BK_WEAPON, BK_EAT, BK_MEDS, BK_HYPO, BK_STIM, BK_SHIELD,
                 BK_MORE, BK_FLEE };
struct FBtn {
    uint8_t kind;
    int     wslot;             // weapon slot (BK_WEAPON) else -1
    char    label[24];
    bool    enabled;
    int     coolLeft, coolTotal;
};
FBtn s_btns[GRID_MAX_CELLS];
int  s_btnN = 0;

// How many fixed (non-weapon, non-"更多") action cells the current state offers.
// Gates are events.js:144-155: meat/meds off the BAG, hypo/stim off the bag too,
// shield off the VILLAGE stores (upstream reads `stores`, not the outfit — kinetic
// armour is worn, not carried). Flee is unconditional.
int fixedActionCount() {
    int n = 1;                                              // flee, always
    if (g_world.ex.outfitRes[R_CURED_MEAT] > 0) n++;
    if (g_world.ex.outfitRes[R_MEDICINE] > 0)   n++;
    if (g_world.ex.outfitRes[R_HYPO] > 0)       n++;
    if (g_world.ex.outfitRes[R_STIM] > 0)       n++;
    if (g_game.items[I_KINETIC_ARMOUR] > 0)     n++;
    return n;
}

void addFixed(uint8_t kind, const char* label, bool enabled, int coolLeft,
              int coolTotal) {
    if (s_btnN >= GRID_MAX_CELLS) return;
    FBtn& b = s_btns[s_btnN++];
    b.kind = kind; b.wslot = -1;
    snprintf(b.label, sizeof b.label, "%s", label);
    b.enabled = enabled; b.coolLeft = coolLeft; b.coolTotal = coolTotal;
}

// Order: the weapon batch, then "更多" when the weapons don't all fit, then the
// fixed actions with 跑 pinned last. cx.weapons is frozen at beginFight, so the
// page split can't shift under the player's finger mid-fight.
void buildButtons() {
    s_btnN = 0;
    const Combat& cx = g_world.combat();
    const int fixedN  = fixedActionCount();
    const int total   = g_world.fightWeaponCount();
    const int maxWpn  = GRID_MAX_CELLS - fixedN;

    int numPages = 1, start = 0, take = total, pg = 0;
    bool more = false;
    if (total > maxWpn) {
        int perPage = maxWpn - 1;                            // one cell buys "更多"
        if (perPage < 1) perPage = 1;
        numPages = (total + perPage - 1) / perPage;
        pg = ((s_wpnPage % numPages) + numPages) % numPages;
        start = pg * perPage;
        take = total - start; if (take > perPage) take = perPage;
        more = true;
    }
    for (int i = 0; i < take && s_btnN < GRID_MAX_CELLS; i++) {
        int s = start + i;
        FBtn& b = s_btns[s_btnN++];
        b.kind = BK_WEAPON; b.wslot = s;
        uint8_t wid = g_world.fightWeaponId(s);
        snprintf(b.label, sizeof b.label, "%s", tr(WEAPONS[wid].verb));
        b.enabled   = g_world.fightWeaponEnabled(s);
        b.coolLeft  = g_world.fightWeaponCoolLeft(s);
        b.coolTotal = WEAPONS[wid].cooldownS;
    }
    if (more && s_btnN < GRID_MAX_CELLS) {
        FBtn& b = s_btns[s_btnN++];
        b.kind = BK_MORE; b.wslot = -1;
        // UI chrome with no upstream string, so it uses the two closure-present
        // glyphs 更/多 + an ASCII page indicator — room_page's exact convention,
        // deliberately NOT routed through tr().
        snprintf(b.label, sizeof b.label, "更多 (%d/%d)",
                 (pg + 1 < numPages ? pg + 2 : 1), numPages);
        b.enabled = true; b.coolLeft = 0; b.coolTotal = 0;
    }

    if (g_world.ex.outfitRes[R_CURED_MEAT] > 0)
        addFixed(BK_EAT, tr("eat meat"),
                 cx.eatCool == 0 && g_world.ex.hp < g_world.ex.maxHp,
                 cx.eatCool, FIGHT_EAT_COOLDOWN_S);
    if (g_world.ex.outfitRes[R_MEDICINE] > 0)
        addFixed(BK_MEDS, tr("use meds"),
                 cx.medsCool == 0 && g_world.ex.hp < g_world.ex.maxHp,
                 cx.medsCool, FIGHT_MEDS_COOLDOWN_S);
    if (g_world.ex.outfitRes[R_HYPO] > 0)
        addFixed(BK_HYPO, tr("use hypo"),
                 cx.hypoCool == 0 && g_world.ex.hp < g_world.ex.maxHp,
                 cx.hypoCool, FIGHT_HYPO_COOLDOWN_S);
    if (g_world.ex.outfitRes[R_STIM] > 0)
        // No hp<max gate: a stim is a damage buff that COSTS health, not a heal.
        addFixed(BK_STIM, tr("use stim"), cx.stimCool == 0,
                 cx.stimCool, FIGHT_STIM_COOLDOWN_S);
    if (g_game.items[I_KINETIC_ARMOUR] > 0)
        addFixed(BK_SHIELD, tr("shield"),
                 cx.shieldCool == 0 && cx.playerStatus != ST_SHIELD,
                 cx.shieldCool, FIGHT_SHIELD_COOLDOWN_S);
    addFixed(BK_FLEE, tr("run"), true, 0, 0);       // 跑 — always last
}

int gridRows()      { return (s_btnN + 1) / 2; }
int gridTop()       { int r = gridRows(); return BTN_BOTTOM - r * BTN_H - (r - 1) * BTN_GAP; }
int btnTopY(int i)  { return gridTop() + (i / 2) * (BTN_H + BTN_GAP); }
int btnX(int i)     { return COL_X0[i % 2]; }

int hitButton(int x, int y) {
    if (x < PAD || x >= 540 - PAD) return -1;
    int gt = gridTop();
    if (y < gt) return -1;
    int pitch = BTN_H + BTN_GAP;
    int row = (y - gt) / pitch;
    if ((y - gt) - row * pitch >= BTN_H) return -1;    // in the inter-row gap
    int col = (x < COL_MID) ? 0 : 1;
    int i = row * 2 + col;
    return (i >= 0 && i < s_btnN) ? i : -1;
}

// The rect of attack-grid band `i` — the ONE description of where a fight
// button is, shared by the draw call and handleHold's invert-flash.
pages::Rect btnRect(int i) {
    return pages::Rect{ btnX(i), btnTopY(i), COL_W, BTN_H };
}

// A HP bar: 2px border, inner black fill = cur/max (the cooldown bar's language,
// HP scale). Empty when max<=0.
void drawHpBar(m5gfx::M5Canvas& c, int x, int y, int w, int h, int cur, int max) {
    c.drawRect(x, y, w, h, TFT_BLACK);
    if (max <= 0) return;
    if (cur < 0) cur = 0; if (cur > max) cur = max;
    int inner = w - 4;
    int fw = (int)((int64_t)inner * cur / max);
    if (fw > 0) c.fillRect(x + 2, y + 2, fw, h - 4, TFT_BLACK);
}

// One attack/action band, through the shared renderer (v0.12: this file's
// hand-copied frame + label + cooldown-bar trio is gone — see action_band.h).
// No subtitle ever: a swing's cost is ammo the header already reports, not a
// per-press price, so every cell centres its lone verb in the 80px band and the
// grid stays level. The cooldown bar is the renderer's, not a local variant.
void drawFightBand(m5gfx::M5Canvas& c, const FBtn& b, int i) {
    action_band::draw(c, btnRect(i), b.label, nullptr, b.enabled,
                      b.coolLeft, b.coolTotal);
}

// A single full-width band (victory 离开). Always enabled today — the renderer
// carries the dashed-disabled branch anyway, so a future gated wide band needs
// no new drawing code here.
void drawWideBand(m5gfx::M5Canvas& c, int top, const char* label) {
    action_band::draw(c, pages::Rect{ PAD, top, CONTENT_W, BTN_H }, label,
                      nullptr, true, 0, 0);
}

void renderFight(m5gfx::M5Canvas& c) {
    const Combat& cx = g_world.combat();
    // enemy combat glyph (baked ASCII), then name if any (a setpiece enemy has no
    // shown name — upstream's fight UI shows only the glyph + notification).
    char ch[2] = { cx.enemyChara, 0 };
    int cw = cjk::textWidth(ch, CHARA_SCALE);
    cjk::drawText(c, (540 - cw) / 2, CHARA_Y, ch, CHARA_SCALE);
    if (cx.enemyNameKey) {
        const char* nm = tr(cx.enemyNameKey);
        int nw = cjk::textWidth(nm, SCALE_BODY);
        cjk::drawText(c, (540 - nw) / 2, NAME_Y, nm, SCALE_BODY);
    }
    drawHpBar(c, PAD, EHP_Y, CONTENT_W, HP_BAR_H, cx.enemyHp, cx.enemyMaxHp);
    if (cx.enemyNotifKey)
        cjk::drawWrapped(c, PAD, NOTIF_Y, CONTENT_W, tr(cx.enemyNotifKey),
                         SCALE_BODY, NOTIF_LINEH);

    char hp[32];
    snprintf(hp, sizeof hp, "%s %d/%d", tr("hp"), g_world.ex.hp, g_world.ex.maxHp);
    cjk::drawText(c, PAD, PHP_LABEL_Y, hp, SCALE_BODY);
    drawHpBar(c, PAD, PHP_BAR_Y, CONTENT_W, PHP_BAR_H, g_world.ex.hp, g_world.ex.maxHp);

    char sup[80];
    snprintf(sup, sizeof sup, "%s %d  %s x%d  %s x%d",
             tr("water"), g_world.ex.water,
             tr("cured meat"), (int)g_world.ex.outfitRes[R_CURED_MEAT],
             tr("medicine"), (int)g_world.ex.outfitRes[R_MEDICINE]);
    cjk::drawText(c, PAD, SUP_Y, sup, SCALE_BODY);

    buildButtons();
    for (int i = 0; i < s_btnN; i++) drawFightBand(c, s_btns[i], i);
}

void renderVictory(m5gfx::M5Canvas& c) {
    const Combat& cx = g_world.combat();
    int y = 140;
    // Only random encounters reach fight_modal's own victory panel (a setpiece win
    // is handed back to setpiece_modal before this). enemyDeathKey is set there.
    if (cx.enemyDeathKey)
        y = cjk::drawWrapped(c, PAD, y, CONTENT_W, tr(cx.enemyDeathKey),
                             SCALE_BODY, NOTIF_LINEH);
    y += 36;
    if (cx.lootN == 0) {
        cjk::drawText(c, PAD, y, tr("nothing"), SCALE_BODY);       // 一无所获
    } else {
        for (int i = 0; i < cx.lootN; i++) {
            const LootLine& L = cx.loot[i];
            const char* key = L.isItem ? ITEM_KEY[L.slot] : RES_KEY[L.slot];
            char line[48];
            snprintf(line, sizeof line, "%s x%d", tr(key), (int)L.got);
            cjk::drawText(c, PAD, y, line, SCALE_BODY);
            y += GLYPH + 10;
        }
    }
    drawWideBand(c, VBTN_TOP, tr("leave"));                        // 离开
}

void render() {
    canvas.fillSprite(TFT_WHITE);
    if (g_world.fightWon()) renderVictory(canvas);
    else                    renderFight(canvas);
    status_bar::drawOnto(canvas);
}

// Put this overlay on the panel. There used to be two of these — a FASTEST push
// of just the HP/cooldown band for the per-tick path, and a QUALITY full-panel
// push for entry and the fight->victory scene swap — because the first had to be
// cheap enough to run every second and the second wanted a deliberate grayscale
// clean. Neither distinction survives: the frame is composed whole either way,
// there is no waveform to pick, and one render is ~8ms of the measured ~23ms
// scan period.
// pager::repaint routes back into renderFrame() below while s_active is set.
void push() { pager::repaint(); }

// Release the guard and repaint the World page underneath (the current ring page
// is always the World page — combat only starts from a World move).
void closeToWorld() {
    s_active = false;
    pager::showPage(pager::currentRingIndex(), false);
}
}  // namespace

// ---- public ---------------------------------------------------------------

bool active() { return s_active; }

void renderFrame() { render(); }

namespace {
// Shared entry: raise the panel over an already-armed g_world combat (begin/
// beginSetpiece differ only in HOW cx was armed).
void raise(uint32_t nowMs) {
    s_active      = true;
    s_lastMs      = nowMs;
    s_lastTickMs  = nowMs;
    s_victoryMs   = 0;
    s_lastPressMs = 0;         // don't inherit a stale press time from a prior fight
    s_wpnPage     = 0;         // every fight opens on the first weapon batch
    push();
    // encounter alert: a short falling two-note chime, distinct from the event
    // pop (1047->1568 rising) and the switcher tone (2000).
    beeper::tone(880, 80);
    delay(90);
    beeper::tone(660, 140);
}
}  // namespace

void begin(uint8_t enemyId, uint32_t nowMs) {
    g_world.beginFight(enemyId);
    raise(nowMs);
    Serial.printf("[fight] begin enemy=%u\n", (unsigned)enemyId);
}

void beginSetpiece(uint32_t nowMs) {
    // g_world.cx already armed by setpiece::choose -> beginFightSetpiece.
    raise(nowMs);
    Serial.println("[fight] begin setpiece combat");
}

bool handleHold(int x, int y) {
    // Tap debounce (pager.cpp's e-ink quirk): one physical tap can report TWO
    // clicks ~<350ms apart. Weapon cooldowns are >=1s so this never blocks
    // intentional combat, but it stops a bounce from low-beeping a spurious
    // cooldown reject right after each landed swing.
    uint32_t nowMs = millis();
    if (s_lastPressMs != 0 && nowMs - s_lastPressMs < 300) return true;
    s_lastPressMs = nowMs;
    s_lastMs = nowMs;

    if (g_world.fightWon()) {                       // victory panel: any press leaves
        if (millis() - s_victoryMs < VICTORY_GUARD_MS) return true;  // swallow spam-through
        g_world.fightEndVictory();
        beeper::tone(1800, 80);
        closeToWorld();
        return true;
    }

    int b = hitButton(x, y);
    if (b < 0) { beeper::tone(600, 120); return true; }   // missed every band

    // Press feedback: invert-flash the pressed band (event_modal parity).
    // flashPressRect restores the normal frame itself once the beat is up.
    pages::Rect pr = btnRect(b);
    pager::flashPressRect(pr);

    if (s_btns[b].kind == BK_MORE) {                // next weapon batch, no combat cost
        s_wpnPage++;
        beeper::tone(2000, 30);
        push();
        return true;
    }

    if (s_btns[b].kind == BK_FLEE) {
        bool sp = g_world.combat().setpiece;
        g_world.fightFlee();
        beeper::tone(1800, 80);
        if (sp) { s_active = false; setpiece_modal::onFightResult(false); }
        else    closeToWorld();
        return true;
    }

    uint8_t st = FIGHT_NOOP;
    switch (s_btns[b].kind) {
        case BK_WEAPON: st = g_world.fightAttack(g_game, s_btns[b].wslot); break;
        case BK_EAT:    st = g_world.fightEat();  break;
        case BK_MEDS:   st = g_world.fightMeds(); break;
        case BK_HYPO:   st = g_world.fightHypo(); break;
        case BK_STIM:   st = g_world.fightStim(); break;
        case BK_SHIELD: st = g_world.fightShield(); break;
    }

    if (st == FIGHT_LOST) {                          // only the stim's self-harm
        s_active = false;
        if (g_world.combat().setpiece) setpiece_modal::abort();
        world_page::enterDeath();
        Serial.println("[fight] stim self-damage killed the wanderer");
        return true;
    }
    if (st == FIGHT_WON) {
        beeper::tone(1568, 120);                 // victory chime
        if (g_world.combat().setpiece) {
            // Setpiece owns the victory: hand the win back (it reads the banked
            // loot, ends the combat state, and shows the scene's continue/run
            // buttons) instead of fight_modal drawing its own loot panel.
            s_active = false;
            setpiece_modal::onFightResult(true);
        } else {
            push();                          // draw the loot panel
            s_victoryMs = millis();
        }
        return true;
    }
    if (st == FIGHT_NOOP) {                          // cooling / no ammo / full hp
        beeper::tone(600, 120);
        push();                          // clean frame back over the flash
        return true;
    }
    beeper::tone(1200, 30);                       // a landed swing / heal — light click
    push();
    return true;
}

void tick(uint32_t nowMs) {
    if (!s_active) return;
    // Clock-backwards guard (same time-base constraint as setpiece_modal::
    // checkTimeout): raise()/begin() stamp s_lastTickMs + s_lastMs with a FRESH
    // millis() from inside handleTouch, then begin() blocks ~300ms — so on the frame
    // the fight opens, s_lastTickMs sits AHEAD of the loop-top `now` snapshot main.cpp
    // passes here. An unsigned nowMs - s_lastTickMs then underflows past the 1s
    // throttle and runs an extra fightTick a beat early. Treat a backwards reading as
    // "throttle not yet elapsed" and sit this pass out (next pass `now` is ahead).
    if (nowMs < s_lastTickMs) return;
    if (s_lastTickMs != 0 && nowMs - s_lastTickMs < 1000) return;
    s_lastTickMs = nowMs;

    if (g_world.fightWon()) {
        // Same guard for the victory-panel idle clock: a fresh-millis s_lastMs (set at
        // the winning press) ahead of nowMs would underflow >= TIMEOUT_MS and dismiss
        // the loot panel the instant the kill lands.
        if (nowMs >= s_lastMs && nowMs - s_lastMs >= TIMEOUT_MS) {   // forgotten victory panel -> leave
            Serial.println("[fight] victory idle -> auto-dismiss");
            g_world.fightEndVictory();
            closeToWorld();
        }
        return;                                      // no combat clock during victory
    }

    bool sp = g_world.combat().setpiece;
    uint8_t st = g_world.fightTick(g_game);
    if (st == FIGHT_LOST) {
        s_active = false;                            // release the guard first
        if (sp) setpiece_modal::abort();             // tear down the setpiece too
        world_page::enterDeath();                    // shared World death frame
        Serial.println("[fight] player died -> death frame");
        return;
    }
    // A scene `explosion` defers the kill: the enemy hit 0 HP, wound up for
    // EXPLOSION_TICKS, and the win only lands here once the blast is survived.
    if (st == FIGHT_WON) {
        beeper::tone(1568, 120);
        if (sp) { s_active = false; setpiece_modal::onFightResult(true); }
        else    { push(); s_victoryMs = millis(); }
        return;
    }
    push();                                // HP bars + cooldown bars drained
}

void endForSleep() {
    if (!s_active) return;
    s_active = false;    // flee semantics: combat is RAM-only and un-saved mid-fight,
                         // so a cold boot resumes the pre-fight tile (decision 7).
    Serial.println("[fight] forced sleep -> flee");
}

}  // namespace fight_modal
