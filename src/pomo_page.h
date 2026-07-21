// The pomodoro VIEW — a firmware-rendered client page (fw 0.10.0). Idle: clock
// header + three duration buttons (25/45/60 min, each a type=1 long-press
// region). Counting: the chosen button block shows MM:SS + phase disc + drain
// bar, redrawn every second via pager::partialRefresh (Task 6). Fonts match the
// daemon's pixel pages glyph-for-glyph: HH:MM/MM:SS/button numerals in VCR OSD
// Mono (60/68px), weekday+date in Minecraftia16, "min" in PixelOperator @32px.
// No CJK font enters the firmware. The service (pomo.h) owns timing; this owns
// pixels.
#pragma once
#include "page.h"
#include <stdint.h>

// Layout — mirrors the server pages' rhythm (host pad = 24, clock top-left,
// weekday/date stacked top-right, dashed rule ~y=112). Shared with Task 6's
// counting overlay (a separate translation unit) so its digits/disc/drain bar
// land inside the same button block as this idle draw.
constexpr int PAD       = 24;
constexpr int HDR_DIV_Y = 112;                 // dashed rule y (host parity)
constexpr int BTN_X0    = PAD;
constexpr int BTN_X1    = 540 - PAD;           // 516
constexpr int BTN_TOP   = HDR_DIV_Y + 28;      // first button top
constexpr int BTN_H     = 148;                 // block height
constexpr int BTN_GAP   = 28;                  // between blocks

// Block top y for a given minutes value / block height (Task 6 re-uses these
// to place its MM:SS + phase disc + drain bar over the chosen button).
int pomoBlockTop(uint8_t minutes);
int pomoBlockH();

class PomoPage : public pages::Page {
public:
    const char* name() const override { return "pomo"; }
    bool draw(m5gfx::M5Canvas& canvas) override;
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x) override;  // x unused; -> pomo::onLocalAction(param)
    void tick(uint32_t nowMs) override;          // Task 6 fills the counting cadence
    bool wantsAwake() const override;            // == pomo::wantsAwake()
};
