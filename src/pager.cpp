#include "pager.h"
#include "frame_store.h"
#include "status_bar.h"
#include "page.h"
#include "client_pages.h"
#include "preview.h"
#include "event_modal.h"
#include "fight_modal.h"
#include "setpiece_modal.h"
#include "page_tabs.h"
#include "assign_page.h"
#include "path_page.h"
#include <M5Unified.h>
#include <climits>
#include <cstring>

extern M5Canvas canvas;   // main.cpp owns the full-screen sprite

namespace pager {

// The full (epd_quality) refresh flashes the whole panel black — jarring when
// it fires while the user is looking. Policy: every visible redraw (background
// push AND user page turn) is epd_fast; the ghosting debt they accumulate is
// normally paid off at SLEEP ENTRY (payGhostDebtIfDue) — a quality redraw of
// the current page right before timerSleep, when nobody is watching. Escape
// valve: a marathon interactive session that piles past HIGH_WATER fast
// refreshes without sleeping pays on the next turn, so ghosting can't grow
// unbounded. Boot restore also uses quality (power-button = user action).
static int s_fastCount = 0;
static const int QUALITY_EVERY = 8;    // fast-refresh debt that a sleep deep-clean settles
static const int HIGH_WATER   = 24;    // in-session escape valve: pay on the next turn past this

// Generic tap regions: any page can have a table of (y0,y1) rows pushed for
// it; the device only ever does geometry (which row did y land in) — what a
// region MEANS (open a URL, or whatever a future host dreams up) is entirely
// the host's business, resolved from (page_idx, region_index) after the tap
// is reported. RAM-only (see pager.h) — every table starts empty (pageIdx
// -1, matches nothing) until the host pushes one this wake, per page.
static const int MAX_REGIONS_PER_PAGE = 12;
static const int MAX_PAGES_WITH_REGIONS = 6;

struct PageRegions {
    int pageIdx = -1;
    int count = 0;
    pages::Region hits[MAX_REGIONS_PER_PAGE];
};
static PageRegions s_regions[MAX_PAGES_WITH_REGIONS];

static uint32_t s_tapSeq      = 0;   // monotonic — 0 means "never"
static uint32_t s_pendingSeq  = 0;   // 0 = no pending tap
static int      s_pendingPage = -1;
static int      s_pendingItem = -1;

// Page-skip events since boot: incremented once per unavailable page
// showPageOrNext steps over (cache hole). Exposed via skipCount() for STATUS.
static uint16_t s_skipCount   = 0;

static int s_curRing = 0;   // ring index of the page currently shown

// Full-page push generation — bumped once per successful showPage() panel push
// (NOT by partialRefresh). dispatchRegion's press-flash reads it to tell whether
// the action already repainted the button (gen changed) or the black flash still
// needs a manual rebound (gen unchanged — a host tap, a silent cooldown reject,
// or an action whose only repaint was a partialRefresh that doesn't cover the
// button, e.g. Room's RC_ERR_COST log-only refresh). Counting only whole-page
// pushes is what lets that log-only partial still leave the button to be rebounded.
static uint32_t s_showGen = 0;

// Last server-page count observed by currentRingIndex(). s_curRing is a raw
// live ring index, but frame_store's server count can change under it (first
// sync 0->N, or a shrink) with no re-resolution; when this differs from the
// live count we lazily re-resolve s_curRing from the persisted page NAME. -1 =
// "no count observed yet", so the first read reconciles.
static int s_lastSrvCount = -1;

static PageRegions* findOrAllocRegions(int pageIdx) {
    for (auto& r : s_regions) if (r.pageIdx == pageIdx) return &r;
    for (auto& r : s_regions) if (r.pageIdx == -1) { r.pageIdx = pageIdx; return &r; }
    return &s_regions[0];   // table full (shouldn't happen — page_count is tiny)
}

void setRegions(int pageIdx, const uint8_t* data, size_t len) {
    PageRegions* r = findOrAllocRegions(pageIdx);
    r->pageIdx = pageIdx;
    r->count = 0;
    if (len < 1) return;
    int n = data[0];
    if (n > MAX_REGIONS_PER_PAGE) n = MAX_REGIONS_PER_PAGE;
    // v2 (fw 0.9.6): 6-byte entries carry type+param (type 1 = firmware-local,
    // param = pomodoro minutes). v1: 4-byte entries, all host actions. The
    // count byte is explicit so the two length checks can't both match; check
    // v2 FIRST (1+6n > 1+4n). Anything shorter than v1 is corrupt — leave the
    // table empty, never partial-parse.
    size_t entry;
    if (len >= 1 + (size_t)n * 6)      entry = 6;
    else if (len >= 1 + (size_t)n * 4) entry = 4;
    else return;
    for (int i = 0; i < n; i++) {
        const uint8_t* p = data + 1 + i * entry;
        r->hits[i].y0    = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
        r->hits[i].y1    = (uint16_t)p[2] | ((uint16_t)p[3] << 8);
        r->hits[i].type  = (entry == 6) ? p[4] : 0;
        r->hits[i].param = (entry == 6) ? p[5] : 0;
    }
    r->count = n;
    Serial.printf("[pager] regions page=%d items=%d v%d\n", pageIdx, n,
                  entry == 6 ? 2 : 1);
}

// Server-page touch table for the ring (ServerPage::regions). Returns the
// host-pushed row array for `pageIdx` + its count, or nullptr/0.
static const pages::Region* serverRegions(int pageIdx, int* n) {
    for (auto& r : s_regions)
        if (r.pageIdx == pageIdx) { *n = r.count; return r.hits; }
    *n = 0;
    return nullptr;
}

bool hasPendingTap()      { return s_pendingSeq != 0; }
uint32_t pendingTapSeq()  { return s_pendingSeq; }
int pendingTapPage()      { return s_pendingPage; }
int pendingTapItem()      { return s_pendingItem; }
uint16_t skipCount()      { return s_skipCount; }

void ackTap(uint32_t seq) {
    if (seq != 0 && seq == s_pendingSeq) {
        s_pendingSeq = 0;
        s_pendingPage = -1;
        s_pendingItem = -1;
    }
}

// A server page: one host-pushed PNG in frame_store, addressed by index. One
// reusable instance whose index is rebound before each use (server pages are
// homogeneous — the differentiator is just the frame_store slot).
class ServerPage : public pages::Page {
public:
    void bind(int idx) { idx_ = idx; }
    const char* name() const override {
        snprintf(name_, sizeof(name_), "srv:%d", idx_);
        return name_;
    }
    bool draw(m5gfx::M5Canvas& canvas) override {
        size_t len = 0;
        const uint8_t* png = frame_store::pagePng(idx_, &len);
        if (!png) return false;                       // missing (cold cache)
        if (!canvas.drawPng(png, len, 0, 0)) {        // torn SD read
            frame_store::invalidate(idx_);            // -> host re-pushes
            return false;
        }
        return true;
    }
    const pages::Region* regions(int* n) const override {
        return serverRegions(idx_, n);
    }
private:
    int idx_ = 0;
    mutable char name_[8];
};

static ServerPage s_serverPage;

int ringCount() { return frame_store::pageCount() + client_pages::count(); }

// Resolve a ring index to a Page* (server pages first, client pages at the
// tail). Rebinds the shared ServerPage for server positions.
static pages::Page* pageAt(int ring) {
    int nSrv = frame_store::pageCount();
    if (ring < 0 || ring >= ringCount()) return nullptr;
    if (ring < nSrv) { s_serverPage.bind(ring); return &s_serverPage; }
    return client_pages::at(ring - nSrv);
}

// How many ring slots are currently displayable (Page::available() true) — the
// status bar draws exactly this many page dots. A conditionally-hidden game page
// (un-unlocked Outside/Trade, a closed AssignPage) is available()==false and
// drops out, so the dot count matches what the user can actually page to. Reads
// the same predicate draw() gates on, so a dot never lies about reachability.
int visibleCount() {
    int n = ringCount(), c = 0;
    for (int r = 0; r < n; r++) {
        pages::Page* pg = pageAt(r);
        if (pg && pg->available()) c++;
    }
    return c;
}

// The given ring index's ordinal among visible pages (0-based) = how many
// available() slots precede it. For the current page (always available — it just
// drew) this is the solid dot's position. Ring indices past ringCount clamp to
// the visible total.
int visibleIndexOf(int ringIdx) {
    int n = ringCount(), idx = 0;
    for (int r = 0; r < ringIdx && r < n; r++) {
        pages::Page* pg = pageAt(r);
        if (pg && pg->available()) idx++;
    }
    return idx;
}

// Resolve the ring index of the page whose name() matches the persisted
// frame_store::currentName(); 0 (first page) when the stored name no longer
// exists (host page-count shrank, or the client page went away). The single
// by-name scan shared by boot restore() and the lazy server-count reconcile.
static int resolveRingByName() {
    char want[24];
    frame_store::currentName(want, sizeof(want));
    for (int r = 0; r < ringCount(); r++) {
        pages::Page* pg = pageAt(r);
        if (pg && strcmp(pg->name(), want) == 0) return r;
    }
    return 0;
}

// Resolve the ring index of the page whose name() matches `name`, or -1 if none
// is registered under that name. Public sibling of resolveRingByName (which is
// hard-wired to the persisted current name): client pages use it to navigate to
// a sibling by identity — e.g. the Outside 分工 cell jumping to "assign", and the
// AssignPage 返回 band jumping back to "outside" — without hardcoding a ring
// index that server pages ahead of them would shift.
int ringIndexByName(const char* name) {
    for (int r = 0; r < ringCount(); r++) {
        pages::Page* pg = pageAt(r);
        if (pg && strcmp(pg->name(), name) == 0) return r;
    }
    return -1;
}

int currentRingIndex() {
    // Lazy reconcile: the server page count can change under s_curRing (first
    // sync 0->N, or a shrink) with no re-resolution — a user parked on "pomo"
    // (ring 0 at server count 0) would silently become "srv:0" (ring 0 after a
    // 5-page sync). When the count differs from last seen, re-resolve s_curRing
    // from the persisted page NAME so the view stays on the same logical page.
    // No drawing here; the caller's draw flow stays in charge and steady state
    // costs one int compare.
    int srv = frame_store::pageCount();
    if (srv != s_lastSrvCount) {
        s_lastSrvCount = srv;
        s_curRing = resolveRingByName();
    }
    int rc = ringCount();
    if (rc <= 0) return 0;
    if (s_curRing < 0 || s_curRing >= rc) return 0;
    return s_curRing;
}

const char* currentName() {
    pages::Page* p = pageAt(currentRingIndex());
    return p ? p->name() : "";
}

bool showPage(int ring, bool quality) {
    // The grid page-switcher owns the panel while it's up: refuse every redraw
    // (background BLE push, auto-rotate, pomo phase flip) so none can clobber
    // the grid. The single central guard the whole preview state relies on;
    // preview::exit() clears it before the caller redraws the chosen page.
    if (preview::active()) return false;
    // Same guard for the event modal (research.md §4.1): while a random event is
    // on screen no background push / tick can repaint the page under it.
    // event_modal::closeAndRestore() clears the flag before its own showPage.
    if (event_modal::active()) return false;
    // And the same for the combat overlay (P2.3): it owns the panel while a fight
    // is live; fight_modal clears its flag before its own closeToWorld showPage.
    if (fight_modal::active()) return false;
    // And the landmark setpiece overlay (P2.4): it owns the panel across its whole
    // run (including an interleaved fight); setpiece_modal clears its flag before
    // its own closeToWorld showPage.
    if (setpiece_modal::active()) return false;
    pages::Page* p = pageAt(ring);
    if (!p) return false;
    M5.Display.setEpdMode(quality ? epd_mode_t::epd_quality
                                  : epd_mode_t::epd_fast);
    bool ok = p->draw(canvas);
    if (ok) {
        // Set current FIRST — the bar reads currentRingIndex() to pick the
        // solid page dot, so bake it in before drawOnto or the dots lag.
        s_curRing = ring;
        frame_store::setCurrentName(p->name());
        // Bar + version bake into the canvas so page + chrome land in ONE EPD
        // update (no vanish-then-return flicker). Every redraw path (turn,
        // push, rotate, boot) funnels through here.
        status_bar::drawOnto(canvas);
        status_bar::drawVersionOnto(canvas);
        canvas.pushSprite(0, 0);
        s_showGen++;   // whole-page push — press-flash rebound reads this (see above)
        if (quality) s_fastCount = 0; else s_fastCount++;
    }
    M5.Display.setEpdMode(epd_mode_t::epd_fast);
    return ok;
}

// Push a sub-rect of the already-drawn canvas under a chosen EPD waveform —
// the client-page repaint primitive (a seconds counter blits ~10x14px, not
// the whole panel). The caller draws its updated pixels into `canvas` first;
// this clips the display to `r`, pushes, and restores. FAST charges the rect
// to the ghosting debt (settled at sleep by payGhostDebtIfDue); QUALITY is a
// grayscale-clean rect that clears local ghosting and resets the debt.
void partialRefresh(const pages::Rect& r, pages::RefreshMode mode) {
    auto& disp = M5.Display;
    epd_mode_t prev = disp.getEpdMode();
    epd_mode_t em = epd_mode_t::epd_fast;
    switch (mode) {
        case pages::RefreshMode::FASTEST: em = epd_mode_t::epd_fastest; break;
        case pages::RefreshMode::QUALITY: em = epd_mode_t::epd_quality; break;
        case pages::RefreshMode::FAST:    em = epd_mode_t::epd_fast;    break;
    }
    disp.setEpdMode(em);
    disp.setClipRect(r.x, r.y, r.w, r.h);   // limit the pushed/updated region
    canvas.pushSprite(0, 0);
    disp.clearClipRect();
    disp.setEpdMode(prev);
    switch (mode) {
        case pages::RefreshMode::FAST:
        case pages::RefreshMode::FASTEST:
            if (s_fastCount < INT_MAX) s_fastCount++;
            break;
        case pages::RefreshMode::QUALITY:
            s_fastCount = 0;
            break;
    }
}

// Invert-flash a button rect as press feedback: XOR-invert the canvas's
// grayscale_8bit pixels inside `r` (1 byte/pixel, row stride = canvas width),
// push just that rect under FASTEST (a quick DU flash), then invert the canvas
// back — so the CANVAS ends up unchanged while the SCREEN briefly shows the rect
// in reverse video. The caller either repaints over it (any showPage) or rebounds
// it with a second partialRefresh of the now-restored rect. Rect is clamped to the
// panel; an empty rect (pressRect's "don't flash" signal) never reaches here.
void flashPressRect(const pages::Rect& r) {
    const int W = canvas.width(), H = canvas.height();
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.w, y1 = r.y + r.h;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x1 <= x0 || y1 <= y0) return;
    uint8_t* buf = (uint8_t*)canvas.getBuffer();
    if (!buf) return;
    for (int pass = 0; pass < 2; pass++) {           // invert -> push -> invert back
        for (int y = y0; y < y1; y++) {
            uint8_t* row = buf + (size_t)y * W;
            for (int x = x0; x < x1; x++) row[x] = (uint8_t)(255 - row[x]);
        }
        if (pass == 0) partialRefresh(r, pages::RefreshMode::FASTEST);
    }
}

// Show the nearest displayable page starting one step from startIdx in `dir`
// (+1/-1, wrapping): try step 1..n-1 and display the first candidate that
// actually shows. A page can be missing when the PSRAM cache is still cold
// after a battery wake and sync hasn't filled it yet, or undecodable on a torn
// SD read — showPage returns false and invalidates it, so we skip to the next
// one instead of re-computing the same dead target on every tap (the
// one-direction soft-lock this fixes). The invalidated page reports "-" in the
// etag line, which already makes the host re-push the hole. Returns false only
// when NO page in that direction can be shown. Shared by the tap pager and boot
// restore. quality is decided once by the caller and passed to each attempt
// (s_fastCount only moves on a page that actually shows).
bool showPageOrNext(int startIdx, int dir, bool quality) {
    int n = ringCount();
    for (int step = 1; step < n; step++) {
        int cand = ((startIdx + step * dir) % n + n) % n;
        if (showPage(cand, quality)) return true;
        if (s_skipCount != 0xFFFF) s_skipCount++;   // saturate, don't wrap
        Serial.printf("[pager] page %d unavailable -> skip\n", cand);
    }
    return false;
}

// Settle the accumulated ghosting debt at sleep entry, when nobody is looking.
// If enough background/turn fast-refreshes have piled up (>= QUALITY_EVERY) and
// there's a page to redraw, repaint the CURRENT page with epd_quality — a ~1s
// full-panel deep-clean that also resets s_fastCount (showPage does it on a
// successful quality push). No-op below the threshold or with no pages, so the
// common case adds zero awake time.
void payGhostDebtIfDue() {
    if (s_fastCount < QUALITY_EVERY) return;
    if (ringCount() <= 0) return;
    int before = s_fastCount;
    if (showPage(currentRingIndex(), true))
        Serial.printf("[pager] ghost debt paid (%d fast)\n", before);
}

// Grip-graze mechanism (2026-07-17 investigation): the GT911 tracks up to 5
// simultaneous points, but examining only slot 0 (M5.Touch.getDetail()) misreads
// a handheld card. The holding hand's finger/thumb can (a) briefly graze an edge
// (<500ms, under the hold threshold) — a genuine wasClicked at edge coordinates
// that spuriously turns a page (a left-edge graze reads as "backward", the user's
// reported symptom), or (b) rest on the panel while the user's deliberate tap
// lands in slot 1+, where slot-0-only code never examines it (taps feel dead).
// Fix: scan every touch slot; among clicks that release in the same frame pick
// the most recent (largest base_msec = the deliberate tap, not the earlier
// graze); and never turn a page on an edge-band click. Rotation was ruled out —
// coords are already rotation-corrected live (0.8.7 investigation).
// Three-finger long-press = global grid page-switcher toggle. GT911 tracks up
// to 5 points; >=3 held continuously for >=600ms fires once (edge-triggered),
// then all touch is swallowed until every finger lifts (s_ignoreUntilClear) so
// the 3-finger release can't read as a preview cell tap. A steady 3-finger grip
// keeps the count at 3; a momentary dip to <3 restarts the timer — acceptable.
static const uint32_t THREE_FINGER_MS = 600;

// Hit-test the current page's region table at (tx,ty) and dispatch the matching
// region exactly the way a long-press does: type=1 -> the page's firmware-local
// onLocalAction; type=0 -> post a pending host tap (+ the confirm chime). Returns
// true when a region was hit. Shared by the long-press AND the new short-tap path
// (fw 0.4.x made a tap trigger the same action) so the two can never drift — the
// press coords are content-frame (see the click-path note below); ty picks the
// band, tx resolves the column/stepper the page cares about.
static bool dispatchRegion(int ring, int tx, int ty) {
    pages::Page* pg = pageAt(ring);
    int rn = 0;
    const pages::Region* tbl = pg ? pg->regions(&rn) : nullptr;
    for (int k = 0; k < rn; k++) {
        if (ty < tbl[k].y0 || ty >= tbl[k].y1) continue;
        // Press feedback: invert-flash the exact button cell the press hit
        // (pressRect resolves the sub-cell — a Room grid column, an Outside verb
        // half — and returns w<=0 for an empty cell so it never flashes) BEFORE
        // the action runs, so the black flash reads as "press registered".
        pages::Rect pr = pg->pressRect(tbl[k], tx, ty);
        bool flashed = pr.w > 0 && pr.h > 0;
        if (flashed) flashPressRect(pr);
        uint32_t gen = s_showGen;
        if (tbl[k].type == 1) {
            pg->onLocalAction(tbl[k].param, tx, ty);
            Serial.printf("[pager] local-action page=%d region=%d param=%u\n",
                          ring, k, tbl[k].param);
        } else {
            s_tapSeq++;
            s_pendingSeq  = s_tapSeq;
            s_pendingPage = ring;
            s_pendingItem = k;
            M5.Speaker.tone(1800, 80);   // confirm the press landed on a region
            Serial.printf("[pager] region-press page=%d region=%d seq=%lu\n",
                          ring, k, (unsigned long)s_tapSeq);
        }
        // Rebound the flash: if the action did NO full-page showPage (gen
        // unchanged — a host tap, a silent cooldown reject, or a partial-only
        // repaint like Room's RC_ERR_COST log refresh that leaves the button rect
        // black), push the now-restored canvas rect back so the black flash bounces
        // off. A showPage already repainted the button, so nothing to do then.
        if (flashed && s_showGen == gen)
            partialRefresh(pr, pages::RefreshMode::FASTEST);
        return true;
    }
    return false;
}

bool handleTouch() {
    int tc = M5.Touch.getCount();
    uint32_t nowMs = millis();

    // Swallow everything until all fingers lift once, after a gesture fired —
    // keeps the multi-touch release out of the normal/preview tap paths.
    static bool s_ignoreUntilClear = false;
    if (s_ignoreUntilClear) {
        if (tc != 0) return true;                  // still interacting, consumed
        s_ignoreUntilClear = false;
    }

    // Event modal (research.md §4.1): while a random event is up it owns every
    // touch. A single-finger press on a button band drives events::choose()
    // (event_modal::handleHold does the hit-test, tones, repaint/exit) — fw 0.4.x
    // accepts a SHORT TAP as well as a long-press, so the modal's choice buttons
    // are tappable like everything else. Page turns and tab switches stay inert
    // here (this branch consumes everything and never falls through); a >=3-finger
    // grip is swallowed too, so the preview switcher can't open over a live event.
    // Sits before the three-finger branch precisely to win that race.
    if (event_modal::active()) {
        for (int i = 0; tc <= 1 && i < tc; i++) {
            auto t = M5.Touch.getDetail(i);
            if (!t.wasHold() && !t.wasClicked()) continue;
            return event_modal::handleHold(t.x, t.y);   // coords already content-frame
        }
        return true;                               // consume anything else / multi-touch
    }

    // Combat overlay (P2.3): same ownership as the event modal — a single-finger
    // press (tap OR hold) drives fight_modal's attack/heal/flee band; page turns
    // and tab switches stay inert (this branch consumes everything and never falls
    // through). No tap-debounce here on purpose: a weapon's own cooldown absorbs
    // the e-ink double-tap bounce, so rapid attack tapping stays responsive.
    if (fight_modal::active()) {
        for (int i = 0; tc <= 1 && i < tc; i++) {
            auto t = M5.Touch.getDetail(i);
            if (!t.wasHold() && !t.wasClicked()) continue;
            return fight_modal::handleHold(t.x, t.y);
        }
        return true;                               // consume anything else / multi-touch
    }

    // Landmark setpiece overlay (P2.4): same ownership. Sits AFTER fight_modal so
    // that while an interleaved setpiece combat is live the fight gets the press;
    // once the fight hands back, this branch drives the narrative choice buttons.
    if (setpiece_modal::active()) {
        for (int i = 0; tc <= 1 && i < tc; i++) {
            auto t = M5.Touch.getDetail(i);
            if (!t.wasHold() && !t.wasClicked()) continue;
            return setpiece_modal::handleHold(t.x, t.y);
        }
        return true;                               // consume anything else / multi-touch
    }

    // Three-finger long-press detection (before any single-finger handling).
    static uint32_t s_multiStart = 0;
    static bool     s_multiFired = false;
    if (tc >= 3) {
        if (s_multiStart == 0) s_multiStart = nowMs;
        if (!s_multiFired && nowMs - s_multiStart >= THREE_FINGER_MS) {
            s_multiFired = true;
            s_ignoreUntilClear = true;
            M5.Speaker.tone(2000, 60);             // switcher feedback
            if (preview::active()) {               // second gesture -> back to origin
                preview::exit();
                showPage(currentRingIndex(), false);
            } else {
                preview::enter();
            }
        }
        return true;                               // 3 fingers down = interaction
    } else {
        s_multiStart = 0;
        s_multiFired = false;
    }

    // Preview modal (focus view): three outcomes — a RAIL cell tap moves the
    // focus to that page and stays in preview (repaint); a BIG-pane tap jumps to
    // the focus page and leaves; any other tap returns to the origin page. Tap
    // regions and the page-turn halves are inert here (state-mutually-exclusive).
    // Pick the most recent click across slots (same graze rule as normal path).
    if (preview::active()) {
        int chosen = -1; uint32_t chosenMsec = 0;
        for (int i = 0; i < tc; i++) {
            auto t = M5.Touch.getDetail(i);
            if (!t.wasClicked()) continue;
            if (chosen < 0 || t.base_msec > chosenMsec) { chosen = i; chosenMsec = t.base_msec; }
        }
        if (chosen < 0) return false;              // no tap yet — still on the view
        auto t = M5.Touch.getDetail(chosen);
        int hit = preview::hitCell(t.x, t.y);      // HIT_BIG / rail idx / HIT_NONE
        Serial.printf("[preview] tap x=%d y=%d -> hit=%d\n", t.x, t.y, hit);
        if (hit >= 0) {                            // rail cell: refocus, stay in view
            preview::setFocus(hit);
        } else if (hit == preview::HIT_BIG) {      // big pane: jump to focus, leave
            int f = preview::focusIndex();
            preview::exit();
            showPage(f, false);
        } else {                                   // outside: back to origin page
            preview::exit();
            showPage(currentRingIndex(), false);
        }
        return true;
    }

    // Long-press (M5Unified's own hold detection, default 500ms threshold)
    // selects a tap region on the current page instead of turning the page.
    // A hold that lands outside every region (or on a page with none) still
    // counts as interaction — it just selects nothing. Take the first slot
    // that reports a hold this frame. Suppressed entirely when >=2 points are
    // down, so a three-finger press-down can't fire a type=1 region hold (e.g.
    // cancel a running pomo) on its way to the switcher gesture.
    for (int i = 0; tc <= 1 && i < tc; i++) {
        auto t = M5.Touch.getDetail(i);
        if (!t.wasHold()) continue;
        // Coords reaching pager code are content-frame at rot 2 (full rationale on
        // the click path below) — no correction needed. dispatchRegion does the
        // hit-test + action; a hold that lands outside every region selects nothing
        // but still counts as interaction (return true either way).
        dispatchRegion(currentRingIndex(), t.x, t.y);
        return true;
    }
    // Collect clicks across all slots; when a holding-hand graze and the
    // deliberate tap release in the same frame, the deliberate tap is the more
    // recent press — the slot with the largest base_msec.
    int chosen = -1;
    uint32_t chosenMsec = 0;
    for (int i = 0; i < tc; i++) {
        auto t = M5.Touch.getDetail(i);
        if (!t.wasClicked()) continue;
        if (chosen < 0 || t.base_msec > chosenMsec) {
            chosen = i;
            chosenMsec = t.base_msec;
        }
    }
    if (chosen < 0) return false;
    auto t = M5.Touch.getDetail(chosen);
    int n = ringCount();
    // Rotation-2 coordinates. DEVICE-EMPIRICAL (2026-07-18 live experiment on
    // 0.9.1, with the dots-lag bug already fixed): taps register cleanly but
    // INVERTED with the 0.9.1 mirror in place — the library's coords were
    // content-frame all along, and the mirror itself was the inversion. The
    // earlier "reversal" observation (that motivated the 0.9.1 mirror) was
    // contaminated by the dots-lag bug fixed the same day. No correction
    // needed; trust the raw coords. tx feeds the edge grip band + the
    // left/right half test; ty feeds regionHit.
    int tx = t.x, ty = t.y;
    int W = M5.Display.width();
    Serial.printf("[pager] tap x=%d y=%d w=%d cnt=%d idx=%d pages=%d\n",
                  tx, ty, W, tc, chosen, n);
    // Edge grip band: a click within 24px of either edge is almost certainly the
    // holding hand, not intent — count it as interaction (the caller still opens
    // the wake window) but take no action on it (a left-edge graze must not turn a
    // page or trip a tab/region).
    if (tx < 24 || tx >= W - 24) {
        Serial.printf("[pager] tap ignored edge x=%d\n", tx);
        return true;
    }
    // Tap debounce: on this hard e-ink screen a single physical tap makes the
    // finger rebound and the controller reports TWO wasClicked events (~<350ms
    // apart, far below any deliberate double-tap) — measured on ~25% of taps. This
    // now guards ALL tap outcomes, not just page turns: unguarded, a bounce would
    // double-turn a page OR double-fire a region action (e.g. gather twice). One
    // tap = one action. (Long-presses don't bounce, so the hold path skips this.)
    static uint32_t s_lastActMs = 0;
    if (s_lastActMs != 0 && nowMs - s_lastActMs < 350) return true;

    // New tap semantics (fw 0.4.x — "long-press to click was unintuitive"): a
    // short tap now ACTS, falling back to a page turn only when it hits nothing.
    // 1) Tab header (top band, on pages that draw tabs): jump straight to the
    //    tapped tab's page. hitTab returns the page name of the tab under x; map it
    //    to a ring via ringIndexByName. Leaving the AssignPage (a village sub-page)
    //    closes its latch so its ring slot re-hides. Re-tapping the current page's
    //    own tab is a no-op (skip the needless refresh).
    if (ty < page_tabs::TAB_H) {
        const char* tabName = page_tabs::hitTab(tx);
        if (tabName) {
            int ring = ringIndexByName(tabName);
            if (ring >= 0 && ring != currentRingIndex()) {
                if (assign_page::isOpen()) assign_page::close();
                if (path_page::isOpen()) path_page::close();   // village sub-page: re-hide its slot
                showPage(ring, false);
            }
            s_lastActMs = nowMs;
            return true;               // a tab tap never falls through to a turn
        }
    }
    // 2) Current-page regions (the SAME table a long-press uses): a hit runs the
    //    identical action path (dispatchRegion). No page turn on a hit.
    if (dispatchRegion(currentRingIndex(), tx, ty)) {
        s_lastActMs = nowMs;
        return true;
    }
    // 3) Nothing hit -> page-turn fallback (left half = prev, right half = next).
    if (n <= 1) return true;              // interaction, nothing to turn
    int cur = currentRingIndex();
    bool left = tx < W / 2;
    int dir = left ? -1 : 1;
    // Turns are epd_fast so the flash never lands on the user's own action — the
    // debt is normally settled at sleep instead (payGhostDebtIfDue). Escape valve:
    // past HIGH_WATER a marathon in-session run pays on this turn so ghosting can't
    // grow unbounded. showPage repaints the bar itself. Scan for the nearest
    // available page in the tapped direction (showPageOrNext) rather than one fixed
    // target — a missing/corrupt neighbour used to soft-lock this direction. If
    // nothing in that direction can be shown, leave the screen as-is.
    if (!showPageOrNext(cur, dir, s_fastCount >= HIGH_WATER))
        Serial.println("[pager] no displayable page");
    s_lastActMs = nowMs;
    return true;
}

// Drive the current page's time axis (seconds counter, header clock). Called
// every loop() pass; no-op for pages whose tick() is empty (ServerPage).
void tickCurrent(uint32_t nowMs) {
    // Suppress every page-tick draw side effect while the switcher is up — a
    // running pomo's per-second partialRefresh would otherwise paint MM:SS over
    // the grid. Least-intrusive choke point (one guard covers all page ticks);
    // the pomo service keeps counting, only its VIEW repaint is held off.
    if (preview::active()) return;
    if (event_modal::active()) return;   // event modal owns the panel too
    if (fight_modal::active()) return;   // combat overlay drives its own tick (main.cpp)
    if (setpiece_modal::active()) return;  // setpiece overlay owns the panel too
    pages::Page* p = pageAt(currentRingIndex());
    if (p) p->tick(nowMs);
}

// Boot restore: resolve the persisted curName to a ring index
// (resolveRingByName — falls back to page 0 when the stored name no longer
// exists) and show it; if that page can't draw, step to the nearest
// displayable one.
void restore(bool quality) {
    if (ringCount() <= 0) return;
    int target = resolveRingByName();
    if (!showPage(target, quality))
        showPageOrNext(target, 1, quality);
}

}  // namespace pager
