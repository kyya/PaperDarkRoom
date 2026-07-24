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
// 24px cells (12px glyph x2), an ODD 19x33 window so the player sits exactly at
// the centre cell. 19 cols x 24 = 456px, centred (x 42..498, inside the 24px
// tap-safe margin); 33 rows x 24 = 792px from y 124, ending 916 < 928 (status
// bar). CENTER_COL/ROW is the wanderer's fixed on-screen cell.
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

// Per-step partial-refresh band: the HUD rows + the whole map (title unchanged).
constexpr int REPAINT_TOP = HUD_A_Y - 4;                // 64

static inline int iabs(int v) { return v < 0 ? -v : v; }

// RTC -> Unix epoch (mirrors path_page/room_page). Stamps g_game.deathAt at a
// death so the post-death embark lockout (§3.4) is measured on the same clock as
// settle()/embark and survives deep sleep.
uint32_t epochNow() {
    m5::rtc_date_t d; m5::rtc_time_t t;
    M5.Rtc.getDateTime(&d, &t);
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

    char ckey[40];
    if (shipCompassKey(ckey, sizeof ckey))
        cjk::drawText(c, PAD, HUD_B_Y, tr(ckey), SCALE);              // 罗盘指向X (left)

    const char* msg = hudMessage(landmarkKey);
    if (msg) {
        int w = cjk::textWidth(msg, SCALE);
        cjk::drawText(c, 540 - PAD - w, HUD_B_Y, msg, SCALE);         // message (right)
    }
}

// The player-centred viewport. Off-map, fogged (unrevealed), and T_VOID cells are
// left blank (upstream fog §2.5); the wanderer's own cell is '@' over its tile.
void drawMap(m5gfx::M5Canvas& c) {
    int px = g_world.ex.x, py = g_world.ex.y;
    for (int vr = 0; vr < ROWS; vr++) {
        int my = py - CENTER_ROW + vr;
        int cellY = MAP_Y0 + vr * CELL;
        for (int vc = 0; vc < COLS; vc++) {
            int mx = px - CENTER_COL + vc;
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
    // Clear the HUD+map band: harmless after draw()'s fillSprite, and required on
    // the per-step partial-repaint path (the title + surrounding pixels stay).
    c.fillRect(0, REPAINT_TOP, 540, MAP_Y1 - REPAINT_TOP, TFT_WHITE);
    drawHud(c, m_msgKey);
    drawMap(c);
}

// Press -> N/S/E/W by the dominant axis of the offset from the centred player.
uint8_t WorldPage::resolveDir(int x, int y) const {
    int cx = MAP_X0 + CENTER_COL * CELL + CELL / 2;
    int cy = MAP_Y0 + CENTER_ROW * CELL + CELL / 2;
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
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::ringIndexByName("outside"), false);
        return;
    }
    if (!g_world.ex.active) { M5.Speaker.tone(600, 120); return; }   // defensive

    StepResult r = g_world.move(g_game, resolveDir(x, y));
    switch (r.kind) {
        case STEP_HOME:
            // move()->goHome() committed the map, banked the bag into g_game, and
            // unlocked cleared mines, then cleared the trek — but did NOT save
            // g_game. Persist it, then return to the village (World now hides).
            g_game.save();
            if (path_page::isOpen()) path_page::close();   // defensive latch cleanup
            M5.Speaker.tone(1800, 80);
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
            M5.Speaker.tone(300, 240);                     // somber
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
            M5.Speaker.tone(600, 120);
            return;
    }
    // A plain step: repaint the HUD + map under FASTEST — no press-flash, no
    // per-step beep (the map's own redraw is the feedback). The driver's pixel
    // diff flips only the cells that actually changed.
    drawMapAndHud(canvas);
    pager::partialRefresh(pages::Rect{ 0, REPAINT_TOP, 540, MAP_Y1 - REPAINT_TOP },
                          pages::RefreshMode::FASTEST);
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
    M5.Speaker.tone(300, 240);                     // somber, matching STEP_DIED
    pager::showPage(pager::currentRingIndex(), false);
}
}  // namespace world_page
