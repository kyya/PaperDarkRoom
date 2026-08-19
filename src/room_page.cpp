// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Room (fire) page — the real Phase-1 Room UI, driven by the game_state engine
// (src/game_state.*). Every piece of text is routed through tr() (strings_zh.h)
// so only the official Simplified-Chinese translation ever reaches the sparse
// 12px CJK face — the §8.3 glyph-closure iron law. Layout follows the §9.4
// vertical budget (24px CJK, <=20 汉字/行, >=80px long-press bands, paginate
// rather than compress). See room_page.h for the region model.
#include "room_page.h"
#include "action_band.h"        // shared band renderer (v0.10.1, room+outside)
#include "cjk_text.h"
#include "page_layout.h"        // PAD (shared layout authority)
#include "page_tabs.h"          // shared two-tab header (生火间 │ 小型村落)
#include "pager.h"
#include "log_view.h"
#include "game_state.h"
#include <M5Unified.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// main.cpp owns both the game model and the full-screen sprite.
extern adr::GameState g_game;
extern M5Canvas canvas;

using namespace adr;

namespace {
// Log-stream type size. These two belong to the LOG AND NOTHING ELSE — every
// button, cost line and tab label sizes itself inside action_band / page_tabs —
// so the names carry the LOG_ prefix to keep it that way.
//
// v0.16 first answered "生火间的日志字太小" by bumping the face one notch,
// x2 -> x3. cjk_text.cpp renders the 12px bitmap face at an INTEGER scale (a
// per-pixel fillRect blit), so there is no 28px or 30px step in between — the
// next size up is 36px, and a 36px face fits only 6 entries in the band that
// held 9 at 24px. On device that read as strictly worse: the log is a scrolling
// history, and losing a third of it costs more than the legibility gained. So
// the face stays at x2/24px, and the missing rows are bought a different way —
// the button grid is BOTTOM-anchored and the log band grows into whatever
// vertical space the grid does not need (see the budget below). Early game,
// with one or two action rows, that is 20+ log lines instead of 9.
constexpr int LOG_SCALE = 2;                 // 12px grid x2 = 24px CJK (log stream)
constexpr int LOG_GLYPH = 12 * LOG_SCALE;    // 24px line box (log)
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)
constexpr int MAX_ROWS  = 5;                 // matches RoomPage::MAX_BANDS (rows)
constexpr int MAX_COLS  = 2;                 // two-column button grid
constexpr int MAX_SLOTS = MAX_ROWS * MAX_COLS;   // 10 action cells / page

// ---- vertical budget (§9.4): the two-tab header (page_tabs::TAB_H = 72px) owns
// the top band, so the log stream reflows BELOW it.
// v0.10.0 ("每个有消耗的按钮下方标明消耗"): every priced action band carries a
// small cost sub-row below its label — but the label+cost block was CENTERED in
// a height that only shrank for a coolable band (light/stoke), so light/stoke's
// title sat measurably higher than every other priced button, reading as a size
// mismatch ("添柴和火把按钮文字高度不一致").
// v0.10.1 ("align bottom" + "第一页按钮没对齐") answered that by making a FREE
// band lay out as if it carried a cost line, so a whole grid's titles landed on
// one y. v0.12 RETIRES that rule at the user's on-device call: a band now
// splits the axes instead: the title owns a LEFT column at one fixed y and the
// costs own a RIGHT column, so a cell's title no longer moves depending on
// whether it is priced — see action_band.h "LEFT TITLE / RIGHT COSTS".
// v0.10.1 ALSO shrank the label 36px -> 24px; v0.12 ("带 subtitle 的
// button 的按钮样式都统一成贸易站的按钮组件") REVERSES that half too:
// the Trade page's 36px-title-over-24px-cost band is now the app-wide button,
// and a Room cell renders through the same action_band::draw as every other
// button in the firmware rather than a Room-sized variant of it. The 96px band
// still needs its height for the RIGHT column: a 3-entry cost stack is 72px and
// clears the cooldown bar only because the band is 96 (the full derivation lives
// in action_band.cpp draw(); it is what pins this height at 96 rather than the
// §9.3 floor of 80).
// MAX_ROWS stays 5, but v0.16 flips which END of the grid is fixed. Up to
// v0.15 the grid grew DOWNWARD from a fixed BTN_TOP=366, so an early-game page
// with one action row put a lone button under the log and left ~420px of white
// between it and the status bar, while the log sat capped at 9 lines two thirds
// of a screen above the empty space it could have used. Now BTN_AREA_BOTTOM is
// the anchor and the grid hangs UP from it (btnTopForRows below): the buttons
// always sit where the thumb already is, the amount of chrome that moves when a
// craftable unlocks is one row rather than the whole page, and the log band
// simply ends 20px above whatever row the grid starts on (logBandBottom /
// logLines). A full 5-row grid reproduces the old layout to the pixel — top
// 366, band 76..346, 9 lines — so the "grid is full" case is not a new design,
// it is the same one, reached from the other side.
// Resource/inventory summary lives on the Outside page's lower band.
// v0.3.1 feedback 1 ("火堆熊熊燃烧 房间很热不应该常驻 原作也没有常驻"): the
// persistent fire/temp state line that used to open this band (STATE_Y, 76..
// 116) is gone — upstream never shows it as a fixed header either, only as a
// notification on change (see game_state.cpp onFireChange/adjustTemp, which
// now push "the fire is {0}" / "the room is {0}" into the log on every
// change). The log reclaims that band: it moves up to LOG_TOP=76. ------------
constexpr int LOG_TOP   = page_tabs::CONTENT_TOP + 4;   // log stream top (76)
constexpr int LOG_LINEH = LOG_GLYPH + LOG_GLYPH / 4;    // 30 = 24px glyph + 6px
                                             // leading (the 1.25 advance ratio
                                             // this face has always used)
constexpr int LOG_GAP   = 20;                // clear space between the log band's
                                             // last line and the first button row
// Log fade — the e-ink reproduction of upstream's notifyGradient. A Dark Room
// runs a CSS gradient down its notification column so older messages dim toward
// nothing; the 16-grey panel does the same with flat grey tiers (the bitmap face
// draws solid, unantialiased strokes, so a tier is exactly the tone asked for).
// Greys are explicit RGB565 rather than TFT_DARKGREY / TFT_LIGHTGREY: the canvas
// is grayscale_8bit so the value collapses to its luma, and TFT_LIGHTGREY's ~210
// is far too faint for a 24px glyph here. 0x4208 ~ luma 66 and 0x8410 ~ luma 132
// are grey levels 4 and 8 of the panel's 16 — visibly apart, both still legible.
// KNOWN TRADEOFF: how grey text GHOSTS under lut_fast is unmeasured, and this
// band only ever pushes FAST (see pushLogBand). If it ghosts badly on device the
// revert is one line — set every LOG_FADE entry to TFT_BLACK.
//
// On-device feedback ("虚化内容所占行数比例太高"): an even three-way split
// across LOG_CAP=16 (8/4/4) read as HALF the log washed out, which is too much
// of a 9-23 line band to spend on hard-to-read text. The gradient's real job is
// signalling "these entries are about to fall out of the ring buffer", not
// decorating the whole list — so only the tail actually near eviction fades now.
constexpr uint16_t LOG_FADE[] = { TFT_BLACK, 0x4208, 0x8410 };
constexpr int LOG_FADE_TIERS  = (int)(sizeof LOG_FADE / sizeof LOG_FADE[0]);
constexpr int LOG_FADE_BLACK  = 12;          // newest 12 of the 16-entry cap stay
                                             // solid black — 75% of the ring
constexpr int LOG_FADE_STEP   = 2;           // remaining 4 split 2/2 (entries
                                             // 13-14 dark grey, 15-16 light grey,
                                             // the two about to be evicted)
constexpr int ROOM_BTN_H = 96;               // long-press band (§9.3 floor is 80;
                                             // grown for the cost sub-row, see above)
constexpr int BTN_GAP   = 10;                // vertical gap between rows
// Two columns of 240px with a 12px gutter fill the 492px content width. Re-
// measured for 变体 B (v0.14), where a priced cell's 36px title now has to share
// its row with a right-aligned cost column instead of owning the full width:
// 208px is usable (240 - 2*action_band::EDGE_PAD, which grew 8 -> 16 to stop the
// titles crowding the rounded frame), and title + MID_GAP + the widest cost entry
// must fit in it. Seven craft titles and one Outside verb do not and are
// auto-shrunk to 24px by the renderer's narrow-cell guard — 狩猎小屋, 贸易站,
// 制革屋, 熏肉房, 双肩包, 炼钢坊, 军械坊 here plus 查看陷阱 next door; every
// other cell keeps its 36px title (the full arithmetic is in action_band.cpp).
// Free cells (更多) have no cost column at all and simply centre.
// The grid still needs no full-width exception. x < COL_MID picks the left
// column (onLocalAction).
constexpr int COL_GAP   = 12;
constexpr int COL_W     = (CONTENT_W - COL_GAP) / 2;         // 240
constexpr int COL_X0[MAX_COLS] = { PAD, PAD + COL_W + COL_GAP };   // {24, 276}
constexpr int COL_MID   = 540 / 2;           // 270: x < MID => left column
// The grid's fixed edge (see the budget note above): 886 leaves the 928 status
// bar a 42px margin, and a full MAX_ROWS grid (5*96 + 4*10 = 520) reaches back
// up to 366 — the same first-row top the layout was hard-coded to before v0.16.
constexpr int BTN_AREA_BOTTOM = 886;

// ---- bottom-anchored derivations. `rows` is how many grid rows the CURRENT
// page actually fills, so every number below moves with an unlock or a "更多"
// page flip. All three are pure functions of rows/btnTop: the ONE stored copy of the
// result is RoomPage::m_btnTop, written on the full-redraw path (see draw()) and
// read by every partial-refresh path, so a cooldown push can never place a cell
// against a top the screen is not actually showing.
int btnTopForRows(int rows) {
    if (rows < 1) rows = 1;                  // buildActions always yields >=1
    return BTN_AREA_BOTTOM - (rows * ROOM_BTN_H + (rows - 1) * BTN_GAP);
}
// Where the log stream stops: LOG_GAP clear of the first button row.
int logBandBottom(int btnTop) { return btnTop - LOG_GAP; }
// How many whole 30px lines fit between LOG_TOP and that edge. 5 rows -> 346,
// (346-76)/30 = 9, the pre-v0.16 count; 1 row -> 770, (770-76)/30 = 23. drawLog
// only ever has LOG_CAP entries to show, so a tall band simply runs short.
int logLines(int btnTop) { return (logBandBottom(btnTop) - LOG_TOP) / LOG_LINEH; }

// Action codes carried in a Region param (uint8). 0..5 are the fixed verbs;
// A_CRAFT_BASE+craftId means "build/craft that craftable". Craft ids run
// [10, 10+24) = 34. (Trading-post buying moved to its own page in v0.3.3 — see
// trade_page.cpp — so the Room stays build/craft-focused; the A_TRADE_BASE code
// range this page carried in v0.3.2 is gone with it.)
// (A_TECH = 5 used to sit here: the 科技树 entry cell. v0.14 moved that entry to
// the Outside page's action grid, right after 伐木 — the tech tree is village
// planning, and it never made sense beside the fire verb. The page itself is
// unchanged; only its doorway moved. The code is left unused rather than
// recycled so a stale persisted region table can never resolve 5 to something
// else.)
enum : uint8_t {
    A_LIGHT  = 0, A_STOKE = 1, A_GATHER = 2, A_TRAPS = 3, A_MORE = 4,
    A_CRAFT_BASE = 10
};

struct BandView {
    uint8_t code;
    char    label[48];
    char    cost[64];                // "-500 木头  -50 毛皮"; empty for a free action
    bool    hasCost;
    bool    enabled;                 // false -> render as unavailable (dashed)
    int     coolLeft, coolTotal;
};

// RTC -> Unix epoch, mirroring main.cpp's epochNow (only differences matter to
// settle()/cooldownLeft, so the mktime timezone is irrelevant if consistent).
uint32_t epochNow() {
    m5::rtc_date_t d; m5::rtc_time_t t;
    M5.Rtc.getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900; tmv.tm_mon = d.month - 1; tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours; tmv.tm_min = t.minutes; tmv.tm_sec = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}

pages::Rect buttonAreaRect(int btnTop) {
    return pages::Rect{ 0, btnTop - 2, 540, BTN_AREA_BOTTOM - (btnTop - 2) };
}

// The rect of grid cell (row, col) — the ONE description of where a button is.
// paintButtons draws through it and pressRect flashes through it, so the frame
// the player sees and the rect that inverts under a press cannot drift apart.
// `btnTop` is the grid's live top (RoomPage::m_btnTop); both callers pass the
// value the current screen was drawn from, never a freshly recomputed one.
pages::Rect cellRect(int btnTop, int row, int col) {
    return pages::Rect{ COL_X0[col], btnTop + row * (ROOM_BTN_H + BTN_GAP),
                        COL_W, ROOM_BTN_H };
}

// Is craftable id offerable now? Delegates the progressive-unlock decision to
// the engine (room.js craftUnlocked: builder Helping + workshop + >=50% wood
// cost + all other materials seen; pushes the availableMsg once on first
// unlock), then applies the firmware UI's own maximum gate (hide at max rather
// than show disabled). Cost is NOT checked — an unaffordable-but-unlocked button
// still shows and beeps low on press, matching the "greyed but visible"
// affordance. g_game.craftUnlocked mutates (latch + log), so this is called from
// the layout/tick path, never a pure const context.
bool craftOfferable(uint8_t id) {
    if (!g_game.craftUnlocked(id)) return false;
    const Craftable& c = CRAFT[id];
    bool bld = craftIsBuilding(id);
    uint8_t slot = craftSlot(id);
    int count = bld ? g_game.buildings[slot] : g_game.items[slot];
    if (c.maximum >= 0 && count >= c.maximum) return false;
    return true;
}

// Can `code` actually fire right now? Mirrors the engine's own accept conditions
// (game_state.cpp lightFire/stokeFire/gatherWood/checkTraps/makeCraftable) so a
// button that WOULD be rejected renders as unavailable (dashed) instead of
// looking pressable — the user's "看起来都能点" complaint. Reads only public
// GameState fields + the data tables; never mutates (game_state.cpp untouched).
// A live cooldown counts as unavailable. The offered set already guarantees the
// unlock/workshop/maximum gates (buildActions + craftOfferable), so only the
// per-press gates (cooldown, cost, room-too-cold) are re-checked here.
bool isActionEnabled(uint8_t code, uint32_t now) {
    switch (code) {
        case A_MORE:  return true;                       // page flip is always live
        case A_LIGHT:
            if (g_game.cooldownLeft(0, now) > 0) return false;
            // free first light while wood is still "undefined" (room.js quirk)
            if (g_game.woodSeen && g_game.stores[R_WOOD] < LIGHT_FIRE_WOOD * FP)
                return false;
            return true;
        case A_STOKE:
            if (g_game.cooldownLeft(0, now) > 0) return false;
            return g_game.stores[R_WOOD] >= STOKE_FIRE_WOOD * FP;
        case A_GATHER: return g_game.cooldownLeft(1, now) == 0;
        case A_TRAPS:  return g_game.cooldownLeft(2, now) == 0;
        default: {
            uint8_t id = (uint8_t)(code - A_CRAFT_BASE);   // craftable id
            if (id >= CRAFT_COUNT) return false;
            const Craftable& c = CRAFT[id];
            if (g_game.temp <= TEMP_COLD) return false;   // "builder just shivers"
            bool bld = craftIsBuilding(id);
            uint8_t slot = craftSlot(id);
            int count = bld ? g_game.buildings[slot] : g_game.items[slot];
            for (int i = 0; i < 3 && c.cost[i].res != RA_END; i++) {
                int need = c.cost[i].amt;
                if (c.cost[i].res == R_WOOD) need += (int)c.woodIncrPerN * count;
                if (g_game.stores[c.cost[i].res] < (int32_t)need * FP) return false;
            }
            return true;
        }
    }
}

// A craftable's current cost sub-line, e.g. "-500 木头  -50 毛皮" — same
// "-amount name" convention (and the same two-space join) trade_page.cpp /
// event_modal.cpp / setpiece_modal.cpp already use for their own cost lines, so
// this reuses the established reading rather than inventing a new one (v0.10.0:
// "每个有消耗的按钮下方以小字标明消耗"). The wood entry folds in the
// count-scaling surcharge (room.js woodIncrPerN) so the shown number always
// matches what isActionEnabled just checked. Returns false (empty) only if the
// craftable were ever free (none are, in practice).
bool craftCostLine(uint8_t id, char* out, size_t cap) {
    out[0] = 0;
    const Craftable& c = CRAFT[id];
    if (c.cost[0].res == RA_END) return false;
    bool bld = craftIsBuilding(id);
    uint8_t slot = craftSlot(id);
    int count = bld ? g_game.buildings[slot] : g_game.items[slot];
    size_t used = 0;
    for (int i = 0; i < 3 && c.cost[i].res != RA_END; i++) {
        int32_t amt = c.cost[i].amt;
        if (c.cost[i].res == R_WOOD) amt += (int32_t)c.woodIncrPerN * count;
        const char* rz = tr(RES_KEY[c.cost[i].res]);
        int wrote = snprintf(out + used, cap - used, "%s-%ld %s",
                             used ? "  " : "", (long)amt, rz);
        if (wrote < 0) break;
        used += (size_t)wrote;
        if (used >= cap) { used = cap - 1; break; }
    }
    return used > 0;
}

// The cost sub-line for any action code, or false (empty) for a free action —
// gather wood / check traps cost nothing (their yield is a random drop, not a
// fixed price) and "more" is UI chrome and pure navigation, so none of the
// three get a subtitle. The two fire verbs have a fixed one-resource wood
// cost (room.js constants); every craftable delegates to craftCostLine above.
bool costLineFor(uint8_t code, char* out, size_t cap) {
    switch (code) {
        case A_LIGHT:
            snprintf(out, cap, "-%d %s", LIGHT_FIRE_WOOD, tr(RES_KEY[R_WOOD]));
            return true;
        case A_STOKE:
            snprintf(out, cap, "-%d %s", STOKE_FIRE_WOOD, tr(RES_KEY[R_WOOD]));
            return true;
        case A_GATHER: case A_TRAPS: case A_MORE:
            out[0] = 0;
            return false;
        default:
            return craftCostLine((uint8_t)(code - A_CRAFT_BASE), out, cap);
    }
}

// Ordered action list for the current game state: the fire verb, then every
// offerable craftable. Returns the count. (gather wood / check traps are 野外
// actions — upstream outside.js, not room.js — so they live on the Outside page;
// trading-post buying moved to the Trade page in v0.3.3, and the 科技树 entry
// moved to the Outside grid in v0.14.) Dropping 科技树 shortens this list by one,
// which the pagination below absorbs on its own — it is purely a function of the
// count, so the batches simply re-pack.
int buildActions(uint8_t* out, int cap) {
    int n = 0;
    if (n < cap) out[n++] = (g_game.fire == FIRE_DEAD) ? A_LIGHT : A_STOKE;
    if (g_game.craftablesUnlocked)
        for (uint8_t id = 0; id < CRAFT_COUNT && n < cap; id++)
            if (craftOfferable(id)) out[n++] = (uint8_t)(A_CRAFT_BASE + id);
    return n;
}

// Cooldown channel (0 fire / 1 gather / 2 traps) + full duration for an action
// code; channel<0 means "no cooldown" (craft / more).
void cooldownFor(uint8_t code, int& channel, int& total) {
    switch (code) {
        case A_LIGHT: case A_STOKE: channel = 0; total = STOKE_COOLDOWN_S; break;
        case A_GATHER:              channel = 1; total = GATHER_DELAY_S;    break;
        case A_TRAPS:               channel = 2; total = TRAPS_DELAY_S;     break;
        default:                    channel = -1; total = -1;               break;
    }
}

// Button label, all via tr(). "more" is UI chrome with no upstream key, so it
// uses the two closure-present glyphs 更/多 plus an ASCII page indicator
// pointing at the batch a press will reveal.
void labelFor(uint8_t code, int page, int numPages, char* out, size_t cap) {
    switch (code) {
        case A_LIGHT:  snprintf(out, cap, "%s", tr("light fire"));  break;
        case A_STOKE:  snprintf(out, cap, "%s", tr("stoke fire"));  break;
        case A_GATHER: snprintf(out, cap, "%s", tr("gather wood")); break;
        case A_TRAPS:  snprintf(out, cap, "%s", tr("check traps")); break;
        case A_MORE:
            snprintf(out, cap, "更多 (%d/%d)",
                     (page + 1 < numPages ? page + 2 : 1), numPages);
            break;
        default:
            snprintf(out, cap, "%s", tr(CRAFT[code - A_CRAFT_BASE].key));
            break;
    }
}

// Compute the visible action grid for `page`. Fills slotCodes[] (row-major:
// slot s -> row s/2, col s%2) + views[] (label + cooldown) for painting, and
// regionsOut[] with ONE y-band per ROW (param = row index). Both columns of a
// row share a row band; the pager hit-tests y only, so onLocalAction resolves
// the column from the press x (COL_MID). Batches of 7 real actions + a trailing
// "more" cell once the full list exceeds MAX_SLOTS. *slotCountOut receives the
// filled cell count; the return value is the ROW count (== region count).
// *btnTopOut receives the bottom-anchored grid top this row count implies — the
// single point where m_btnTop is produced, so the row bands handed to the pager
// and the cells paintButtons draws are derived from the same number by
// construction.
int layoutBands(pages::Region* regionsOut, uint8_t* slotCodes, BandView* views,
                int page, uint32_t now, int* slotCountOut, int* btnTopOut) {
    uint8_t all[64];
    int total = buildActions(all, (int)sizeof(all));

    int numPages, start, take;
    bool more;
    int pg = 0;
    if (total <= MAX_SLOTS) {
        numPages = 1; start = 0; take = total; more = false;
    } else {
        int perPage = MAX_SLOTS - 1;                 // 7 real + 1 "more"
        numPages = (total + perPage - 1) / perPage;
        pg = ((page % numPages) + numPages) % numPages;
        start = pg * perPage;
        take = total - start; if (take > perPage) take = perPage;
        more = true;
    }

    int k = 0;
    for (int i = 0; i < take && k < MAX_SLOTS; i++) slotCodes[k++] = all[start + i];
    if (more && k < MAX_SLOTS) slotCodes[k++] = A_MORE;
    int slotCount = k;

    for (int s = 0; s < slotCount; s++) {
        views[s].code = slotCodes[s];
        labelFor(slotCodes[s], pg, numPages, views[s].label, sizeof(views[s].label));
        views[s].hasCost = costLineFor(slotCodes[s], views[s].cost, sizeof(views[s].cost));
        views[s].enabled = isActionEnabled(slotCodes[s], now);
        int ch, tot; cooldownFor(slotCodes[s], ch, tot);
        views[s].coolTotal = tot;
        views[s].coolLeft  = (ch >= 0) ? g_game.cooldownLeft(ch, now) : 0;
    }

    int rows   = (slotCount + MAX_COLS - 1) / MAX_COLS;
    int btnTop = btnTopForRows(rows);
    for (int r = 0; r < rows; r++) {
        int top = btnTop + r * (ROOM_BTN_H + BTN_GAP);
        regionsOut[r].y0 = (uint16_t)top;
        regionsOut[r].y1 = (uint16_t)(top + ROOM_BTN_H);
        regionsOut[r].type = 1;                      // firmware-local
        regionsOut[r].param = (uint8_t)r;            // row; onLocalAction adds col from x
    }
    *slotCountOut = slotCount;
    *btnTopOut    = btnTop;
    return rows;
}

// ---- drawing pieces --------------------------------------------------------

// Log stream: newest on top, wrapped, filling the band that ends LOG_GAP above
// the grid. Picks how many recent entries fit (via wrapLineCount) before drawing
// so nothing overruns. The budget is logLines(btnTop), so a page with few action
// rows shows deeper history for free; with only LOG_CAP entries to draw, the
// tallest bands still run out of log before they run out of room.
//
// Older lines fade to grey (LOG_FADE — upstream notifyGradient). The tier is
// picked from the entry's AGE (its distance from the newest entry), never from
// its screen row, so a message that wraps to three lines keeps ONE tone and the
// gradient reads as "messages sinking into the dark" rather than a striped band.
void drawLog(m5gfx::M5Canvas& c, int btnTop) {
    log_view::draw(c, PAD, LOG_TOP, CONTENT_W, logBandBottom(btnTop));
}

// The log band's partial-refresh target (buttonAreaRect parity): the whole log
// rect plus a 2px bleed. It runs to logBandBottom rather than to the last line
// drawLog could fill, so the clear/repaint/push trio covers every pixel between
// the header and the grid whatever the current line budget rounds to. Used to
// surface a failed long-press's reason (v0.3.1 feedback 2) immediately, instead
// of waiting up to 1s for the next tick.
pages::Rect logAreaRect(int btnTop) {
    return pages::Rect{ 0, LOG_TOP - 2, 540, logBandBottom(btnTop) - LOG_TOP + 4 };
}

// Clear the log rect and repaint it into `c` (for the partial-refresh path —
// the surrounding full-page pixels already sit in the canvas).
void repaintLog(m5gfx::M5Canvas& c, int btnTop) {
    pages::Rect r = logAreaRect(btnTop);
    c.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
    drawLog(c, btnTop);
}

// Ship the freshly repainted log band to the panel. This is the ONE exit for
// every log-only update — tick()'s "only the log moved" path and onLocalAction's
// immediate failure-reason line both come through here — so the two share a
// single push policy instead of each inventing its own.
//
// ALWAYS FAST. A periodic "clean just this band with QUALITY" ration used to live
// here (every 4th push) and it was WRONG — it is what the player reported as
// "每隔一段时间整个屏幕全刷一下":
//   * Panel_EPD's per-pixel diff compares values that carry the EPD MODE in the
//     low bits, so a rect pushed under a DIFFERENT mode than the one already on
//     the panel diffs as "everything changed": the whole clip rect is re-driven
//     unconditionally. A local QUALITY push is therefore not a quiet touch-up of
//     the changed glyphs — it is the entire band inverted by the eraser pass and
//     then re-driven black/white over ~36 GC16 frames (~400ms).
//   * The band is not small. Since the grid became bottom-anchored the log grows
//     into whatever the button rows do not need — up to 698px of a 960px panel
//     (73%). A "strip flash" at that size reads as a full-screen flash.
//   * It also POISONS what follows: the rect keeps the quality mode marker, so the
//     next whole-page epd_fast redraw sees a mode change over that region and
//     re-drives it again (lut_fast inverts on its first two frames) — one ration
//     buys two flashes.
// The ghosting this band accrues is real, but it is settled where nobody is
// looking, never mid-session: at sleep entry (pager::payGhostDebtIfDue) on
// battery, and by the idle deep-clean (pager::idleDeepCleanIfDue) while on USB,
// where the device never sleeps.
void pushLogBand(int btnTop) {
    pager::partialRefresh(logAreaRect(btnTop), pages::RefreshMode::FAST);
}

// Paint the whole button area (clears it first) from the given slot views,
// placing slot s at row s/2, column s%2 (row-major reading order). Every cell
// goes through the shared action_band renderer: a priced cell puts its title in
// a LEFT column and its costs, one entry per line, in a RIGHT column; a free one
// (更多) centres its lone title. Both land the title on the SAME y, so a mixed
// row is level — see action_band.h "LEFT TITLE / RIGHT COSTS".
void paintButtons(m5gfx::M5Canvas& c, int btnTop, const BandView* views, int slotCount) {
    pages::Rect r = buttonAreaRect(btnTop);
    c.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
    for (int s = 0; s < slotCount; s++) {
        action_band::draw(c, cellRect(btnTop, s / MAX_COLS, s % MAX_COLS),
                          views[s].label,
                          views[s].hasCost ? views[s].cost : nullptr,
                          views[s].enabled,
                          views[s].coolLeft, views[s].coolTotal);
    }
}

// Content signature — a hash of every live value OUTSIDE THE LOG that alters a
// painted number or label (fire/temp, builder level, population, unlock flags,
// whole resource units, buildings). tick() compares it each second to decide a
// full redraw; onLocalAction re-baselines it right after its own showPage so the
// same action's state change doesn't force a SECOND full redraw on the next tick
// (see onLocalAction). Reads only g_game, never mutates.
//
// The log is DELIBERATELY not in here (it used to contribute g_game.logCount).
// The log band is the busiest thing on the page and the only one that can repaint
// alone, so tick() tracks it through logSig() and repaints just its strip — a new
// event line no longer drags the whole 540x960 page (and its ghosting) along with
// it. See tick().
uint32_t contentSig() {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix(g_game.fire); mix(g_game.temp);
    mix((uint32_t)(uint8_t)g_game.builderLevel);
    mix(g_game.population);
    mix((g_game.outsideUnlocked ? 1u : 0u) | (g_game.craftablesUnlocked ? 2u : 0u)
        | (g_game.woodSeen ? 4u : 0u));
    for (int i = 0; i < RES_COUNT; i++) mix((uint32_t)g_game.whole((uint8_t)i));
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);
    return sig;
}

// Log signature — a hash of exactly what drawLog would print, held separately
// from contentSig so tick() can tell "only the log moved" (repaint one strip)
// from "the world moved" (redraw the page).
//
// It hashes the ring's CONTENTS, not g_game.logCount, because logCount is not a
// change detector at all: game_state::pushLog drops the oldest entry and pins
// logCount at LOG_CAP once the ring saturates, and a repeat of the newest line
// collapses into that entry's counter ("...x3") without touching logCount even
// before saturation. A logCount baseline therefore goes permanently blind a
// handful of lines into a session — which is why the old contentSig could miss a
// log change outright whenever no resource happened to move with it.
uint32_t logSig() { return log_view::sig(); }

// Bounding rect (2px bleed) of the grid cells named in `mask` (bit s = slot s):
// each cell is COL_X0[col], its row top, COL_W x ROOM_BTN_H. The cooldown tick
// pushes just this union — never the whole button area — so only the cell whose
// progress bar is draining (Room has at most one, the fire verb) flips on screen.
// `btnTop` must be the top the screen was last DRAWN from (m_btnTop), not one
// recomputed here: a row-count change never reaches this path (it moves
// contentSig, which redraws the page and returns before any cooldown push).
// Empty mask -> zero rect (the caller gates on mask, so an empty rect never ships).
pages::Rect coolingRect(int btnTop, uint16_t mask) {
    int x0 = 540, y0 = BTN_AREA_BOTTOM, x1 = 0, y1 = btnTop;
    for (int s = 0; s < MAX_SLOTS; s++) {
        if (!(mask & (1u << s))) continue;
        int col = s % MAX_COLS;
        int top = btnTop + (s / MAX_COLS) * (ROOM_BTN_H + BTN_GAP);
        if (COL_X0[col] < x0)         x0 = COL_X0[col];
        if (COL_X0[col] + COL_W > x1) x1 = COL_X0[col] + COL_W;
        if (top < y0)                 y0 = top;
        if (top + ROOM_BTN_H > y1)    y1 = top + ROOM_BTN_H;
    }
    if (x1 <= x0) return pages::Rect{ 0, 0, 0, 0 };
    return pages::Rect{ x0 - 2, y0 - 2, (x1 - x0) + 4, (y1 - y0) + 4 };
}
}  // namespace

// ================================ Page API =================================

const pages::Region* RoomPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

// Press-flash target: the row band (rg.param) + the press x pick the exact grid
// cell — the SAME row*MAX_COLS+col slot onLocalAction resolves. A slot past the
// filled cells (an odd action count's trailing column) returns w=0 so the empty
// cell never flashes black.
pages::Rect RoomPage::pressRect(const pages::Region& rg, int x, int y) const {
    (void)y;
    int row  = rg.param;
    int col  = (x < COL_MID) ? 0 : 1;
    int slot = row * MAX_COLS + col;
    if (slot < 0 || slot >= m_slotCount) return pages::Rect{ 0, rg.y0, 0, 0 };
    return cellRect(m_btnTop, row, col);   // the exact rect paintButtons framed
}

// Full-page paint, and the ONLY writer of m_btnTop — every partial path reads
// the value this left behind, so it is also the only place the log band's height
// can change. layoutBands therefore runs FIRST, before a single pixel is laid
// down: it is what settles the row count, and the log below it cannot pick a
// line budget until the grid has claimed its share of the column. (Running it
// first also means a craftUnlocked latch fired from craftOfferable pushes its
// availableMsg into the log in time for THIS frame to show it, instead of one
// tick later.)
bool RoomPage::draw(m5gfx::M5Canvas& c) {
    c.fillSprite(TFT_WHITE);
    page_tabs::draw(c, 0);           // shared tab header, Room active
    BandView views[MAX_SLOTS];
    m_regionCount = layoutBands(m_regions, m_slotCodes, views, m_page, epochNow(),
                                &m_slotCount, &m_btnTop);
    drawLog(c, m_btnTop);            // log stream, from the header gap down to the grid
    paintButtons(c, m_btnTop, views, m_slotCount);
    return true;
}

// Long-press on a band -> the mapped engine action. Success: high beep, persist,
// full redraw. Failure branches by reason (v0.3.1 feedback 2 — "disabled 按钮
// 长按要说明原因"): cost-insufficient (RC_ERR_COST) and room-too-cold
// (RC_ERR_COLD) both already got their upstream-exact reason pushed into the
// log by the engine itself (game_state.cpp lightFire/stokeFire/makeCraftable) —
// this just repaints the log band immediately so it's visible without waiting up
// to 1s for the next tick. A live cooldown (RC_ERR_COOLDOWN) stays silent: the
// draining progress bar already explains itself, and re-notifying on every
// cooling press would spam the log. "more" flips to the next batch. save()
// lives here because the engine actions do not persist themselves (single
// write — no double-save). (y is unused — the Room grid resolves columns by x.)
void RoomPage::onLocalAction(uint8_t param, int x, int y) {
    (void)y;
    uint32_t now = epochNow();

    // param is the ROW index; the press x resolves which of the row's two
    // columns was hit (x < COL_MID = left). An empty cell (odd action count's
    // trailing column) low-beeps and does nothing.
    int row  = param;
    int col  = (x < COL_MID) ? 0 : 1;
    int slot = row * MAX_COLS + col;
    if (slot < 0 || slot >= m_slotCount) { M5.Speaker.tone(600, 120); return; }
    uint8_t code = m_slotCodes[slot];

    if (code == A_MORE) {
        m_page++;
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::currentRingIndex(), false);
        return;
    }

    Result r;
    switch (code) {
        case A_LIGHT:  r = g_game.lightFire(now);  break;
        case A_STOKE:  r = g_game.stokeFire(now);  break;
        case A_GATHER: r = g_game.gatherWood(now); break;
        case A_TRAPS:  r = g_game.checkTraps(now); break;
        default: {
            uint8_t id = (uint8_t)(code - A_CRAFT_BASE);
            if (id >= CRAFT_COUNT) { M5.Speaker.tone(600, 120); return; }
            r = craftIsBuilding(id) ? g_game.build(id) : g_game.craft(id);
            break;
        }
    }

    if (r == RC_OK) {
        // Every successful action redraws the whole page. This is right even for a
        // lone fire verb: A_LIGHT flips the fire dead->lit, which changes the SHARED
        // tab title (page_tabs::roomTitle — 小黑屋 when FIRE_DEAD vs 生火间 when lit),
        // so the header itself must repaint (and it is not wasteful — the driver
        // diffs per-pixel, see the RC_ERR branch). Re-baseline tick()'s content
        // signature to the state we JUST drew (AFTER showPage, so a draw-time
        // craftUnlocked latch/log is captured too). No extra settle: draw() paints
        // un-settled g_game and contentSig() must mirror exactly that. tick() then
        // settles + compares against this, so this same action no longer trips a
        // SECOND full-page redraw next tick — only genuine economy advancing in the
        // following second still does.
        M5.Speaker.tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
        m_lastSig    = contentSig();
        m_lastLogSig = logSig();     // the full redraw repainted the log too
    } else if (r == RC_ERR_COST || r == RC_ERR_COLD) {
        M5.Speaker.tone(600, 120);
        // m_btnTop, not a fresh layout: a rejected action changes no row count,
        // and the band being repainted is the one already on the panel.
        repaintLog(canvas, m_btnTop);
        pushLogBand(m_btnTop);       // same single FAST exit as tick's
        // The engine already pushed the reason (game_state lightFire/stokeFire/
        // makeCraftable) and the partial above already put it on screen, so sync BOTH
        // tick baselines to it. Without the log one the next tick would see
        // logSig() != m_lastLogSig and repaint + re-push the very band we just
        // pushed — a second flash of the same unchanged pixels, and one more slab of
        // ghosting debt, for nothing. contentSig() is
        // re-synced for symmetry with the RC_OK branch: a rejected action moves
        // nothing outside the log, so this is a no-op today, but it keeps the two
        // baselines from drifting if the engine ever starts touching state on a
        // rejection.
        //
        // (What a redundant showPage costs, for the record: it would NOT visibly
        // re-flash the log — the M5GFX Panel_EPD driver diffs per-pixel, task_update
        // compares _step_framebuf vs _buf and only a changed pixel gets a drive step
        // — but it still burns a full 540x960 draw() + a full-panel scanline sweep +
        // status-bar rebuild for nothing. Same reason a SMALLER push never saves
        // flicker: shrinking the rect changes no driven pixels. Do not reintroduce a
        // "narrow the rect / point-refresh to reduce flicker" design — it was tried
        // in 0.5.4 and reverted once the driver diff was confirmed.
        // IMPORTANT LIMIT on that diff, learned the hard way (see pushLogBand): it
        // only holds while the EPD MODE STAYS THE SAME. The compared byte carries the
        // mode in its low bits, so a push under a different mode than the panel last
        // saw over that area diffs as all-changed and re-drives the ENTIRE clip rect,
        // unchanged pixels included. "Unchanged pixels are free" is a same-mode rule,
        // and this branch stays free only because everything here pushes epd_fast.)
        m_lastSig    = contentSig();
        m_lastLogSig = logSig();
    } else {
        M5.Speaker.tone(600, 120);   // cooldown / locked / max — unchanged, silent
    }
}

// Time axis (awake only). Settle the economy each second, then repaint the
// SMALLEST region that actually changed. Three tiers, cheapest last:
//
//   1. A content change (fire/temp/stores/unlocks — which also flips a button's
//      available/dashed state) redraws the whole page.
//   2. Otherwise a LOG-ONLY change repaints just the log strip (repaintLog +
//      pushLogBand). This tier is why contentSig() no longer carries the log: an
//      event stream used to make every single line a full-page epd_fast redraw,
//      so the buttons, tabs and status bar all sat there collecting ghosting they
//      had no reason to collect — the "生火间日志区残影严重" report. A page-wide
//      redraw is now reserved for a page-wide change, and the log strip pays only
//      for itself. Its own ghosting is never cleaned in place (a local QUALITY push
//      re-drives the whole band — see pushLogBand); it is settled off-screen by
//      pager's sleep-entry / USB-idle deep-cleans.
//   3. While an action cools its bar drains — paint every button into the canvas
//      as before but push ONLY the cell(s) whose bar actually MOVED.
//
// Tiers 2 and 3 are not exclusive: a second can both log a line and move a bar,
// and each pushes its own rect. Tier 1 subsumes both and returns early.
//
// The tick still runs every second, but a push no longer does: the bar is
// quantized to action_band::BAR_LEVELS steps, so most seconds redraw the cell
// identically and pushing them was pure ghosting for no visible change. A cell
// is pushed only when its quantized level differs from the one this tick's
// predecessor left on screen — which is also exactly how the "cooldown just hit
// zero" repaint (level >0 -> 0, the cell losing its bar and its dashed frame)
// still ships. For the 60s traps cooldown that is 16 pushes instead of 60.
//
// Those pushes are FAST (GC16-lite), never QUALITY: a full-area grayscale wipe to
// chase the bar/dashed-frame ghost is exactly the "jarring when it fires while
// the user is looking" flash (pager.cpp) the user reported as a big black block.
// FAST charges pager's s_fastCount, so the ghost it leaves is on the books and
// gets cleaned at sleep entry by pager::payGhostDebtIfDue, when nobody is
// watching. onLocalAction re-baselines m_lastSig after its own showPage, so a
// press no longer forces a second full redraw here. Mirrors the outside_page
// cadence.
void RoomPage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick = 0;
    // Quantized bar level per slot as the screen currently shows it — the
    // baseline a push is decided against. Zero = that cell has no bar on screen.
    static uint8_t  s_lastLevel[MAX_SLOTS] = { 0 };

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    uint32_t now = epochNow();
    g_game.settle(now);

    uint32_t sig  = contentSig();
    uint32_t lsig = logSig();

    // Quantized bar level of every drawn slot right now — the SAME expression
    // action_band::draw uses to pick the fill width, so "level unchanged" really
    // does mean "the cell would be repainted pixel-identically".
    uint8_t level[MAX_SLOTS] = { 0 };
    for (int s = 0; s < m_slotCount; s++) {
        int ch, tot; cooldownFor(m_slotCodes[s], ch, tot);
        if (ch < 0) continue;
        level[s] = (uint8_t)action_band::barLevel(g_game.cooldownLeft(ch, now), tot);
    }

    if (sig != m_lastSig) {
        m_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);   // recomputes bands
        // A full redraw repaints the log band as part of the page, so re-baseline
        // the log too — otherwise the tier-2 branch below would repaint and push
        // that same strip again on the very next tick.
        m_lastLogSig = lsig;
        // The full redraw just put THIS tick's levels on screen; re-baseline or the
        // next tick would read a stale level and push a cell for nothing.
        memcpy(s_lastLevel, level, sizeof(s_lastLevel));
        return;
    }

    // Tier 2: nothing outside the log moved, so repaint the strip alone. Reaching
    // here means contentSig() is unchanged, hence the offered action set — and so
    // the row count and m_btnTop — are exactly what the last full draw() left on
    // screen. Both the clear and the push therefore use m_btnTop as-is.
    if (lsig != m_lastLogSig) {
        m_lastLogSig = lsig;
        repaintLog(canvas, m_btnTop);
        pushLogBand(m_btnTop);
    }

    uint16_t pushMask = 0;
    for (int s = 0; s < MAX_SLOTS; s++)
        if (level[s] != s_lastLevel[s]) pushMask |= (uint16_t)(1u << s);

    if (pushMask) {
        BandView views[MAX_SLOTS];
        // Re-layout only to refresh the bar values; the row count cannot have moved
        // (tier 1 returned above), so this rewrites m_btnTop with the same number
        // draw() put there — and paintButtons/coolingRect below read it back rather
        // than each deriving a top of their own.
        m_regionCount = layoutBands(m_regions, m_slotCodes, views, m_page, now,
                                    &m_slotCount, &m_btnTop);
        paintButtons(canvas, m_btnTop, views, m_slotCount);
        // Only the cells whose level moved — never the whole button area. FAST; the
        // ghost cleanup is deferred to sleep (see the function note).
        pager::partialRefresh(coolingRect(m_btnTop, pushMask), pages::RefreshMode::FAST);
    }
    memcpy(s_lastLevel, level, sizeof(s_lastLevel));
}
