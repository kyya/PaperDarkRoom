// MSG 1bpp whole-firmware spike — "can the game's UI code draw straight into
// MSG's framebuffer, in portrait, with no copy and no rewrite?"
//
// Two questions, and nothing else:
//
//   1. Does MSG come up under THIS repo's build system at all (lib/msg vendored
//      from PaperBar, arduino-esp32 2.0.17 / IDF 4.4.7, -O2)?
//   2. Can 540x960 PORTRAIT UI code — every drawing function in this firmware
//      takes m5gfx::M5Canvas& and works in portrait coordinates — render
//      DIRECTLY into MSG's 960x540 LANDSCAPE 1bpp buffer?
//
// The answer to (2) is yes, zero-copy, and it rests on three facts read out of
// M5GFX's own sources rather than guessed at:
//
//   a) LGFX_Sprite::setBuffer() adopts an EXTERNAL buffer without copying it,
//      and marks it AllocationSource::Preallocated — SpriteBuffer::release()
//      explicitly does not free that (SpriteBuffer.cpp:148), so handing it
//      MSG's framebuffer is safe.  At colorDepth 1 the row stride it computes
//      is (960+7)&~7 / 8 = 120 B, byte-for-byte MSG's EPD_LINE_BYTES, MSB =
//      leftmost pixel (Panel_Sprite::drawPixelPreclipped's sub-byte path uses
//      mask 0x80 >> (index & 7)).  Same layout, same bit order: no transpose,
//      no blit, no second framebuffer.
//
//   b) Panel_Sprite REALLY ROTATES.  setRotation() swaps _width/_height
//      (LGFX_Sprite.cpp:95-111) and every write path applies the transform to
//      the coordinates before addressing the buffer — drawPixelPreclipped at
//      LGFX_Sprite.cpp:127-135, writeFillRectPreclipped at :166-172 — INCLUDING
//      the bits<8 branch.  So the sprite presents a 540x960 portrait canvas
//      whose pixels land in a 960x540 landscape buffer.  This is real rotated
//      addressing, not a flag that only affects pushSprite.
//
//   c) THE INK POLARITY IS INVERTED, and it is fixable for free.  MSG's buffer
//      is "set bit = black" (msg_kernel.h:31, "plane 0 colour 1 = black").
//      LovyanGFX's 1bpp is palette-indexed and its converter is
//      convert_uint32_to_palette1(c) = (c & 1) ? 0xFF : 0 (colortype.hpp:52) —
//      i.e. TFT_BLACK (0x0000) -> bit CLEAR and TFT_WHITE (0xFFFF) -> bit SET,
//      exactly backwards from MSG.  Note the palette itself is NOT consulted on
//      the write path, so setPaletteColor() cannot fix this.
//      What can: getColorConverter() is PUBLIC (LGFXBase.hpp:136) and hands out
//      the color_conv_t whose function pointers every draw call goes through.
//      Swapping in an inverted palette1 converter re-aligns the two conventions
//      at zero per-pixel cost and with ZERO changes to any UI source file —
//      cjk_text.cpp and action_band.cpp below are compiled unmodified and keep
//      passing TFT_BLACK for ink.
//
// Deliberately NOT done, and this is load-bearing: M5.begin() is never called.
// It would bring up M5GFX's Bus_EPD/Panel_EPD, which claims the i80 bus and the
// panel pins that msg.c drives itself — and esp_lcd_new_i80_bus() aborts if the
// bus is already taken, so msg_init() would kill the boot.  <M5Unified.h> is
// only included for the M5Canvas type and the TFT_* constants (cjk_text.cpp and
// action_band.cpp include it for the same reason); no M5 subsystem is started.
//
// The whole file is wrapped in #ifdef MSG_SPIKE so that env:adarkroom — which
// has no build_src_filter and therefore compiles the entire src/ tree — sees an
// empty translation unit here instead of a second setup()/loop().  Same guard,
// same reason, as src/bench/bench.cpp.
#ifdef MSG_SPIKE

#include <Arduino.h>
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <string.h>

#include "action_band.h"
#include "cjk_text.h"
#include "msg.h"
#include "page.h"

// Which way up.  1 and 3 both present a 540x960 portrait canvas over the
// 960x540 buffer and differ only by 180 degrees; which one is "up" depends on
// how the panel sits, and the game picks it from the accelerometer at boot
// (main.cpp's rotForAccel).  The spike hardcodes one and draws a corner marker
// so the bench can see at a glance whether the other is wanted.
static const uint8_t SPIKE_ROT = 3;

// Portrait canvas geometry, i.e. exactly the PANEL_W/PANEL_H the game's pages
// are written against (main.cpp:61-62).  Stated in terms of MSG's landscape
// constants so the transposition is visible rather than asserted.
static const int UI_W = EPD_HEIGHT;   // 540
static const int UI_H = EPD_WIDTH;    // 960

// ---- the bridge -------------------------------------------------------------

// No parent: nothing is ever pushed out of this sprite, it IS the framebuffer.
// Passing &M5.Display would be the usual M5Canvas idiom and is precisely what
// must not happen here — see the header note on M5.begin().
static m5gfx::M5Canvas s_spr;

// MSG's polarity, as a drop-in for convert_uint32_to_palette1: set bit = black.
// (c & 1) is LovyanGFX's own rule for reading a palette index out of whatever
// integer the caller passed, so TFT_BLACK (0x0000) -> 0x00 -> 0xFF here, and
// TFT_WHITE (0xFFFF) -> 0x01 -> 0x00.  The 0xFF/0x00 (rather than 1/0) is
// required: writeFillRectPreclipped memsets whole bytes with this value in the
// sub-byte path, so it has to be the replicated bit pattern.
static uint32_t msgInkPalette1(uint32_t c) { return (c & 1) ? 0x00 : 0xFF; }

// Point the sprite at a framebuffer.  Called EVERY frame because msg_flip()
// alternates between two buffers, so the canvas has to follow the back buffer.
//
// Two ordering rules, both learned from the sources rather than by experiment:
//   - bpp is left at its 0 default here, which is what makes setBuffer() SKIP
//     _write_conv.setColorDepth() (LGFX_Sprite.hpp:158) and therefore keep the
//     ink converter installed by bridgeInit().  Passing 1 again would silently
//     reinstall the stock palette1 converter and invert the whole UI.
//   - setRotation() must follow, because setBuffer() resets the clip rect to
//     the UNROTATED 960x540 (LGFX_Sprite.hpp:163-169) and LGFXBase::setRotation
//     is what calls clearClipRect() against the rotated width()/height()
//     (LGFXBase.cpp:59-64, :117-123).  Skip it and everything below y=540 is
//     clipped away.
static inline void bindFrame(uint8_t* fb) {
    s_spr.setBuffer(fb, EPD_WIDTH, EPD_HEIGHT);
    s_spr.setRotation(SPIKE_ROT);
}

// One-time setup of the canvas side of the bridge.
static void bridgeInit(uint8_t* fb) {
    // BEFORE setBuffer, and this order is not stylistic: LGFX_Sprite::
    // setColorDepth() is the only thing that propagates the depth into the
    // panel (_panel_sprite.setColorDepth at LGFX_Sprite.hpp:318), which is
    // where _write_bits comes from.  setBuffer() alone would leave the panel at
    // its rgb565_2Byte default and address the buffer 16 bits per pixel.
    // It is safe to call with no buffer attached — it returns early at
    // LGFX_Sprite.hpp:320 instead of allocating anything.
    s_spr.setColorDepth(1);
    bindFrame(fb);

    // Re-align LovyanGFX's 1bpp polarity with MSG's.  All five source-type
    // entries, not just rgb565: the stock palette path installs the same
    // converter for every one of them (colortype.hpp:775-780), so leaving any
    // behind would give a caller that passes a uint32_t colour the opposite
    // ink from one that passes a uint16_t.
    auto* cc = s_spr.getColorConverter();
    cc->convert_rgb565   = msgInkPalette1;
    cc->convert_rgb888   = msgInkPalette1;
    cc->convert_rgb332   = msgInkPalette1;
    cc->convert_bgr888   = msgInkPalette1;
    cc->convert_argb8888 = msgInkPalette1;
}

// ---- the scene --------------------------------------------------------------
//
// Everything below draws in PORTRAIT 540x960 coordinates through the plain
// m5gfx::M5Canvas& that every page in this firmware already takes, using the
// repo's real components (cjk::drawText, action_band::draw) with their real
// signatures and their real TFT_BLACK ink.  That is the whole proof: if this
// lands right side up and right side black, the game's pages will too.

static const int BALL_W = 60;
static const int BALL_H = 20;

// Per-stage render cost, in microseconds, of the last frame.  Split because the
// migration question is not "is it fast enough" but "where does the budget go":
// clear is a 64800 B memset through PSRAM, the bands are the component under
// test, and the ball is the only thing that actually has to move at frame rate.
static uint32_t s_usClear, s_usText, s_usBands, s_usBall;

static void renderFrame(uint32_t frame, int ballX, const char* statLine) {
    uint32_t t0 = micros();
    // TFT_WHITE through the inverted converter -> 0x00 -> every bit clear ->
    // white on the glass.  Deliberately NOT a memset(fb, 0, EPD_FB_SIZE): going
    // through fillScreen is what proves the converter is wired up on the
    // wholesale path as well as the per-pixel one.
    s_spr.fillScreen(TFT_WHITE);
    s_usClear = micros() - t0;

    t0 = micros();
    // Corner marker: portrait (0,0) must land at the top-left of what the user
    // sees.  If it comes up bottom-right, SPIKE_ROT wants to be 1 instead of 3.
    s_spr.fillRect(0, 0, 24, 24, TFT_BLACK);
    cjk::drawText(s_spr, 32, 4, "TL", 2);

    cjk::drawText(s_spr, 24, 60, "小黑屋 MSG 试验", 3);
    char buf[96];
    snprintf(buf, sizeof(buf), "帧 %lu  转向 %d  %dx%d",
             (unsigned long)frame, (int)SPIKE_ROT, UI_W, UI_H);
    cjk::drawText(s_spr, 24, 110, buf, 2);
    cjk::drawText(s_spr, 24, 140, statLine, 2);

    // The full portrait height has to be reachable, not just the first 540 rows
    // that an un-rotated clip rect would allow — so put something at the very
    // bottom and let the panel prove the clip followed the rotation.
    cjk::drawText(s_spr, 24, UI_H - 40, "底边 540x960 可达", 2);
    s_usText = micros() - t0;

    t0 = micros();
    // Two real buttons, drawn by the real component with the real geometry the
    // game's Room page uses: one live, one unavailable (the dashed frame).
    pages::Rect r1 = {24, 200, UI_W - 48, 96};
    action_band::draw(s_spr, r1, "狩猎小屋", "-200 木头  -10 毛皮", true, 0, 0);
    pages::Rect r2 = {24, 312, UI_W - 48, 96};
    action_band::draw(s_spr, r2, "熏肉房", "-500 木头", false, 0, 0);
    // ... and one with a draining cooldown bar, the third state.
    pages::Rect r3 = {24, 424, UI_W - 48, 96};
    action_band::draw(s_spr, r3, "添柴", "-1 木头", true, 3, 10);
    s_usBands = micros() - t0;

    t0 = micros();
    s_spr.fillRect(ballX, 600, BALL_W, BALL_H, TFT_BLACK);
    // A static rule under the ball, so the eye has a fixed reference to judge
    // trailing/ghosting against as the block passes over it.
    s_spr.drawFastHLine(0, 640, UI_W, TFT_BLACK);
    s_usBall = micros() - t0;
}

// ---- scan statistics --------------------------------------------------------
//
// Same shape as PaperBar's reportStats: sample once per interval and difference
// the cumulative counters, so wraparound cancels.  fps here is the measured
// SCAN rate — the number the whole spike exists to confirm against the 43.4 Hz
// PaperBar measured — and frame avg is the scan PERIOD, not just the work.
static msg_stats_t s_prev;
static uint32_t    s_prevMs;
static bool        s_primed;

static void reportStats(uint32_t nowMs, char* out, size_t n) {
    msg_stats_t now;
    msg_get_stats(&now);
    if (!s_primed) {
        s_prev = now; s_prevMs = nowMs; s_primed = true;
        snprintf(out, n, "priming");
        return;
    }
    uint32_t dFrames = now.frames - s_prev.frames;
    uint32_t dUs     = now.frame_us_sum - s_prev.frame_us_sum;
    uint32_t dMs     = nowMs - s_prevMs;
    s_prev = now; s_prevMs = nowMs;
    if (dMs == 0) return;

    uint32_t avgUs = dFrames ? dUs / dFrames : 0;
    float    fps   = 1000.0f * (float)dFrames / (float)dMs;

    snprintf(out, n, "%.1f Hz  %uus", fps, (unsigned)avgUs);
    Serial.printf("[spike] STATS fps=%5.1f frame avg=%6uus max=%6uus "
                  "dmato=%u wakes=%u active=%u | render clear=%uus text=%uus "
                  "bands=%uus ball=%uus total=%uus\n",
                  fps, (unsigned)avgUs, (unsigned)now.frame_us_max,
                  (unsigned)now.dma_timeouts, (unsigned)now.wakes,
                  (unsigned)now.active_bytes,
                  (unsigned)s_usClear, (unsigned)s_usText,
                  (unsigned)s_usBands, (unsigned)s_usBall,
                  (unsigned)(s_usClear + s_usText + s_usBands + s_usBall));
}

// ---- the app task -----------------------------------------------------------
//
// Core 0.  msg.c pins its scan task to core 1 (msg.c's xTaskCreatePinnedToCore
// in msg_start), and the whole point of the driver is that the scan owns that
// core outright — so every line of application code has to be somewhere else.
static void appTask(void* arg) {
    (void)arg;

    // Clear the panel white through IMAGE mode (the conventional 8-field LUT
    // push) before video mode ever runs, exactly as PaperBar's main does: the
    // back buffer doubles as the image source because video is still off and
    // nothing else is reading it.  0xFF + to_white is the image-mode idiom;
    // video mode's polarity is the opposite one (0 = white), which is why the
    // memset that follows is 0x00 and not another 0xFF.
    uint8_t* fb = msg_flip_nowait();
    memset(fb, 0xFF, EPD_FB_SIZE);
    msg_display_image(fb, true);
    vTaskDelay(pdMS_TO_TICKS(1000));
    memset(fb, 0x00, EPD_FB_SIZE);

    msg_enable_video(true);
    fb = msg_flip();

    bridgeInit(fb);
    Serial.printf("[spike] bridge: sprite %dx%d depth=%d rot=%d "
                  "buflen=%u (EPD_FB_SIZE=%u)\n",
                  (int)s_spr.width(), (int)s_spr.height(),
                  (int)s_spr.getColorDepth(), (int)SPIKE_ROT,
                  (unsigned)s_spr.bufferLength(), (unsigned)EPD_FB_SIZE);

    int  ballX  = 0;
    int  ballDx = 8;
    uint32_t frame = 0;
    uint32_t lastReport = millis();
    char statLine[64] = "…";

    while (true) {
        // The buffer alternates on every flip, so re-bind before drawing.
        bindFrame(fb);
        renderFrame(frame, ballX, statLine);
        fb = msg_flip();

        ballX += ballDx;
        if (ballX <= 0)              { ballX = 0;              ballDx = -ballDx; }
        if (ballX >= UI_W - BALL_W)  { ballX = UI_W - BALL_W;  ballDx = -ballDx; }
        frame++;

        uint32_t now = millis();
        if (now - lastReport >= 1000) {
            lastReport = now;
            reportStats(now, statLine, sizeof(statLine));
        }
    }
}

void setup() {
    Serial.begin(115200);
    // TinyUSB CDC needs a moment before the host reattaches; without it the
    // banner and — more importantly — the selftest line are lost.
    delay(3000);

    Serial.println();
    Serial.println("[spike] ==== PaperDarkRoom MSG 1bpp spike ====");
    Serial.printf("[spike] cpu=%uMHz internal free=%u B  psram free=%u B\n",
                  (unsigned)getCpuFrequencyMhz(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (!msg_init()) {
        Serial.println("[spike] FATAL: msg_init() failed (allocation)");
        while (true) delay(1000);
    }
    Serial.printf("[spike] msg_init ok  fb=%s out=%s | internal free=%u B "
                  "psram free=%u B\n",
                  msg_fb_in_psram() ? "psram" : "sram",
                  msg_out_in_psram() ? "psram" : "sram",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // The kernel selftest re-derives a randomised block of rows with the scalar
    // reference and with each candidate and compares them byte for byte; msg.c
    // has no Serial, so printing the line it writes is the caller's job.
    char simd[192];
    bool ok = msg_selftest(simd, sizeof(simd));
    Serial.printf("[spike] %s\n", simd);
    if (!ok) Serial.println("[spike] WARNING: selftest reported a mismatch");

    msg_start();
    xTaskCreatePinnedToCore(appTask, "spike", 8192, NULL, 1, NULL, 0);
}

void loop() {
    // The scan owns core 1 and appTask owns core 0; Arduino's loop task has no
    // work and must not sit spinning against either.
    vTaskSuspend(NULL);
}

#endif  // MSG_SPIKE
