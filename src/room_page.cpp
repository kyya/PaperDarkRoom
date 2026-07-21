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
#include "cjk_text.h"
#include "page_header.h"
#include "pomo_page.h"          // PAD, HDR_DIV_Y (shared layout authority)
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
constexpr int SCALE     = 2;                 // 12px grid x2 = 24px CJK
constexpr int CONTENT_W = 540 - 2 * PAD;     // 492px usable (§9.2)
constexpr int GLYPH     = 12 * SCALE;        // 24px line box
constexpr int MAX_BANDS = 4;                 // matches RoomPage::MAX_BANDS

// ---- vertical budget (§9.4), all measured to clear the 32px status bar -----
constexpr int STATE_Y   = 120;               // fire/temp state line (below rule)
constexpr int RES_TOP   = 158;               // resource summary top
constexpr int RES_ROWH  = 28;                // per resource row
constexpr int RES_ROWS  = 5;                 // <=5 rows x 2 cols
constexpr int RES_COLX[2] = { PAD, 288 };    // two-column x origins
constexpr int LOG_TOP   = 300;               // log stream top
constexpr int LOG_LINEH = 30;
constexpr int LOG_LINES = 5;                 // 5 x 30 = 150px band
constexpr int LOG_H     = LOG_LINEH * LOG_LINES;
constexpr int BTN_TOP   = 464;               // first action band top
constexpr int ROOM_BTN_H = 92;               // long-press band (§9.3: >=80px)
constexpr int BTN_GAP   = 12;
constexpr int BTN_X0    = PAD;
constexpr int BTN_X1    = 540 - PAD;
constexpr int BTN_AREA_BOTTOM = BTN_TOP + (MAX_BANDS - 1) * (ROOM_BTN_H + BTN_GAP)
                                + ROOM_BTN_H;   // 868

// Action codes carried in a Region param (uint8). 0..4 are the fixed verbs;
// A_CRAFT_BASE+craftId means "build/craft that craftable".
enum : uint8_t {
    A_LIGHT  = 0, A_STOKE = 1, A_GATHER = 2, A_TRAPS = 3, A_MORE = 4,
    A_CRAFT_BASE = 10
};

struct BandView {
    uint8_t code;
    char    label[48];
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
// the template carries a {0} (population lines pass a count).
void logText(const LogEntry& e, char* out, size_t cap) {
    const char* zh = tr(e.enKey);
    if (e.hasArg && strstr(zh, "{0}")) {
        char num[16]; snprintf(num, sizeof(num), "%ld", (long)e.arg);
        fmt1(out, cap, zh, num);
    } else {
        snprintf(out, cap, "%s", zh);
    }
}

// Is craftable id offerable now (unlocked, workshop-gated, under maximum)? Cost
// is NOT checked — an unaffordable button still shows and beeps low on press,
// matching the upstream "greyed but visible" affordance.
bool craftOfferable(uint8_t id) {
    const Craftable& c = CRAFT[id];
    if (craftNeedsWorkshop(c.type) && g_game.buildings[B_WORKSHOP] == 0)
        return false;
    bool bld = craftIsBuilding(id);
    uint8_t slot = craftSlot(id);
    int count = bld ? g_game.buildings[slot] : g_game.items[slot];
    if (c.maximum >= 0 && count >= c.maximum) return false;
    return true;
}

// Ordered action list for the current game state: fire verb, then gather/traps,
// then every offerable craftable. Returns the count.
int buildActions(uint8_t* out, int cap) {
    int n = 0;
    if (n < cap) out[n++] = (g_game.fire == FIRE_DEAD) ? A_LIGHT : A_STOKE;
    if (g_game.outsideUnlocked && n < cap)       out[n++] = A_GATHER;
    if (g_game.buildings[B_TRAP] > 0 && n < cap) out[n++] = A_TRAPS;
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

// Compute the visible bands for `page`: fills regionsOut[] (y-geometry + action
// param) and views[] (label + cooldown). Batches of 3 real actions + a trailing
// "more" band once the full list exceeds MAX_BANDS. Returns the band count.
int layoutBands(pages::Region* regionsOut, BandView* views, int page,
                uint32_t now) {
    uint8_t all[64];
    int total = buildActions(all, (int)sizeof(all));

    int numPages, start, take;
    bool more;
    int pg = 0;
    if (total <= MAX_BANDS) {
        numPages = 1; start = 0; take = total; more = false;
    } else {
        int perPage = MAX_BANDS - 1;                 // 3 real + 1 "more"
        numPages = (total + perPage - 1) / perPage;
        pg = ((page % numPages) + numPages) % numPages;
        start = pg * perPage;
        take = total - start; if (take > perPage) take = perPage;
        more = true;
    }

    int k = 0;
    for (int i = 0; i < take && k < MAX_BANDS; i++) views[k++].code = all[start + i];
    if (more && k < MAX_BANDS) views[k++].code = A_MORE;

    for (int i = 0; i < k; i++) {
        int top = BTN_TOP + i * (ROOM_BTN_H + BTN_GAP);
        regionsOut[i].y0 = (uint16_t)top;
        regionsOut[i].y1 = (uint16_t)(top + ROOM_BTN_H);
        regionsOut[i].type = 1;                      // firmware-local
        regionsOut[i].param = views[i].code;
        labelFor(views[i].code, pg, numPages, views[i].label,
                 sizeof(views[i].label));
        int ch, tot; cooldownFor(views[i].code, ch, tot);
        views[i].coolTotal = tot;
        views[i].coolLeft  = (ch >= 0) ? g_game.cooldownLeft(ch, now) : 0;
    }
    return k;
}

// ---- drawing pieces --------------------------------------------------------

// Fire + temperature state line from the official templated strings.
void drawStateLine(m5gfx::M5Canvas& c) {
    char fire[64], room[64];
    fmt1(fire, sizeof(fire), tr("the fire is {0}"), tr(FIRE_TEXT[g_game.fire]));
    fmt1(room, sizeof(room), tr("the room is {0}"), tr(TEMP_TEXT[g_game.temp]));
    int x = cjk::drawText(c, PAD, STATE_Y, fire, SCALE);
    cjk::drawText(c, x + 2 * GLYPH, STATE_Y, room, SCALE);   // gap then room
}

// Non-zero resources as "名 数量" (integer part), 2 columns x up to 5 rows.
// Overflow past the 10 cells collapses to a trailing "..." (… is not in the
// glyph closure — ASCII dots are).
void drawResources(m5gfx::M5Canvas& c) {
    const int cap = RES_ROWS * 2;               // 10 cells
    int nz = 0;
    for (int r = 0; r < RES_COUNT; r++)
        if (g_game.whole((uint8_t)r) > 0) nz++;
    bool overflow = nz > cap;
    int limit = overflow ? cap - 1 : nz;        // reserve the last cell for "..."
    int shown = 0;
    for (int r = 0; r < RES_COUNT && shown < limit; r++) {
        if (g_game.whole((uint8_t)r) <= 0) continue;
        int col = shown % 2, row = shown / 2;
        char line[48];
        snprintf(line, sizeof(line), "%s %ld",
                 tr(RES_KEY[r]), (long)g_game.whole((uint8_t)r));
        cjk::drawText(c, RES_COLX[col], RES_TOP + row * RES_ROWH, line, SCALE);
        shown++;
    }
    if (overflow) {                             // last cell = (cap-1)
        int col = (cap - 1) % 2, row = (cap - 1) / 2;
        cjk::drawText(c, RES_COLX[col], RES_TOP + row * RES_ROWH, "...", SCALE);
    }
}

// Log stream: newest on top, wrapped, filling the 5-line band. Picks how many
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

// One action band: 2px frame, centered 24px label, and (while cooling) a
// draining progress bar plus a 1-in-4 diagonal stipple "disabled" cue
// (town_page's手法).
void drawBand(m5gfx::M5Canvas& c, int top, const char* label,
              int coolLeft, int coolTotal) {
    c.drawRect(BTN_X0, top, BTN_X1 - BTN_X0, ROOM_BTN_H, TFT_BLACK);
    c.drawRect(BTN_X0 + 1, top + 1, BTN_X1 - BTN_X0 - 2, ROOM_BTN_H - 2, TFT_BLACK);

    int lw = cjk::textWidth(label, SCALE);
    cjk::drawText(c, (540 - lw) / 2, top + (ROOM_BTN_H - GLYPH) / 2 - 4, label, SCALE);

    if (coolTotal > 0 && coolLeft > 0) {
        for (int yy = top + 3; yy < top + ROOM_BTN_H - 3; yy++)
            for (int xx = BTN_X0 + 3; xx < BTN_X1 - 3; xx++)
                if (((xx + yy) & 3) == 0) c.drawPixel(xx, yy, TFT_BLACK);
        int barX0 = BTN_X0 + 12, barX1 = BTN_X1 - 12;
        int barY = top + ROOM_BTN_H - 16, barH = 8;
        c.drawRect(barX0, barY, barX1 - barX0, barH, TFT_BLACK);
        int inner = barX1 - barX0 - 4;
        int fw = (int)((int64_t)inner * coolLeft / coolTotal);   // drains L->R
        if (fw > 0) c.fillRect(barX0 + 2, barY + 2, fw, barH - 4, TFT_BLACK);
    }
}

// Paint the whole button area (clears it first) from the given band views.
void paintButtons(m5gfx::M5Canvas& c, const BandView* views, int count) {
    pages::Rect r = buttonAreaRect();
    c.fillRect(r.x, r.y, r.w, r.h, TFT_WHITE);
    for (int i = 0; i < count; i++) {
        int top = BTN_TOP + i * (ROOM_BTN_H + BTN_GAP);
        drawBand(c, top, views[i].label, views[i].coolLeft, views[i].coolTotal);
    }
}
}  // namespace

// ================================ Page API =================================

const pages::Region* RoomPage::regions(int* n) const {
    *n = m_regionCount;
    return m_regionCount ? m_regions : nullptr;
}

bool RoomPage::draw(m5gfx::M5Canvas& c) {
    c.fillSprite(TFT_WHITE);
    page_header::draw(c);            // clock header + dashed rule (every page)
    drawStateLine(c);
    drawResources(c);
    drawLog(c);
    BandView views[MAX_BANDS];
    m_regionCount = layoutBands(m_regions, views, m_page, epochNow());
    paintButtons(c, views, m_regionCount);
    return true;
}

// Long-press on a band -> the mapped engine action. Success: high beep, persist,
// full redraw; failure (cost/cooldown/locked): low beep, no redraw. "more" flips
// to the next batch. save() lives here because the engine actions do not persist
// themselves (single write — no double-save).
void RoomPage::onLocalAction(uint8_t param, int x) {
    (void)x;                          // Room bands are full-width, no ±split
    uint32_t now = epochNow();

    if (param == A_MORE) {
        m_page++;
        M5.Speaker.tone(1800, 80);
        pager::showPage(pager::currentRingIndex(), false);
        return;
    }

    Result r;
    switch (param) {
        case A_LIGHT:  r = g_game.lightFire(now);  break;
        case A_STOKE:  r = g_game.stokeFire(now);  break;
        case A_GATHER: r = g_game.gatherWood(now); break;
        case A_TRAPS:  r = g_game.checkTraps(now); break;
        default: {
            uint8_t id = (uint8_t)(param - A_CRAFT_BASE);
            if (id >= CRAFT_COUNT) { M5.Speaker.tone(600, 120); return; }
            r = craftIsBuilding(id) ? g_game.build(id) : g_game.craft(id);
            break;
        }
    }

    if (r == RC_OK) {
        M5.Speaker.tone(1800, 80);
        g_game.save();
        pager::showPage(pager::currentRingIndex(), false);
    } else {
        M5.Speaker.tone(600, 120);   // the engine may have logged the reason;
                                     // the low beep is the immediate cue
    }
}

// Time axis (awake only). Settle the economy each second, then repaint what
// changed: a content change (fire/temp/stores/log/unlocks) redraws the page
// (FAST); otherwise a wall-minute rollover refreshes the header clock (QUALITY,
// clears ghosting) and any live cooldown drains its bar in the button area
// (FAST; QUALITY on the tick it hits zero to wipe the bar's ghost). Mirrors the
// town_page / pomo_page cadence.
void RoomPage::tick(uint32_t nowMs) {
    static uint32_t s_lastTick = 0;
    static uint32_t s_lastSig  = 0;
    static int      s_lastMin  = -1;
    static bool     s_wasCooling = false;

    if (s_lastTick != 0 && nowMs - s_lastTick < 1000) return;
    s_lastTick = nowMs;

    uint32_t now = epochNow();
    g_game.settle(now);

    // Content signature — anything that alters a painted number/label.
    uint32_t sig = 2166136261u;
    auto mix = [&](uint32_t v) { sig = (sig ^ v) * 16777619u; };
    mix(g_game.fire); mix(g_game.temp);
    mix((uint32_t)(uint8_t)g_game.builderLevel);
    mix(g_game.population); mix(g_game.logCount);
    mix((g_game.outsideUnlocked ? 1u : 0u) | (g_game.craftablesUnlocked ? 2u : 0u)
        | (g_game.woodSeen ? 4u : 0u));
    for (int i = 0; i < RES_COUNT; i++) mix((uint32_t)g_game.whole((uint8_t)i));
    for (int i = 0; i < BLD_COUNT; i++) mix(g_game.buildings[i]);

    m5::rtc_time_t tm; M5.Rtc.getTime(&tm);
    bool minuteRolled = (tm.minutes != s_lastMin);
    s_lastMin = tm.minutes;

    bool cooling = false;
    for (int i = 0; i < m_regionCount; i++) {
        int ch, tot; cooldownFor(m_regions[i].param, ch, tot);
        if (ch >= 0 && g_game.cooldownLeft(ch, now) > 0) { cooling = true; break; }
    }

    if (sig != s_lastSig) {
        s_lastSig = sig;
        pager::showPage(pager::currentRingIndex(), false);   // recomputes bands
        s_wasCooling = cooling;
        return;
    }

    if (minuteRolled) {
        page_header::draw(canvas);
        pager::partialRefresh(pages::Rect{ 0, 0, 540, HDR_DIV_Y + 4 },
                              pages::RefreshMode::QUALITY);
    }

    if (cooling || s_wasCooling) {
        BandView views[MAX_BANDS];
        m_regionCount = layoutBands(m_regions, views, m_page, now);
        paintButtons(canvas, views, m_regionCount);
        bool cleared = (!cooling && s_wasCooling);
        pager::partialRefresh(buttonAreaRect(),
                              cleared ? pages::RefreshMode::QUALITY
                                      : pages::RefreshMode::FAST);
    }
    s_wasCooling = cooling;
}
