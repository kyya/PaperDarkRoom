// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "log_view.h"
#include "cjk_text.h"
#include "game_state.h"
#include "pager.h"
#include <M5GFX.h>
#include <string.h>
#include <stdio.h>

extern adr::GameState g_game;

using namespace adr;

namespace {

void fmt1(char* out, size_t cap, const char* tmpl, const char* arg) {
    const char* h = strstr(tmpl, "{0}");
    if (!h) { snprintf(out, cap, "%s", tmpl); return; }
    int pre = (int)(h - tmpl);
    snprintf(out, cap, "%.*s%s%s", pre, tmpl, arg, h + 3);
}

void fmtN(char* out, size_t cap, const char* tmpl, const char* const* args, int n) {
    size_t o = 0;
    for (const char* p = tmpl; *p && o + 1 < cap; ) {
        int idx = -1;
        if (p[0] == '{' && p[1] >= '0' && p[1] <= '9' && p[2] == '}') idx = p[1] - '0';
        if (idx >= 0 && idx < n) {
            o += (size_t)snprintf(out + o, cap - o, "%s", args[idx]);
            p += 3;
        } else {
            out[o++] = *p++;
        }
    }
    out[o < cap ? o : cap - 1] = 0;
}

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

constexpr uint16_t LOG_FADE[] = { TFT_BLACK, 0x4208, 0x8410 };
constexpr int LOG_FADE_TIERS = 3;
constexpr int LOG_FADE_BLACK = 12;
constexpr int LOG_FADE_STEP  = 2;

}  // namespace

namespace log_view {

void format(const LogEntry& e, char* out, size_t cap) {
    const char* zh = tr(e.enKey);
    char base[160];
    if (strchr(e.enKey, LOG_KEY_SEP)) {
        char seg[LOG_KEY_MAX];
        size_t o = 0;
        int idx = 0;
        for (const char* p = e.enKey; ; idx++) {
            const char* q = strchr(p, LOG_KEY_SEP);
            size_t len = q ? (size_t)(q - p) : strlen(p);
            if (len >= sizeof(seg)) len = sizeof(seg) - 1;
            memcpy(seg, p, len); seg[len] = 0;
            int w = snprintf(base + o, sizeof(base) - o, "%s%s",
                             idx > 1 ? "、" : "", tr(seg));
            if (w > 0) o += (size_t)w;
            if (!q || o >= sizeof(base)) break;
            p = q + 1;
        }
    } else if (e.hasArg && strcmp(e.enKey, "the fire is {0}") == 0) {
        uint8_t idx = (e.arg >= 0 && e.arg < 5) ? (uint8_t)e.arg : 0;
        fmt1(base, sizeof(base), zh, tr(FIRE_TEXT[idx]));
    } else if (e.hasArg && strcmp(e.enKey, "the room is {0}") == 0) {
        uint8_t idx = (e.arg >= 0 && e.arg < 5) ? (uint8_t)e.arg : 0;
        fmt1(base, sizeof(base), zh, tr(TEMP_TEXT[idx]));
    } else if (e.hasArg && (strcmp(e.enKey, "{0} short of {1}, {2} idle") == 0 ||
                            strcmp(e.enKey, "{0} back to work") == 0)) {
        uint8_t job = idleArgJob(e.arg), res = idleArgRes(e.arg);
        char num[16]; snprintf(num, sizeof(num), "%d", idleArgIdle(e.arg));
        const char* a[3] = { tr(JOB_KEY[job < JOB_COUNT ? job : 0]),
                             tr(RES_KEY[res < RES_COUNT ? res : 0]), num };
        fmtN(base, sizeof(base), zh, a, 3);
    } else if (e.hasArg && strstr(zh, "{0}")) {
        char num[16]; snprintf(num, sizeof(num), "%ld", (long)e.arg);
        fmt1(base, sizeof(base), zh, num);
    } else {
        snprintf(base, sizeof(base), "%s", zh);
    }
    if (e.count > 1) snprintf(out, cap, "%s x%u", base, (unsigned)e.count);
    else             snprintf(out, cap, "%s", base);
}

uint32_t sig(Keep keep) {
    uint32_t s = 2166136261u;
    auto mix = [&](uint32_t v) { s = (s ^ v) * 16777619u; };
    int n = 0;
    for (int i = 0; i < g_game.logCount; i++) {
        const LogEntry& e = g_game.log[i];
        if (keep && !keep(e)) continue;
        n++;
        for (const char* p = e.enKey; *p; p++) mix((uint32_t)(uint8_t)*p);
        mix((uint32_t)e.arg);
        mix((uint32_t)e.count | (e.hasArg ? 0x100u : 0u));
    }
    mix((uint32_t)n);
    return s;
}

void draw(m5gfx::M5Canvas& c, int x, int top, int w, int bottom, Keep keep) {
    const int budget = (bottom - top) / LINEH;
    if (budget <= 0) return;
    int start = g_game.logCount;
    int usedLines = 0;
    for (int i = g_game.logCount - 1; i >= 0; i--) {
        if (keep && !keep(g_game.log[i])) continue;
        char t[160]; format(g_game.log[i], t, sizeof(t));
        int lines = wrapLineCount(t, w, SCALE);
        if (usedLines + lines > budget) break;
        usedLines += lines;
        start = i;
    }
    int y = top;
    int age = 0;
    for (int i = g_game.logCount - 1; i >= start; i--) {
        if (keep && !keep(g_game.log[i])) continue;
        char t[160]; format(g_game.log[i], t, sizeof(t));
        int tier = (age < LOG_FADE_BLACK) ? 0 : 1 + (age - LOG_FADE_BLACK) / LOG_FADE_STEP;
        if (tier >= LOG_FADE_TIERS) tier = LOG_FADE_TIERS - 1;
        y = cjk::drawWrapped(c, x, y, w, t, SCALE, LINEH, LOG_FADE[tier]);
        age++;
        if (y >= bottom) break;
    }
}

pages::Rect areaRect(int top, int bottom) {
    if (bottom < top) bottom = top;
    return pages::Rect{ 0, top - 2, 540, bottom - top + 4 };
}

void pushBand(int top, int bottom) {
    pager::partialRefresh(areaRect(top, bottom), pages::RefreshMode::FAST);
}

}  // namespace log_view
