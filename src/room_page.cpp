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
#include "pomo_page.h"          // PAD (shared layout authority)
#include "page_tabs.h"          // shared two-tab header (生火间 │ 小型村落)
#include "pager.h"
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
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px CJK (log stream)
constexpr int GLYPH     = 12 * SCALE;        // 24px line box (log)
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
// MAX_ROWS stays 5 and LOG_LINES stays 9 — unchanged from v0.10.0. 5 rows x 96 +
// 4x10 gaps = 520, landing BTN_AREA_BOTTOM at 886 (< 928 status bar, 42px
// margin — matches the original v0.10.0 number). Resource/inventory summary
// lives on the Outside page's lower band.
// v0.3.1 feedback 1 ("火堆熊熊燃烧 房间很热不应该常驻 原作也没有常驻"): the
// persistent fire/temp state line that used to open this band (STATE_Y, 76..
// 116) is gone — upstream never shows it as a fixed header either, only as a
// notification on change (see game_state.cpp onFireChange/adjustTemp, which
// now push "the fire is {0}" / "the room is {0}" into the log on every
// change). The log reclaims that band: it moves up to LOG_TOP=76. ------------
constexpr int LOG_TOP   = page_tabs::CONTENT_TOP + 4;   // log stream top (76)
constexpr int LOG_LINEH = 30;
constexpr int LOG_LINES = 9;                 // 9 x 30 = 270px band -> ends 346,
                                             // 20px clear of BTN_TOP
constexpr int BTN_TOP   = 366;               // first action row top
constexpr int ROOM_BTN_H = 96;               // long-press band (§9.3 floor is 80;
                                             // grown for the cost sub-row, see above)
constexpr int BTN_GAP   = 10;                // vertical gap between rows
// Two columns of 240px with a 12px gutter fill the 492px content width. Re-
// measured for 变体 B (v0.14), where a priced cell's 36px title now has to share
// its row with a right-aligned cost column instead of owning the full width:
// 224px is usable (240 - 2*action_band::EDGE_PAD), and title + MID_GAP + the
// widest cost entry must fit in it. Three craft titles and one Outside verb do
// not and are auto-shrunk to 24px by the renderer's narrow-cell guard —
// 狩猎小屋, 炼钢坊, 军械坊 here plus 查看陷阱 next door; every other cell keeps
// its 36px title. Free cells (更多) have no cost column at all and simply centre.
// The grid still needs no full-width exception. x < COL_MID picks the left
// column (onLocalAction).
constexpr int COL_GAP   = 12;
constexpr int COL_W     = (CONTENT_W - COL_GAP) / 2;         // 240
constexpr int COL_X0[MAX_COLS] = { PAD, PAD + COL_W + COL_GAP };   // {24, 276}
constexpr int COL_MID   = 540 / 2;           // 270: x < MID => left column
constexpr int BTN_AREA_BOTTOM = BTN_TOP + (MAX_ROWS - 1) * (ROOM_BTN_H + BTN_GAP)
                                + ROOM_BTN_H;   // 886 (5 rows, clears status bar)

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

// Splice one arg into a "...{0}..." template (the game's own placeholder form).
void fmt1(char* out, size_t cap, const char* tmpl, const char* arg) {
    const char* h = strstr(tmpl, "{0}");
    if (!h) { snprintf(out, cap, "%s", tmpl); return; }
    int pre = (int)(h - tmpl);
    snprintf(out, cap, "%.*s%s%s", pre, tmpl, arg, h + 3);
}

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

pages::Rect buttonAreaRect() {
    return pages::Rect{ 0, BTN_TOP - 2, 540, BTN_AREA_BOTTOM - (BTN_TOP - 2) };
}

// The rect of grid cell (row, col) — the ONE description of where a button is.
// paintButtons draws through it and pressRect flashes through it, so the frame
// the player sees and the rect that inverts under a press cannot drift apart.
pages::Rect cellRect(int row, int col) {
    return pages::Rect{ COL_X0[col], BTN_TOP + row * (ROOM_BTN_H + BTN_GAP),
                        COL_W, ROOM_BTN_H };
}

// Count the lines cjk::drawWrapped would emit for `utf8` at width w — same
// greedy CJK/space wrap, no drawing — so the log stream can pick how many
// recent entries fit its 5-line band before painting (nothing spills onto the
// buttons).
int wrapLineCount(const char* utf8, int w, int scale) {
    constexpr int MAX = 256;
    static uint32_t cps[MAX];
    static int      advs[MAX];
    int n = 0;
    const char* p = utf8;
    while (n < MAX) {
        const unsigned char* s = (const unsigned char*)p;
        if (*s == 0) break;
        int32_t cp; int len;
        if (*s < 0x80)              { cp = *s;        len = 1; }
        else if ((*s >> 5) == 0x6)  { cp = *s & 0x1F; len = 2; }
        else if ((*s >> 4) == 0xE)  { cp = *s & 0x0F; len = 3; }
        else if ((*s >> 3) == 0x1E) { cp = *s & 0x07; len = 4; }
        else                        { cp = *s;        len = 1; }
        for (int i = 1; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80) { len = i; break; }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        char one[5]; memcpy(one, s, (size_t)len); one[len] = 0;
        cps[n]  = (uint32_t)cp;
        advs[n] = cjk::textWidth(one, scale);
        p += len;
        n++;
    }
    if (n == 0) return 1;
    auto breakAfter = [](uint32_t cp) { return cp == 0x20 || cp >= 0x2000; };
    int lines = 0, lineStart = 0;
    while (lineStart < n) {
        int curW = 0, lastBreak = -1, j = lineStart;
        for (; j < n; j++) {
            if (curW + advs[j] > w && j > lineStart) break;
            curW += advs[j];
            if (breakAfter(cps[j])) lastBreak = j;
        }
        int lineEnd = (j >= n) ? n
                    : (lastBreak >= lineStart) ? lastBreak + 1 : j;
        lines++;
        lineStart = lineEnd;
        if (lineStart < n && cps[lineStart] == 0x20) lineStart++;
    }
    return lines < 1 ? 1 : lines;
}

// Render one log entry's en_key into `out` as official zh, splicing its arg if
// the template carries a {0}. Most {0} templates take a number (population
// lines pass a count) but "the fire is {0}" / "the room is {0}" (v0.3.1
// feedback 1: pushed by game_state.cpp on every fire/temp change now that the
// persistent state line is gone) pass a Fire/Temp enum value instead — look
// that up in FIRE_TEXT/TEMP_TEXT and splice the translated status word.
// A repeated entry (v0.3.1: GameState::pushLog collapses a repeat of the
// newest line instead of scrolling a duplicate — see game_state.h LogEntry::
// count) gets an ASCII " x<count>" suffix, same "CJK label x%u" mixed-script
// convention outside_page.cpp already uses for worker band labels.
void logText(const LogEntry& e, char* out, size_t cap) {
    const char* zh = tr(e.enKey);
    char base[160];
    if (e.hasArg && strcmp(e.enKey, "the fire is {0}") == 0) {
        uint8_t idx = (e.arg >= 0 && e.arg < 5) ? (uint8_t)e.arg : 0;
        fmt1(base, sizeof(base), zh, tr(FIRE_TEXT[idx]));
    } else if (e.hasArg && strcmp(e.enKey, "the room is {0}") == 0) {
        uint8_t idx = (e.arg >= 0 && e.arg < 5) ? (uint8_t)e.arg : 0;
        fmt1(base, sizeof(base), zh, tr(TEMP_TEXT[idx]));
    } else if (e.hasArg && strstr(zh, "{0}")) {
        char num[16]; snprintf(num, sizeof(num), "%ld", (long)e.arg);
        fmt1(base, sizeof(base), zh, num);
    } else {
        snprintf(base, sizeof(base), "%s", zh);
    }
    if (e.count > 1) snprintf(out, cap, "%s x%u", base, (unsigned)e.count);
    else             snprintf(out, cap, "%s", base);
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
int layoutBands(pages::Region* regionsOut, uint8_t* slotCodes, BandView* views,
                int page, uint32_t now, int* slotCountOut) {
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

    int rows = (slotCount + MAX_COLS - 1) / MAX_COLS;
    for (int r = 0; r < rows; r++) {
        int top = BTN_TOP + r * (ROOM_BTN_H + BTN_GAP);
        regionsOut[r].y0 = (uint16_t)top;
        regionsOut[r].y1 = (uint16_t)(top + ROOM_BTN_H);
        regionsOut[r].type = 1;                      // firmware-local
        regionsOut[r].param = (uint8_t)r;            // row; onLocalAction adds col from x
    }
    *slotCountOut = slotCount;
    return rows;
}

// ---- drawing pieces --------------------------------------------------------

// Log stream: newest on top, wrapped, filling the 11-line band. Picks how many
// recent entries fit (via wrapLineCount) before drawing so nothing overruns.
void drawLog(m5gfx::M5Canvas& c) {
    int start = g_game.logCount;   // lowest index that still fits
    int usedLines = 0;
    for (int i = g_game.logCount - 1; i >= 0; i--) {
        char t[160]; logText(g_game.log[i], t, sizeof(t));
        int lines = wrapLineCount(t, CONTENT_W, SCALE);
        if (usedLines + lines > LOG_LINES) break;
        usedLines += lines;
        start = i;
    }
    int y = LOG_TOP;
    for (int i = g_game.logCount - 1; i >= start; i--) {
        char t[160]; logText(g_game.log[i], t, sizeof(t));
        y = cjk::drawWrapped(c, PAD, y, CONTENT_W, t, SCALE, LOG_LINEH);
    }
}

// The log band's partial-refresh target (buttonAreaRect parity): the whole log
// rect plus a 2px bleed. Used to surface a failed long-press's reason (v0.3.1
// feedback 2) immediately, instead of waiting up to 1s for the next tick.
pages::Rect logAreaRect() {
    return pages::Rect{ 0, LOG_TOP - 2, 540, LOG_LINES * LOG_LINEH + 4 };
}

// Clear the log rect and repaint it into `c` (for the partial-refresh path —
// the surrounding full-page pixels already sit in the canvas).
void repaintLog(m5gfx::M5Canvas& c) {
    pages::Rect r = logAreaRect();
    c.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
    drawLog(c);
}

// Paint the whole button area (clears it first) from the given slot views,
// placing slot s at row s/2, column s%2 (row-major reading order). Every cell
// goes through the shared action_band renderer: a priced cell puts its title in
// a LEFT column and its costs, one entry per line, in a RIGHT column; a free one
// (更多) centres its lone title. Both land the title on the SAME y, so a mixed
// row is level — see action_band.h "LEFT TITLE / RIGHT COSTS".
void paintButtons(m5gfx::M5Canvas& c, const BandView* views, int slotCount) {
    pages::Rect r = buttonAreaRect();
    c.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
    for (int s = 0; s < slotCount; s++) {
        action_band::draw(c, cellRect(s / MAX_COLS, s % MAX_COLS),
                          views[s].label,
                          views[s].hasCost ? views[s].cost : nullptr,
                          views[s].enabled,
                          views[s].coolLeft, views[s].coolTotal);
    }
}

// Content signature — a hash of every live value that alters a painted number or
// label (fire/temp, builder level, population, log count, unlock flags, whole
// resource units, buildings). tick() compares it each second to decide a full
// redraw; onLocalAction re-baselines it right after its own showPage so the same
// action's state change doesn't force a SECOND full redraw on the next tick (see
// onLocalAction). Reads only g_game, never mutates.
uint32_t contentSig() {
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix(g_game.fire); mix(g_game.temp);
    mix((uint32_t)(uint8_t)g_game.builderLevel);
    mix(g_game.population); mix(g_game.logCount);
    mix((g_game.outsideUnlocked ? 1u : 0u) | (g_game.craftablesUnlocked ? 2u : 0u)
        | (g_game.woodSeen ? 4u : 0u));
    for (int i = 0; i < RES_COUNT; i++) mix((uint32_t)g_game.whole((uint8_t)i));
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);
    return sig;
}

// Bounding rect (2px bleed) of the grid cells named in `mask` (bit s = slot s):
// each cell is COL_X0[col], its row top, COL_W x ROOM_BTN_H. The cooldown tick
// pushes just this union — never the 540x442 button area — so only the cell whose
// progress bar is draining (Room has at most one, the fire verb) flips on screen.
// Empty mask -> zero rect (the caller gates on mask, so an empty rect never ships).
pages::Rect coolingRect(uint16_t mask) {
    int x0 = 540, y0 = BTN_AREA_BOTTOM, x1 = 0, y1 = BTN_TOP;
    for (int s = 0; s < MAX_SLOTS; s++) {
        if (!(mask & (1u << s))) continue;
        int col = s % MAX_COLS;
        int top = BTN_TOP + (s / MAX_COLS) * (ROOM_BTN_H + BTN_GAP);
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
    return cellRect(row, col);          // the exact rect paintButtons framed
}

bool RoomPage::draw(m5gfx::M5Canvas& c) {
    c.fillSprite(TFT_WHITE);
    page_tabs::draw(c, 0);           // shared tab header, Room active
    drawLog(c);                      // log stream, reflowed up into the header gap
    BandView views[MAX_SLOTS];
    m_regionCount = layoutBands(m_regions, m_slotCodes, views, m_page, epochNow(),
                                &m_slotCount);
    paintButtons(c, views, m_slotCount);
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
        m_lastSig = contentSig();
    } else if (r == RC_ERR_COST || r == RC_ERR_COLD) {
        M5.Speaker.tone(600, 120);
        repaintLog(canvas);
        pager::partialRefresh(logAreaRect(), pages::RefreshMode::FAST);
        // The engine already pushed the reason (game_state lightFire/stokeFire/
        // makeCraftable) and the partial above already put it on screen, so sync the
        // tick baseline to it. Without this the +1 logCount makes the next tick see
        // contentSig() != m_lastSig and run a whole redundant full-page showPage.
        // That showPage would not even visibly re-flash the log — the M5GFX Panel_EPD
        // driver diffs per-pixel (task_update compares _step_framebuf vs _buf, only a
        // changed pixel gets a drive step), so the already-drawn line is not re-driven
        // — but it still burns a full 540x960 draw() + a full-panel scanline sweep +
        // status-bar rebuild for nothing. (Same reason a SMALLER push never saves
        // flicker: shrinking the rect changes no driven pixels. Do not reintroduce a
        // "narrow the rect / point-refresh to reduce flicker" design — it was tried
        // in 0.5.4 and reverted once the driver diff was confirmed.)
        m_lastSig = contentSig();
    } else {
        M5.Speaker.tone(600, 120);   // cooldown / locked / max — unchanged, silent
    }
}

// Time axis (awake only). Settle the economy each second, then repaint what
// changed: a content change (fire/temp/stores/log/unlocks — which also flips a
// button's available/dashed state) redraws the whole page. Otherwise, while an
// action cools its bar drains — so paint every button into the canvas as before
// but push ONLY the cooling cell(s) (coolingRect), and on the tick a cooldown
// hits zero push just the cell that JUST cleared. Both use FASTEST (DU), never
// QUALITY: a full-area grayscale wipe to chase the bar/dashed-frame ghost is
// exactly the "jarring when it fires while the user is looking" flash (pager.cpp)
// the user reported as a big black block. That ghost is instead cleaned at sleep
// entry by pager::payGhostDebtIfDue, when nobody is watching. onLocalAction
// re-baselines m_lastSig after its own showPage, so a press no longer forces a
// second full redraw here. Mirrors the outside_page cadence.
void RoomPage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick     = 0;
    static uint16_t s_lastCoolMask = 0;   // cooling cells the previous tick pushed

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    uint32_t now = epochNow();
    g_game.settle(now);

    uint32_t sig = contentSig();

    // Which drawn slots carry a live cooldown right now (bit s = slot s) — only
    // these cells change on a cooldown tick (a draining bar in one grid cell).
    uint16_t coolMask = 0;
    for (int s = 0; s < m_slotCount; s++) {
        int ch, tot; cooldownFor(m_slotCodes[s], ch, tot);
        if (ch >= 0 && g_game.cooldownLeft(ch, now) > 0)
            coolMask |= (uint16_t)(1u << s);
    }

    if (sig != m_lastSig) {
        m_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);   // recomputes bands
        s_lastCoolMask = coolMask;
        return;
    }

    if (coolMask || s_lastCoolMask) {
        BandView views[MAX_SLOTS];
        m_regionCount = layoutBands(m_regions, m_slotCodes, views, m_page, now,
                                    &m_slotCount);
        paintButtons(canvas, views, m_slotCount);
        // Union of the cells cooling now and the ones that just cleared this tick
        // (were cooling last tick) — never the whole button area. FASTEST; the
        // ghost cleanup is deferred to sleep (see the function note).
        pager::partialRefresh(coolingRect((uint16_t)(coolMask | s_lastCoolMask)),
                              pages::RefreshMode::FASTEST);
    }
    s_lastCoolMask = coolMask;
}
