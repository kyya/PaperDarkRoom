// Full-page focus-style page-switcher (fw 0.10.10). A three-finger long-press on
// ANY page opens a focus view: a large 9:16 thumbnail of the FOCUS page on the
// left, a vertical rail of small thumbnails (every reachable ring page) on the
// right. Tapping a rail cell moves the focus to that page (repaint, stay in
// preview); tapping the big pane jumps to the focus page and leaves; tapping
// anywhere else (or a second three-finger long-press) returns to the origin
// page. A pager-scheduled MODAL state — not a ring Page — and RAM-only, never
// persisted, so a deep-sleep wake returns to the normal page. Focus starts on
// the current page, which is always reachable and so always has a rail cell.
// pager::handleTouch drives it; pager::showPage/tickCurrent no-op while active
// so background pushes / ticks can't clobber the view. See pager.cpp.
#pragma once

namespace preview {

// hitCell() sentinels (rail cell indices are >= 0).
constexpr int HIT_BIG  = -2;   // tap landed in the big (focus) pane
constexpr int HIT_NONE = -1;   // tap landed on header / bar / blank

bool active();               // true while the switcher is on screen

// Render the focus view (one epd_quality full-panel push — a deliberate flash
// that also clears ghosting) and mark active. Focus starts at the current page.
void enter();

// Clear the active flag. The caller redraws the page it wants shown next
// (showPage only works once this has run — the active guard is released here).
void exit();

// Move the focus to ring index `idx` and repaint the whole view with a single
// full-panel epd_fast push (big pane + both border weights land in one
// consistent update). No-op unless `idx` currently owns a rail cell (i.e. is a
// reachable page) — what hitCell returns always does.
void setFocus(int idx);

// Ring index of the page currently in the big pane.
int focusIndex();

// Hit test for (x,y) in content-frame coords (what pager::handleTouch reads from
// M5.Touch): HIT_BIG for the big pane, the RING INDEX behind a rail cell (the
// rail lists only reachable pages, so cell position != ring index), or HIT_NONE
// for a tap outside every target (header / gaps / status bar / empty slots).
int hitCell(int x, int y);

}  // namespace preview
