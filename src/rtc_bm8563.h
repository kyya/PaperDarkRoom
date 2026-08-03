// Bare BM8563 (PCF8563) real-time clock — the M5.Rtc replacement.
//
// This firmware drives the panel with lib/msg instead of M5GFX's Panel_EPD, and
// M5.begin() is therefore never called (msg_init() claims the i80 bus and the
// panel pins that Panel_EPD would take; see main.cpp). M5.Rtc dies with it, so
// the ~150 lines of PCF8563 the game actually needs are open-coded here:
//   - the wall clock, which epochNow() differences to settle the offline economy
//     and which the status bar draws;
//   - the countdown timer, which is the ONLY thing that can bring this card back
//     from a timerSleep power-off (the RTC INT line drives the power latch, so
//     every wake is a cold boot — main.cpp's deep-sleep model note).
//
// Semantics are a deliberate byte-for-byte copy of M5Unified's PCF8563_Class
// (.pio/libdeps/*/M5Unified/src/utility/rtc/PCF8563_Class.cpp) rather than a
// fresh reading of the datasheet: the save files, the quiet-hours window and
// b79a7bb's stale-timer fix were all validated against THAT register discipline,
// and a subtly different one (a differently-rounded countdown, a clearIRQ that
// also clears the enable bits) would silently change when the card wakes.
//
// Wiring (M5PaperS3): SDA=GPIO41, SCL=GPIO42, I2C_NUM_1, address 0x51 — the same
// bus the GT911 digitizer sits on at 0x14/0x5D. Whichever of touch_gt911::init()
// and rtc::init() runs first installs the driver; the second one tolerates the
// "already installed" error. Both are polled from the app task, so there is no
// concurrent use of the bus to arbitrate.
#pragma once
#include <stdint.h>

namespace rtc {

// Drop-in shapes for m5::rtc_date_t / m5::rtc_time_t (same field names, so the
// call sites that already declared `m5::rtc_date_t d; m5::rtc_time_t t;` change
// only the type). year is the full year (2026), month/date are 1-based.
struct Date { uint16_t year; uint8_t month, date, weekDay; };
struct Time { uint8_t hours, minutes, seconds; };

// Bring the I2C bus up (if nobody has) and probe 0x51. False = no chip answered;
// every call below then reports failure / no-ops, and the card runs without a
// wall clock (the bar draws --:--, settle() sees epoch 0 and does nothing).
bool init();

// True once init() found the chip. Callers that must not act on a bogus time
// (the sleep scheduler) check this rather than inferring it from a zero read.
bool present();

// Read date and/or time (either pointer may be null). False on an I2C failure
// or before init() succeeded; the output is then untouched.
bool getDateTime(Date* date, Time* time);
bool getTime(Time* time);            // convenience — the bar wants only hh:mm

// Write date and/or time. Clears the oscillator-stopped (VL) flag as a side
// effect, because that flag lives in the seconds register.
bool setDateTime(const Date* date, const Time* time);

// The alarm/timer IRQ flag (control reg 0x01 bits 0x0C). Set on a cold boot ->
// this wake came from the countdown we armed at sleep entry; clear -> a human
// pressed the power button. Read it BEFORE clearIRQ()/disableIRQ().
bool getIRQstatus();
void clearIRQ();      // clear the fired flag ONLY (leaves the timer armed)

// Fully disarm alarm + timer + the INT enable bits. This is the b79a7bb fix:
// clearIRQ() clears the FLAG but leaves the repeating countdown running, so
// after a manual power-off the battery-backed timer would fire again ~15 min
// later and pull the card back on. MUST run after the getIRQstatus() read.
void disableIRQ();

// Arm the countdown timer, M5Unified's rounding exactly: < 270 s uses the 1 Hz
// source (reg 0x0E = 0x82) with a 1-second granularity capped at 255 s; longer
// spans switch to the 1/60 Hz source (0x83) and round to whole minutes. Returns
// the countdown actually programmed, in milliseconds (0 = timer disabled), so a
// caller can log what it really asked for rather than what it wanted.
uint32_t setTimerIRQ(uint32_t msec);

}  // namespace rtc
