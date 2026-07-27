// Tap → page turn (short tap: left half = previous page, right half = next,
// wrap-around). Long-press selects a tap region on the current page instead
// — the device only does geometry (which region did y land in); what a
// region MEANS is entirely up to the host, resolved from (page_idx,
// region_index) after the tap is reported. See setRegions/pendingTap below
// and docs/superpowers/specs/2026-07-13-tap-regions-design.md.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "page.h"

namespace pager {

// Poll touch (call right after M5.update()). True = the user interacted —
// counts even when there's only one page to show or a long-press missed
// every region.
bool handleTouch();

// Draw the page at ring index `idx` to the EPD and persist it as current.
// quality=true → epd_quality (slow, clears ghosting; boot + sync redraws),
// false → epd_fast (fast page turns). False on missing/corrupt page (the
// page is invalidated so the host re-pushes it).
bool showPage(int idx, bool quality);

// Show the nearest displayable page one step from ring index startIdx in `dir`
// (+1/-1, wrapping), skipping missing/undecodable pages (step 1..n-1). False
// only when no page in that direction can be shown. Shared by tap paging and
// boot restore so a cold/torn cache can't soft-lock one tap direction.
bool showPageOrNext(int startIdx, int dir, bool quality);

// Ring = [server pages 0..pageCount) + client registry pages at the tail].
int  ringCount();
int  currentRingIndex();        // clamped to [0, ringCount)

// Displayable-page statistics for the status bar's page dots and the preview
// rail. A ring slot is "visible" when its Page::available() is true (draw()
// would paint, not be skipped by showPageOrNext) — so a conditionally-hidden
// game page (un-unlocked Outside/Trade, a closed AssignPage) drops out of the
// count. ringAvailable(ring) is that predicate for one slot (false out of
// range); visibleCount() is how many dots to draw; visibleIndexOf(ringIdx) is
// which of those the given ring index maps to (its 0-based ordinal among
// visible pages, = the count of visible pages before it). The bar draws no dots
// at all when visibleCount()<=1 (a single reachable page has no page-turn to
// advertise).
bool ringAvailable(int ring);
int  visibleCount();
int  visibleIndexOf(int ringIdx);
const char* currentName();      // the current page's name() ("srv:0", "pomo")

// Ring index of the registered page named `name`, or -1 if none. Lets a client
// page navigate to a sibling by identity (e.g. Outside 分工 -> "assign", the
// assign 返回 -> "outside") without hardcoding a shiftable ring index.
int  ringIndexByName(const char* name);

// Drive the current page's tick() (time axis). Call once per loop().
void tickCurrent(uint32_t nowMs);

// Boot restore by persisted name (see pager.cpp) — replaces the raw-index
// showPage(currentPage()) the boot path used before curName persistence.
void restore(bool quality);

// Pay off any accumulated ghosting debt at sleep entry: if >= QUALITY_EVERY fast
// refreshes have piled up (background pushes + turns) and there's a page to show,
// redraw the current page with epd_quality — a full-panel deep-clean done right
// before timerSleep, when no one is looking (showPage resets the debt counter).
// No-op otherwise, so it adds ~1s awake time only when debt is actually due.
void payGhostDebtIfDue();

// Push just `r` of the current canvas under FAST (fast/DU, ghosting accrues to
// the debt) or QUALITY (grayscale-clean, clears local ghosting + resets debt).
// The caller must have drawn the new pixels into the canvas first. See
// pager.cpp; used by client pages for seconds/minute counter repaints.
void partialRefresh(const pages::Rect& r, pages::RefreshMode mode);

// Invert-flash a button rect as press feedback (see pager.cpp): briefly shows
// `r` in reverse video on the panel while leaving the canvas itself unchanged.
// The dispatchRegion press path uses it internally; event_modal reuses it for its
// own (non-dispatchRegion) choice buttons. The caller repaints over the flash, or
// rebounds it with a partialRefresh of the same rect once the action is done.
void flashPressRect(const pages::Rect& r);

// Count of page-skip events since boot — each unavailable page showPageOrNext
// had to step over (a cache hole). Surfaced in STATUS as skips= so a host can
// see "unstable page order" (skipping onto nearest available pages) without
// serial. Saturates at UINT16_MAX; a wake-scoped counter (cold boot resets it).
uint16_t skipCount();

// A REGIONS transfer landed for `pageIdx` (ble_link::rx.isRegions). Two wire
// formats, length-disambiguated (fw 0.9.6, see
// docs/superpowers/specs/2026-07-20-pomodoro-focus-design.md):
//   v1: u8 count | count * (u16 LE y0, u16 LE y1)                    — host action
//   v2: u8 count | count * (u16 LE y0, u16 LE y1, u8 type, u8 param) — type=1
//       is firmware-local (param = pomodoro minutes), type=0 = host action
// Replaces any table already held for that page — RAM-only, so the host
// resends it every session that page is shown (device is a cold boot on
// every wake).
void setRegions(int pageIdx, const uint8_t* data, size_t len);

// The most recent long-press that landed inside a region, awaiting the
// host's TAP_ACK. seq is monotonic (one per detected long-press); hosts
// dedupe on it increasing rather than on the ack round-trip.
bool     hasPendingTap();
uint32_t pendingTapSeq();
int      pendingTapPage();
int      pendingTapItem();

// Host has resolved & acted on `seq` — clear it if it's still current (a
// stale ack for an already-superseded seq is a no-op).
void ackTap(uint32_t seq);

}  // namespace pager
