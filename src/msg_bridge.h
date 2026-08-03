// The MSG bridge — this firmware's entire relationship with the panel.
//
// lib/msg (Wenting Zhang's free-running 1bpp driver, vendored unmodified) owns
// the panel now, in place of M5GFX's Panel_EPD. It scans a 960x540 LANDSCAPE
// 1bpp framebuffer continuously and drives each pixel until it has settled;
// there is no "update the panel" call, only "here is the next frame".
//
// THE SCAN RATE IS AN OBSERVATION, NOT A CONTRACT, and the two must not be
// confused when judging whether the driver is healthy. msg.c paces itself to
// MSG_SCAN_PERIOD_US = 16667 (60 Hz) and msg.h describes a healthy scan as
// "avg == max == ~16670 us" — but that is the pacer's TARGET. What this panel
// actually does through the esp_lcd transport is ~23 ms a frame, i.e. ~43 Hz;
// the spike measured it here and PaperBar measures the same on the same
// hardware, so the per-line gap is a property of the transport rather than of
// anything the application does. Judging health against 60 Hz would therefore
// flag a permanently, uniformly "30% overrun" scan that is in fact its normal
// steady state. The baseline is the MEASURED avg; what actually indicates
// trouble is `max` pulling away from `avg` (something overran a frame) or any
// `dmato` at all (a line's DMA completion was lost). See main.cpp's heartbeat.
// Every page in this firmware, meanwhile, draws 540x960 PORTRAIT through an
// m5gfx::M5Canvas&. This file is the whole of what reconciles the two, and it
// costs nothing per pixel: an LGFX_Sprite is pointed AT MSG's framebuffer at
// colour depth 1 and rotation 3, so the pages render straight into the buffer
// the scan is about to read. No second canvas, no blit, no transpose.
//
// The three facts it rests on were read out of M5GFX's sources and then proved
// on the device by src/msg_spike/spike.cpp, whose header carries the full
// citation trail. In brief:
//
//   a) LGFX_Sprite::setBuffer() adopts an EXTERNAL buffer without copying and
//      marks it Preallocated, so SpriteBuffer::release() will not free MSG's
//      framebuffer out from under the scan. At depth 1 the row stride it
//      computes is 960/8 = 120 B with MSB = leftmost pixel — byte-for-byte
//      MSG's EPD_LINE_BYTES and bit order.
//   b) Panel_Sprite really rotates: at rotation 3 a portrait (px,py) is written
//      to buffer (py, 539-px), which is exactly the portrait mapping PaperBar's
//      gfx1bpp uses against this same panel — hence touch_gt911's raw chip
//      coordinates are already the frame the pages drew in, with no transform.
//   c) LovyanGFX's stock 1bpp converter is the INVERSE of MSG's ink convention
//      (LovyanGFX: TFT_WHITE -> bit set; MSG: set bit = black), and the palette
//      is not consulted on the write path so setPaletteColor cannot fix it.
//      getColorConverter() is public, so swapping the converter's function
//      pointers re-aligns the two at zero per-pixel cost and with ZERO changes
//      to any UI source file — every page keeps passing TFT_BLACK for ink.
//
// TWO RULES THE CALLERS LIVE UNDER, both consequences of double buffering:
//
//   1. EVERY FRAME IS A WHOLE FRAME. present() hands the back buffer to the scan
//      and returns the OTHER one, which holds the frame from two flips ago — not
//      the one just drawn. So a caller may never "touch up" what is on screen;
//      it redraws all 540x960 and presents again. That is affordable, and it is
//      why the partial-refresh machinery this firmware used to carry (waveform
//      selection, clip-rect pushes, coolingRect unions) is deleted rather than
//      ported: a complete UI render measures ~8 ms against the ~23 ms measured
//      scan period,
//      so a sub-rect push saves nothing, and there is no longer any waveform to
//      choose between.
//   2. THE SPRITE MUST BE RE-BOUND AFTER EVERY FLIP. present() does it for you.
//      setBuffer() also resets the clip rectangle to the UNROTATED 960x540, so
//      the setRotation() that follows it is load-bearing, not cosmetic — skip it
//      and everything below y=540 is clipped away.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace m5gfx { class M5Canvas; }

namespace msg_bridge {

// The portrait canvas the pages are written against, stated here so the
// transposition against MSG's landscape geometry is visible in one place. The
// .cpp static_asserts these against EPD_HEIGHT/EPD_WIDTH.
static const int UI_W = 540;
static const int UI_H = 960;

// Bring the driver up: allocate, run the kernel selftest (and print the line it
// writes — msg.c has no Serial of its own), and start the scan task on core 1.
// False = an allocation failed and the driver must not be started; the caller
// has no panel and should say so and stop. Call once, early in setup(), BEFORE
// anything that might claim the i80 bus.
bool init();

// Clear the panel to white through image mode's 8-field LUT push, switch the
// scan to video mode, and bind `spr` to the first back buffer. Call once from
// the app task, before the first frame is drawn. `spr` is this firmware's shared
// full-screen canvas; the bridge keeps a reference to it for present()'s
// re-bind, so it must outlive the program (it is a global).
void begin(m5gfx::M5Canvas& spr);

// The back buffer the canvas is currently bound to — the frame being composed,
// and `frameBytes()` of it (EPD_FB_SIZE, i.e. 960*540/8). Exposed so the deghost
// sequence can snapshot a frame without pager.cpp having to include msg.h.
uint8_t* frame();
size_t   frameBytes();

// Hand the composed frame to the scan and bind the canvas to the next back
// buffer. Blocks until the next VSYNC. See rule 1 above: whatever is in the
// buffer this returns is two frames stale and must be fully overdrawn.
void present();

// A spare full-size framebuffer, used by the deghost sequence to remember the
// exact frame it has to put back. Not touched by anything else.
uint8_t* scratch();

// Invert every pixel of a portrait rect in the CURRENT back buffer. The press
// feedback flash: compose the frame, invert the pressed button, present, wait,
// compose and present again. Rect is clipped to the canvas.
void invertRect(int x, int y, int w, int h);

// Block until every pixel on the panel has finished its drive sequence
// (msg_get_active_bytes() == 0), or until `timeoutMs` elapses. Used before the
// deghost's image push, and before sleep, so what the eye is left with is a
// settled picture rather than a half-driven one.
void waitSettled(uint32_t timeoutMs);

// Push `fb` through image mode's fixed 8-field LUT waveform and wait for the 8
// fields to go out. toWhite=true drives "lighten" wherever a bit is set (a full
// 0xFF buffer therefore whitens the panel); toWhite=false drives "darken" where
// a bit is set, which is exactly the video-mode framebuffer convention, so the
// same buffer a page was rendered into can be pushed straight through it.
//
// USE IT ONLY IN MATCHED PAIRS — see pager::deghost(). Image mode writes
// the glass without touching the video state model, and lib/msg (which is
// vendored unmodified, by policy) exposes no way to re-seed that model, so the
// only safe sequence is one that ENDS on the same picture the model already
// believes is displayed.
void pushImage(uint8_t* fb, bool toWhite);

// Park the scan and drop the panel rails. The picture stays on the glass —
// e-ink is bistable — and core 1 goes idle. Called on the way into sleep so the
// panel is not being driven while the power latch is released, and around a BLE
// OTA so that flash erases cannot glitch a running scan.
//
// FALSE MEANS THE SCAN IS NOT PARKED, and a caller about to touch flash must
// treat that as fatal to its own operation — lib/msg says so in as many words
// (the PARK OWNERSHIP AXIOM in msg.h). Read that comment before adding a third
// caller: pause_req is ONE request, not a count, so park/resume are single-owner
// with handoff and MUST NOT run concurrently. The two owners here are sleep
// (park, then power off — never resumed) and the OTA path (parked by the BLE
// BEGIN callback, resumed by the app task on every exit).
bool park();

// Restart the scan after park(). False means the scan is NOT running afterwards:
// msg_resume() deliberately gives up early, leaving it parked, if someone
// requested a new pause while it was waiting. So a caller cannot infer success
// from the call returning — re-check its own reason for having parked.
bool resume();

// One line of scan statistics for the serial heartbeat: measured scan rate, mean
// and worst frame period, DMA timeouts, governor parks. Differences cumulative
// counters against the previous call, so wraparound cancels; the first call
// primes and reports nothing useful.
void statsLine(uint32_t nowMs, char* out, size_t n);

}  // namespace msg_bridge
