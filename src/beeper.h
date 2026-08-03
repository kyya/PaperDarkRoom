// Bare piezo buzzer — the M5.Speaker.tone replacement.
//
// M5.begin() is gone with the MSG migration (see rtc_bm8563.h), and with it
// M5.Speaker. The M5PaperS3's "speaker" is a passive piezo element on GPIO21 —
// no DAC, no amplifier (M5Unified's own board table configures it as
// `spk_cfg.buzzer = true`, pin_data_out = GPIO_NUM_21). A resonant plate only
// makes an audible noise from a full-amplitude square wave, which is exactly
// what LEDC produces for free, so the whole of what this firmware ever asked
// M5.Speaker for — `tone(freq, ms)`, one note at a time, never a chord and never
// a sample — is ~40 lines of LEDC here.
//
// NON-BLOCKING, like M5.Speaker.tone: tone() programs the timer and returns, and
// the note is silenced by tick() once its deadline passes. That matters because
// the two-note chimes in this firmware are written as
// `tone(1047,90); delay(100); tone(1568,150);` — the second call simply retunes
// the still-running channel, which is what M5.Speaker did too.
//
// LEDC_TIMER_0 / LEDC_CHANNEL_0 in LOW_SPEED mode. Nothing else in this firmware
// uses LEDC except power::setLed(), which is deliberately on a plain GPIO rather
// than a second LEDC channel so the two can never fight over a timer.
#pragma once
#include <stdint.h>

namespace beeper {

// Configure the LEDC timer + channel, silent. Call once from setup().
void init();

// Start `hz` for `ms` and return immediately. A second call before the first
// note expires retunes the channel (last writer wins) — the chime idiom. hz
// outside 20..19000 or ms == 0 silences instead of sounding.
//
// Callable from any task: the BLE connect chime fires from a Bluedroid callback
// while everything else comes from the app task. The two LEDC register writes
// are not serialised, but the worst case is one note landing at the other's
// frequency for a few milliseconds, which is inaudible on a piezo.
void tone(uint32_t hz, uint32_t ms);

// Silence now, whatever is playing. Every path out of the app (sleep entry)
// calls it — a buzzer left sounding through a power-off is the worst exit state
// there is.
void stop();

// Silence the channel if the current note's deadline has passed. Call it every
// pass of the app loop; it is a single millis() compare when nothing is playing.
void tick(uint32_t nowMs);

}  // namespace beeper
