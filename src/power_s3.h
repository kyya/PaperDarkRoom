// Bare M5PaperS3 power plumbing — the M5.Power replacement.
//
// M5.begin() is gone with the MSG migration (see rtc_bm8563.h). Everything this
// firmware asked M5.Power for is here, reached the short way, with the same pin
// assignments and the same battery curve M5Unified's Power_Class uses for this
// board (.pio/libdeps/*/M5Unified/src/utility/Power_Class.cpp, the
// board_M5PaperS3 cases):
//
//   battery   GPIO3 -> ADC1, halved by an external divider (_adc_ratio = 2.0),
//             level = (mv - 3300) * 100 / 800 clamped to 0..100.
//   charging  CHG_STAT on GPIO4, active LOW.
//   LED       GPIO0 (M5Unified drives it as a PWM backlight; a plain GPIO is
//             enough for the two boot blinks this firmware does).
//   power off POWER_HOLD on GPIO44, pulsed five times — M5Unified's own comment
//             says the PaperS3 latch will not release from a single level.
//
// THE SLEEP PATH IS THE LOAD-BEARING PART. timerSleep() is a line-for-line
// re-derivation of Power_Class::timerSleep(int) -> _timerSleep() -> _powerOff(true)
// for THIS board, and every wake source the old firmware had is preserved:
//
//   Power_Class::timerSleep(s):  Rtc.disableIRQ(); Rtc.clearIRQ();
//                                Rtc.setAlarmIRQ(s)  [= setTimerIRQ(s*1000)];
//                                esp_sleep_enable_timer_wakeup(s * 1e6);
//                                _timerSleep()
//   _timerSleep():               Display.sleep(); Display.waitDisplay();
//                                _powerOff(true)
//   _powerOff(true), PaperS3:    _rtcIntPin is 255 on this board (never set in
//                                Power_Class::begin), so the ext0/gpio wakeup
//                                branch is NOT taken and use_deepsleep stays
//                                true; _wakeupPin is GPIO48 (touch INT) but the
//                                `_rtcIntPin == GPIO_NUM_MAX` guard is false
//                                (255 != 49) so no ext0 is armed there either.
//                                What actually happens is: pulse POWER_HOLD five
//                                times, then esp_deep_sleep_start().
//
// So the real wake mechanism is the BM8563 countdown driving the board's power
// latch — a full power-off and a COLD BOOT, exactly as main.cpp's header
// describes, with the esp_sleep timer as the belt-and-braces fallback for the
// case where the latch does not actually cut power. Touch CANNOT wake from
// sleep on this board and never could; the button does, by re-latching power.
// b79a7bb's stale-timer fix lives on the boot side (rtc::disableIRQ after the
// getIRQstatus read) and is unaffected by anything here.
#pragma once
#include <stdint.h>

namespace power {

// Configure CHG_STAT as an input and the LED pin as an output. The battery ADC
// is set up lazily on the first read (M5Unified does the same).
void init();

// Filtered-at-the-caller battery percentage 0..100, or -1 when no conversion has
// succeeded. Same curve as M5Unified for pmic_adc; status_bar applies its own
// EMA + deadband on top, unchanged.
int batteryLevel();

// Last battery reading in millivolts, or -1. Diagnostic only (the boot line and
// the BLE STATUS line).
int batteryVoltage();

// Battery current in mA. NOT MEASURABLE on this board — there is no fuel gauge
// and no shunt, only the divider above; M5Unified's Power_Class has no PaperS3
// path for it either. Always 0, kept so ble_link's STATUS line keeps its shape.
int batteryCurrent();

// CHG_STAT (GPIO4, active low). Has the blind spot main.cpp documents — a full
// battery pauses the charger and this reads false while still plugged in — which
// is why usbPresent() ORs it with TinyUSB's enumerated state.
bool isCharging();

// The status LED on GPIO0. 0 = off, anything else = on (the pin is driven as a
// plain GPIO here, so there is no brightness; the only caller blinks it).
void setLed(uint8_t brightness);

// Full power-off with the BM8563 countdown armed to bring the card back in
// `seconds`. Does not return. See the derivation in the header note above.
void timerSleep(uint32_t seconds);

}  // namespace power
