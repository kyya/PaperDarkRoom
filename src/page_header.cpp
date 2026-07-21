#include "page_header.h"
#include "pomo_page.h"          // PAD, HDR_DIV_Y (shared layout authority)
#include "minecraftia16.h"
#include "vcr_osd_60.h"
#include <M5Unified.h>
#include <stdio.h>

namespace page_header {

// Zeller-ish day-of-week (0=Sun..6=Sat) — the RTC weekDay field is unset in
// this firmware (applyPendingTimeConfig writes 0), so derive it from the date.
static const char* WD[7] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
static const char* weekday(int y, int m, int d) {
    static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
    if (m < 3) y -= 1;
    int w = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
    return WD[(w % 7 + 7) % 7];
}

// Header: big HH:MM (VCR OSD Mono @60px) left, weekday + MM/DD stacked small
// right (Minecraftia16), dashed rule — the daemon's _clock_block, glyph-for-
// glyph. The draw y's are OFFSET from the daemon's logical coords by each face's
// ink bearing: the daemon's PIL "la"/"ra" anchor lands on the ASCENDER line,
// LovyanGFX's top datum lands the INK top on y, and the difference (VCR60 +9,
// Minecraftia +4) must be added so the ink rows coincide exactly. Values pinned
// by scratchpad/align.py, which renders each string both ways and pixel-diffs:
// clock ink y[33..73], weekday y[34..47], date y[60..73] — server == firmware.
void draw(m5gfx::M5Canvas& c) {
    m5::rtc_date_t dt; m5::rtc_time_t tm;
    M5.Rtc.getDate(&dt); M5.Rtc.getTime(&tm);

    c.setTextColor(TFT_BLACK, TFT_WHITE);

    // HH:MM, VCR OSD Mono @60px. Daemon draws at (pad, pad)=(24,24) la -> ink
    // top 33; top_left aligns ink to y, so +9 (PAD+9=33). Measured ink y[33..73].
    c.setFont(&VCR_OSD_60);
    c.setTextDatum(top_left);
    char clk[6];
    snprintf(clk, sizeof(clk), "%02d:%02d", tm.hours, tm.minutes);
    c.drawString(clk, PAD, PAD + 9);

    // Weekday (%a upper) + MM/DD, Minecraftia16 (now carries the letters), right
    // aligned. Daemon y+3*s=30 / y+16*s=56 ra -> ink 34 / 60 (bearing +4).
    c.setFont(&Minecraftia16);
    c.setTextDatum(top_right);
    c.drawString(weekday(dt.year, dt.month, dt.date), 540 - PAD, PAD + 10);   // ink y[34..47]
    char md[6];
    snprintf(md, sizeof(md), "%02d/%02d", dt.month, dt.date);
    c.drawString(md, 540 - PAD, PAD + 36);                                    // ink y[60..73]

    // Dashed rule (host-parity y). 7px on / 5px off across the padded span.
    for (int x = PAD; x < 540 - PAD; x += 12)
        c.drawFastHLine(x, HDR_DIV_Y, 7, TFT_BLACK);
}

}  // namespace page_header
