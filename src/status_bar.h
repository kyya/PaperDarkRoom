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
// Two draw paths, both compositing the SAME element set (see status_bar.cpp's
// drawTo), chosen so the bar never flickers:
//  - drawOnto(): a full-page repaint bakes the bar into the page canvas's
//    bottom band BEFORE pager pushes the whole sprite, so bar + page land in
//    ONE EPD update (a page turn no longer wipes-then-redraws the bar).
//  - draw(): an independent bar refresh (BLE connect/disconnect, the 1s tick's
//    minute/USB/battery change) composes the band into an off-screen strip
//    sprite and pushes it once — a single strip-local EPD update, not a
//    fillRect-then-N-primitives sequence that flickers under epd_fast.
#pragma once
#include <cstdint>                    // uint32_t — drawOtaProgress args

namespace m5gfx { class M5Canvas; }   // fwd — drawOnto's target is the page
                                      // canvas (global M5Canvas aliases this)

namespace status_bar {

// Bake the band into the bottom BAR_H rows of `canvas` (the full-page sprite),
// to be pushed by the caller (pager::showPage) in the same update as the page.
// No EPD-mode work — the caller owns the push and its mode. Reads live state
// itself (see draw()), so callers pass only the target canvas.
void drawOnto(m5gfx::M5Canvas& canvas);

// Bake the firmware version (CARD_VERSION, short "fw X.Y.Z" form) into the page
// header's top-right whitespace — the blank band the host leaves below its date
// row. Called by pager::showPage right after drawOnto, before the single
// pushSprite, so the version rides the page's own EPD update (no extra refresh)
// on every page draw: new push, SD restore, page turn. Same
// Minecraftia16 face as the bar; right-aligned to the host's pad (24px) margin.
void drawVersionOnto(m5gfx::M5Canvas& canvas);

// Repaint the whole band as a standalone update: composed into a strip sprite
// then pushed once (epd_fast, restored on exit). Reads live state itself — BLE
// from ble_link::rx.connected, USB from main.cpp's g_onUsb (extern; reuses its
// usbPresent() detection rather than re-probing), RTC/battery from M5, page
// position from frame_store — so callers just invoke draw() with no state.
void draw();

// OTA-progress variant of draw() (fw 0.8.9): same off-screen strip + single
// epd_fast push, but the center page-dots are replaced by a labelled progress
// bar — "OTA" text + a ~180px outline rect with proportional black fill + "NN%"
// (percent = received/total, clamped 0-100). Left clock/BLE glyph and right
// battery block are unchanged. main.cpp's loop calls this on a throttle while an
// OTA streams (reading ble_link::otaReceived()/otaTotal()); on ota=err/abort it
// calls draw() once to restore the steady-state bar (ota=ok reboots anyway).
void drawOtaProgress(uint32_t received, uint32_t total);

// Battery % as shown on the bar: samples M5.Power.getBatteryLevel() and runs
// it through an EMA + deadband (the raw voltage-derived reading flaps several
// points, see status_bar.cpp). Call at a steady cadence — main.cpp's 1s bar
// tick is the sampler — and redraw only when the RETURNED value changes;
// draw() renders this same filtered value. -1 until a first valid reading.
int batteryPercent();

}  // namespace status_bar
