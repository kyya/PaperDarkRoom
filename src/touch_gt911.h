// Bare GT911 digitizer — the M5.Touch replacement.
//
// M5.begin() is gone with the MSG migration (see rtc_bm8563.h), so M5.Touch's
// press/hold/click state machine goes with it. The pager's own touch policy —
// the grip-graze multi-slot scan, the >=3-finger grip latch, the edge band, the
// 350 ms tap debounce (pager.cpp) — is untouched by this port and stays exactly
// where it is; what this file provides is the layer underneath it, shaped to
// the same three questions pager.cpp asked M5.Touch:
//
//     count()        how many contacts are on the panel this pass
//     detail(i).held  did contact i just cross the long-press threshold
//     detail(i).clicked  did contact i just release as a short tap
//
// Adapted from PaperBar's src/paper_s3_msg/touch_gt911.cpp (same board, same
// bus, same driver lineage) with its gesture layer removed: PaperBar turns a
// press/release pair into a tap or a swipe because its pager is swipe-driven;
// ours needs the raw press/release and does its own hit-testing, so the swipe
// classifier, the edge band and the tap-gap filter are all left out here rather
// than duplicated (pager.cpp already owns equivalents of the last two).
//
// Wiring (M5PaperS3): SDA=GPIO41, SCL=GPIO42, INT=GPIO48, I2C_NUM_1. RST is NOT
// connected to the MCU, so the chip's I2C address is latched at board power-up
// and the same unit can come up on either 0x14 or 0x5D — hence the alternating
// probe rather than a hardcoded address.
//
// FIVE-POINT UNLOCK. The GT911's factory config reports at most 2 contacts,
// which is what makes a handheld card feel dead: the holding hand's thumb takes
// slot 0 and the deliberate tap lands in a slot nobody reads. The old firmware
// fixed that with M5Unified's Touch_GT911::setTouchNums(5) (main.cpp:404-407,
// retired with M5.begin); the same 184-byte config read-modify-write with a
// recomputed checksum is done here, so pager.cpp's slot scan and its >=3-finger
// grip guard keep working. A unit that refuses the unlock stays at 2 contacts
// and says so on the serial line — degraded, not broken.
//
// COORDINATES ARE ALREADY THE PORTRAIT CONTENT FRAME, and that is not a
// coincidence to be re-derived per call site: the chip's config resolution is
// 540x960 with the origin at the top-left of the card as the user holds it, and
// msg_bridge binds the UI sprite at LovyanGFX rotation 3, whose transform is
// (px,py) -> buffer(py, 539-px) — byte-identical to the portrait mapping
// PaperBar's gfx1bpp uses against the same panel. So a raw (x,y) out of the chip
// IS the (x,y) a page drew at, with no flip and no swap, which is what
// pager.cpp's rotation note already assumed of M5.Touch's output.
#pragma once
#include <stdint.h>

namespace touch {

// Up to five simultaneous contacts (the unlocked GT911 maximum).
static const int MAX_POINTS = 5;

// One contact, shaped like m5::touch_detail_t's subset that pager.cpp reads.
struct Detail {
    int      x = 0, y = 0;     // portrait content frame, see the header note
    bool     clicked = false;  // released this pass, having never become a hold
    bool     held    = false;  // crossed the long-press threshold this pass
    uint32_t baseMsec = 0;     // millis() at press — the grip-graze tiebreak
};

// Probe the chip, unlock five contacts, install the I2C driver. Call from the
// app task after msg_start(). False = no GT911 answered; count() is then always
// 0 and the firmware runs (visibly) without touch.
bool init();

// Poll the chip once and advance the press state machine. Call once per app-loop
// pass, before count()/detail() — the pager's `M5.update()` equivalent.
void update(uint32_t nowMs);

// Contacts visible to this pass. A contact that released during this update is
// still counted (with clicked set) for exactly one pass, so the release is
// observable — M5.Touch behaves the same way and pager.cpp's click scan depends
// on it.
int count();

// Contact `i`, 0 <= i < count(). Out of range returns a default-constructed
// Detail (all false), never a stale one.
Detail detail(int i);

}  // namespace touch
