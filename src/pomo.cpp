#include "pomo.h"
#include "pager.h"
#include "frame_store.h"
#include "status_bar.h"
#include "beeper.h"
#include "power_s3.h"
#include <string.h>

extern bool g_lowBattery;   // main.cpp — gate pomo start at <=5% (SYNC-mode
                            // sleep would kill an unattended timer in seconds)

namespace pomo {

enum class Phase { IDLE, WORK, BREAK };
static Phase    s_phase      = Phase::IDLE;
static uint32_t s_startMs    = 0;   // phase start (millis)
static uint32_t s_spanMs     = 0;   // phase length
static int      s_shownMin   = -1;  // last minute value the off-page bar drew
static uint8_t  s_startParam = 0;   // WORK minutes that started this pomo
static const uint32_t BREAK_MS = 5UL * 60UL * 1000UL;

bool     active()         { return s_phase != Phase::IDLE; }
bool     inBreak()        { return s_phase == Phase::BREAK; }
bool     wantsAwake()     { return active(); }
uint8_t  startedMinutes() { return active() ? s_startParam : 0; }
uint32_t spanMs()         { return s_spanMs; }

uint32_t remainingMs() {
    if (!active()) return 0;
    uint32_t el = millis() - s_startMs;
    return el >= s_spanMs ? 0 : (s_spanMs - el);
}

int remainingMinutes() {
    uint32_t r = remainingMs();
    return r ? (int)((r + 59999UL) / 60000UL) : 0;
}

// WORK end: three rising beeps + LED burst. The accompanying full-screen
// quality refresh is the redraw at the call site — the flash IS part of the
// alert. Blocking delays total ~600ms; nothing else needs the loop then.
static void alertWorkEnd() {
    Serial.println("[pomo] WORK end -> break");
    for (int i = 0; i < 3; i++) {
        beeper::tone(1800 + i * 200, 120);
        power::setLed(255); delay(100);
        power::setLed(0);   delay(100);
    }
}

// BREAK end: two beeps + LED, back to the buttons.
static void alertBreakEnd() {
    Serial.println("[pomo] BREAK end -> idle");
    for (int i = 0; i < 2; i++) {
        beeper::tone(1500, 120);
        power::setLed(255); delay(120);
        power::setLed(0);   delay(120);
    }
}

// True when the pomo client page is the one on screen (its view owns repaint
// then — the service must NOT also poke the bar). Off the pomo page, the
// service refreshes the bar so its mini-variant tracks the minute.
static bool onPomoPage() { return strcmp(pager::currentName(), "pomo") == 0; }

// A full-screen quality redraw wherever the user is (the WORK-end flash) OR a
// plain repaint of the current page. On the pomo page this redraws the view;
// off it, the page redraw + baked bar refreshes the mini-variant.
static void redrawCurrent(bool quality) {
    if (pager::ringCount() > 0)
        pager::showPage(pager::currentRingIndex(), quality);
    else
        status_bar::draw();
}

void onLocalAction(uint8_t minutes) {
    if (!active()) {
        if (g_lowBattery) {                    // spec: no pomo at <=5% — the
            Serial.println("[pomo] start refused: low battery");
            return;                            // SYNC sleep would kill it anyway
        }
        if (minutes == 0) return;              // corrupt param — ignore
        s_phase      = Phase::WORK;
        s_startMs    = millis();
        s_spanMs     = (uint32_t)minutes * 60000UL;
        s_startParam = minutes;
        s_shownMin   = -1;
        beeper::tone(1800, 80);             // start chime
        Serial.printf("[pomo] start %u min\n", minutes);
        redrawCurrent(false);                  // button block -> MM:SS view
    } else {
        s_phase    = Phase::IDLE;
        s_shownMin = -1;
        beeper::tone(1200, 120);            // cancel: lower, longer
        Serial.println("[pomo] cancelled");
        redrawCurrent(false);                  // back to the button page
    }
}

void tick() {
    if (!active()) return;
    uint32_t el = millis() - s_startMs;
    if (el >= s_spanMs) {
        if (s_phase == Phase::WORK) {
            alertWorkEnd();
            s_phase    = Phase::BREAK;
            s_startMs  = millis();
            s_spanMs   = BREAK_MS;
            s_shownMin = -1;
            redrawCurrent(true);   // the one quality flash — also settles ghost
        } else {
            alertBreakEnd();
            s_phase    = Phase::IDLE;
            s_shownMin = -1;
            redrawCurrent(false);
        }
        return;
    }
    // Off the pomo page: repaint the bar on each minute rollover so the
    // mini-variant tracks. On the pomo page, PomoPage::tick owns all repaint.
    if (!onPomoPage()) {
        int rem = remainingMinutes();
        if (rem != s_shownMin) {
            s_shownMin = rem;
            status_bar::draw();
        }
    }
}

}  // namespace pomo
