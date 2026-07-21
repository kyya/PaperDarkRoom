// Standard clock header (fw 0.10.6): big HH:MM (VCR OSD Mono @60px) left,
// weekday + MM/DD stacked small right (Minecraftia16), dashed rule at
// HDR_DIV_Y — the daemon's _clock_block replicated glyph-for-glyph. Extracted
// from pomo_page.cpp so the pomo view AND the grid page-switcher (preview.cpp)
// paint the same header from one implementation.
#pragma once

namespace m5gfx { class M5Canvas; }

namespace page_header {

// Paint the clock header into the top band of `c` (up to HDR_DIV_Y + the rule).
// Reads the RTC live; no EPD-mode work — the caller owns the push and its mode.
void draw(m5gfx::M5Canvas& c);

}  // namespace page_header
