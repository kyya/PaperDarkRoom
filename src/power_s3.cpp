#include "power_s3.h"
#include "rtc_bm8563.h"

#include <Arduino.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>
#include <esp_sleep.h>

// The power half of the M5.begin() teardown; power_s3.h carries the derivation
// of the sleep path and is the thing to read first. This file is the four pins
// that derivation names, plus the battery curve, and nothing else — there is no
// PMIC on this board to talk to, so "power management" here is one ADC, one
// status input, one LED and one latch pulse.

namespace power {

// ---- pins ------------------------------------------------------------------
// All four from M5Unified's board_M5PaperS3 cases: CHG_STAT and the ADC channel
// from Power_Class.cpp:244-251, the LED from Power_Class.cpp:916-937, and
// POWER_HOLD from M5Unified.cpp's _pin_table_other1 (line 253).
static const int             CHG_PIN        = 4;    // CHG_STAT, active LOW
static const int             LED_PIN        = 0;    // status LED
static const int             POWER_HOLD_PIN = 44;   // the latch, see powerHold()
static const adc1_channel_t  BAT_CH         = ADC1_CHANNEL_2;   // GPIO3

// 12 dB of attenuation puts the usable input span at roughly 0-3.1 V, which
// covers a 4.2 V cell seen through the board's 2:1 divider. ADC_ATTEN_DB_12 is
// the spelling this toolchain wants: IDF 4.4.7's hal/adc_types.h:53-54 defines
// DB_12 and marks ADC_ATTEN_DB_11 deprecated as an alias for it, and M5Unified
// itself selects DB_12 on exactly this version test (Power_Class.cpp:1328-1334).
static const adc_atten_t     BAT_ATTEN      = ADC_ATTEN_DB_12;

// The divider halves the cell, so the calibrated reading is doubled back —
// M5Unified's _adc_ratio = 2.0f for this board.
static const int             ADC_RATIO      = 2;

// M5Unified's discharge curve for pmic_adc (Power_Class.cpp:1576):
//   level = (mv - 3300) * 100 / (4150 - 3350)
// clamped to 0..100. The two constants are not a typo and are reproduced as
// written: the numerator's floor is 3300 mV while the denominator's span is
// computed from 3350, so the curve reaches 100% a little before 4.15 V. Whatever
// its origin, it is the curve every battery reading this firmware has ever
// displayed, and status_bar's EMA + deadband were tuned on top of it.
static const int BAT_MV_FLOOR = 3300;
static const int BAT_MV_SPAN  = 4150 - 3350;

static esp_adc_cal_characteristics_t s_cal;
static bool s_adcReady = false;
static int  s_mv       = -1;    // last successful reading, -1 = never

// THE POWER_HOLD / PWROFF_PULSE PIN, and why init() does NOT drive it high.
//
// The name in M5Unified's pin table is power_hold, which reads like a latch that
// has to be held high for the board to stay alive. It is not, on this board, and
// getting that backwards either kills the boot or breaks power-off — so it was
// checked against both libraries rather than inferred from the name:
//
//   - Power_Class::begin() never touches it. grep for power_hold across the
//     whole of M5Unified finds exactly two hits: the enumerator in
//     M5Unified.hpp:47 and the read in Power_Class.cpp:1075, which is inside
//     _powerOff(). Nothing drives it at startup.
//   - M5GFX's autodetect, which is what actually runs on this board at
//     M5.begin() time, drives it LOW: M5GFX.cpp:1956-1958, commented
//     "PWROFF_PULSE_PIN", does pinMode(GPIO_NUM_44, output) then gpio_lo().
//
// So the latch is held by the hardware once the button has turned the board on,
// GPIO44 is a pulse INPUT to that latch, and LOW-as-an-output is the idle state
// the old firmware ran in for its entire life. This function reproduces exactly
// that state. The pinMode is not optional even though the level is idle: with
// M5.begin() gone nothing else claims the pin, and arduino-esp32's digitalWrite
// on a pin whose output driver was never enabled writes the register and changes
// nothing on the wire — the pulse in timerSleep() would be a no-op and the card
// would never power off.
static void powerHoldIdle() {
    pinMode(POWER_HOLD_PIN, OUTPUT);
    digitalWrite(POWER_HOLD_PIN, LOW);
}

void init() {
    powerHoldIdle();
    pinMode(CHG_PIN, INPUT);          // CHG_STAT is driven by the charger
    // The LED pin is left FLOATING here rather than driven low — see setLed().
    pinMode(LED_PIN, INPUT);
    // The ADC is set up on the first read instead of here, the same way
    // M5Unified defers it into _getBatteryAdcRaw().
}

// IDF 4.4 has no esp_adc/adc_oneshot.h (the framework's esp32s3 sdk ships only
// esp_adc_cal), so this is the legacy branch — the same one M5Unified compiles
// into on this toolchain (Power_Class.cpp:1376-1398) and the one PaperBar's
// bar_sources.cpp uses on this board. The 1100 mV default Vref argument is inert
// on an ESP32-S3: the part always calibrates from eFuse, so characterize()
// reports a fitted curve and never falls back to the default.
static bool adcSetup() {
    if (s_adcReady) return true;
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(BAT_CH, BAT_ATTEN);
    esp_adc_cal_characterize(ADC_UNIT_1, BAT_ATTEN, ADC_WIDTH_BIT_12, 1100, &s_cal);
    s_adcReady = true;
    return true;
}

// Eight conversions averaged before the caller's filter ever sees them: one raw
// sample off this rail is worth very little, and status_bar's EMA is there to
// smooth the discharge curve, not to do the ADC's noise rejection for it.
static int sample() {
    adcSetup();
    uint32_t acc = 0;
    for (int i = 0; i < 8; i++) {
        int raw = adc1_get_raw(BAT_CH);
        if (raw < 0) return -1;       // conversion failed; leave s_mv alone
        acc += (uint32_t)raw;
    }
    s_mv = (int)(esp_adc_cal_raw_to_voltage(acc / 8, &s_cal) * ADC_RATIO);
    return s_mv;
}

int batteryLevel() {
    int mv = sample();
    if (mv < 0) return -1;
    int level = (mv - BAT_MV_FLOOR) * 100 / BAT_MV_SPAN;
    return (level < 0) ? 0 : (level > 100) ? 100 : level;
}

// The cached millivolts, taking one reading if nothing has ever landed — a
// diagnostic line that printed -1 purely because it ran before the first
// batteryLevel() would be a bug report about hardware that is working.
int batteryVoltage() {
    if (s_mv < 0) sample();
    return s_mv;
}

// No fuel gauge and no shunt on this board — see power_s3.h. Kept so ble_link's
// STATUS line keeps its shape.
int batteryCurrent() { return 0; }

bool isCharging() { return digitalRead(CHG_PIN) == LOW; }

// M5Unified drives this pin through a Light_PWM instance so it can dim; the only
// caller here blinks it, so a plain GPIO saves an LEDC channel that beeper.cpp
// would otherwise have to negotiate for.
//
// OFF RELEASES THE PIN INSTEAD OF DRIVING IT LOW, and that is the important part:
// LED_PIN is GPIO0, which is also this chip's BOOT STRAPPING PIN. A reset — a
// panic, a watchdog, a brownout — samples GPIO0 while whatever drove it last is
// still driving, and a LOW sample puts the ROM into serial-download mode instead
// of running the app. Since a blink lasts 90 ms and the rest of the session is
// "off", leaving the pin an input for all of that time means the strap reads its
// own external pull-up on every reset that matters, and a crash costs a reboot
// rather than a device that silently stops being a device. (M5Unified's PWM path
// has the same hazard, idling at duty 0; it simply never came up because that
// firmware did not crash.)
void setLed(uint8_t brightness) {
    if (brightness) {
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(LED_PIN, HIGH);
    } else {
        pinMode(LED_PIN, INPUT);
    }
}

// Power_Class::timerSleep(int) -> _timerSleep() -> _powerOff(true), collapsed to
// the path this board actually takes. power_s3.h walks that collapse branch by
// branch; the order of the five steps below is the part that must not move:
//
//   1. disableIRQ BEFORE setTimerIRQ, or the alarm registers left over from a
//      previous wake stay armed alongside the new countdown.
//   2. clearIRQ after it, so the flag the boot path already read is not still
//      set when the chip re-latches power (it would look like a second wake).
//   3. The countdown itself, which is the ONLY thing that brings this card back.
//   4. esp_sleep timer wakeup as belt and braces, for the case where the latch
//      does not actually cut power and this is a plain deep sleep after all.
//   5. The latch pulses, last, because nothing after them is guaranteed to run.
void timerSleep(uint32_t seconds) {
    rtc::disableIRQ();
    rtc::clearIRQ();
    uint32_t armed = rtc::setTimerIRQ(seconds * 1000);
    Serial.printf("[power] sleep %lus (rtc countdown %lums)\n",
                  (unsigned long)seconds, (unsigned long)armed);

    esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);

    // USB CDC is buffered and the panel is about to lose power; without this the
    // line above is lost on every sleep, which is the one line that says whether
    // the countdown was armed at all.
    Serial.flush();
    delay(20);

    // FIVE PULSES, NOT ONE LEVEL CHANGE. M5Unified's own comment at
    // Power_Class.cpp:1078-1080: "For PaperS3, the power cannot be turned off
    // simply by setting the GPIO to LOW, so a loop is performed to ensure that
    // the power is turned off by repeatedly outputting a pulse." The timings are
    // its timings — 50 ms in each state, five times round.
    for (int i = 0; i < 5; ++i) {
        digitalWrite(POWER_HOLD_PIN, LOW);
        delay(50);
        digitalWrite(POWER_HOLD_PIN, HIGH);
        delay(50);
    }

    esp_deep_sleep_start();
    // Unreachable: esp_deep_sleep_start() does not return. Spelled out anyway so
    // the compiler knows this function has no fall-through path, which is what
    // the header promises its callers.
    for (;;) { delay(1000); }
}

}  // namespace power
