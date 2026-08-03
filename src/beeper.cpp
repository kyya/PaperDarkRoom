#include "beeper.h"

#include <Arduino.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>

// The piezo half of the M5.begin() teardown; beeper.h carries the why. The LEDC
// setup is PaperBar's tune_player.cpp reduced to the one thing this firmware
// ever asked M5.Speaker for: a single square-wave note with a deadline. There is
// no mixer, no polyphony and no sample path, because nothing here ever wanted
// one — every sound in this game is `tone(hz, ms)`.
//
// ---------------------------------------------------------------------------
// WHY THE NOTE DEADLINE IS A TIMER AND NOT A POLL. (2026-08-02, the stuck-buzzer
// bug: the card came up sounding continuously and never stopped.)
//
// The first version of this file ended a note from tick(), called once per pass
// of the application loop. That is wrong here for a reason specific to this
// firmware, and it is worth stating plainly because the shape recurs: THE
// APPLICATION LOOP IS NOT A RELIABLE CLOCK ON THIS DEVICE. It blocks, and it
// blocks for far longer than a note:
//
//   msg_bridge::present()   waits for the next VSYNC          ~23 ms
//   pager::flashPressRect() holds the inverted frame          120 ms
//   pager::deghost()        waitSettled + two 8-field pushes  up to ~2.5 s
//   msg_bridge::begin()     the boot clear's image push       ~200 ms
//
// A note started just before any of those — and the press chime is started by
// exactly the code path that then flashes and repaints — is held on for the
// whole blocking stretch. Worse, ble_link's connect chime is raised from the
// Bluedroid task while the app task is inside one of those waits, so nothing
// polls the deadline at all until the wait ends.
//
// A FreeRTOS one-shot timer removes the coupling entirely: the note is stopped
// by the timer service task, which no amount of blocking in the app task can
// delay. tick() survives as a pure backstop (see below) and is now incapable of
// being the only thing standing between a chime and a permanently sounding
// buzzer.
// ---------------------------------------------------------------------------

namespace beeper {

// GPIO21 is the piezo on this board — M5Unified's own board table says so
// (M5Unified.cpp:2407-2414 sets spk_cfg.pin_data_out = GPIO_NUM_21 with
// buzzer = true for board_M5PaperS3). Low-speed mode is the only mode an
// ESP32-S3 has. Timer 0 / channel 0 are claimed outright: with M5.begin() gone
// there is no M5.Speaker to allocate LEDC channels behind our back, and nothing
// in this firmware calls analogWrite() or Arduino's tone(), which are the other
// two things that would hand out channels from 0 upwards.
static const int              BUZZER_GPIO = 21;
static const ledc_mode_t      MODE        = LEDC_LOW_SPEED_MODE;
static const ledc_timer_t     TIMER       = LEDC_TIMER_0;
static const ledc_channel_t   CHANNEL     = LEDC_CHANNEL_0;
static const ledc_timer_bit_t DUTY_RES    = LEDC_TIMER_12_BIT;
static const uint32_t         DUTY_MAX    = 1u << 12;

// Duty as a volume knob: a square wave of duty d has a fundamental proportional
// to sin(pi*d), so 50% is the loudest this element goes and anything either side
// of it is quieter. 15% (-6.9 dB from full) is the level PaperBar measured as
// "audible across a desk without being obnoxious" on the same piezo, and it is
// the level this firmware's chimes were written against.
static const uint32_t DUTY_ON = (DUTY_MAX * 15) / 100;

// What LEDC can synthesize at 12-bit resolution off the 80 MHz APB clock:
// freq * 4096 <= 80e6 gives the ceiling and the divider's 1024 range gives the
// floor. Out of range is played as SILENCE rather than clamped to the nearest
// reachable pitch, because ledc_set_freq() fails WITHOUT changing anything — a
// clamp would leave the previous note sounding at its own pitch for the new
// note's whole duration, and a confidently wrong note is worse than a rest.
static const uint32_t FREQ_MIN_HZ = 20;
static const uint32_t FREQ_MAX_HZ = 19000;

// No sound this firmware makes is longer than 240 ms (world_page's death tone).
// Anything still sounding a second later is a bug in this file, not a long note,
// so the backstop below silences it and says so on the wire rather than letting
// the card sit there buzzing at the user. This is the last line of defence, not
// the mechanism — if it ever fires, read the log line and fix the cause.
static const uint32_t MAX_NOTE_MS = 1000;

static bool              s_ready    = false;   // LEDC configured — false = silent no-op
static volatile bool     s_playing  = false;
static volatile uint32_t s_startMs  = 0;       // millis() when the note began
static TimerHandle_t     s_noteTimer = nullptr;

// Force the channel to a known-silent state.
//
// ledc_stop() rather than the ledc_set_duty(0)/ledc_update_duty() pair the first
// version used: set_duty(0) asks the PWM generator for a zero-width pulse, which
// leaves the driver running and the pin's resting level a property of hpoint and
// of whatever the generator was mid-cycle. ledc_stop() disables the channel's
// output and DRIVES THE PIN TO THE IDLE LEVEL GIVEN — here 0, matching the level
// the pin sits at before this module ever configures it. That is the difference
// between "asking for silence" and "being silent", and on a piezo the two are
// audibly different.
static void silence() {
    if (!s_ready) return;
    ledc_stop(MODE, CHANNEL, 0);
    s_playing = false;
}

// The one-shot's callback: end the note. Runs on the FreeRTOS timer service
// task, so it is independent of anything the application task is blocked inside.
static void noteExpired(TimerHandle_t) { silence(); }

void init() {
    if (s_ready) return;

    ledc_timer_config_t tcfg = {};
    tcfg.speed_mode      = MODE;
    tcfg.duty_resolution = DUTY_RES;
    tcfg.timer_num       = TIMER;
    tcfg.freq_hz         = 1000;      // placeholder; every note retunes it
    tcfg.clk_cfg         = LEDC_AUTO_CLK;
    if (ledc_timer_config(&tcfg) != ESP_OK) {
        Serial.println("[beep] LEDC timer config failed — silent");
        return;
    }

    ledc_channel_config_t ccfg = {};
    ccfg.gpio_num   = BUZZER_GPIO;
    ccfg.speed_mode = MODE;
    ccfg.channel    = CHANNEL;
    ccfg.intr_type  = LEDC_INTR_DISABLE;
    ccfg.timer_sel  = TIMER;
    ccfg.duty       = 0;              // start silent
    ccfg.hpoint     = 0;
    if (ledc_channel_config(&ccfg) != ESP_OK) {
        Serial.println("[beep] LEDC channel config failed — silent");
        return;
    }

    // Period is a placeholder — every tone() resets it before starting the
    // timer. Created non-reloading: a note ends once.
    s_noteTimer = xTimerCreate("beep", pdMS_TO_TICKS(100), pdFALSE, nullptr,
                               noteExpired);
    if (!s_noteTimer) {
        Serial.println("[beep] note timer alloc failed — tick() is the only stop");
    }

    s_ready = true;
    // Explicitly park the channel rather than trusting the duty=0 the config
    // above asked for: binding LEDC to the pin is the moment it starts being
    // driven, and this firmware (unlike PaperBar, which configures the buzzer
    // lazily and usually not at all) does it on every single boot.
    silence();
    Serial.println("[beep] ok");
}

void stop() { silence(); }

// Retune-and-return, never block. A second call while a note is still running
// simply reprograms the timer and pushes the deadline out, which is what makes
// the `tone(1047,90); delay(100); tone(1568,150);` chime idiom in this firmware
// work — M5.Speaker.tone() behaved the same way.
void tone(uint32_t hz, uint32_t ms) {
    if (!s_ready) return;
    if (ms == 0 || hz < FREQ_MIN_HZ || hz > FREQ_MAX_HZ) { silence(); return; }
    if (ms > MAX_NOTE_MS) ms = MAX_NOTE_MS;

    // Arm the stop BEFORE making a sound, so there is no window in which the
    // buzzer is on and nothing yet knows when to turn it off. Callable from any
    // task (ble_link's connect chime comes off the Bluedroid task), and
    // xTimerChangePeriod on a running timer restarts it — which is exactly the
    // retune semantics the chime idiom wants.
    s_startMs = millis();
    s_playing = true;
    // Block time 0 deliberately — tone() is called from the BLE callback task and
    // must not wait on the timer command queue. If the queue is full the command
    // is dropped and this note falls through to tick()'s backstop, which is up to
    // MAX_NOTE_MS late; say so rather than let a stretched note look like a
    // hardware quirk.
    if (s_noteTimer &&
        xTimerChangePeriod(s_noteTimer, pdMS_TO_TICKS(ms), 0) != pdPASS)
        Serial.println("[beep] timer queue full — note falls back to the backstop");

    if (ledc_set_freq(MODE, TIMER, hz) != ESP_OK) { silence(); return; }
    ledc_set_duty(MODE, CHANNEL, DUTY_ON);
    ledc_update_duty(MODE, CHANNEL);
}

// Backstop only — the note timer above is what actually ends a note. This exists
// for the two cases the timer cannot cover: the timer failing to allocate at
// boot, and any future bug that leaves the channel driven with no pending stop.
// It silences anything still sounding MAX_NOTE_MS after it started and logs it,
// because a stuck buzzer that fixes itself quietly is a bug that never gets
// found — and this one cost a device-side debugging round already.
void tick(uint32_t nowMs) {
    if (!s_playing) return;
    if ((int32_t)(nowMs - s_startMs) < (int32_t)MAX_NOTE_MS) return;
    Serial.printf("[beep] BACKSTOP: note still on %lums after start — silencing\n",
                  (unsigned long)(nowMs - s_startMs));
    silence();
}

}  // namespace beeper
