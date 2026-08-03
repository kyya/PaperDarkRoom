// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// World (荒芜世界) map-exploration page — see world_page.h for the role/layout
// model. A thin renderer + input mapper over the world_state.h engine: it draws
// a player-centred viewport of the working map plus a survival HUD, and turns a
// press into a single move() step. All map/upkeep/goHome/die/persistence logic is
// the engine's; this page never mutates g_world except through move(). Every
// string routes through tr() (strings_zh.h, §8.3 glyph closure); tile glyphs are
// baked ASCII (0x20..0x7E), not tr() text.
#include "world_page.h"
#include "cjk_text.h"
#include "pomo_page.h"          // PAD (shared layout authority)
#include "page_tabs.h"          // resetHitCache — tab-less page clears the header cache
#include "pager.h"
#include "path_page.h"          // close the Path latch on goHome (defensive)
#include "fight_modal.h"        // combat overlay (STEP_FIGHT starts it)
#include "setpiece_modal.h"     // landmark setpiece overlay (STEP_LANDMARK starts it)
#include "game_state.h"
#include "world_state.h"
#include "beeper.h"
#include "rtc_bm8563.h"
#include <M5Unified.h>
#include <stdio.h>
#include <time.h>

// main.cpp owns the models and the full-screen sprite.
extern adr::GameState  g_game;
extern adr::WorldState g_world;
extern M5Canvas canvas;

using namespace adr;

namespace world_page {
// Death-frame latch (RAM-only) — a namespace static so the fight overlay can raise
// it via enterDeath() after a combat death, not just the World page's own move
// loop. Mirrors path_page's s_active latch model.
static bool s_death = false;
bool inDeath() { return s_death; }
}  // namespace world_page

namespace {
constexpr int SCALE       = 2;                 // 12px grid x2 = 24px CJK body
constexpr int GLYPH       = 12 * SCALE;        // 24px line box
constexpr int TITLE_SCALE = 3;                 // 12px grid x3 = 36px title
constexpr int TITLE_Y     = 16;                // title ink 16..52 (page_tabs rhythm)

// ---- HUD rows (below the title) -------------------------------------------
constexpr int HUD_A_Y = 68;    // 水 N/M · 熏肉 xK · 生命 N/M
constexpr int HUD_B_Y = 96;    // 罗盘指向X · message slot

// ---- viewport geometry ----------------------------------------------------
// 24px cells (12px glyph x2), an ODD 19x33 window. 19 cols x 24 = 456px, centred
// (x 42..498, inside the 24px tap-safe margin); 33 rows x 24 = 792px from y 124,
// ending 916 < 928 (status bar). CENTER_COL/ROW is where a recenter re-parks the
// wanderer — NOT its fixed cell every step: the camera holds still between
// recenters (updateCamera) so a plain step only moves the '@' within an otherwise
// static frame, keeping the view visually calm. See m_camX/m_camY in world_page.h.
constexpr int CELL       = 24;
constexpr int COLS       = 19;
constexpr int ROWS       = 33;
constexpr int MAP_W      = COLS * CELL;                 // 456
constexpr int MAP_H      = ROWS * CELL;                 // 792
constexpr int MAP_X0     = (540 - MAP_W) / 2;           // 42
constexpr int MAP_Y0     = 124;
constexpr int MAP_Y1     = MAP_Y0 + MAP_H;              // 916
constexpr int CENTER_COL = COLS / 2;                    // 9
constexpr int CENTER_ROW = ROWS / 2;                    // 16

// Top of the HUD+map band that drawMapAndHud clears before repainting (title
// sits above it and is left alone).
constexpr int REPAINT_TOP = HUD_A_Y - 4;                // 64

// Look-ahead margin (cells) the wanderer must reach before the camera recenters.
// Between recenters the viewport is frozen, so a step only moves the '@' glyph and
// the terrain around it holds still. Smaller = fewer recenters (fewer viewport
// jumps) but less terrain visible ahead; 4 keeps 4 cells of look-ahead in the
// travel direction and still recenters only every ~5 (a horizontal run,
// CENTER_COL 9 - margin 4) / ~12 (vertical, CENTER_ROW 16 - 4) straight steps.
// MUST stay < CENTER_COL and < CENTER_ROW so a fresh recenter (which parks the
// player at CENTER) doesn't immediately re-trip the margin.
constexpr int RECENTER_MARGIN = 4;

static inline int iabs(int v) { return v < 0 ? -v : v; }

// RTC -> Unix epoch (mirrors path_page/room_page). Stamps g_game.deathAt at a
// death so the post-death embark lockout (§3.4) is measured on the same clock as
// settle()/embark and survives deep sleep.
uint32_t epochNow() {
    rtc::Date d; rtc::Time t;
    rtc::getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}

// world_data LANDMARKS label (tr() key) for a landmark tile, or nullptr. Read-only
// use of the engine tables (no modification).
const char* landmarkLabel(uint8_t tile) {
    for (int i = 0; i < LANDMARK_ROWS; i++)
        if (LANDMARKS[i].tile == tile) return LANDMARKS[i].label;
    return nullptr;
}

// world.js compassDir: build the "the compass points <dir>" tr() key pointing
// from the wanderer toward the crashed starship (found by scanning the working
// map for the single T_SHIP). Primary axis by the |dx|/2 vs |dy| heuristic
// (§2.7); the diagonal concatenates north/south then east/west, matching the 8
// translated keys. false when no ship is on the map (never in a generated map).
bool shipCompassKey(char* out, size_t cap) {
    int sx = -1, sy = -1;
    for (int y = 0; y < WORLD_DIM && sx < 0; y++)
        for (int x = 0; x < WORLD_DIM; x++)
            if (g_world.exTileAt(x, y) == T_SHIP) { sx = x; sy = y; break; }
    if (sx < 0) return false;
    int dx = sx - g_world.ex.x, dy = sy - g_world.ex.y;
    int a = iabs(dx), b = iabs(dy);
    const char* dir;
    char diag[16];
    if (b / 2 > a)      dir = dy < 0 ? "north" : "south";
    else if (a / 2 > b) dir = dx < 0 ? "west"  : "east";
    else {
        snprintf(diag, sizeof diag, "%s%s",
                 dy < 0 ? "north" : "south", dx < 0 ? "west" : "east");
        dir = diag;
    }
    snprintf(out, cap, "the compass points %s", dir);
    return true;
}

// The HUD message: the PERSISTING survival latches (starving/thirsty, upstream
// keys) win over msgKey — WorldPage::m_msgKey, itself either a one-shot
// StepResult.notice (meat/water-out, danger crossing, terrain narration) or the
// landmark-name hint (2.4). Returns a tr()'d string or nullptr.
const char* hudMessage(const char* msgKey) {
    if (g_world.ex.starving) return tr("starvation sets in");
    if (g_world.ex.thirsty)  return tr("the thirst becomes unbearable");
    if (msgKey)               return tr(msgKey);
    return nullptr;
}

// One 24px tile/player glyph, centred in its cell.
void drawGlyph(m5gfx::M5Canvas& c, int cellX, int cellY, char ch) {
    char s[2] = { ch, 0 };
    int gw = cjk::textWidth(s, SCALE);
    cjk::drawText(c, cellX + (CELL - gw) / 2, cellY, s, SCALE);
}

// HUD line A (water / cured meat / hp) + line B (compass · message).
void drawHud(m5gfx::M5Canvas& c, const char* landmarkKey) {
    const Expedition& ex = g_world.ex;

    char wbuf[32];
    snprintf(wbuf, sizeof wbuf, "%s %d/%d", tr("water"), ex.water, ex.maxWater);
    cjk::drawText(c, PAD, HUD_A_Y, wbuf, SCALE);                       // 水 N/M (left)

    char hbuf[32];
    snprintf(hbuf, sizeof hbuf, "%s %d/%d", tr("hp"), ex.hp, ex.maxHp);
    int hw = cjk::textWidth(hbuf, SCALE);
    cjk::drawText(c, 540 - PAD - hw, HUD_A_Y, hbuf, SCALE);            // 生命 N/M (right)

    char mbuf[32];
    snprintf(mbuf, sizeof mbuf, "%s x%d", tr("cured meat"),
             (int)ex.outfitRes[R_CURED_MEAT]);
    int mw = cjk::textWidth(mbuf, SCALE);
    cjk::drawText(c, (540 - mw) / 2, HUD_A_Y, mbuf, SCALE);            // 熏肉 xK (centre)

    // Compass (left) and message (right) share HUD line B. A step notice can be a
    // full terrain-narration sentence (world_state.cpp TERRAIN_CHANGE / the danger
    // warning) far longer than the landmark-name hints this row was sized for, so
    // it can reach back past the compass's own text and paint over it. The message
    // is the transient, higher-priority one (matches hudMessage's own starving/
    // thirsty-beats-msgKey ordering above) and the compass recomputes fresh next
    // frame regardless, so on a width collision the compass just sits this one
    // frame out instead of the two overlapping.
    char ckey[40];
    bool showCompass = shipCompassKey(ckey, sizeof ckey);

    const char* msg = hudMessage(landmarkKey);
    int msgW = msg ? cjk::textWidth(msg, SCALE) : 0;
    if (showCompass && msg) {
        int compassW = cjk::textWidth(tr(ckey), SCALE);
        if (compassW + msgW >= (540 - 2 * PAD)) showCompass = false;
    }

    if (showCompass) cjk::drawText(c, PAD, HUD_B_Y, tr(ckey), SCALE);  // 罗盘指向X (left)
    if (msg) cjk::drawText(c, 540 - PAD - msgW, HUD_B_Y, msg, SCALE);  // message (right)
}

// The viewport anchored at camera origin (camX,camY = world coord of the top-left
// visible cell). Off-map, fogged (unrevealed), and T_VOID cells are left blank
// (upstream fog §2.5); the wanderer's own cell is '@' over its tile. The camera is
// held still between recenters (updateCamera), so most steps only shift the '@'
// within a static frame — the two-cell EPD diff the flash fix relies on.
void drawMap(m5gfx::M5Canvas& c, int camX, int camY) {
    int px = g_world.ex.x, py = g_world.ex.y;
    for (int vr = 0; vr < ROWS; vr++) {
        int my = camY + vr;
        int cellY = MAP_Y0 + vr * CELL;
        for (int vc = 0; vc < COLS; vc++) {
            int mx = camX + vc;
            int cellX = MAP_X0 + vc * CELL;
            char ch;
            if (mx == px && my == py) {
                ch = PLAYER_CHAR;                                     // '@'
            } else if (mx < 0 || mx >= WORLD_DIM || my < 0 || my >= WORLD_DIM) {
                continue;                                            // off-map: blank
            } else if (!g_world.exRevealed(mx, my)) {
                continue;                                            // fog: blank
            } else {
                ch = TILE_CHAR[g_world.exTileAt(mx, my)];
                if (ch == ' ') continue;                             // T_VOID: blank
            }
            drawGlyph(c, cellX, cellY, ch);
        }
    }
}

// The death frame (STEP_DIED): a simple full-screen "the world fades" over a
// cleared panel, with a small 返回 press hint. The expedition was already dropped
// by die(); a press returns to the village.
void drawDeathFrame(m5gfx::M5Canvas& c) {
    const char* d = tr("the world fades");
    int dw = cjk::textWidth(d, TITLE_SCALE);
    cjk::drawText(c, (540 - dw) / 2, 430, d, TITLE_SCALE);

    const char* hint = tr("go home");            // "返回" — press affordance
    int hw = cjk::textWidth(hint, SCALE);
    cjk::drawText(c, (540 - hw) / 2, 430 + 12 * TITLE_SCALE + GLYPH, hint, SCALE);
}
}  // namespace

// ================================ Page API =================================

const pages::Region* WorldPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Shown while an expedition is live (trek active — the embark/cold-boot-resume
// state) OR while the death frame is up. After goHome/die the trek is gone and
// this returns false, so showPageOrNext skips the slot (World auto-hides), exactly
// like the un-opened AssignPage/Path. This is what the cold-boot restore chain
// relies on: g_world.restore() re-arms ex.active from trek.bin, so a device that
// slept on "world" re-resolves to a drawable World page.
bool WorldPage::available() const {
    return g_world.ex.active || world_page::s_death;
}

bool WorldPage::draw(m5gfx::M5Canvas& c) {
    if (!available()) return false;
    c.fillSprite(TFT_WHITE);
    page_tabs::resetHitCache();      // tab-less page: drop any prior page's tab spans
    if (g_world.ex.active) {
        world_page::s_death = false;  // a live trip supersedes a stale death latch
        cjk::drawText(c, PAD, TITLE_Y, tr("A Barren World"), TITLE_SCALE);
        drawMapAndHud(c);
        m_regions[0] = { (uint16_t)MAP_Y0, (uint16_t)MAP_Y1, 1, PARAM_MAP };
        m_regionCount = 1;
    } else {                         // available() guaranteed s_death
        drawDeathFrame(c);
        m_regions[0] = { 0, 928, 1, PARAM_DEATH };   // any press dismisses
        m_regionCount = 1;
    }
    return true;
}

void WorldPage::drawMapAndHud(m5gfx::M5Canvas& c) const {
    updateCamera();   // freeze / recenter the viewport BEFORE painting this frame
    // Clear the HUD+map band. Redundant after draw()'s own fillSprite — draw() is
    // this function's only caller now that onLocalAction repaints the whole page
    // instead of this band alone — but left as a harmless belt-and-braces clear.
    c.fillRect(0, REPAINT_TOP, 540, MAP_Y1 - REPAINT_TOP, TFT_WHITE);
    drawHud(c, m_msgKey);
    drawMap(c, m_camX, m_camY);
}

// Freeze the viewport between recenters. On an ordinary step the wanderer stays
// inside the look-ahead margin so the camera doesn't move — drawMap then repaints
// an unchanged frame except the '@' that shifted one cell, which keeps the map
// visually calm (the terrain doesn't reshuffle underfoot) between recenters, even
// though every step now costs the same full-panel redraw regardless (see
// onLocalAction). The camera recenters (parks the player back at CENTER) only
// when it reaches the margin, or when m_camInit is false / the player is off the
// current window (embark, resume, goHome-and-re-embark, the death-frame return).
void WorldPage::updateCamera() const {
    int px = g_world.ex.x, py = g_world.ex.y;
    int col = px - m_camX, row = py - m_camY;    // player's current on-screen cell
    if (!m_camInit ||
        col < RECENTER_MARGIN || col >= COLS - RECENTER_MARGIN ||
        row < RECENTER_MARGIN || row >= ROWS - RECENTER_MARGIN) {
        m_camX = (int16_t)(px - CENTER_COL);
        m_camY = (int16_t)(py - CENTER_ROW);
        m_camInit = true;
    }
}

// Press -> N/S/E/W by the dominant axis of the offset from the player's ACTUAL
// on-screen cell. With the freeze/recenter camera the wanderer is no longer pinned
// to CENTER, so the reference point is its live camera-relative cell (px-camX,
// py-camY) — computed against the frame currently on the panel (resolveDir runs at
// tap time, before move() and the next draw), so a tap above the '@' is always
// north, below is south, wherever the '@' happens to sit this frame.
uint8_t WorldPage::resolveDir(int x, int y) const {
    int pcol = g_world.ex.x - m_camX, prow = g_world.ex.y - m_camY;
    int cx = MAP_X0 + pcol * CELL + CELL / 2;
    int cy = MAP_Y0 + prow * CELL + CELL / 2;
    int dx = x - cx, dy = y - cy;
    if (iabs(dx) >= iabs(dy)) return dx < 0 ? DIR_WEST : DIR_EAST;
    return dy < 0 ? DIR_NORTH : DIR_SOUTH;
}

// Never flash: a step's own map redraw is the feedback (§task 2 — movement is
// high-frequency, a press-flash only slows it), and death-dismiss navigates away.
pages::Rect WorldPage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)rg; (void)x; (void)y;
    return pages::Rect{ 0, 0, 0, 0 };
}

// A press in the map region is one step (or, in the death frame, a dismiss).
void WorldPage::onLocalAction(uint8_t param, int x, int y) {
    if (param == PARAM_DEATH || world_page::s_death) {
        // die() already discarded the trip + emptied the bag. Drop the frame and
        // return to the village (the Path latch was already closed at embark).
        world_page::s_death = false;
        m_msgKey = nullptr;
        beeper::tone(1800, 80);
        pager::showPage(pager::ringIndexByName("outside"), false);
        return;
    }
    if (!g_world.ex.active) { beeper::tone(600, 120); return; }   // defensive

    StepResult r = g_world.move(g_game, resolveDir(x, y));
    switch (r.kind) {
        case STEP_HOME:
            // move()->goHome() committed the map, banked the bag into g_game, and
            // unlocked cleared mines, then cleared the trek — but did NOT save
            // g_game. Persist it, then return to the village (World now hides).
            g_game.save();
            if (path_page::isOpen()) path_page::close();   // defensive latch cleanup
            beeper::tone(1800, 80);
            pager::showPage(pager::ringIndexByName("outside"), false);
            return;
        case STEP_DIED:
            // die() already dropped the trip. Stamp the death epoch (arms the
            // post-death embark lockout, §3.4) + persist it, raise the death
            // frame; a press dismisses it to the village.
            g_game.deathAt = epochNow();
            g_game.save();
            world_page::s_death = true;
            m_msgKey = nullptr;
            beeper::tone(300, 240);                     // somber
            pager::showPage(pager::currentRingIndex(), false);  // paints the frame
            return;
        case STEP_LANDMARK:
            // 2.4: open the landmark's setpiece (r.scene is its SetpieceId). The
            // overlay owns the screen until a leave / flee / clear / death. If the
            // landmark has no Phase-2 table (executioner = Phase 3), begin() is
            // inert and we fall back to r.notice (meat/water/danger, §3.1/§3.3) or,
            // failing that, the landmark's own name in the HUD hint slot.
            m_msgKey = nullptr;
            setpiece_modal::begin(r.scene, millis());
            if (setpiece_modal::active()) return;
            m_msgKey = r.notice
                     ? r.notice
                     : landmarkLabel(g_world.exTileAt(g_world.ex.x, g_world.ex.y));
            break;
        case STEP_FIGHT:
            // 2.3: a random encounter triggered. r.scene is the EncounterId; open
            // the combat overlay (fight_modal owns the screen until win/flee/death).
            // beginFight + show co-locate in fight_modal::begin. No map repaint here
            // — the full-screen overlay covers it; closing it repaints the World map
            // at the new tile. Any r.notice this step is superseded by the overlay
            // (matches upstream's instant-open combat covering any toast underneath).
            m_msgKey = nullptr;
            fight_modal::begin(r.scene, millis());
            return;
        case STEP_MOVED:
            // r.notice: meat/water just ran out, a danger-zone crossing, or a
            // terrain-change narration (§3.1/§3.3/§7.3) — nullptr on a quiet step.
            m_msgKey = r.notice;
            break;
        default:  // STEP_BLOCKED — no active expedition (shouldn't reach here)
            beeper::tone(600, 120);
            return;
    }
    // A plain step: whole-page redraw, no press-flash, no per-step beep (the map's
    // own redraw is the feedback). The panel driver free-runs and re-renders the
    // entire 540x960 canvas in ~8ms out of the measured ~23ms scan period, so the
    // pixel EPD diff that used to make a frozen-camera step cheap (927b072) no
    // longer has anything to save — every step costs the same full render whether
    // the '@' moved one cell or the camera just recentred across the whole map.
    pager::showPage(pager::currentRingIndex(), false);
}

// Raise the shared death frame from the fight overlay (research decision 4). die()
// has already run in fight_modal (dropped the trip + emptied the bag), so this just
// latches the frame and repaints the World page under it — the fight overlay clears
// its own active() guard before calling this so pager::showPage can draw.
namespace world_page {
void enterDeath() {
    g_game.deathAt = epochNow();                   // arm the post-death embark lockout (§3.4)
    g_game.save();
    s_death = true;
    beeper::tone(300, 240);                     // somber, matching STEP_DIED
    pager::showPage(pager::currentRingIndex(), false);
}
}  // namespace world_page
