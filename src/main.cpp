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
#include <Arduino.h>
#include <M5Unified.h>
#include <SPI.h>
#include <SD.h>
#include <nvs.h>
#include <esp_gap_ble_api.h>
#include <esp_ota_ops.h>
#include <soc/usb_struct.h>
#include <lgfx/v1/touch/Touch_GT911.hpp>
#include "frame_store.h"
#include "pager.h"
#include "ble_link.h"
#include "status_bar.h"
#include "quiet_hours.h"
#include "pomo.h"
#include "client_pages.h"
#include "game_state.h"
#include "event_engine.h"
#include "event_modal.h"
#include "assign_page.h"        // assign_page::isOpen/close (adr:reset re-hides it)
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

static const int PANEL_W = 540;
static const int PANEL_H = 960;

M5Canvas canvas(&M5.Display);

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
static int      g_rot             = 2;
bool            g_sdOk            = false;   // ble_link.cpp reads it (extern) for
                                             // STATUS's sd= token.
static uint32_t g_bootMs          = 0;
static uint32_t g_lastInteraction = 0;

// The Room+Outside game model. Loaded (or freshly started) each cold boot, its
// passive economy settled forward from the RTC (offline income/population while
// the card slept), and persisted before sleep. room_page wiring is the next
// task; this is the minimal init/settle/save spine that keeps it linked and the
// save file live. See game_state.h.
adr::GameState g_game;

// RTC -> Unix epoch (seconds). Only differences matter to settle(), so the
// timezone mktime assumes is irrelevant as long as it's consistent.
static uint32_t epochNow() {
    m5::rtc_date_t d; m5::rtc_time_t t;
    M5.Rtc.getDateTime(&d, &t);
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

// BM8563 alarm/timer IRQ flag, read immediately after M5.begin. Set -> this
// cold boot came from the RTC alarm (timerSleep); clear -> a human pressed the
// power button. Fallback on any doubt: timer (conservative).
static bool wokeByRtcTimer() {
    bool irq = M5.Rtc.getIRQstatus();
    Serial.printf("[boot] rtc_irq=%d -> wake=%s\n", irq ? 1 : 0,
                  irq ? "timer" : "button");
    M5.Rtc.clearIRQ();
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
    m5::rtc_date_t d;
    d.year    = 2000 + ble_link::rx.rtcYearSince2000;
    d.month   = ble_link::rx.rtcMonth;
    d.date    = ble_link::rx.rtcDay;
    d.weekDay = 0;
    m5::rtc_time_t t;
    t.hours   = ble_link::rx.rtcHour;
    t.minutes = ble_link::rx.rtcMinute;
    t.seconds = ble_link::rx.rtcSecond;
    M5.Rtc.setDateTime(&d, &t);
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
        M5.Speaker.tone(1800, 80);
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
        M5.Speaker.tone(600, 120);
        return;
    }
    // Injection bound: 1..1e6 whole units. 1e6 × FP(100) = 1e8 per write, well
    // inside int32 stores (max ~2.1e9), so a single inject can't overflow.
    if (amount < 1 || amount > 1000000) {
        Serial.printf("[cmd] amount out of range (1..1000000): %d\n", amount);
        M5.Speaker.tone(600, 120);
        return;
    }
    for (char* q = res; *q; q++) if (*q == '_') *q = ' ';   // "cured_meat" -> "cured meat"
    int r = -1;
    for (int i = 0; i < adr::RES_COUNT; i++)
        if (strcmp(res, adr::RES_KEY[i]) == 0) { r = i; break; }
    if (r < 0) {
        Serial.printf("[cmd] unknown resource: '%s'\n", res);
        M5.Speaker.tone(600, 120);
        return;
    }
    g_game.stores[r] += amount * adr::FP;         // stores are fixed-point × FP
    if (g_game.stores[r] < 0) g_game.stores[r] = 0;   // never leave it negative
    g_game.markSeen((uint8_t)r);   // injected == "owned": unlock its craft/buy gates
    g_game.save();
    M5.Speaker.tone(1800, 80);
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
    pager::payGhostDebtIfDue();
    uint32_t sleepSecs = WAKE_INTERVAL_SECS;
    uint16_t qStart, qEnd;
    loadQuietHours(qStart, qEnd);
    if (quiet_hours::enabled(qStart, qEnd)) {
        m5::rtc_date_t d; m5::rtc_time_t t;
        M5.Rtc.getDateTime(&d, &t);
        uint16_t nowMin = (uint16_t)t.hours * 60 + t.minutes;
        if (quiet_hours::inWindow(nowMin, qStart, qEnd)) {
            sleepSecs = quiet_hours::secondsUntilEnd(nowMin, (uint8_t)t.seconds, qEnd);
            Serial.printf("[sleep] quiet hours %02u:%02u-%02u:%02u — sleeping %lus\n",
                          qStart / 60, qStart % 60, qEnd / 60, qEnd % 60,
                          (unsigned long)sleepSecs);
        }
    }
    g_game.save();                     // persist the game before power-off
    Serial.printf("[sleep] %s — timerSleep(%lus)\n", reason, (unsigned long)sleepSecs);
    Serial.flush();
    delay(40);
    M5.Power.timerSleep(sleepSecs);   // full power-off; no return
    esp_restart();
}

// Absolute display rotation from the accelerometer. X is this board's
// discriminating axis: ax<0 = upright (rotation 2), ax>0 = flipped 180.
static int rotForAccel() {
    float ax = 0, ay = 0, az = 0;
    if (!M5.Imu.isEnabled() || !M5.Imu.getAccel(&ax, &ay, &az)) return g_rot;
    if (ax < -0.35f) return 2;
    if (ax >  0.35f) return 0;
    return g_rot;
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
    if ((int)M5.Power.isCharging() == 1) return true;
    return tud_mounted();
}

static bool sdInit() {
    int8_t sck  = M5.getPin(m5::pin_name_t::sd_spi_sclk);
    int8_t mosi = M5.getPin(m5::pin_name_t::sd_spi_mosi);
    int8_t miso = M5.getPin(m5::pin_name_t::sd_spi_miso);
    int8_t cs   = M5.getPin(m5::pin_name_t::sd_spi_cs);
    if (sck < 0 || cs < 0) return false;
    SPI.begin(sck, miso, mosi, cs);
    return SD.begin(cs, SPI, 20000000);
}

void setup() {
    auto cfg = M5.config();
    cfg.clear_display = false;     // keep the last frame on the EPD
    M5.begin(cfg);

    // GT911 factory config reports only 2 touch points — the multi-finger
    // gestures (preview switcher) need more. Volatile: re-apply every cold boot.
    if (auto t = M5.Display.touch())
        static_cast<lgfx::Touch_GT911*>(t)->setTouchNums(5);

    Serial.begin(115200);
    delay(400);
    g_bootMs = millis();
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
    // every sleep (Power::timerSleep -> disableIRQ+clearIRQ+setAlarmIRQ), so
    // periodic wakes are unaffected; only a hand power-off now stays off.
    M5.Rtc.disableIRQ();

    Serial.printf("[boot] adarkroom v%s interval=%ds reset=%d\n",
                  CARD_VERSION, WAKE_INTERVAL_SECS, (int)esp_reset_reason());

    // Before anything heavy: count this boot toward OTA probation / roll back.
    otaRollbackCheck();

    M5.Display.setRotation(0);
    M5.Display.setEpdMode(epd_mode_t::epd_fast);
    canvas.setColorDepth(m5gfx::grayscale_8bit);
    if (!canvas.createSprite(PANEL_W, PANEL_H))
        Serial.println("[boot] FATAL: canvas alloc failed");
    canvas.fillSprite(TFT_WHITE);

    g_onUsb = usbPresent();
    int bat = (int)M5.Power.getBatteryLevel();
    g_lowBattery = (bat >= 0 && bat <= LOW_BATTERY_PCT);
    Serial.printf("[boot] chg=%d mnt=%d onUsb=%d bat=%d%% v=%dmV low=%d\n",
                  (int)M5.Power.isCharging(), tud_mounted() ? 1 : 0,
                  g_onUsb ? 1 : 0, bat,
                  (int)M5.Power.getBatteryVoltage(), g_lowBattery ? 1 : 0);

    g_rot = rotForAccel();
    M5.Display.setRotation(g_rot);
    Serial.printf("[boot] rotation=%d\n", g_rot);

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

    // Bind the random-event engine to the settled model. Its scheduler state
    // (nextEventAt / delayed echo) is persisted in GameState; bind() only wires
    // the pointer and clears the RAM-only "event on screen" flag. Events fire
    // only while awake (research.md §5.4); the offline echo was already redeemed
    // by settle() above.
    events::bind(&g_game);

    // The ring is all client (game) pages — no host pages exist. Restore the
    // last-shown game page by NAME and paint it in quality mode (a cold-boot
    // redraw isn't latency-sensitive and clears ghosting).
    if (pager::ringCount() > 0) {
        pager::restore(true);
        Serial.printf("[boot] restored page %d/%d\n",
                      pager::currentRingIndex(), pager::ringCount());
    }
    status_bar::draw();

    for (int i = 0; i < 2; i++) {   // "I just woke" heartbeat blink
        M5.Power.setLed(255); delay(90);
        M5.Power.setLed(0);   delay(90);
    }

    ble_link::init("M5PaperS3");

    // BLE-ready — clear any OTA rollback markers (the new image works).
    otaConfirmHealthy();

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

void loop() {
    M5.update();
    uint32_t now = millis();

    // Touch first: any tap = interaction — page + open/extend the wake window.
    if (pager::handleTouch()) {
        g_lastInteraction = now;
        enterInteractive("tap");
    }

    // Continuous auto-rotate (~700ms cadence): on flip, redraw the current page.
    static uint32_t s_lastRot = 0;
    if (M5.Imu.isEnabled() && now - s_lastRot > 700) {
        s_lastRot = now;
        int want = rotForAccel();
        if (want != g_rot) {
            g_rot = want;
            M5.Display.setRotation(g_rot);
            if (pager::ringCount() > 0)
                pager::showPage(pager::currentRingIndex(), false);
            Serial.printf("[rot] -> rotation %d\n", g_rot);
        }
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
        events::tick(now, epochNow());
    }
    if (events::active() && !event_modal::active())
        event_modal::show(now);
    event_modal::checkTimeout(now);

    // OTA: on the last streamed byte, verify + commit + reboot (never returns)
    // or report ota=err. No-op unless an OTA transfer just finished. THE
    // cross-flash lifeline — untouched from the dashboard firmware.
    ble_link::otaPollFinish(CARD_VERSION, g_onUsb, g_rot,
                            g_mode == Mode::INTERACTIVE);

    // OTA-progress bar while an image streams (throttled).
    static bool     s_otaBarActive = false;
    static int      s_otaBarPct    = -1;
    static uint32_t s_otaBarDraw   = 0;
    if (ble_link::otaBusy()) {
        uint32_t recv = ble_link::otaReceived();
        uint32_t tot  = ble_link::otaTotal();
        int pct = tot ? (int)((recv * 100ULL) / tot) : 0;
        if (!s_otaBarActive || pct - s_otaBarPct >= 2 ||
            now - s_otaBarDraw >= 300) {
            status_bar::drawOtaProgress(recv, tot);
            s_otaBarActive = true;
            s_otaBarPct    = pct;
            s_otaBarDraw   = now;
        }
    } else if (s_otaBarActive) {
        s_otaBarActive = false;
        s_otaBarPct    = -1;
        if (ble_link::rx.connected) status_bar::draw();   // ota=err path restore
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
    static uint32_t s_barTick = 0;
    static int s_barMin = -1, s_barUsb = -1, s_barBat = -1;
    if (now - s_barTick > 1000) {
        s_barTick = now;
        m5::rtc_time_t t;
        M5.Rtc.getTime(&t);
        int min = t.minutes;
        int usb = g_onUsb ? 1 : 0;
        int bat = status_bar::batteryPercent();
        if ((min != s_barMin || usb != s_barUsb || bat != s_barBat) &&
            pager::ringCount() > 0 && !ble_link::otaBusy())
            status_bar::draw();
        s_barMin = min; s_barUsb = usb; s_barBat = bat;
    }

    static uint32_t s_hb = 0;
    if (now - s_hb > 3000) {
        s_hb = now;
        bool sof = usbSofActive();
        int mnt = tud_mounted() ? 1 : 0;
        int sus = tud_suspended() ? 1 : 0;
        ble_link::setUsbDiag((int)M5.Power.isCharging(),
                             sof ? 1 : 0, mnt * 10 + sus);
        Serial.printf("[hb] mode=%s awake=%lums ble=%d sec=%d page=%d/%d\n",
                      g_mode == Mode::INTERACTIVE ? "int" : "sync",
                      (unsigned long)(now - g_bootMs),
                      ble_link::rx.connected ? 1 : 0,
                      ble_link::rx.secure ? 1 : 0,
                      pager::currentRingIndex(), pager::ringCount());
    }
    delay(5);
}
