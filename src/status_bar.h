// Bottom status bar — a ~32px band the firmware self-draws, same model as the
// old BLE-connected corner icon it absorbs. The host used to bake a page
// counter into every PNG (card_render_pixel._page_indicator); fw 0.7.0 owns
// that plus the live clock/battery instead, so the bar reflects real-time
// device state (RTC minute, battery %, USB) that a once-a-minute host render
// can't.
//
// Left  : BLE glyph (only while connected) then the RTC clock HH:MM.
// Center: iOS TabView-style page dots (current = solid disc, others = 2px ring).
// Right : battery icon + % only.
//
// One draw path, since the MSG migration: drawOnto() bakes the band into the
// page canvas's bottom rows and pager::repaint presents the whole frame. The bar
// used to have a second path — an off-screen 540x32 strip sprite blitted over
// just the band, so an independent refresh was one strip-local EPD update rather
// than a flickering fillRect-then-primitives sequence. There is no such thing as
// updating one band on a free-running double-buffered panel, and no waveform to
// flicker under either, so that path is gone and draw() is now a whole-frame
// repaint (status_bar.cpp says the same at more length).
#pragma once
#include <cstdint>                    // uint32_t — drawOtaProgress args

namespace m5gfx { class M5Canvas; }   // fwd — drawOnto's target is the page
                                      // canvas (global M5Canvas aliases this)

namespace status_bar {

// Bake the band into the bottom BAR_H rows of `canvas` (the full-page sprite),
// to be presented by the caller (pager::drawFrame) in the same frame as the
// page. Reads live state itself (see draw()), so callers pass only the target.
void drawOnto(m5gfx::M5Canvas& canvas);

// Bake the firmware version (CARD_VERSION, short "fw X.Y.Z" form) into the page
// header's top-right whitespace — the blank band the host leaves below its date
// row. Called by pager::drawFrame right after drawOnto, so the version rides
// the page's own frame on every draw: page turn, tick repaint, SD restore. Same
// Minecraftia16 face as the bar; right-aligned to the host's pad (24px) margin.
void drawVersionOnto(m5gfx::M5Canvas& canvas);

// Refresh the bar: a whole-frame pager::repaint (see the header note). Reads
// live state itself — BLE from ble_link::rx.connected, USB from main.cpp's
// g_onUsb (extern; reuses its usbPresent() detection rather than re-probing),
// clock from rtc::, battery from power::, page position from frame_store — so
// callers just invoke draw() with no state.
void draw();

// OTA-progress variant of draw() (fw 0.8.9): same whole-frame repaint, but the
// center page-dots are replaced by a labelled progress bar — "OTA" text + a ~180px outline rect with proportional black fill + "NN%"
// (percent = received/total, clamped 0-100). Left clock/BLE glyph and right
// battery block are unchanged. main.cpp's loop calls this on a throttle while an
// OTA streams (reading ble_link::otaReceived()/otaTotal()); on ota=err/abort it
// calls draw() once to restore the steady-state bar (ota=ok reboots anyway).
void drawOtaProgress(uint32_t received, uint32_t total);

// Take one reading of every live input the bar shows: the battery (an ADC burst
// through the EMA + deadband filter) and the RTC clock. THE ONLY SAMPLER — call
// it exactly once per second from main.cpp's bar tick, plus once at boot before
// the first frame. Everything below is a pure read of what it cached, and
// drawOnto() touches no hardware, so the frame rate cannot drive the filter and
// the change detector cannot disagree with what gets drawn (status_bar.cpp
// explains what went wrong when they were the same function).
void sample();

// Filtered battery percentage as last cached by sample(); -1 until a valid
// reading has landed. Free to call as often as you like.
int batteryPercent();

// Minute-of-hour as last cached by sample(); -1 until the clock has been read.
// main.cpp's tick compares this to spot a visible change worth a repaint.
int clockMinute();

}  // namespace status_bar
