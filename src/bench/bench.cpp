// EPD fast-path bench — standalone firmware (env:bench-epd).
//
// Question it answers: can M5GFX's Panel_EPD fast paths (epd_fastest / epd_fast)
// drive a ~10fps space mini-game on the M5PaperS3's 540x960 16-grayscale panel?
//
// Three measurements, all printed to Serial at 115200:
//   (a) full-screen canvas pushSprite + waitDisplay, broken into push vs settle,
//       at epd_fastest and epd_fast;
//   (b) a "no canvas" floor: the same primitives drawn straight onto M5.Display,
//       so the sprite-compositing + blit cost drops out of the number;
//   (c) a free-run window with NO waitDisplay between frames, to probe what the
//       panel's async update queue actually does under backpressure.
//
// Everything happens in setup(); loop() just idles. Nothing here touches the
// game's save files, SD, BLE or the RTC — this firmware only drives the panel.
//
// BENCH_EPD guard: env:adarkroom has no build_src_filter, so it compiles this
// file too. Only env:bench-epd defines BENCH_EPD, so under the game build this
// whole file is an empty translation unit and its setup()/loop() never collide
// with main.cpp's. See platformio.ini.
#ifdef BENCH_EPD

#include <Arduino.h>
#include <M5Unified.h>

static const int PANEL_W = 540;
static const int PANEL_H = 960;

// Simulated-frame geometry. The ship is a solid rect that slides across the
// panel; the asteroids are solid circles scattered over the whole field. Both
// are deliberately chunky so each frame does real per-pixel fill work, in the
// same ballpark as an actual mini-game frame.
static const int SHIP_W    = 48;
static const int SHIP_H    = 36;
static const int ASTEROIDS = 12;

M5Canvas canvas(&M5.Display);

static bool g_canvasOk = false;

// Fixed-seed LCG. Deterministic *within* a run so repeated frames cost roughly
// the same; it is not reseeded between bench() calls, which is fine — the runs
// only need comparable per-frame work, not byte-identical frames.
static uint32_t g_lcg = 0x12345678u;
static inline uint32_t lcgNext() {
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg;
}

// ESP.getFreeHeap() is internal SRAM only on this core; PSRAM has its own getter.
static inline uint32_t freeInternal() { return ESP.getFreeHeap(); }
static inline uint32_t freePsram()    { return ESP.getFreePsram(); }

// One simulated game frame, composed into the offscreen canvas.
static void composeFrame(int frameIdx) {
    canvas.fillSprite(TFT_WHITE);
    int x = (frameIdx * 7) % (PANEL_W - SHIP_W);
    canvas.fillRect(x, PANEL_H - 140, SHIP_W, SHIP_H, TFT_BLACK);
    for (int i = 0; i < ASTEROIDS; ++i) {
        int cx = (int)(lcgNext() % (uint32_t)PANEL_W);
        int cy = (int)(lcgNext() % (uint32_t)PANEL_H);
        int r  = 8 + (int)(lcgNext() % 20u);
        canvas.fillCircle(cx, cy, r, TFT_BLACK);
    }
}

// The same asteroid field, drawn straight onto the panel framebuffer with no
// sprite in the middle. NOTE: positions span the whole panel, so Panel_EPD's
// accumulated _range_mod bounding box still ends up ~full-screen — this run
// isolates the cost of the canvas compositing + pushSprite blit, it does NOT
// measure a small-rect partial update.
static void drawAsteroidsDirect() {
    for (int i = 0; i < ASTEROIDS; ++i) {
        int cx = (int)(lcgNext() % (uint32_t)PANEL_W);
        int cy = (int)(lcgNext() % (uint32_t)PANEL_H);
        int r  = 8 + (int)(lcgNext() % 20u);
        M5.Display.fillCircle(cx, cy, r, TFT_BLACK);
    }
}

// Full-screen canvas path: compose -> pushSprite -> waitDisplay, timed per stage.
static void bench(const char* name, epd_mode_t mode, int frames) {
    M5.Display.setEpdMode(mode);

    // Settle once on a clean field so frame 0 isn't paying for stale panel state.
    canvas.fillSprite(TFT_WHITE);
    canvas.pushSprite(0, 0);
    M5.Display.waitDisplay();

    uint64_t pushUs = 0, settleUs = 0, totalUs = 0;
    uint32_t worstTotalUs = 0;

    for (int f = 0; f < frames; ++f) {
        composeFrame(f);
        uint32_t t0 = micros();
        canvas.pushSprite(0, 0);
        uint32_t t1 = micros();
        M5.Display.waitDisplay();
        uint32_t t2 = micros();

        pushUs   += (uint32_t)(t1 - t0);
        settleUs += (uint32_t)(t2 - t1);
        uint32_t frameUs = (uint32_t)(t2 - t0);
        totalUs += frameUs;
        if (frameUs > worstTotalUs) worstTotalUs = frameUs;
    }

    unsigned long avgTotal = (unsigned long)(totalUs / frames);
    Serial.printf("[bench] %-14s mode=%d frames=%3d push_avg=%6luus settle_avg=%6luus "
                  "total_avg=%6luus worst=%6luus fps=%.2f\n",
                  name, (int)mode, frames,
                  (unsigned long)(pushUs / frames),
                  (unsigned long)(settleUs / frames),
                  avgTotal, (unsigned long)worstTotalUs,
                  avgTotal ? 1e6 / (double)avgTotal : 0.0);
}

// No-canvas floor: primitives straight onto M5.Display, then settle.
static void benchDirty(const char* name, epd_mode_t mode, int frames) {
    M5.Display.setEpdMode(mode);
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.waitDisplay();

    uint64_t drawUs = 0, settleUs = 0, totalUs = 0;
    uint32_t worstTotalUs = 0;

    for (int f = 0; f < frames; ++f) {
        uint32_t t0 = micros();
        drawAsteroidsDirect();
        uint32_t t1 = micros();
        M5.Display.waitDisplay();
        uint32_t t2 = micros();

        drawUs   += (uint32_t)(t1 - t0);
        settleUs += (uint32_t)(t2 - t1);
        uint32_t frameUs = (uint32_t)(t2 - t0);
        totalUs += frameUs;
        if (frameUs > worstTotalUs) worstTotalUs = frameUs;
    }

    unsigned long avgTotal = (unsigned long)(totalUs / frames);
    Serial.printf("[bench] %-14s mode=%d frames=%3d draw_avg=%6luus settle_avg=%6luus "
                  "total_avg=%6luus worst=%6luus fps=%.2f\n",
                  name, (int)mode, frames,
                  (unsigned long)(drawUs / frames),
                  (unsigned long)(settleUs / frames),
                  avgTotal, (unsigned long)worstTotalUs,
                  avgTotal ? 1e6 / (double)avgTotal : 0.0);
}

// Free run: push frames as fast as they'll go, never waiting for settle.
//
// This measures how many pushSprite calls the panel's async update queue will
// accept without the caller ever waiting. Panel_EPD's queue is only 8 deep
// (xQueueCreate(8, ...) in Panel_EPD.cpp) and the enqueue uses a 128ms timeout
// (xQueueSend(..., 128 / portTICK_PERIOD_MS)), so once the queue saturates
// pushSprite stops returning instantly and starts blocking — possibly dropping
// the frame outright when the send times out. Whichever of those two happens is
// exactly what this window is meant to expose; do not "fix" it here.
static void benchFreeRun(uint32_t windowMs) {
    M5.Display.setEpdMode(epd_mode_t::epd_fastest);

    canvas.fillSprite(TFT_WHITE);
    canvas.pushSprite(0, 0);
    M5.Display.waitDisplay();

    uint32_t count = 0;
    uint32_t start = millis();
    while ((uint32_t)(millis() - start) < windowMs) {
        composeFrame((int)count);
        canvas.pushSprite(0, 0);
        ++count;
    }
    uint32_t elapsed = millis() - start;

    // Drain whatever is still queued before anything else touches the panel.
    M5.Display.waitDisplay();

    Serial.printf("[bench] %-14s mode=%d pushes=%4lu elapsed=%5lums fps=%.2f "
                  "(no waitDisplay between frames)\n",
                  "freerun", (int)epd_mode_t::epd_fastest,
                  (unsigned long)count, (unsigned long)elapsed,
                  elapsed ? (double)count * 1000.0 / (double)elapsed : 0.0);
}

// "洗玻璃": leave the panel clean at a quality refresh so a subsequent game
// flash doesn't inherit ghosting from all the fast-path frames above.
static void washGlass() {
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    M5.Display.fillScreen(TFT_WHITE);
    M5.Display.waitDisplay();
}

void setup() {
    auto cfg = M5.config();
    cfg.clear_display = false;   // we control every panel refresh ourselves
    M5.begin(cfg);

    Serial.begin(115200);
    // The whole value of this firmware is its serial log and all of it is
    // printed from setup(), so give USB-CDC a bounded moment to enumerate on
    // the host rather than losing the header lines. Never blocks on a host.
    delay(2000);

    Serial.println();
    Serial.println("[bench] epd fast-path bench starting");
    Serial.printf("[bench] panel=%dx%d reset=%d\n", PANEL_W, PANEL_H,
                  (int)esp_reset_reason());
    Serial.printf("[bench] heap before canvas: internal=%lu psram=%lu\n",
                  (unsigned long)freeInternal(), (unsigned long)freePsram());

    M5.Display.setRotation(0);
    M5.Display.setEpdMode(epd_mode_t::epd_fast);

    canvas.setColorDepth(m5gfx::grayscale_8bit);
    g_canvasOk = canvas.createSprite(PANEL_W, PANEL_H);
    Serial.printf("[bench] heap after  canvas: internal=%lu psram=%lu\n",
                  (unsigned long)freeInternal(), (unsigned long)freePsram());

    if (!g_canvasOk) {
        Serial.println("[bench] FATAL: canvas alloc failed");
        washGlass();
        Serial.println("[bench] done.");
        return;   // loop() idles forever; never dereference the null sprite
    }
    canvas.fillSprite(TFT_WHITE);

    bench("fastest/full", epd_mode_t::epd_fastest, 30);
    bench("fast/full",    epd_mode_t::epd_fast,    10);
    benchDirty("fastest/dirty", epd_mode_t::epd_fastest, 30);
    benchFreeRun(3000);

    washGlass();
    Serial.println("[bench] done.");
}

void loop() {
    delay(1000);
}

#endif  // BENCH_EPD
