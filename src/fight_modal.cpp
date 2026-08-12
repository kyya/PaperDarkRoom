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
// in the firmware — then 吃肉 / 服药 when carried, then 跑 to flee). A victory
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
#include "enemy_art_data.h"     // ENEMY_ART[] / ENEMY_ART_GLYPH[] — ONE includer
#include "art_blit.h"           // art::blit — shared RLE/4bpp unpacker
#include "page_layout.h"        // PAD (shared layout authority)
#include "status_bar.h"
#include "pager.h"
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
// v0.20: the 48px ASCII glyph became a full-width 492x276 plate
// (enemy_art_data.h) — the same spec as the event illustrations, flush with PAD
// across the whole CONTENT_W column so it lines up with the bars and the button
// columns under it.
//
// The plate is ELASTIC. The attack grid is bottom-anchored and its row count
// follows the weapons the expedition packs, so the room above it is not a
// constant: 2 rows leave 742, a convoy hauling all 8 weapons (11 buttons, 6
// rows) leaves only 382. Sizing every plate for that worst case would spend the
// common fight's picture on an outlier, so instead the stored plate is CROPPED
// vertically at draw time (art::blit srcY0/rows, centred) to whatever is left —
// the full 276 in an ordinary fight, letterboxing toward ART_H_MIN when the
// player is carrying an armoury.
//
// The height is resolved ONCE in raise() and frozen for the fight (s_artH).
// Recomputing it per repaint would make the whole panel jump the moment an
// action consumes the last meat and drops a button row — the same rebuild that
// already had to be defended against in handleHold's debounce.
// ---- v0.20 layout: two名牌 + a speech box, after the Pokémon battle screen ----
// The previous panel stacked enemy bar, encounter line, player bar and supplies
// as four equal strips, so the two HP bars were identical objects and only
// position said which was whose. A battle screen wants that read instantly, and
// the genre's answer is a framed CARD per side, set on opposite diagonals:
//
//   +-----------------------------------------+
//   | [enemy card]                            |   left, under the plate's top
//   |            . . . plate . . .            |
//   | +-------------------------------------+ |
//   | | encounter line (speech box)         | |
//   | +-------------------------------------+ |
//   |                          [player card]  |   right — the diagonal
//   |  cmd | cmd | cmd                        |
//   +-----------------------------------------+
//
// The frames cost real height (each card spends CARD_PAD twice), and that comes
// out of the plate's elastic budget — deliberately: a fight where you cannot
// tell your own HP from the enemy's at a glance is worse than one with a
// slightly shorter picture.
// ---- enemy header: title, bar, rule — the event modal's own opening ---------
// event_modal reads title (36px) / rule / illustration, and a fight is the other
// interruptive full-screen scene in the game, so it opens the same way. The bar
// slots between title and rule because it belongs to the enemy being named, not
// to the picture below the line.
constexpr int TITLE_Y     = 20;                // 36px enemy name -> [20, 56]
constexpr int SCALE_TITLE = 3;
constexpr int EHP_Y       = 64;                // enemy HP bar under the name
constexpr int HP_BAR_H    = 20;
constexpr int RULE_Y      = 96;                // 2px separator, as in the modal
constexpr int ART_X       = PAD;               // plate 492xN, N = s_artH
constexpr int ART_Y_MIN   = 112;               // 14px of air under the rule
constexpr int ART_H_MIN   = 110;               // tightest crop, heaviest loadout
constexpr int ART_GAP     = 10;                // minimum air under the plate
constexpr int CHARA_SCALE = 4;                 // 48px glyph — no-art fallback only
constexpr int NOTIF_LINEH = 34;

// Cards. Both are drawn with action_band::drawFrame so a name plate, a speech
// box and a button all carry the SAME ring — the panel reads as one widget set.
// The player block: full width, flush with PAD like everything else on the
// panel. The right-aligned card was borrowed from the genre's diagonal, but that
// diagonal only reads when both halves are framed boxes floating over a battle
// background; with the frames gone (用户: 名牌不要边框) it was just a block of
// text mysteriously indented, fighting the left edge every other element shares.
constexpr int CARD_GAP    = 10;                // between the plate and the block
constexpr int PCARD_H     = GLYPH + 6 + HP_BAR_H + 6 + GLYPH;         // 80
// NO speech box. The genre's dialogue box narrates a turn ("X used Y", "it's
// super effective"), and ours had no such stream to carry: enemyNotifKey is the
// ENCOUNTER line, fixed for the whole fight, so the box spent 88px restating
// something the player read once. Upstream needs that line because its enemy is
// a single ASCII letter; ours is a portrait with a name card on it, which
// answers "what am I fighting" without a sentence. The height goes to the plate.
// (The death line still gets its say — renderVictory prints enemyDeathKey.)

// ---- player block ----
// Bottom-anchored to the attack grid rather than measured from the top: the
// plate above is elastic, and the player's own HP/supplies are what the thumb
// reads while fighting, so they stay parked directly over the buttons at a
// constant offset no matter how tall the picture ended up.
constexpr int PHP_BAR_H   = 24;
constexpr int PLAYER_GAP  = 20;                // air between supplies and buttons

// ---- button grid (2-col, bottom-anchored) ----
constexpr int BTN_H      = 80;                 // long-press band (§9.3 floor)
constexpr int BTN_GAP    = 10;
constexpr int BTN_BOTTOM = 912;                // bands stack UP; status bar [928,960]
constexpr int COL_GAP    = 12;
// Columns in the attack grid. Upstream's fight buttons are 120px wide and float
// horizontally (main.css div.button + .eventPanel .button), so eight weapons
// take two rows there; ours were 240px in two columns and took six, because
// every weapon verb is TWO 汉字 (戳刺/挥斩/劈砍/扎刺/开火/引爆/投掷/牵绊 = 72px)
// and a 240px cell spends 70% of itself on air. Narrower columns buy the height
// straight back — and since the plate above is sized by what the grid leaves,
// they buy it for the picture.
//   cols   cell   11 buttons
//     2    240px   6 rows, 530px
//     3    156px   4 rows, 350px
//     4    114px   3 rows, 260px   (82px usable vs a 72px label — still fits)
int s_cols = 3;                                // runtime for the "cols:" probe
int colW()          { return (CONTENT_W - (s_cols - 1) * COL_GAP) / s_cols; }
int colX(int col)   { return PAD + col * (colW() + COL_GAP); }
constexpr int VBTN_TOP   = BTN_BOTTOM - BTN_H; // victory 离开 band (832)

// Everything below the plate is derived from the frozen plate height, so the
// whole lower half of the panel slides with it. s_artH is set by raise().
int s_artH = ART_H_MIN;
// Slack is split evenly above and below the plate, so the picture keeps its
// stored proportion and sits optically centred in whatever room the grid left,
// instead of stretching to fill it.
int s_artY  = ART_Y_MIN;
int s_slack = 0;                                   // total leftover, both sides

int artTop()      { return s_artY; }
int artBottom()   { return s_artY + s_artH; }             // plate's last row + 1
int pCardY()      { return artBottom() + ART_GAP + (s_slack - s_slack / 2); }

// TWO dynamic bands, not one — the plate sits BETWEEN the things that change.
//
// The enemy bar is live (it drains as you swing) but now lives in the header,
// above the picture; the player block and the grid are live and live below it.
// A single band spanning both would enclose the plate, and render(false) would
// scrub it white on the first combat tick — after which only a full re-decode
// could bring it back, every second, for the whole fight.
//
// So repaintDynamic() clears and pushes exactly these two rects and leaves the
// rows between them alone.
pages::Rect dynTopBand() {
    return pages::Rect{ PAD, EHP_Y - 2, CONTENT_W, HP_BAR_H + 4 };
}
pages::Rect dynBottomBand() {
    const int top = artBottom();
    return pages::Rect{ 0, top, 540, 928 - top };
}

// The tightest the panel ever gets: a full 4-row grid at 3 columns (11 buttons).
// Everything below the plate — the player block and the gaps — has to fit
// between the plate's bottom and the grid.
constexpr int GRID_TOP_MIN = BTN_BOTTOM - 4 * BTN_H - 3 * BTN_GAP;   // 562
static_assert(ART_Y_MIN + ART_H_MIN + ART_GAP + PCARD_H + CARD_GAP <= GRID_TOP_MIN,
              "at ART_H_MIN the player block must still clear a full 4-row grid");
static_assert(ART_H_MIN <= ENEMY_ART_H, "crop cannot exceed the stored plate");

constexpr uint32_t TIMEOUT_MS      = 120u * 1000u;   // idle victory-panel dismiss
constexpr uint32_t VICTORY_GUARD_MS = 400u;          // ignore spam-through taps

// ---- overlay state (RAM-only) ----
bool     s_active     = false;
uint32_t s_lastMs     = 0;     // last interaction (idle-timeout clock)
uint32_t s_lastTickMs = 0;     // 1s combat-tick gate
uint32_t s_victoryMs  = 0;     // when the victory panel appeared (spam guard)
uint32_t s_lastPressMs = 0;    // press debounce (e-ink double-tap bounce, pager.cpp)
int      s_lastPressBtn = -1;  // band that press hit (-1 = missed every band) — the
                               // debounce is scoped to a repeat of THAT same target
int16_t  s_lastPressX = 0;     // ...and to the physical coordinates of that press, which
int16_t  s_lastPressY = 0;     // survive a button-grid rebuild (see handleHold)
constexpr int BOUNCE_R2 = 24 * 24;   // same-tap radius^2 (px^2, integer compare)

// ---- button model, rebuilt each render ----
enum : uint8_t { BK_WEAPON, BK_EAT, BK_MEDS, BK_FLEE };
struct FBtn {
    uint8_t kind;
    int     wslot;             // weapon slot (BK_WEAPON) else -1
    char    label[24];
    bool    enabled;
    int     coolLeft, coolTotal;
};
FBtn s_btns[16];
int  s_btnN = 0;

void buildButtons() {
    s_btnN = 0;
    const Combat& cx = g_world.combat();
    for (int s = 0; s < g_world.fightWeaponCount() && s_btnN < 16; s++) {
        FBtn& b = s_btns[s_btnN++];
        b.kind = BK_WEAPON; b.wslot = s;
        uint8_t wid = g_world.fightWeaponId(s);
        snprintf(b.label, sizeof b.label, "%s", tr(WEAPONS[wid].verb));
        b.enabled   = g_world.fightWeaponEnabled(s);
        b.coolLeft  = g_world.fightWeaponCoolLeft(s);
        b.coolTotal = WEAPONS[wid].cooldownS;
    }
    if (g_world.ex.outfitRes[R_CURED_MEAT] > 0 && s_btnN < 16) {
        FBtn& b = s_btns[s_btnN++];
        b.kind = BK_EAT; b.wslot = -1;
        snprintf(b.label, sizeof b.label, "%s", tr("eat meat"));
        b.enabled   = cx.eatCool == 0 && g_world.ex.hp < g_world.ex.maxHp;
        b.coolLeft  = cx.eatCool; b.coolTotal = FIGHT_EAT_COOLDOWN_S;
    }
    if (g_world.ex.outfitRes[R_MEDICINE] > 0 && s_btnN < 16) {
        FBtn& b = s_btns[s_btnN++];
        b.kind = BK_MEDS; b.wslot = -1;
        snprintf(b.label, sizeof b.label, "%s", tr("use meds"));
        b.enabled   = cx.medsCool == 0 && g_world.ex.hp < g_world.ex.maxHp;
        b.coolLeft  = cx.medsCool; b.coolTotal = FIGHT_MEDS_COOLDOWN_S;
    }
    if (s_btnN < 16) {                              // flee — always offered
        FBtn& b = s_btns[s_btnN++];
        b.kind = BK_FLEE; b.wslot = -1;
        snprintf(b.label, sizeof b.label, "%s", tr("run"));   // 跑
        b.enabled = true; b.coolLeft = 0; b.coolTotal = 0;
    }
}

int gridRows()      { return (s_btnN + s_cols - 1) / s_cols; }
int gridTop()       { int r = gridRows(); return BTN_BOTTOM - r * BTN_H - (r - 1) * BTN_GAP; }
int btnTopY(int i)  { return gridTop() + (i / s_cols) * (BTN_H + BTN_GAP); }
int btnX(int i)     { return colX(i % s_cols); }

int hitButton(int x, int y) {
    if (x < PAD || x >= 540 - PAD) return -1;
    int gt = gridTop();
    if (y < gt) return -1;
    int pitch = BTN_H + BTN_GAP;
    int row = (y - gt) / pitch;
    if ((y - gt) - row * pitch >= BTN_H) return -1;    // in the inter-row gap
    // Column by pitch, then reject the inter-column gap the same way the row
    // test rejects the inter-row one — with narrow cells a press landing in a
    // 12px gutter should miss, not silently belong to the cell on its left.
    int pitchX = colW() + COL_GAP;
    int col = (x - PAD) / pitchX;
    if (col >= s_cols) col = s_cols - 1;
    if ((x - PAD) - col * pitchX >= colW()) return -1;
    int i = row * s_cols + col;
    return (i >= 0 && i < s_btnN) ? i : -1;
}

// The rect of attack-grid band `i` — the ONE description of where a fight
// button is, shared by the draw call and handleHold's invert-flash.
pages::Rect btnRect(int i) {
    return pages::Rect{ btnX(i), btnTopY(i), colW(), BTN_H };
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

// This fight's portrait: a random encounter indexes ENEMY_ART by its EncounterId,
// a setpiece enemy carries no id (0xFF) and only a glyph, so it falls back to the
// generic plate for that glyph. nullptr = draw the old ASCII glyph instead.
const uint8_t* enemyArt(const Combat& cx) {
    if (cx.enemyId < ENCOUNTER_COUNT) return ENEMY_ART[cx.enemyId];
    for (const auto& g : ENEMY_ART_GLYPH)
        if (g.ch == cx.enemyChara) return g.art;
    return nullptr;
}

// `drawStatic` false = the portrait and name are already in the canvas from the
// entry push and must NOT be redrawn (render(false) left everything above
// DYN_TOP alone). Skipping it is what keeps the per-second repaint off the RLE
// decoder — see DYN_TOP.
// Resolve how tall this fight's plate may be and freeze it for the encounter.
//
// Measured downward from the grid: whatever the attack buttons and the player
// block do not claim is the picture's, capped at the stored plate height. Called
// once from raise() — buildButtons() must have run, and it does, because
// armWeapons() fills the weapon list before the overlay is raised.
//
// Frozen deliberately: an action that consumes the last meat drops a button and
// can shrink the grid by a whole row mid-fight, and re-resolving here would make
// the plate (and every band under it) jump at that moment.
void lockArtHeight() {
    buildButtons();
    const int btnTop = gridTop();
    // Everything the plate has to leave room for: air, the speech box, the gap,
    // the player card, and the gap above the grid. The enemy card is NOT here —
    // it overlaps the plate rather than following it.
    const int below = ART_GAP + PCARD_H + CARD_GAP;
    const int room = btnTop - ART_Y_MIN - below;
    // Keep the stored proportion whenever it fits; only a loadout heavy enough
    // to squeeze past it makes the plate crop.
    int h = (room > ENEMY_ART_H) ? ENEMY_ART_H : room;
    if (h < ART_H_MIN) h = ART_H_MIN;
    // Even height keeps the centred crop symmetric (and the 4bpp row arithmetic
    // boring), matching how the plate itself is packed.
    s_artH  = h & ~1;
    s_slack = room - s_artH;
    if (s_slack < 0) s_slack = 0;
    s_artY  = ART_Y_MIN + s_slack / 2;              // half above, half below
}

void renderFight(m5gfx::M5Canvas& c, bool drawStatic) {
    const Combat& cx = g_world.combat();
    if (drawStatic) {
        // Enemy name, 36px, where the event modal puts its title. A setpiece
        // enemy has no shown name (upstream shows only the glyph); the rule and
        // the bar still open the scene.
        if (cx.enemyNameKey)
            cjk::drawText(c, PAD, TITLE_Y, tr(cx.enemyNameKey), SCALE_TITLE);
        c.fillRect(PAD, RULE_Y, CONTENT_W, 2, TFT_BLACK);
        // The plate, cropped to the height this fight was given. This is the one
        // thing whose redraw costs real work (an RLE decode), which is why it is
        // gated on drawStatic and why the repaint bands are split around it. The
        // crop is centred, so a subject composed in the middle of the source
        // survives every grid size.
        const uint8_t* art = enemyArt(cx);
        if (art) {
            art::blit(c, art, ENEMY_ART_W, ENEMY_ART_H, ART_X, artTop(),
                      (ENEMY_ART_H - s_artH) / 2, s_artH);
        } else {                                  // unknown glyph: the old 48px char
            char ch[2] = { cx.enemyChara, 0 };
            int cw = cjk::textWidth(ch, CHARA_SCALE);
            cjk::drawText(c, ART_X + (ENEMY_ART_W - cw) / 2,
                          artTop() + (s_artH - 48) / 2, ch, CHARA_SCALE);
        }
    }
    // Enemy HP: header furniture, but LIVE — it is the top repaint band.
    drawHpBar(c, PAD, EHP_Y, CONTENT_W, HP_BAR_H, cx.enemyHp, cx.enemyMaxHp);

    // ---- player block: full width, flush left like everything else ----------
    {
        const int iy = pCardY();
        char hp[32];
        snprintf(hp, sizeof hp, "%s %d/%d", tr("hp"), g_world.ex.hp,
                 g_world.ex.maxHp);
        cjk::drawText(c, PAD, iy, hp, SCALE_BODY);
        drawHpBar(c, PAD, iy + GLYPH + 6, CONTENT_W, HP_BAR_H,
                  g_world.ex.hp, g_world.ex.maxHp);
        char sup[80];
        snprintf(sup, sizeof sup, "%s %d  %s x%d  %s x%d",
                 tr("water"), g_world.ex.water,
                 tr("cured meat"), (int)g_world.ex.outfitRes[R_CURED_MEAT],
                 tr("medicine"), (int)g_world.ex.outfitRes[R_MEDICINE]);
        cjk::drawText(c, PAD, iy + GLYPH + 6 + HP_BAR_H + 6, sup, SCALE_BODY);
    }

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

// `full` distinguishes the two callers. The entry/victory pushes pass true:
// clear everything and paint the enemy portrait + name as well. repaintDynamic()
// passes false, and then ONLY the band below the portrait is cleared and
// redrawn, so the decoded plate stays put in the canvas — the per-second combat
// tick never re-runs the RLE decoder, and the panel's per-pixel diff sees an
// identical header region and drives none of it.
//
// The victory panel replaces the whole body, so it always forces the full path.
void render(bool full) {
    const bool won = g_world.fightWon();
    if (full || won) {
        canvas.fillSprite(TFT_WHITE);
    } else {
        // Clear ONLY the two live bands, leaving the plate (and the title and
        // rule above it) exactly as the entry push decoded them.
        const pages::Rect t = dynTopBand(), b = dynBottomBand();
        canvas.fillRect(t.x, t.y, t.w, t.h, TFT_WHITE);
        canvas.fillRect(b.x, b.y, b.w, b.h, TFT_WHITE);
    }
    if (won) renderVictory(canvas);
    else     renderFight(canvas, full);
    status_bar::drawOnto(canvas);
}

// Repaint just the live bands (HP bars + cooldown bars) under FASTEST — the
// per-tick / per-landed-action path. The title, rule and plate stay from the
// entry push, which is why this pushes two rects around them rather than one
// spanning both: see dynTopBand/dynBottomBand.
void repaintDynamic() {
    render(false);
    pager::partialRefresh(dynTopBand(), pages::RefreshMode::FASTEST);
    pager::partialRefresh(dynBottomBand(), pages::RefreshMode::FASTEST);
}

// Full-panel QUALITY push — entry and the fight->victory panel change (a scene
// swap earns a deliberate grayscale clean, no ghost of the previous body).
void pushQuality() {
    render(true);
    auto& disp = M5.Display;
    disp.setEpdMode(epd_mode_t::epd_quality);
    canvas.pushSprite(0, 0);
    disp.setEpdMode(epd_mode_t::epd_fast);
}

// Release the guard and repaint the World page underneath (the current ring page
// is always the World page — combat only starts from a World move).
void closeToWorld() {
    s_active = false;
    pager::showPage(pager::currentRingIndex(), false);
}
}  // namespace

// ---- public ---------------------------------------------------------------

bool active() { return s_active; }

namespace {
// Shared entry: raise the panel over an already-armed g_world combat (begin/
// beginSetpiece differ only in HOW cx was armed).
void raise(uint32_t nowMs) {
    s_active      = true;
    s_lastMs      = nowMs;
    s_lastTickMs  = nowMs;
    s_victoryMs   = 0;
    s_lastPressMs = 0;         // don't inherit a stale press time from a prior fight
    s_lastPressBtn = -1;
    s_lastPressX  = 0;
    s_lastPressY  = 0;
    lockArtHeight();
    pushQuality();
    // encounter alert: a short falling two-note chime, distinct from the event
    // pop (1047->1568 rising) and the switcher tone (2000).
    M5.Speaker.tone(880, 80);
    delay(90);
    M5.Speaker.tone(660, 140);
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
    uint32_t nowMs = millis();
    s_lastMs = nowMs;

    if (g_world.fightWon()) {                       // victory panel: any press leaves
        if (millis() - s_victoryMs < VICTORY_GUARD_MS) return true;  // swallow spam-through
        g_world.fightEndVictory();
        M5.Speaker.tone(1800, 80);
        closeToWorld();
        return true;
    }

    int b = hitButton(x, y);

    // Per-target tap debounce (pager.cpp's e-ink quirk): one physical tap can
    // report TWO clicks ~<350ms apart — always at the SAME coordinates. The
    // COORDINATE test is what actually models that double-report: a landed action
    // can consume its last item (eat the last meat), so repaintDynamic() rebuilds
    // and bottom-anchors a SMALLER grid before the bounced touch arrives, and the
    // same physical point now resolves to a different index — an index-only test
    // would let the bounce fire a real attack. The index test is kept on top of it
    // so intentional same-button spam stays debounced even when a rebuild shifts
    // that button under the finger. Weapon cooldowns are >=1s so neither blocks
    // intentional combat; together they stop a bounce from low-beeping a spurious
    // cooldown reject right after each landed swing. A tap on a different band AND
    // a different spot is a second real intent and passes immediately.
    if (s_lastPressMs != 0 && nowMs - s_lastPressMs < 300) {
        int dx = x - s_lastPressX, dy = y - s_lastPressY;
        if (b == s_lastPressBtn || dx * dx + dy * dy <= BOUNCE_R2) return true;
    }
    s_lastPressMs = nowMs;
    s_lastPressBtn = b;
    s_lastPressX = (int16_t)x;
    s_lastPressY = (int16_t)y;

    if (b < 0) { M5.Speaker.tone(600, 120); return true; }   // missed every band

    // Press feedback: invert-flash the pressed band (event_modal parity). A repaint
    // paints over it; a rejected press rebounds the rect so the flash bounces off.
    pages::Rect pr = btnRect(b);
    pager::flashPressRect(pr);

    if (s_btns[b].kind == BK_FLEE) {
        bool sp = g_world.combat().setpiece;
        g_world.fightFlee();
        M5.Speaker.tone(1800, 80);
        if (sp) { s_active = false; setpiece_modal::onFightResult(false); }
        else    closeToWorld();
        return true;
    }

    uint8_t st = FIGHT_NOOP;
    switch (s_btns[b].kind) {
        case BK_WEAPON: st = g_world.fightAttack(g_game, s_btns[b].wslot); break;
        case BK_EAT:    st = g_world.fightEat();  break;
        case BK_MEDS:   st = g_world.fightMeds(); break;
    }

    if (st == FIGHT_WON) {
        M5.Speaker.tone(1568, 120);                 // victory chime
        if (g_world.combat().setpiece) {
            // Setpiece owns the victory: hand the win back (it reads the banked
            // loot, ends the combat state, and shows the scene's continue/run
            // buttons) instead of fight_modal drawing its own loot panel.
            s_active = false;
            setpiece_modal::onFightResult(true);
        } else {
            pushQuality();                          // draw the loot panel
            s_victoryMs = millis();
        }
        return true;
    }
    if (st == FIGHT_NOOP) {                          // cooling / no ammo / full hp
        M5.Speaker.tone(600, 120);
        pager::partialRefresh(pr, pages::RefreshMode::FASTEST);   // rebound the flash
        return true;
    }
    M5.Speaker.tone(1200, 30);                       // a landed swing / heal — light click
    repaintDynamic();
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
    uint8_t st = g_world.fightTick();
    if (st == FIGHT_LOST) {
        s_active = false;                            // release the guard first
        if (sp) setpiece_modal::abort();             // tear down the setpiece too
        world_page::enterDeath();                    // shared World death frame
        Serial.println("[fight] player died -> death frame");
        return;
    }
    repaintDynamic();                                // HP bars + cooldown bars drained
}

void setCols(int n) {
    s_cols = (n < 2) ? 2 : (n > 4 ? 4 : n);
    if (s_active) { lockArtHeight(); pushQuality(); }   // re-derive the plate too
}
int cols() { return s_cols; }

void endForSleep() {
    if (!s_active) return;
    s_active = false;    // flee semantics: combat is RAM-only and un-saved mid-fight,
                         // so a cold boot resumes the pre-fight tile (decision 7).
    Serial.println("[fight] forced sleep -> flee");
}

}  // namespace fight_modal
