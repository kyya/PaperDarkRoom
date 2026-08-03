#include "status_bar.h"
#include "ble_link.h"
#include "frame_store.h"
#include "pager.h"
#include "pomo.h"
#include "minecraftia16.h"
#include "tomato_icons.h"
#include "power_s3.h"
#include "rtc_bm8563.h"
#include <M5Unified.h>
#include <math.h>       // lroundf — page-dot spacing
#include <string.h>

#ifndef CARD_VERSION
#define CARD_VERSION "0.0.0-dev"   // build always -D's the real one (platformio.ini)
#endif

// USB/charging state lives in main.cpp (computed once per loop via usbPresent()
// — isCharging OR TinyUSB-mounted; see main.cpp's blind-spot notes). Read it
// rather than re-probing so the bar and the FSM never disagree.
extern bool g_onUsb;

namespace status_bar {

static const int BAR_H  = 32;   // band height, panel-bottom
static const int MARGIN = 8;    // left/right inset

// BLE glyph, absorbed from ble_link.cpp. Lucide "bluetooth-connected" (ISC),
// rasterized to a 20x20 1bpp bitmap, MSB-first, rows padded to a byte boundary
// (LovyanGFX drawBitmap convention). Drawn only while connected — the real-time
// ground truth (ble_link::rx.connected), not a telemetry-freshness proxy.
static const int BT_ICON_SIZE = 20;
static const uint8_t BT_ICON_BITS[] = {
    0x00, 0x00, 0x00,
    0x00, 0x60, 0x00,
    0x00, 0x70, 0x00,
    0x00, 0x78, 0x00,
    0x00, 0x6c, 0x00,
    0x06, 0x66, 0x00,
    0x07, 0x6e, 0x00,
    0x03, 0xfc, 0x00,
    0x01, 0xf8, 0x00,
    0x3c, 0xf3, 0xc0,
    0x3c, 0xf3, 0xc0,
    0x01, 0xf8, 0x00,
    0x03, 0xfc, 0x00,
    0x07, 0x6e, 0x00,
    0x06, 0x66, 0x00,
    0x00, 0x6c, 0x00,
    0x00, 0x78, 0x00,
    0x00, 0x70, 0x00,
    0x00, 0x60, 0x00,
    0x00, 0x00, 0x00,
};

// A small charging bolt as two filled triangles (a zigzag), drawn at a pixel
// offset so callers can stamp a white halo around a black core — the bolt has
// to read on BOTH the battery's black fill and its white remainder, so a bare
// black glyph would vanish over a nearly-full (mostly-black) battery.
static void drawBolt(LovyanGFX* dst, int cx, int cy, uint16_t color, int dx, int dy) {
    dst->fillTriangle(cx + 3 + dx, cy - 7 + dy, cx - 4 + dx, cy + 1 + dy,
                      cx + 1 + dx, cy + 1 + dy, color);
    dst->fillTriangle(cx - 3 + dx, cy + 7 + dy, cx + 4 + dx, cy - 1 + dy,
                      cx - 1 + dx, cy - 1 + dy, color);
}

// Displayed battery %: raw getBatteryLevel() is derived from a noisy voltage
// read and flaps several points around the discharge curve's steep region
// (observed bouncing around the 90s on USB, mv jittering 4136-4150), which
// both made the number jump and forced a visible bar repaint every second.
// EMA (time constant ~8 samples at main.cpp's 1s tick) smooths the noise; a
// deadband (the shown value only moves once the filtered level strays >=1.5%
// from it) stops the display flapping between adjacent integers. Real drain/
// charge still tracks — it just steps calmly instead of dithering.
static int32_t s_batEmaX256 = -1;   // 8.8 fixed point; -1 = no valid sample yet
static int     s_batShown   = -1;   // last value adopted for display

// Cached clock, refreshed by sample() alongside the battery. Read by drawTo()
// and by main.cpp's change detector — the SAME value, which is the point.
static int s_clkHour = -1, s_clkMin = -1;

// SAMPLING AND DRAWING ARE SEPARATE, and keeping them separate is not tidiness.
//
// Under MSG the bar is composited into EVERY frame (pager::drawFrame), and a
// frame happens on every repaint: four or so per button press, one a second
// while anything is cooling. When the ADC burst and the EMA step lived inside
// drawTo() that cadence became the filter's cadence, so the ~8-sample time
// constant this filter was designed around collapsed to about a second and the
// 1.5% deadband was being punched through by raw voltage jitter — the exact
// flapping the EMA exists to suppress.
//
// It also broke the "only repaint on a visible change" gate outright: main.cpp
// sampled once to decide, then drawTo() sampled AGAIN and drew a different
// number, so the gate could fire on nothing and miss a real change. On e-ink
// that is a full-panel refresh a second, for a digit that did not move.
//
// So: sample() is the ONE sampler, called from main.cpp's 1 s tick (and once at
// boot, before the first frame); batteryPercent() and clockHHMM() are pure
// reads of what it cached; drawTo() touches no hardware at all.
void sample() {
    int raw = power::batteryLevel();
    if (raw >= 0 && raw <= 100) {
        int32_t x = (int32_t)raw << 8;
        s_batEmaX256 = (s_batEmaX256 < 0) ? x
                     : s_batEmaX256 + (x - s_batEmaX256) / 8;
        int32_t err = s_batEmaX256 - ((int32_t)s_batShown << 8);
        if (s_batShown < 0 || err >= 384 || err <= -384)   // 384 = 1.5 in 8.8
            s_batShown = (int)((s_batEmaX256 + 128) >> 8);
    }
    rtc::Time t;
    if (rtc::getTime(&t)) { s_clkHour = t.hours; s_clkMin = t.minutes; }
}

int batteryPercent() { return s_batShown; }   // -1 until a reading has landed
int clockMinute()    { return s_clkMin; }     // -1 until the clock has been read

// Composite the whole band onto `dst` with its top row at `barTop`. Pure
// drawing — the caller sequences the push. Reads all live state itself.
// The target is the shared 1bpp page canvas; TFT_BLACK/TFT_WHITE reach the
// panel's ink convention through the bridge's colour converter (msg_bridge.h),
// so every element's geometry and colour is written exactly as it always was.
// otaPct < 0 (default) → the steady-state bar (center = page dots). otaPct >= 0
// → the OTA-progress variant: the center slot is replaced by a graphical
// progress bar + "NN%" (see the center section). Left clock/BLE glyph and right
// battery block are identical in both, so the bar stays coherent mid-update.
static void drawTo(LovyanGFX* dst, int barTop, int otaPct = -1) {
    int W = dst->width();
    int cy = barTop + BAR_H / 2;   // vertical centre line for every element

    dst->fillRect(0, barTop, W, BAR_H, TFT_WHITE);   // clear the band's rows

    // Minecraftia @16px — the same face (at the same size) the host's pixel
    // pages use for their small header text, so the bar matches the page
    // style instead of clashing with M5GFX's built-in Font2. middle_* datums
    // are safe with this GFXfont: LovyanGFX derives GFX metrics by scanning
    // actual glyph ink (getDefaultMetric), and every digit here is a uniform
    // 14px tall on a shared baseline, so "center the ink box on cy" is exact.
    dst->setFont(&Minecraftia16);
    dst->setTextColor(TFT_BLACK, TFT_WHITE);

    // ---- left: RTC clock, then BLE glyph (if connected) ----
    int xl = MARGIN;
    // Cached by sample(), never read from the chip here — see the note there.
    char clk[6];
    if (s_clkMin < 0) snprintf(clk, sizeof(clk), "--:--");
    else              snprintf(clk, sizeof(clk), "%02d:%02d", s_clkHour, s_clkMin);
    dst->setTextDatum(middle_left);
    dst->drawString(clk, xl, cy);
    xl += dst->textWidth(clk) + 6;
    if (ble_link::rx.connected) {
        dst->drawBitmap(xl, barTop + (BAR_H - BT_ICON_SIZE) / 2,
                        BT_ICON_BITS, BT_ICON_SIZE, BT_ICON_SIZE, TFT_BLACK);
    }

    // ---- center: OTA progress bar, else iOS TabView-style page dots ----
    if (otaPct >= 0) {
        // OTA in flight (driven by main.cpp's throttle): the page-dot slot is
        // reused for a labelled graphical progress bar — "OTA" text, then a
        // 180x12 outline rect (12px = the battery body height) with a
        // proportional black fill, then "NN%". Same visual language as the
        // battery icon. The "OTA" label is legal as of fw 0.8.9: minecraftia16.h
        // gained 'O'/'T'/'A' glyphs (before that the font had no letters, so this
        // variant deliberately shipped label-less — see the header charset note).
        // The block (label + gap + bar + gap + a fixed "100%" reserve) is
        // centered on W/2, clearing the left clock/glyph and the right battery.
        //
        // INTENTIONAL DIVERGENCE: this OTA variant is NOT mirrored in the iOS
        // FirmwareBarSim — that sim models only the steady-state bar (page dots),
        // so the page-dot geometry below stays the sync contract, and this
        // OTA-only center is firmware-only by design.
        int pct = otaPct > 100 ? 100 : otaPct;
        const int pbw = 180, pbh = 12, gap = 8;
        char opct[8];
        snprintf(opct, sizeof(opct), "%d%%", pct);
        int lblW = dst->textWidth("OTA");
        int numW = dst->textWidth("100%");     // reserve so the bar edge is fixed
        int x0   = W / 2 - (lblW + gap + pbw + gap + numW) / 2;
        dst->setTextDatum(middle_left);
        dst->drawString("OTA", x0, cy);
        int pbx = x0 + lblW + gap;
        int pby = cy - pbh / 2;
        dst->drawRect(pbx, pby, pbw, pbh, TFT_BLACK);
        int fillW = (pbw - 4) * pct / 100;
        if (fillW > 0) dst->fillRect(pbx + 2, pby + 2, fillW, pbh - 4, TFT_BLACK);
        dst->drawString(opct, pbx + pbw + gap, cy);
    } else if (pomo::active() &&
               strcmp(pager::currentName(), "pomo") != 0) {
        // Pomodoro on ANOTHER page: phase disc + remaining minutes + a small
        // progress-style bar — digits only (Minecraftia16 has no letters
        // beyond OTA; a "POMO" label would render as bars, see the charset
        // note in drawVersionOnto).
        int rem = pomo::remainingMinutes();
        if (rem > 99) rem = 99;   // u8 param can reach 255 min; the center
                                  // block reserves 2 digits — display-clamp
                                  // only, the real countdown is untouched
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", rem);
        dst->setTextDatum(middle_left);             // don't inherit a stale datum
        int numW = dst->textWidth("60");            // fixed reserve
        const int dotR = 6, gap = 8, pbw = 120, pbh = 12;
        int x0 = W / 2 - (dotR * 2 + gap + pbw + gap + numW) / 2;
        int dcy = cy;
        // A 16px tomato on the old dot's centre (solid=WORK, hollow=BREAK); it
        // fits the 32px band and its gloss reads off the white band cleared above.
        const uint8_t* tomato = pomo::inBreak() ? TOMATO_BAR_RING_BITS
                                                : TOMATO_BAR_FILL_BITS;
        dst->drawBitmap(x0 + dotR - TOMATO_BAR_W / 2, dcy - TOMATO_BAR_H / 2,
                        tomato, TOMATO_BAR_W, TOMATO_BAR_H, TFT_BLACK);
        int pbx = x0 + dotR * 2 + gap;
        dst->drawRect(pbx, cy - pbh / 2, pbw, pbh, TFT_BLACK);
        // Bar shows REMAINING fraction (drains right->left as time passes —
        // distinct from OTA's filling bar, and reads naturally as "time left").
        // Real remaining-of-span ratio, both from the service (fw 0.10.0+);
        // span is only meaningful while active(), which this branch already is.
        uint32_t ms = pomo::remainingMs(), span = pomo::spanMs();
        int fw = (int)((uint64_t)(pbw - 4) * ms / (span ? span : 1));
        if (fw > 0) dst->fillRect(pbx + 2, cy - pbh / 2 + 2, fw, pbh - 4, TFT_BLACK);
        dst->drawString(buf, pbx + pbw + gap, cy);
    } else {
        // iOS TabView-style page dots. Geometry: radius 4, pitch 16, dots
        // centered on the band's mid-line. Current page is a solid black disc;
        // every other page is a 2px ring (black disc, white 2px core).
        //
        // Count only DISPLAYABLE pages (pager::visibleCount) — a conditionally
        // hidden game page (un-unlocked Outside/Trade, closed AssignPage) is
        // skipped by showPageOrNext and so must not get a dot, or the strip would
        // advertise page turns that go nowhere. The solid dot sits at the current
        // page's ordinal AMONG the visible pages (visibleIndexOf), not its raw
        // ring index. With <=1 reachable page there is no page turn to show, so
        // the whole dot group is suppressed (a lone dot reads as a stuck UI).
        int cnt = pager::visibleCount();
        int cur = pager::visibleIndexOf(pager::currentRingIndex());
        if (cnt > 1) {             // >1 reachable page -> a turn worth advertising
            for (int i = 0; i < cnt; i++) {
                int cx = W / 2 + (int)lroundf((i - (cnt - 1) / 2.0f) * 16);
                int cyDot = cy;
                dst->fillCircle(cx, cyDot, 4, TFT_BLACK);
                if (i != cur) dst->fillCircle(cx, cyDot, 2, TFT_WHITE);
            }
        }
    }

    // ---- right: battery icon + % (laid out right-to-left) ----
    int xr = W - MARGIN;
    dst->setTextDatum(middle_right);

    int bat = batteryPercent();    // filtered — see above; -1 when unknown
    char pct[8];
    if (bat >= 0) snprintf(pct, sizeof(pct), "%d%%", bat);
    else          snprintf(pct, sizeof(pct), "--%%");
    dst->drawString(pct, xr, cy);
    xr -= dst->textWidth(pct) + 6;

    // Battery outline: body rect + positive-terminal nub, inner filled by %.
    const int bw = 22, bh = 12, nub = 2;
    int bx = xr - (bw + nub);
    int by = cy - bh / 2;
    dst->drawRect(bx, by, bw, bh, TFT_BLACK);
    dst->fillRect(bx + bw, by + 3, nub, bh - 6, TFT_BLACK);
    int pctc = bat < 0 ? 0 : (bat > 100 ? 100 : bat);
    int fillW = (bw - 4) * pctc / 100;
    if (fillW > 0) dst->fillRect(bx + 2, by + 2, fillW, bh - 4, TFT_BLACK);
    if (g_onUsb) {                 // charging/USB: bolt over the icon, haloed
        int bcx = bx + bw / 2;
        for (int oy = -1; oy <= 1; oy++)
            for (int ox = -1; ox <= 1; ox++)
                if (ox || oy) drawBolt(dst, bcx, cy, TFT_WHITE, ox, oy);
        drawBolt(dst, bcx, cy, TFT_BLACK, 0, 0);
    }
}

// Which center variant the NEXT composed frame gets: <0 = steady state (page
// dots), >=0 = the OTA progress bar at that percentage. It has to be state
// rather than an argument now, because the bar no longer has an update path of
// its own — see draw() below.
static int s_otaPct = -1;

// The bar is composed into the page canvas's bottom band, so page + chrome land
// in one frame (no vanish-then-return across a page turn). The canvas is always
// 540x960, so the band's top row is height()-BAR_H. Pure drawing; pager::repaint
// owns the push.
void drawOnto(m5gfx::M5Canvas& canvas) {
    drawTo(&canvas, canvas.height() - BAR_H, s_otaPct);
}

// An independent bar refresh (main.cpp's 1s tick, on a visible clock/battery/USB
// change) is a WHOLE-FRAME repaint now.
//
// It used to be a 540x32 off-screen strip sprite blitted over just the band, so
// that the white clear and every element landed in a single strip-local EPD
// update instead of flickering through a fillRect. Both halves of that reasoning
// are gone: the panel driver is free-running and double-buffered, so there is no
// such thing as updating one band (the buffer a partial write would land in is
// two frames stale), and there is no waveform to spare either. Recomposing the
// whole page to move a clock digit costs ~8 ms of the measured ~23 ms scan
// period, which is
// cheaper than the strip alloc was.
void draw() {
    s_otaPct = -1;
    pager::repaint();
}

// OTA-progress variant of the bar (fw 0.8.9): center replaced by "OTA <bar> NN%"
// (see drawTo). Called by main.cpp's loop on a throttle (percent-advanced or a
// time floor) while an OTA is streaming — NOT from the BLE callback; it just
// reads ble_link::otaReceived()/otaTotal() via the caller. total==0 → 0%.
//
// UNREACHABLE IN THIS WAVE: ble_link's OTA BEGIN is refused outright while the
// panel driver has no park protocol around a flash erase, so otaBusy() is never
// true. Kept live (rather than deleted with the rest of the display-side dead
// code) because it is a pure drawing path with nothing EPD-specific left in it,
// and the OTA wave will want it back unchanged.
void drawOtaProgress(uint32_t received, uint32_t total) {
    int pct = total ? (int)((received * 100ULL) / total) : 0;
    if (pct > 100) pct = 100;
    s_otaPct = pct;
    pager::repaint();
}

// Firmware version, self-drawn into the panel's TOP-RIGHT corner so a flashed
// build announces which image it is right on the screen (the STATUS fw-string is
// identical across re-flashes of the same version — useless for telling two
// builds apart). Baked into the page canvas by pager::drawFrame right after the
// bar, so it rides the page's own frame — there is no separate refresh. Same
// Minecraftia16 face as the bottom bar, black on the already-white header.
//
// GEOMETRY — this firmware's page-tab header (page_tabs.cpp), NOT the retired
// PaperS3 host-dashboard layout the old y=90 coordinate came from. The tab titles
// occupy y=TAB_Y..TAB_Y+36 (16..52) with a 3px underline at y≈58; the very top
// band y=0..16 is whitespace above every tab. Minecraftia16 ink is ~14px tall, so
// drawing the version at y=1 (ink ~1..15) keeps it entirely in that top gap,
// above the tab row. Right-aligned to canvas.width()-HDR_PAD (540-24 = 516), the
// same right margin the tabs grow AWAY from (tabs are left-anchored from PAD and
// extend rightward on the y=16.. rows). Because the version lives in its own
// horizontal band ABOVE the tab titles, it cannot collide with tab text no matter
// how far the widest three-tab combination reaches — the separation is vertical,
// not horizontal, so no left-edge budgeting is needed.
static const int HDR_PAD   = 24;   // right margin (matches the tab header's PAD)
static const int HDR_VER_Y = 1;    // top of version text — in the y=0..16 gap above the tabs

void drawVersionOnto(m5gfx::M5Canvas& canvas) {
    // Drop the constant "-papers3ble" suffix (adds no info across builds); the
    // patch number is the disambiguator. "0.8.1-papers3ble" -> "fw 0.8.1".
    const char* v = CARD_VERSION;
    const char* dash = strchr(v, '-');
    int vlen = dash ? (int)(dash - v) : (int)strlen(v);
    // CHARSET CONSTRAINT — a limit for ANYONE drawing text here: Minecraftia16
    // only has glyphs for "0123456789:/%-. OTA" (see minecraftia16.h; '.' is a
    // real 2x4 dot, and 'O'/'T'/'A' were added in fw 0.8.9 for the bar's OTA
    // label). ANY other character (other letters, '=', etc.) has no glyph and
    // the GFX font renderer draws a vertical BAR in its place. A debug build
    // that appended "fw ... ota=NULL st=" here rendered as a row of bars on the
    // panel — those letters had no glyphs. So the version string MUST stay
    // within this charset: "0.8.9" is safe (digits + the real '.' dot). Keep any
    // on-panel text within "0123456789:/%-. OTA".
    char ver[24];
    snprintf(ver, sizeof(ver), "%.*s", vlen, v);

    canvas.setFont(&Minecraftia16);
    canvas.setTextColor(TFT_BLACK, TFT_WHITE);
    canvas.setTextDatum(top_right);
    canvas.drawString(ver, canvas.width() - HDR_PAD, HDR_VER_Y);
}

}  // namespace status_bar
