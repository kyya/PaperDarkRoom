// A Dark Room · M5PaperS3 — standalone game firmware (fw 0.1.x).
//
// A port of the open-source game "A Dark Room" (Doublespeak Games, MPL-2.0)
// to the M5PaperS3 e-ink card. This is a STANDALONE firmware: unlike the
// dashboard build it has no host that pushes pages — the ring is
// entirely firmware-rendered game pages (client_pages). What it keeps from the
// dashboard firmware, byte for byte, is the deep-sleep model and the BLE GATT
// service (OTA / STAT / rollback / TIME_CONFIG), so `tools/ble_ota.py` can
// wirelessly cross-flash dashboard <-> game and back. The CTRL / TAP_ACK
// characteristics stay REGISTERED (the UUID set must never change — GATT-cache
// iron law) but their host-page/tap-report semantics are unused here: this loop
// simply never consumes the page-transfer / tap fields ble_link still populates.
//
// Deep-sleep model (unchanged from the dashboard firmware — every wake is a
// COLD BOOT via the BM8563 power latch; touch CANNOT wake from sleep):
//   background timer wake, nobody interacting -> advertise briefly for an OTA
//     cross-flash, then timerSleep again;
//   button / tap / USB wake -> INTERACTIVE: stay awake, sleep after 5 idle
//     minutes (battery; USB never sleeps; an OTA in flight never sleeps).
// Design: docs/adarkroom-port/research.md §3, §5.
//
// ---------------------------------------------------------------------------
// THE MSG MIGRATION, and why this file no longer calls M5.begin().
//
// The panel is driven by lib/msg (see msg_bridge.h) instead of M5GFX's
// Panel_EPD: a free-running 1bpp scan that owns core 1 and re-drives the glass
// continuously, with the UI rendering straight into its framebuffer through a
// rotated 1bpp sprite. M5.begin() would bring up Bus_EPD/Panel_EPD, which claims
// the i80 bus and the panel pins msg.c drives itself — and esp_lcd_new_i80_bus()
// aborts if the bus is taken, so it would not merely conflict, it would kill the
// boot. <M5Unified.h> is therefore included for the M5Canvas type and the TFT_*
// constants ONLY, and every runtime singleton it used to provide has a bare
// replacement compiled in beside it: touch_gt911 for M5.Touch, rtc_bm8563 for
// M5.Rtc, beeper for M5.Speaker, power_s3 for M5.Power.
//
// Two structural consequences show up below:
//   - the application runs on ITS OWN TASK PINNED TO CORE 0 (appTask), because
//     the scan owns core 1 outright; Arduino's loop task suspends itself at the
//     first opportunity and never runs again.
//   - there is no such thing as a partial panel update any more, so every path
//     that puts pixels up goes through pager::repaint() and redraws all 540x960.
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <nvs.h>
#include <esp_gap_ble_api.h>
#include <esp_ota_ops.h>
#include <soc/usb_struct.h>
#include <esp_heap_caps.h>
#include "msg_bridge.h"
#include "touch_gt911.h"
#include "rtc_bm8563.h"
#include "beeper.h"
#include "power_s3.h"
#include "frame_store.h"
#include "pager.h"
#include "ble_link.h"
#include "status_bar.h"
#include "quiet_hours.h"
#include "pomo.h"
#include "client_pages.h"
#include "game_state.h"
#include "world_state.h"       // g_world: committed map + volatile expedition (P2)
#include "setpiece_engine.h"   // landmark setpiece scene machine (P2.4)
#include "setpiece_modal.h"    // landmark setpiece overlay (P2.4)
#include "event_engine.h"
#include "event_modal.h"
#include "fight_modal.h"        // World combat overlay (P2.3)
#include "assign_page.h"        // assign_page::isOpen/close (adr:reset re-hides it)
#include "path_page.h"          // path_page::isOpen/close (adr:reset re-hides it)
#include "tech_page.h"          // tech_page::isOpen/close (adr:reset re-hides it)
#include <time.h>

#ifndef CARD_VERSION
#define CARD_VERSION "0.5.1-adarkroom"
#endif
#ifndef WAKE_INTERVAL_SECS
#define WAKE_INTERVAL_SECS 900
#endif

static const uint32_t ADVERTISE_WINDOW_MS = 15000;  // background wake: no host, sleep
static const uint32_t HARD_CAP_MS         = 45000;  // background wake: absolute ceiling
static const uint32_t IDLE_TIMEOUT_MS     = 5 * 60 * 1000;
static const int      LOW_BATTERY_PCT     = 5;

// The full-screen UI surface. NOT a sprite of its own: msg_bridge::begin() binds
// it to MSG's framebuffer at colour depth 1 and rotation 3, so it presents a
// 540x960 portrait canvas over the 960x540 landscape scan buffer and every page
// draws straight into the memory the scan is about to read. Deliberately
// constructed with NO parent — passing &M5.Display would be the usual M5Canvas
// idiom and is exactly what must not happen here (see the header note).
M5Canvas canvas;

enum class Mode { SYNC, INTERACTIVE };
static Mode     g_mode            = Mode::SYNC;
bool            g_interactive     = false;  // mirrors INTERACTIVE — ble_link.cpp
                                            // reads it (extern) to gate the
                                            // connect chime (silent background
                                            // sync must not beep).
static bool     g_wokeByTimer     = true;
bool            g_lowBattery      = false;   // pomo.cpp reads it (extern) — the
                                             // dormant pomo service's low-batt
                                             // start gate; kept for link parity.
bool            g_onUsb           = false;   // status_bar.cpp reads it (extern)
                                             // for the charge glyph.
// Display orientation, as reported in the BLE STATUS line. It is a CONSTANT now:
// the accelerometer auto-rotate (rotForAccel, a 700ms poll that flipped
// M5.Display between rotation 0 and 2 when the card was turned over) went with
// M5.begin — the IMU is behind M5.Imu and this firmware links no M5 runtime any
// more. The panel orientation is fixed by msg_bridge's UI_ROTATION instead. The
// value is kept, and kept at 2, purely so the STATUS wire format is unchanged.
static const int g_rot            = 2;
bool            g_sdOk            = false;   // ble_link.cpp reads it (extern) for
                                             // STATUS's sd= token.
// Kept so the heartbeat can report the task's stack high-water mark: 16 KB was
// reasoned about rather than measured, and the deepest chain (a page tick's
// showPage -> drawFrame -> Page::draw -> action_band -> cjk::drawWrapped -> LGFX)
// only ever runs on hardware. The number in [hb] is what turns that estimate
// into a fact.
static TaskHandle_t s_appTask = nullptr;

static uint32_t g_bootMs          = 0;
static uint32_t g_lastInteraction = 0;

// The Room+Outside game model. Loaded (or freshly started) each cold boot, its
// passive economy settled forward from the RTC (offline income/population while
// the card slept), and persisted before sleep. room_page wiring is the next
// task; this is the minimal init/settle/save spine that keeps it linked and the
// save file live. See game_state.h.
adr::GameState g_game;

// The Phase-2 World model: the committed 61x61 map (world.bin) plus any volatile
// expedition (trek.bin). restore()d each cold boot (below), embarked into by the
// Path page. The World rendering + move loop page lands in milestone 2.2; here it
// is only kept live so the Path page's embark can deduct stores, fill hp/water,
// and write trek.bin — the committed map persists across sleeps via restore().
adr::WorldState g_world;

// RTC -> Unix epoch (seconds). Only differences matter to settle(), so the
// timezone mktime assumes is irrelevant as long as it's consistent.
static uint32_t epochNow() {
    rtc::Date d; rtc::Time t;
    rtc::getDateTime(&d, &t);
    struct tm tmv = {};
    tmv.tm_year = d.year - 1900;
    tmv.tm_mon  = d.month - 1;
    tmv.tm_mday = d.date;
    tmv.tm_hour = t.hours;
    tmv.tm_min  = t.minutes;
    tmv.tm_sec  = t.seconds;
    time_t e = mktime(&tmv);
    return e > 0 ? (uint32_t)e : 0;
}

// BM8563 alarm/timer IRQ flag, read first thing at boot. Set -> this
// cold boot came from the RTC alarm (timerSleep); clear -> a human pressed the
// power button. Fallback on any doubt: timer (conservative).
static bool wokeByRtcTimer() {
    bool irq = rtc::getIRQstatus();
    Serial.printf("[boot] rtc_irq=%d -> wake=%s\n", irq ? 1 : 0,
                  irq ? "timer" : "button");
    rtc::clearIRQ();
    return irq;
}

static void enterInteractive(const char* why) {
    if (g_mode == Mode::INTERACTIVE || g_lowBattery) return;
    g_mode = Mode::INTERACTIVE;
    g_interactive = true;
    g_lastInteraction = millis();
    Serial.printf("[fsm] -> interactive (%s)\n", why);
}

// Quiet-hours window, NVS namespace "cfg" keys "qh_start"/"qh_end" (minutes
// since midnight, u16). Survives timerSleep's full power-off; a missing/corrupt
// namespace reads back as 0/0 (disabled).
static const char* NVS_CFG_NS = "cfg";

static void loadQuietHours(uint16_t& startMin, uint16_t& endMin) {
    startMin = endMin = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_CFG_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint16_t s = 0, e = 0;
    if (nvs_get_u16(h, "qh_start", &s) == ESP_OK) startMin = s;
    if (nvs_get_u16(h, "qh_end", &e) == ESP_OK) endMin = e;
    nvs_close(h);
}

static void saveQuietHoursIfChanged(uint16_t startMin, uint16_t endMin) {
    uint16_t curStart, curEnd;
    loadQuietHours(curStart, curEnd);
    if (curStart == startMin && curEnd == endMin) return;
    nvs_handle_t h;
    if (nvs_open(NVS_CFG_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, "qh_start", startMin);
    nvs_set_u16(h, "qh_end", endMin);
    nvs_commit(h);
    nvs_close(h);
    Serial.printf("[cfg] quiet hours -> %02u:%02u-%02u:%02u\n",
                  startMin / 60, startMin % 60, endMin / 60, endMin % 60);
}

// Apply a pending TIME_CONFIG write: RTC set + quiet-hours NVS save. Keeping the
// RTC correct matters here too — offline economy settlement (a later task) is
// driven by RTC epoch differences.
static void applyPendingTimeConfig() {
    if (!ble_link::rx.timeCfgPending) return;
    ble_link::rx.timeCfgPending = false;
    rtc::Date d;
    d.year    = 2000 + ble_link::rx.rtcYearSince2000;
    d.month   = ble_link::rx.rtcMonth;
    d.date    = ble_link::rx.rtcDay;
    d.weekDay = 0;
    rtc::Time t;
    t.hours   = ble_link::rx.rtcHour;
    t.minutes = ble_link::rx.rtcMinute;
    t.seconds = ble_link::rx.rtcSecond;
    rtc::setDateTime(&d, &t);
    saveQuietHoursIfChanged(ble_link::rx.quietStartMin, ble_link::rx.quietEndMin);
    Serial.printf("[rtc] set 20%02u-%02u-%02u %02u:%02u:%02u\n",
                  ble_link::rx.rtcYearSince2000, ble_link::rx.rtcMonth,
                  ble_link::rx.rtcDay, ble_link::rx.rtcHour,
                  ble_link::rx.rtcMinute, ble_link::rx.rtcSecond);
}

// Apply a pending debug game command (the BLE CTRL "adr:" intercept — see
// ble_link CtrlCb). Verbs: `adr:give <res> <amount>` injects <amount> whole
// units of the RES_KEY-named resource (a dev/testing aid — "蓝牙直接推铁"); a
// multi-word RES_KEY ("cured meat") is sent with '_' for the space
// ("cured_meat") and un-escaped back here before matching. `adr:reset` (no args)
// is the GM wipe — factory reset for a fresh playthrough. Same capture-in-
// callback / act-in-loop split as applyPendingTimeConfig; the engine write +
// save + repaint all live here, off the BLE callback.
static void applyPendingGameCmd() {
    if (!ble_link::rx.gameCmdPending) return;
    ble_link::rx.gameCmdPending = false;

    // adr:reset — GM factory wipe. Delete the save (+ any stray atomic-write
    // tmp) and reset the model to a brand-new dark room. init() covers EVERY
    // persistent field (stores/buildings/items/workers, fire/temp/builder, the
    // v0.4.3 seen/craftShown bitsets, and the event scheduler fields), so no
    // stale state survives. Deliberately NO save(): we leave "no save file" so
    // the device sits in the same state a first-ever boot would, and the next
    // player action lands the first fresh write. A power loss before then just
    // boots a new game — identical outcome, so the un-saved window is safe.
    // Reset hides Outside/Trade/Assign (outsideUnlocked=false), so we can't
    // refresh the current page — jump explicitly to the always-visible Room.
    if (strcmp(ble_link::rx.gameCmd, "adr:reset") == 0) {
        SD.remove(ADR_SAVE_PATH);            // primary save file
        SD.remove(ADR_SAVE_PATH ".tmp");     // stray tmp from a torn atomic write
        g_game.init();                       // factory state (all fields)
        events::reset();                     // drop any RAM-only on-screen event latch
        if (assign_page::isOpen()) assign_page::close();   // its ring slot re-hides
        if (path_page::isOpen()) path_page::close();       // ditto for the Path sub-page
        if (tech_page::isOpen()) tech_page::close();       // ditto for the tech-tree sub-page
        // World (P2.2) is now a visible ring slot gated on the trek / committed
        // map — a factory wipe must drop it too, else a stale expedition would
        // survive the reset and re-render over the wiped game. init() clears the
        // RAM state; clearTrek() + removing world.bin drop the SD layers that
        // g_world.restore() would otherwise reload on the next boot.
        g_world.init();
        g_world.clearTrek();                 // remove trek.bin (volatile expedition)
        SD.remove(ADR_WORLD_PATH);           // remove the committed map
        SD.remove(ADR_WORLD_PATH ".tmp");    // stray tmp from a torn atomic write
        fight_modal::endForSleep();          // drop a live combat overlay's guard
        setpiece_modal::endForSleep();       // and a live setpiece overlay's guard
                                             // (else their active() blocks the room jump)
        beeper::tone(1800, 80);
        int room = pager::ringIndexByName("room");
        if (room >= 0) pager::showPage(room, false);
        Serial.println("[cmd] reset -> save wiped, new game, jumped to room");
        return;
    }

    char res[24]; int amount = 0;
    // %23s stops at whitespace; the host sends multi-word keys with '_' for the
    // space (so they stay one token), un-escaped just below before the strcmp.
    if (sscanf(ble_link::rx.gameCmd, "adr:give %23s %d", res, &amount) != 2) {
        Serial.printf("[cmd] parse fail: '%s'\n", ble_link::rx.gameCmd);
        beeper::tone(600, 120);
        return;
    }
    // Injection bound: 1..1e6 whole units. 1e6 × FP(100) = 1e8 per write, well
    // inside int32 stores (max ~2.1e9), so a single inject can't overflow.
    if (amount < 1 || amount > 1000000) {
        Serial.printf("[cmd] amount out of range (1..1000000): %d\n", amount);
        beeper::tone(600, 120);
        return;
    }
    for (char* q = res; *q; q++) if (*q == '_') *q = ' ';   // "cured_meat" -> "cured meat"
    int r = -1;
    for (int i = 0; i < adr::RES_COUNT; i++)
        if (strcmp(res, adr::RES_KEY[i]) == 0) { r = i; break; }
    if (r < 0) {
        Serial.printf("[cmd] unknown resource: '%s'\n", res);
        beeper::tone(600, 120);
        return;
    }
    g_game.stores[r] += amount * adr::FP;         // stores are fixed-point × FP
    if (g_game.stores[r] < 0) g_game.stores[r] = 0;   // never leave it negative
    g_game.markSeen((uint8_t)r);   // injected == "owned": unlock its craft/buy gates
    g_game.save();
    beeper::tone(1800, 80);
    pager::showPage(pager::currentRingIndex(), false);
    Serial.printf("[cmd] give %s %d -> stores[%d]=%ld\n",
                  res, amount, r, (long)g_game.stores[r]);
}

// ---- OTA app-level rollback (NVS namespace "ota") ------------------------
// Same poor-man's rollback as the dashboard firmware: ble_link stamps
// pending=1/boots=0 before rebooting into a freshly-received image;
// otaRollbackCheck() counts each cold boot and, at 3 straight boots that never
// reach BLE-ready, flips the boot partition back. otaConfirmHealthy() clears
// the markers once we advertise (the image proved itself). This is the second
// safety net under the cross-flash lifeline (USB is the last-ditch one).
static const char* NVS_OTA_NS = "ota";

static void otaRollbackCheck() {
    nvs_handle_t h;
    if (nvs_open(NVS_OTA_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t pending = 0;
    if (nvs_get_u8(h, "pending", &pending) != ESP_OK || !pending) {
        nvs_close(h);
        return;
    }
    uint8_t boots = 0;
    nvs_get_u8(h, "boots", &boots);
    boots++;
    nvs_set_u8(h, "boots", boots);
    nvs_commit(h);
    Serial.printf("[ota] pending image, boot attempt %u/3\n", boots);
    if (boots >= 3) {
        const esp_partition_t* prev = esp_ota_get_next_update_partition(NULL);
        nvs_set_u8(h, "pending", 0);
        nvs_set_u8(h, "boots", 0);
        nvs_commit(h);
        nvs_close(h);
        Serial.printf("[ota] 3 failed boots -> rolling back to %s\n",
                      prev ? prev->label : "?");
        if (prev) {
            esp_ota_set_boot_partition(prev);
            delay(50);
            esp_restart();
        }
        return;
    }
    nvs_close(h);
}

static void otaConfirmHealthy() {
    nvs_handle_t h;
    if (nvs_open(NVS_OTA_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t pending = 0;
    esp_err_t r = nvs_get_u8(h, "pending", &pending);
    nvs_close(h);
    if (r != ESP_OK || !pending) return;
    if (nvs_open(NVS_OTA_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "pending", 0);
    nvs_set_u8(h, "boots", 0);
    nvs_commit(h);
    nvs_close(h);
    Serial.println("[ota] image confirmed healthy — cleared rollback markers");
}

static void sleepNow(const char* reason) {
    // If a random event is on screen, click its no-cost default (safe exit) so we
    // never power off mid-choice (research.md §5.4). The save() below persists the
    // resulting state; the modal is RAM-only and gone after the cold-boot wake.
    events::dismissDefault();
    // ...and drop the overlay that was showing it. dismissDefault() ends the
    // ENGINE's event, not the modal, and an overlay left active renders a page
    // with no title and no buttons — which the deghost below would then freeze
    // onto the glass as a blank sheet for the whole sleep.
    event_modal::endForSleep();
    // A live fight can't be safely paused: combat is RAM-only and wasn't re-saved
    // mid-fight, so releasing the overlay here treats a forced sleep as a flee (a
    // cold boot resumes the pre-fight tile). In practice this is only reachable on
    // the background hard-cap — interactive mode stays awake through combat
    // (anyWantsAwake). Release the guard so the deghost below can repaint.
    fight_modal::endForSleep();
    setpiece_modal::endForSleep();   // same for a live setpiece overlay (abandon)
    uint32_t sleepSecs = WAKE_INTERVAL_SECS;
    uint16_t qStart, qEnd;
    loadQuietHours(qStart, qEnd);
    if (quiet_hours::enabled(qStart, qEnd)) {
        rtc::Date d; rtc::Time t;
        rtc::getDateTime(&d, &t);
        uint16_t nowMin = (uint16_t)t.hours * 60 + t.minutes;
        if (quiet_hours::inWindow(nowMin, qStart, qEnd)) {
            sleepSecs = quiet_hours::secondsUntilEnd(nowMin, (uint8_t)t.seconds, qEnd);
            Serial.printf("[sleep] quiet hours %02u:%02u-%02u:%02u — sleeping %lus\n",
                          qStart / 60, qStart % 60, qEnd / 60, qEnd % 60,
                          (unsigned long)sleepSecs);
        }
    }
    g_game.save();                     // persist the game before power-off
    // Leave a CLEAN picture behind — unconditionally, not on the debt threshold
    // the in-session escape valve uses: the frame
    // the user is left staring at for the next 15 minutes is the one thing on
    // this device that is on show while nothing is running, and 400ms of an
    // already-terminal wake is free. Then silence the buzzer (a note left
    // sounding through a power-off is the worst possible exit state) and park the
    // scan, which drops the panel rails — the ink stays, e-ink is bistable, but
    // nothing is being driven while the power latch is released.
    pager::deghost();
    beeper::stop();
    msg_bridge::waitSettled(2000);
    msg_bridge::park();
    Serial.printf("[sleep] %s — timerSleep(%lus)\n", reason, (unsigned long)sleepSecs);
    Serial.flush();
    delay(40);
    power::timerSleep(sleepSecs);     // full power-off; no return
    esp_restart();
}

// USB SOF activity on the OTG controller — a live host sends a Start-Of-Frame
// every 1ms and the device latches the frame number. Diagnostic candidate.
static bool usbSofActive() {
    uint32_t a = (USB0.dsts >> 8) & 0x3FFF;
    delay(12);
    uint32_t b = (USB0.dsts >> 8) & 0x3FFF;
    return a != b;
}

extern "C" bool tud_mounted(void);
extern "C" bool tud_suspended(void);

// External power detection. isCharging (CHG_STAT) has a blind spot (a full
// battery pauses the charger -> reads false on USB); tud_mounted() closes it (a
// live host keeps TinyUSB enumerated).
static bool usbPresent() {
    if (power::isCharging()) return true;
    return tud_mounted();
}

// SD over SPI. The pin numbers came from M5.getPin(m5::pin_name_t::sd_spi_*)
// and are now literals, copied from M5Unified's own _pin_table_sd row for
// board_M5PaperS3 — there is no M5 runtime left to ask (see the header note).
static const int SD_SCK = 39, SD_MOSI = 38, SD_MISO = 40, SD_CS = 47;

static bool sdInit() {
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    return SD.begin(SD_CS, SPI, 20000000);
}

// Everything the old setup() did apart from bringing the panel up, run on the
// app task (core 0) rather than on Arduino's loop task (core 1, which the scan
// owns). Nothing here is order-sensitive with respect to that move: the panel is
// already scanning by the time this starts, and the first frame is the boot
// restore at the bottom.
static void appSetup() {
    g_bootMs = millis();

    // Bare hardware, in the order the rest of this function needs it: the RTC
    // (whose IRQ flag is the very next thing read), the digitizer, the buzzer and
    // the power/battery pins. rtc::init() and touch::init() share I2C_NUM_1 and
    // each tolerates the other having installed the driver already.
    rtc::init();
    touch::init();
    beeper::init();
    power::init();

    g_wokeByTimer = wokeByRtcTimer();       // reads the RTC IRQ flag FIRST (wake reason)

    // Cancel any BM8563 (PCF8563) countdown timer still armed from the previous
    // timerSleep(). timerSleep() arms a REPEATING countdown (RTC_Class::
    // setAlarmIRQ(int) -> setTimerIRQ -> reg 0x0E TE bit); wokeByRtcTimer()'s
    // clearIRQ() above clears only the fired FLAG (reg 0x01 bits 0x0C), leaving
    // the timer running. So after a MANUAL power-off (power button while awake)
    // the battery-backed timer keeps counting and fires again ~WAKE_INTERVAL_SECS
    // later, pulling the device back on — the "关机后约 15 分钟自动开机" bug.
    // disableIRQ() fully disarms timer+alarm+INT-enable. MUST run AFTER the
    // getIRQstatus read above (先判断后撤销). The scheduled-sleep path re-arms it
    // every sleep (power::timerSleep -> disableIRQ+clearIRQ+setTimerIRQ), so
    // periodic wakes are unaffected; only a hand power-off now stays off.
    rtc::disableIRQ();

    Serial.printf("[boot] adarkroom v%s interval=%ds reset=%d\n",
                  CARD_VERSION, WAKE_INTERVAL_SECS, (int)esp_reset_reason());

    // Before anything heavy: count this boot toward OTA probation / roll back.
    otaRollbackCheck();

    // The canvas is already bound to MSG's framebuffer (msg_bridge::begin, run
    // by appTask before this) — no createSprite, no colour-depth choice, no
    // rotation to set. What used to be a 540x960 grayscale_8bit sprite (518 KB
    // of PSRAM) is now a 64800-byte view onto the scan buffer itself.
    canvas.fillSprite(TFT_WHITE);

    g_onUsb = usbPresent();
    int bat = power::batteryLevel();
    g_lowBattery = (bat >= 0 && bat <= LOW_BATTERY_PCT);
    Serial.printf("[boot] chg=%d mnt=%d onUsb=%d bat=%d%% v=%dmV low=%d\n",
                  power::isCharging() ? 1 : 0, tud_mounted() ? 1 : 0,
                  g_onUsb ? 1 : 0, bat,
                  power::batteryVoltage(), g_lowBattery ? 1 : 0);

    g_sdOk = sdInit();
    Serial.printf("[boot] sd %s\n", g_sdOk ? "ok" : "none");
    if (g_sdOk) SD.mkdir("/.darkroom");   // parent dir; mkdir on existing dir just returns false
    frame_store::init(g_sdOk);

    // Load the save (or start a new dark room), then settle the passive economy
    // forward across however long we slept (RTC epoch diff, 10s steps, 24h cap).
    if (!g_game.load()) {
        g_game.init();
        Serial.println("[game] new game (no save)");
    }
    uint32_t nowE = epochNow();
    uint32_t steps = g_game.settle(nowE, /*offline=*/true);   // deep-sleep catch-up: fire frozen (§5.3)
    Serial.printf("[game] settled %lu step(s); fire=%d temp=%d builder=%d pop=%u wood=%ld\n",
                  (unsigned long)steps, g_game.fire, g_game.temp,
                  g_game.builderLevel, g_game.population, (long)g_game.whole(adr::R_WOOD));

    // Restore the committed World map (world.bin) and resume any interrupted
    // expedition (trek.bin). No-op when neither exists (a fresh game); the first
    // Path embark lazily generates the map. Rendering the resumed expedition is
    // milestone 2.2 — for now this just keeps the committed map alive across sleeps.
    bool trekActive = g_world.restore();
    Serial.printf("[game] world: generated=%d trek=%s\n",
                  g_world.generated ? 1 : 0, trekActive ? "active" : "none");

    // Bind the random-event engine to the settled model. Its scheduler state
    // (nextEventAt / delayed echo) is persisted in GameState; bind() only wires
    // the pointer and clears the RAM-only "event on screen" flag. Events fire
    // only while awake (research.md §5.4); the offline echo was already redeemed
    // by settle() above.
    events::bind(&g_game);

    // Bind the landmark setpiece engine to the World + game models (P2.4). Like the
    // event engine it is pure/UI-independent; the setpiece_modal drives it and
    // hands its combat scenes to fight_modal.
    setpiece::bind(&g_world, &g_game);

    // The ring is all client (game) pages — no host pages exist. Restore the
    // last-shown game page by NAME and paint it in quality mode (a cold-boot
    // redraw isn't latency-sensitive and clears ghosting).
    // Prime the bar's cached clock/battery before anything draws: drawTo() is a
    // pure reader now, so an unsampled bar would render --:-- and --% into the
    // boot frame and hold it there until the first 1s tick.
    status_bar::sample();

    if (pager::ringCount() > 0) {
        pager::restore(true);
        Serial.printf("[boot] restored page %d/%d\n",
                      pager::currentRingIndex(), pager::ringCount());
    }
    status_bar::draw();

    for (int i = 0; i < 2; i++) {   // "I just woke" heartbeat blink
        power::setLed(255); delay(90);
        power::setLed(0);   delay(90);
    }

    ble_link::init("M5PaperS3");

    // BLE-ready — clear any OTA rollback markers (the new image works).
    otaConfirmHealthy();

    // The memory line that actually means something. The one printed by
    // msg_bridge::init() is taken before Bluedroid exists, so it describes a
    // budget nobody is competing for yet; this one is measured with the radio up
    // and the stack allocated, which is the state the device lives in.
    Serial.printf("[boot] internal free after ble=%u B  psram free=%u B\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    {
        int bonds = esp_ble_get_bond_device_num();
        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) == ESP_OK)
            Serial.printf("[diag] bonds=%d nvs_used=%d nvs_free=%d nvs_total=%d ns=%d\n",
                          bonds, (int)st.used_entries, (int)st.free_entries,
                          (int)st.total_entries, (int)st.namespace_count);
        else
            Serial.printf("[diag] bonds=%d nvs_stats=err\n", bonds);
    }

    g_lastInteraction = millis();
    if (g_onUsb) enterInteractive("usb");
    else if (!g_wokeByTimer) enterInteractive("button wake");
    Serial.printf("[boot] mode=%s ready\n",
                  g_mode == Mode::INTERACTIVE ? "interactive" : "sync");
}

// One pass of the application loop. Body-for-body the old loop(), minus the
// accelerometer auto-rotate block (~700ms poll, redraw on a flip) which went
// with M5.Imu — see g_rot.
static void appLoop() {
    // Poll the digitizer and expire any sounding note. Between them these are
    // what M5.update() used to do for this firmware.
    uint32_t now = millis();
    touch::update(now);
    beeper::tick(now);

    // Touch first: any tap = interaction — page + open/extend the wake window.
    if (pager::handleTouch()) {
        g_lastInteraction = now;
        enterInteractive("tap");
    }

    // STATUS every 2s while connected (a one-shot is missed mid-pairing), or
    // immediately when the host just subscribed to STAT (statSubPending) so a
    // host timing out on its first STATUS can't misjudge the protocol version.
    // The tap= token is still emitted by sendStatus, harmless here (no page has
    // regions yet) — kept for wire-format parity with the dashboard firmware.
    static uint32_t s_lastStatus = 0;
    bool freshSub = ble_link::rx.statSubPending;
    if (ble_link::rx.connected &&
        (freshSub || (now - ble_link::rx.connectMs > 300 && now - s_lastStatus > 2000))) {
        ble_link::rx.statSubPending = false;
        ble_link::sendStatus(CARD_VERSION, g_onUsb, g_rot,
                             g_mode == Mode::INTERACTIVE);
        s_lastStatus = now;
    }

    // TIME_CONFIG landed: RTC set + quiet-hours NVS save.
    applyPendingTimeConfig();

    // Debug game command landed (BLE CTRL "adr:" intercept): inject resources.
    applyPendingGameCmd();

    // Dormant pomo service tick (no PomoPage / pomo region exists in this
    // firmware, so it never activates) + the current page's time axis. Kept so
    // the reused ble_link/status_bar link cleanly; both are guarded no-ops here.
    pomo::tick();
    pager::tickCurrent(now);

    // Random-event engine (research.md §4.1/§5.4), page-independent:
    //  - drive the scheduler ~1s (RTC epoch = scheduling/echo clock);
    //  - the instant an event activates, pop its modal (once, QUALITY draw);
    //  - run the modal's 2-minute idle-timeout watchdog every pass.
    // The modal's own active() guard keeps pager pushes/ticks off the panel while
    // it's up; anyWantsAwake() (via events::active()) keeps the card awake here.
    static uint32_t s_evTick = 0;
    if (now - s_evTick >= 1000) {
        s_evTick = now;
        events::tick(now, epochNow(), g_world.trekActive());
    }
    // A random event never pops OVER a live fight (the fight owns the panel); a
    // queued event waits and shows once the fight ends. The combat overlay drives
    // its own 1s tick here (pager::tickCurrent no-ops while it's active), the
    // per-second clock the enemy swings + weapon cooldowns run on.
    if (events::active() && !event_modal::active() && !fight_modal::active() &&
        !setpiece_modal::active())
        event_modal::show(now);
    event_modal::checkTimeout(now);
    if (fight_modal::active())
        fight_modal::tick(now);
    // Landmark setpiece overlay (P2.4): its own 2-minute idle watchdog. The combat
    // it may interleave is ticked by fight_modal above; checkTimeout no-ops while
    // that fight is live (the fight owns the clock).
    setpiece_modal::checkTimeout(now);

    // OTA: on the last streamed byte, verify + commit + reboot (never returns)
    // or report ota=err. No-op unless an OTA transfer just finished. THE
    // cross-flash lifeline — untouched from the dashboard firmware.
    ble_link::otaPollFinish(CARD_VERSION, g_onUsb, g_rot,
                            g_mode == Mode::INTERACTIVE);

    // OTA frame, in the one shape the park allows: ONE static frame, drawn
    // before the panel driver stops, and left alone until the transfer ends.
    //
    // The BEGIN callback is blocked waiting for this (ble_link.h's handshake) —
    // it cannot draw the frame itself, and it must not park until the frame is
    // up, because after the park nothing reaches the glass. There is deliberately
    // no periodic progress bar: refreshing one would mean handing the park back
    // and forth between this task and the BLE callback once a second, and
    // lib/msg's ownership axiom is explicit that a park is single-owner with
    // handoff, not something to ping-pong. The upgrade path, if a live bar is
    // ever wanted, is to move the whole transfer onto this task so that park and
    // resume share one owner — not to interleave two.
    static bool s_otaBarActive = false;
    if (ble_link::otaFrameWanted()) {
        status_bar::drawOtaProgress(0, ble_link::otaTotal());
        s_otaBarActive = true;
        ble_link::otaFrameReady();      // releases the BEGIN callback to park
    } else if (s_otaBarActive && !ble_link::otaBusy()) {
        s_otaBarActive = false;
        // Unconditional: status_bar::draw() is what clears the bar's sticky
        // s_otaPct, so gating it on the link being up (as this used to) left the
        // progress variant latched forever when an OTA died BY disconnecting —
        // which is the common way one dies. Every subsequent frame would then
        // draw a frozen progress bar where the page dots belong. This is also
        // the far end of the disconnect unwind: otaPollFinish has by now aborted
        // the write and resumed the scan, so this repaint is what actually
        // reaches the glass again.
        status_bar::draw();
    }

    // NOTE: no host-page handling here. ble_link still populates rx.total/off/
    // complete when a CTRL/DATA write arrives (the characteristics stay
    // registered — GATT iron law), but this firmware has no server pages, so
    // those page-transfer fields are simply never consumed. Only the OTA path
    // above drains the DATA stream.

    // Live power recheck: plugging in opens the window; unplugging arms sleep.
    g_onUsb = usbPresent();
    if (g_onUsb) enterInteractive("usb");

    if (!g_onUsb) {
        if (ble_link::otaBusy()) {
            // OTA in flight: never sleep — the reboot (ota=ok) or a disconnect
            // abort ends this wake instead of timerSleep cutting the transfer.
        } else if (g_mode == Mode::SYNC) {
            // Background timer wake, nobody interacting and no host pages to
            // receive. Stay up only long enough to advertise for an OTA
            // cross-flash: sleep once the advertise window elapses with no
            // connection, or at the hard cap if a connection lingers idle.
            if (now - g_bootMs > HARD_CAP_MS) {
                sleepNow("hard cap");
            } else if (!ble_link::rx.connected &&
                       now - g_bootMs > ADVERTISE_WINDOW_MS) {
                sleepNow("no host in window");
            }
        } else {  // INTERACTIVE
            if (!client_pages::anyWantsAwake() &&
                now - g_lastInteraction > IDLE_TIMEOUT_MS)
                sleepNow("idle 5min");
        }
    }

    // 1s bar tick: keep the self-drawn clock/battery/USB fresh (the host can't —
    // there is none). Repaint only on a visible change, and only while a page is
    // on screen and no OTA owns the bar.
    // `ble` joined this change set when the BLE callbacks stopped drawing: a
    // connect/disconnect used to repaint from the Bluedroid task, which
    // deadlocked the app task against msg_flip()'s single waiter slot (see
    // ble_link.cpp's ConnectCb). Watching the flag here is what still gets the
    // glyph on and off the bar, one whole-frame repaint, on this task, within a
    // second — and it replaces BOTH of the calls the disconnect path used to
    // make, since a repaint now redraws page and bar together anyway.
    static uint32_t s_barTick = 0;
    static int s_barMin = -1, s_barUsb = -1, s_barBat = -1, s_barBle = -1;
    if (now - s_barTick > 1000) {
        s_barTick = now;
        // The one sampler, once a second (status_bar.h). Everything read below
        // is the value it just cached — the same value drawTo() will render, so
        // "repaint only on a visible change" is now actually true.
        status_bar::sample();
        int min = status_bar::clockMinute();
        int usb = g_onUsb ? 1 : 0;
        int bat = status_bar::batteryPercent();
        int ble = ble_link::rx.connected ? 1 : 0;
        if ((min != s_barMin || usb != s_barUsb || bat != s_barBat ||
             ble != s_barBle) &&
            pager::ringCount() > 0 && !ble_link::otaBusy())
            status_bar::draw();
        s_barMin = min; s_barUsb = usb; s_barBat = bat; s_barBle = ble;
    }

    static uint32_t s_hb = 0;
    if (now - s_hb > 3000) {
        s_hb = now;
        bool sof = usbSofActive();
        int mnt = tud_mounted() ? 1 : 0;
        int sus = tud_suspended() ? 1 : 0;
        ble_link::setUsbDiag(power::isCharging() ? 1 : 0,
                             sof ? 1 : 0, mnt * 10 + sus);
        // The scan statistics ride the heartbeat — the only periodic line on the
        // wire, and the only evidence of whether the panel driver is healthy.
        //
        // HOW TO READ IT. Do NOT judge `fps`/`avg` against msg.c's 60 Hz pacer
        // target: this panel's steady state through the esp_lcd transport is
        // ~23 ms a frame (~43 Hz), measured here by the spike and independently
        // by PaperBar on the same hardware, so a "30% overrun" against 16.7 ms
        // is simply what normal looks like (msg_bridge.h argues this at length).
        // The baseline is whatever `avg` settles at. What actually indicates
        // trouble is `max` pulling away from that settled `avg` — one frame
        // overran, and the gap says by how much — or any `dmato` at all, which
        // means a line's DMA completion was lost and rows were dropped.
        //
        // internal-SRAM free is printed alongside because the real memory
        // pressure is here, not at boot: msg's precomputed output frame holds
        // ~138 KB of internal SRAM outright and Bluedroid wants the rest, so the
        // figure logged before BLE came up says nothing about the margin.
        char msg[96];
        msg_bridge::statsLine(now, msg, sizeof(msg));
        Serial.printf("[hb] mode=%s awake=%lums ble=%d sec=%d page=%d/%d "
                      "iheap=%u stack=%u | %s\n",
                      g_mode == Mode::INTERACTIVE ? "int" : "sync",
                      (unsigned long)(now - g_bootMs),
                      ble_link::rx.connected ? 1 : 0,
                      ble_link::rx.secure ? 1 : 0,
                      pager::currentRingIndex(), pager::ringCount(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                      (unsigned)(s_appTask ? uxTaskGetStackHighWaterMark(s_appTask)
                                           : 0),
                      msg);
    }
    delay(5);
}

// The application task. Core 0, because msg.c pins its scan to core 1 and the
// whole design of that driver is that the scan owns its core outright — every
// line of application code has to be somewhere else.
//
// Stack: the deepest call chain here is a page draw (cjk::drawWrapped over the
// 12px CJK face, through LovyanGFX) inside a boot restore that also has SD and
// NVS frames below it. 16 KB is comfortable; the old code ran the same chain on
// Arduino's 8 KB loop task and the extra headroom costs nothing we need.
static void appTask(void* arg) {
    (void)arg;
    msg_bridge::begin(canvas);
    appSetup();
    while (true) appLoop();
}

void setup() {
    Serial.begin(115200);

    // Wait for the USB host, but ONLY if there is one. TinyUSB CDC needs a beat
    // after enumeration or the boot banner and the driver's selftest line are
    // lost — and that selftest line is the only evidence the SIMD row kernel was
    // ever validated on this silicon, so it is worth waiting for. What it is not
    // worth is a flat delay: every wake on this board is a cold boot (the BM8563
    // latch cuts power outright), so a fixed 3s would be charged to every one of
    // the 96 background wakes a day AND to the user's own power-button press,
    // where it is 3 seconds of blank panel before the game appears. Poll for the
    // host instead and give up quickly when running on battery.
    uint32_t t0 = millis();
    while (millis() - t0 < 700 && !tud_mounted()) delay(20);
    if (tud_mounted()) delay(900);

    Serial.println();
    Serial.printf("[boot] ==== PaperDarkRoom %s (MSG 1bpp) ====\n", CARD_VERSION);

    // The panel first and alone: msg_init() claims the i80 bus and the panel
    // pins, and nothing else in this firmware may touch them.
    if (!msg_bridge::init()) {
        // No panel, so nothing can be shown and appTask is never created — which
        // means no BLE to recover through and no sleepNow to reach. Spinning here
        // would drain the cell flat with no wireless path back in, so arm the RTC
        // and power off instead: the next scheduled wake is a fresh boot, and a
        // one-off allocation failure costs one cycle rather than the battery.
        // power::timerSleep does the RTC arming itself and does not return.
        Serial.println("[boot] FATAL: no panel — sleeping to the next wake");
        Serial.flush();
        rtc::init();
        power::init();
        power::timerSleep(WAKE_INTERVAL_SECS);
    }

    xTaskCreatePinnedToCore(appTask, "adr", 16384, NULL, 1, &s_appTask, 0);
}

void loop() {
    // Core 1 belongs to the scan and core 0 to appTask; Arduino's loop task has
    // no work and must not sit spinning against either.
    vTaskSuspend(NULL);
}
