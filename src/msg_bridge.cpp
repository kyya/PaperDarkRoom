#include "msg_bridge.h"
#include <Arduino.h>
#include <M5Unified.h>          // the M5Canvas type and TFT_* only — see below
#include <esp_heap_caps.h>
#include <string.h>
#include "msg.h"

// <M5Unified.h> is included for m5gfx::M5Canvas and nothing else. M5.begin() is
// never called anywhere in this firmware and must not be: it brings up M5GFX's
// Bus_EPD/Panel_EPD, which claims the i80 bus and the panel pins msg.c drives
// itself, and esp_lcd_new_i80_bus() aborts outright if the bus is already taken
// — so a stray M5.begin() would not misbehave, it would kill the boot. Every
// M5.* runtime singleton the firmware used to lean on has a bare replacement:
// touch_gt911, rtc_bm8563, beeper, power_s3.

namespace msg_bridge {

// The portrait canvas is MSG's landscape buffer with the axes exchanged. Assert
// it rather than restate it, so a future panel with different geometry fails to
// compile instead of drawing garbage.
static_assert(UI_W == EPD_HEIGHT, "portrait width must be MSG's scan height");
static_assert(UI_H == EPD_WIDTH,  "portrait height must be MSG's scan width");

// Rotation 3 puts portrait (px,py) at buffer (py, 539-px): Panel_Sprite's
// transform for r=3 is "x = _width-(x+1); swap(x,y)" with _width already the
// ROTATED width (540). That is the same mapping PaperBar's gfx1bpp applies to
// this panel, which is why touch_gt911's raw chip coordinates need no
// correction. Rotation 1 is the same canvas turned 180 degrees; if the picture
// ever comes up upside down, this constant and nothing else is what changes —
// but the touch frame turns with it, so they must be reasoned about together.
static const uint8_t UI_ROTATION = 3;

// Ceiling on the 8 fields of an image push. A field is one scan period, so the
// push itself is eight of those; the ceiling is generous because the governor
// may have had the scan parked when the request went in and the wake costs a
// frame. Reaching it means the scan stopped, which is a fault worth a log line.
static const uint32_t IMAGE_PUSH_TIMEOUT_MS = 1200;

// Settling time after the last field's VSYNC — see pushImage(). One scan period
// plus margin, so the rows of the field the counter just announced are actually
// on the glass before anything else drives the panel.
static const uint32_t IMAGE_PUSH_GRACE_MS = 30;

static m5gfx::M5Canvas* s_spr  = nullptr;   // the shared full-screen canvas
static uint8_t*         s_back = nullptr;   // buffer the canvas is bound to
static uint8_t*         s_scratch = nullptr;

// MSG's ink polarity, as a drop-in for LovyanGFX's convert_uint32_to_palette1.
// (c & 1) is LovyanGFX's own rule for reading a palette index out of whatever
// integer the caller passed, so TFT_BLACK (0x0000) -> 0x00 -> 0xFF here and
// TFT_WHITE (0xFFFF) -> 0x01 -> 0x00, which is MSG's "set bit = black". The
// 0xFF/0x00 rather than 1/0 is required, not stylistic: writeFillRectPreclipped
// memsets whole bytes with this value on the sub-byte path, so it has to be the
// replicated bit pattern.
static uint32_t inkPalette1(uint32_t c) { return (c & 1) ? 0x00 : 0xFF; }

// Defined below with the rest of the public surface; begin() needs it for the
// boot clear, which is the same 8-field LUT push the deghost pair uses.
void pushImage(uint8_t* fb, bool toWhite);

// Point the canvas at `fb`. Called after every flip because the two buffers
// alternate. bpp is left at its 0 default deliberately: that is what makes
// setBuffer() SKIP _write_conv.setColorDepth() and therefore keep the ink
// converter installed by begin(). Passing 1 again would silently reinstall the
// stock palette1 converter and invert the entire UI. setRotation() must follow,
// because setBuffer() resets the clip rect to the UNROTATED 960x540 and
// LGFXBase::setRotation is what re-clips against the rotated width/height.
static void bindFrame(uint8_t* fb) {
    s_spr->setBuffer(fb, EPD_WIDTH, EPD_HEIGHT);
    s_spr->setRotation(UI_ROTATION);
}

bool init() {
    if (!msg_init()) {
        Serial.println("[msg] FATAL: msg_init() failed (allocation)");
        return false;
    }
    // One spare framebuffer for the deghost sequence. PSRAM: it is touched twice
    // per sleep, never in the render path, so it has no claim on internal SRAM —
    // which msg.c wants for its precomputed output frame and Bluedroid wants for
    // everything else.
    s_scratch = (uint8_t*)heap_caps_malloc(EPD_FB_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_scratch)
        Serial.println("[msg] WARN: no scratch buffer — deghost will be skipped");

    Serial.printf("[msg] init ok  fb=%s out=%s | internal free=%u B psram free=%u B\n",
                  msg_fb_in_psram() ? "psram" : "sram",
                  msg_out_in_psram() ? "psram" : "sram",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // The kernel selftest re-derives a randomised block of rows with the scalar
    // reference and with each SIMD candidate and compares them byte for byte; a
    // candidate that disagrees is dropped rather than shipped. msg.c has no
    // Serial, so printing the line it writes is the caller's job.
    char simd[192];
    bool ok = msg_selftest(simd, sizeof(simd));
    Serial.printf("[msg] %s\n", simd);
    if (!ok) Serial.println("[msg] WARNING: selftest reported a mismatch");

    msg_start();
    return true;
}

void begin(m5gfx::M5Canvas& spr) {
    s_spr = &spr;

    // Clear to white through IMAGE mode (the conventional 8-field LUT push)
    // before video mode ever runs. The back buffer doubles as the image source
    // because video is still off and nothing else is reading it. 0xFF + to_white
    // is the image-mode idiom — a set bit means "lighten this pixel" there —
    // which is why the memset that follows is 0x00 and not another 0xFF: video
    // mode's convention is the opposite one, set bit = black.
    //
    // pushImage() rather than the flat one-second sleep the spike (and PaperBar)
    // used here: it waits on the VSYNC counter, so it costs the eight fields the
    // push actually takes (~185 ms) instead of five times that. EVERY WAKE ON
    // THIS BOARD IS A COLD BOOT, including the 15-minute background ones and the
    // user's own power-button press, so a fixed delay on this path is charged to
    // battery life and to time-to-first-pixel alike.
    uint8_t* fb = msg_flip_nowait();
    memset(fb, 0xFF, EPD_FB_SIZE);
    pushImage(fb, true);
    memset(fb, 0x00, EPD_FB_SIZE);

    msg_enable_video(true);
    s_back = msg_flip();

    // setColorDepth BEFORE setBuffer, and the order is not stylistic:
    // LGFX_Sprite::setColorDepth() is the only thing that propagates the depth
    // into the panel object, which is where _write_bits comes from. setBuffer()
    // alone would leave the panel at its rgb565 default and address the buffer
    // 16 bits per pixel. It is safe to call with no buffer attached — it returns
    // early instead of allocating anything.
    s_spr->setColorDepth(1);
    bindFrame(s_back);

    // Re-align LovyanGFX's 1bpp polarity with MSG's. All five source-type
    // entries, not just rgb565: the stock palette path installs the same
    // converter for every one of them, so leaving any behind would give a caller
    // that passes a uint32_t colour the opposite ink from one that passes a
    // uint16_t.
    auto* cc = s_spr->getColorConverter();
    cc->convert_rgb565   = inkPalette1;
    cc->convert_rgb888   = inkPalette1;
    cc->convert_rgb332   = inkPalette1;
    cc->convert_bgr888   = inkPalette1;
    cc->convert_argb8888 = inkPalette1;

    Serial.printf("[msg] bridge: sprite %dx%d depth=%d rot=%d buflen=%u "
                  "(EPD_FB_SIZE=%u)\n",
                  (int)s_spr->width(), (int)s_spr->height(),
                  (int)s_spr->getColorDepth(), (int)UI_ROTATION,
                  (unsigned)s_spr->bufferLength(), (unsigned)EPD_FB_SIZE);
}

uint8_t* frame()      { return s_back; }
size_t   frameBytes() { return EPD_FB_SIZE; }
uint8_t* scratch()    { return s_scratch; }

// THE PANEL HAS EXACTLY ONE WRITING TASK, and this is where that is enforced.
//
// Two things behind present() are single-owner and neither is defended by a lock
// of its own. The canvas is one global sprite bound to the live back buffer, so
// two renderers interleave into the same pixels. Worse, msg_flip() records its
// caller in ONE global slot (lib/msg/src/msg.c:2379, `flip_waiter_task = self`)
// and then waits with portMAX_DELAY; a second caller overwrites that slot, the
// scan notifies only the survivor (msg.c:1316-1318), and the loser blocks
// FOREVER. There is no timeout to recover through and lib/msg is vendored
// unmodified by policy, so the discipline has to live here.
//
// This cost a full debugging round: ble_link's connect callback drew the status
// bar from the Bluedroid task, which deadlocked the app task on the first BLE
// connection of every boot — and because the same callback had just started an
// 80ms chime whose stop ran on the now-dead app loop, the symptom the user got
// was a card that buzzed continuously and forever. A one-line ownership check is
// cheap; a silent deadlock that presents as a stuck buzzer is not.
static TaskHandle_t s_owner = nullptr;

void present() {
    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    if (s_owner == nullptr) {
        s_owner = self;                       // first presenter claims the panel
    } else if (s_owner != self) {
        // Refuse rather than deadlock. Losing a frame is recoverable; losing the
        // app task is not, and the log line names the bug outright.
        Serial.printf("[msg] REFUSED present() from task '%s' — the panel "
                      "belongs to '%s'. Capture in the callback and repaint "
                      "from the app loop.\n",
                      pcTaskGetName(self), pcTaskGetName(s_owner));
        return;
    }
    s_back = msg_flip();
    bindFrame(s_back);
}

void invertRect(int x, int y, int w, int h) {
    if (!s_back) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > UI_W) x1 = UI_W;
    if (y1 > UI_H) y1 = UI_H;
    if (x1 <= x0 || y1 <= y0) return;
    // Portrait (px,py) -> buffer (py, 539-px), the rotation-3 mapping this file
    // is built on. Walking py in the inner loop therefore walks a CONTIGUOUS run
    // of buffer bytes, which is why the loops are nested this way round and not
    // the intuitive one; the rect's portrait rows are the buffer's columns.
    for (int px = x0; px < x1; px++) {
        int by = (UI_W - 1) - px;
        uint8_t* row = s_back + (size_t)by * EPD_LINE_BYTES;
        for (int py = y0; py < y1; py++)
            row[py >> 3] ^= (uint8_t)(0x80 >> (py & 7));
    }
}

void waitSettled(uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while (msg_get_active_bytes() != 0) {
        if (millis() - t0 >= timeoutMs) {
            Serial.printf("[msg] waitSettled: %u B still driving after %lu ms\n",
                          (unsigned)msg_get_active_bytes(),
                          (unsigned long)timeoutMs);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void pushImage(uint8_t* fb, bool toWhite) {
    if (!fb) return;
    // msg_display_image() only raises a request and wakes the scan; the scan
    // then spends EPD_IMAGE_FIELDS whole frames on it, taking priority over
    // video mode, and clears the request on the last one. There is no completion
    // callback, so wait on the VSYNC counter, which advances once per frame.
    //
    // WAIT FOR EIGHT, NOT NINE, and the distinction is not pedantry — it is the
    // difference between a ~190 ms push and a guaranteed 1.2 s timeout on every
    // single call. msg.c increments s_vsync_count at the TOP of a frame
    // (msg.c:1381), so the eighth field runs in the frame that takes the counter
    // to v0+8. That same frame clears img_req as it finishes the field
    // (msg.c:1436-1439), and the governor's end-of-frame test then reads
    // active_bytes==0 && !flip_req && !img_req && !pause_req and PARKS
    // (msg.c:1650) — active_bytes being zero because the image branch, unlike
    // the video branch, never assigns to it (it is initialised to 0 at
    // msg.c:1378). So the counter stops at v0+8 and a ninth frame is never
    // produced: there is nothing left to scan for and nothing to unpark it.
    //
    // The grace after it covers what the counter cannot see. The counter moved
    // at the START of the eighth field, so its ~23 ms of rows are still going
    // out; a scan period plus a little is enough for the last row to be latched
    // and the panel to settle before the caller pushes the next image at it.
    uint32_t v0 = msg_get_vsync_count();
    msg_display_image(fb, toWhite);
    uint32_t t0 = millis();
    while (msg_get_vsync_count() - v0 < (uint32_t)EPD_IMAGE_FIELDS) {
        if (millis() - t0 >= IMAGE_PUSH_TIMEOUT_MS) {
            // A real fault now, not the everyday case: the scan stopped
            // producing frames with an image request outstanding.
            Serial.printf("[msg] pushImage: only %u/%u fields after %lu ms\n",
                          (unsigned)(msg_get_vsync_count() - v0),
                          (unsigned)EPD_IMAGE_FIELDS,
                          (unsigned long)IMAGE_PUSH_TIMEOUT_MS);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelay(pdMS_TO_TICKS(IMAGE_PUSH_GRACE_MS));
}

bool park() {
    if (msg_pause()) return true;
    Serial.println("[msg] park FAILED — the scan did not acknowledge the pause");
    return false;
}

bool resume() {
    if (msg_resume()) return true;
    // Not necessarily an error: msg_resume() bails out early and leaves the scan
    // parked if a new pause was requested while it waited. Either way the panel
    // is not scanning, which the caller has to know about.
    Serial.println("[msg] resume FAILED — the scan is still parked");
    return false;
}

// Same shape as the spike's reportStats: sample the cumulative counters once per
// interval and difference them, so wraparound cancels. `fps` is the measured
// SCAN rate and `avg` is the scan PERIOD rather than the work in a frame — a
// frame that finishes early is held to the pacer's period and the wait counts.
void statsLine(uint32_t nowMs, char* out, size_t n) {
    static msg_stats_t s_prev;
    static uint32_t    s_prevMs;
    static bool        s_primed;

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
    if (dMs == 0) { snprintf(out, n, "--"); return; }

    uint32_t avgUs = dFrames ? dUs / dFrames : 0;
    float    fps   = 1000.0f * (float)dFrames / (float)dMs;
    snprintf(out, n, "fps=%.1f avg=%uus max=%uus dmato=%u wakes=%u active=%u",
             fps, (unsigned)avgUs, (unsigned)now.frame_us_max,
             (unsigned)now.dma_timeouts, (unsigned)now.wakes,
             (unsigned)now.active_bytes);
}

}  // namespace msg_bridge
