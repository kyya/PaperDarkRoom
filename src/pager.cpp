#include "pager.h"
#include "frame_store.h"
#include "status_bar.h"
#include "page.h"
#include "client_pages.h"
#include "event_modal.h"
#include "fight_modal.h"
#include "setpiece_modal.h"
#include "space_page.h"
#include "page_tabs.h"
#include "assign_page.h"
#include "path_page.h"
#include "tech_page.h"
#include "msg_bridge.h"
#include "touch_gt911.h"
#include "beeper.h"
#include <M5Unified.h>
#include <climits>
#include <cstring>

extern M5Canvas canvas;   // main.cpp owns the full-screen sprite

namespace pager {

// Ghosting debt, in USER-VISIBLE SCREEN CHANGES (see presentFrame, which is the
// only thing that charges it). The panel driver (lib/msg, see msg_bridge.h)
// drives each pixel only until it has settled, which is a DU-class update and
// accumulates the same ghosting an epd_fast push used to: the counter, its unit
// and its two thresholds are all carried over unchanged from the M5GFX era, and
// only what "paying" MEANS changed.
// It is now the deghost pair in deghost() — eight fields of "lighten
// everything" followed by eight fields of the frame that was up — rather than an
// epd_quality redraw. Policy is unchanged too: the debt is normally settled at
// SLEEP ENTRY (main.cpp's sleepNow calls deghost() outright, since the frame it
// leaves behind is on show for the whole sleep), with a marathon interactive
// session past HIGH_WATER paying on the next turn so ghosting can't grow
// unbounded. Boot restore also pays (power-button = user action).
static int s_fastCount = 0;
static const int QUALITY_EVERY = 8;    // refresh debt that a sleep deep-clean settles
static const int HIGH_WATER   = 24;    // in-session escape valve: pay on the next turn past this

// How long the press-feedback flash holds the inverted frame on the glass. The
// driver's pipeline puts a submitted frame on the panel one scan (~23 ms) after
// the flip returns, so anything under ~60 ms would be submitted and superseded
// before the eye ever saw it.
static const uint32_t FLASH_MS = 120;

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

// Swallow every touch until the panel reads fully untouched once. Set by the
// >=3-finger grip latch in handleTouch() and by deghost(); cleared there.
static bool s_ignoreUntilClear = false;

static int s_curRing = 0;   // ring index of the page currently shown

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
    // DEAD CODE WITH A TRIPWIRE ON IT. This firmware has no host and never
    // registers a server page (frame_store::pageCount() is always 0), so this
    // never runs — but it is NOT safe to start using again as-is under MSG, for
    // two reasons that both arrived with the migration:
    //   - it does not fillSprite() first, and every frame is now composed into a
    //     buffer holding the frame from two flips ago (msg_bridge.h), so a PNG
    //     smaller than the panel would leave stale pixels around it;
    //   - the canvas is 1bpp behind a REPLACED colour converter (inkPalette1),
    //     which maps by LSB parity — so decoded photographic pixels come out as
    //     noise, not as a dither.
    // Anyone re-enabling host pages has to fix both before trusting this.
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

// Is ring slot `ring` currently displayable? The single reachability predicate
// behind the counters below: Page::available() is what draw() gates on and what
// showPageOrNext steps over, so anything built on this agrees with where the
// user can actually land. Out-of-range slots are not.
bool ringAvailable(int ring) {
    pages::Page* pg = pageAt(ring);
    return pg && pg->available();
}

// How many ring slots are currently displayable (Page::available() true) — the
// status bar draws exactly this many page dots. A conditionally-hidden game page
// (un-unlocked Outside/Trade, a closed AssignPage) is available()==false and
// drops out, so the dot count matches what the user can actually page to. Reads
// the same predicate draw() gates on, so a dot never lies about reachability.
int visibleCount() {
    int n = ringCount(), c = 0;
    for (int r = 0; r < n; r++) if (ringAvailable(r)) c++;
    return c;
}

// The given ring index's ordinal among visible pages (0-based) = how many
// available() slots precede it. For the current page (always available — it just
// drew) this is the solid dot's position. Ring indices past ringCount clamp to
// the visible total.
int visibleIndexOf(int ringIdx) {
    int n = ringCount(), idx = 0;
    for (int r = 0; r < ringIdx && r < n; r++) if (ringAvailable(r)) idx++;
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

// Compose the WHOLE 540x960 frame from scratch into the canvas — page (or
// whichever overlay owns the panel) plus the chrome band.
//
// This exists because the panel is double-buffered now: msg_bridge::present()
// hands the composed buffer to the scan and gives back the OTHER one, which
// holds the frame from two flips ago. Nothing on screen can be touched up, so
// every path that used to push a sub-rect goes through here instead. It is
// affordable — a complete render measures ~8 ms against the measured ~23 ms
// scan period —
// and it is why partialRefresh() and the waveform selection around it are gone
// rather than ported.
//
// The overlay dispatch is the same ownership ladder showPage() enforces below,
// read in the same order: an interleaved setpiece combat is drawn by the FIGHT,
// so the fight is tested before the setpiece that raised it.
static void drawFrame() {
    if (event_modal::active())    { event_modal::renderFrame();    return; }
    if (fight_modal::active())    { fight_modal::renderFrame();    return; }
    if (setpiece_modal::active()) { setpiece_modal::renderFrame(); return; }
    // The Space level (3b). Last of the four full-screen owners and the only one
    // that is not pumped by appLoop: it runs a blocking sub-loop that composes and
    // presents its own frames. It is in this ladder for ONE reason — deghost()
    // composes through here, and the level's entry/exit deghosts must snapshot the
    // GAME's frame, not the Starship page hiding behind it.
    if (space_page::active())     { space_page::renderFrame();     return; }
    pages::Page* p = pageAt(currentRingIndex());
    if (p && p->draw(canvas)) {
        status_bar::drawOnto(canvas);
        status_bar::drawVersionOnto(canvas);
        return;
    }
    // No page, or one that declined to paint (a cold/torn server-page cache).
    // The buffer is two frames stale, so it cannot simply be left alone.
    canvas.fillSprite(TFT_WHITE);
    status_bar::drawOnto(canvas);
}

// What the USER would call "a different screen": which overlay owns the panel,
// or which page is up when none does. Two frames with the same identity are the
// same screen redrawn — a cooldown bar draining, a clock digit, a press flash —
// however many of them there are.
static uint32_t frameIdentity() {
    if (event_modal::active())    return 0xE0000000u;
    if (fight_modal::active())    return 0xF0000000u;
    if (setpiece_modal::active()) return 0x50000000u;
    if (space_page::active())     return 0x5AC30000u;
    return (uint32_t)currentRingIndex();
}

static uint32_t s_shownId = 0xFFFFFFFFu;   // identity of the last frame presented

// Present, and charge the ghosting debt ONLY when the screen actually changed.
//
// THE DEBT COUNTS SCREEN CHANGES, NOT FRAMES, and under whole-frame redraw those
// two had to be decoupled or the counter stops meaning anything. Its original
// sense was "user-visible fast page turns since the last deep clean", which is a
// sound proxy for accumulated ghosting because that is what leaves a ghost: new
// ink in place of old. The migration accidentally bound it to the repaint rate
// instead, and repaints now happen for reasons that put no new ink on the glass
// — a 1 s bar tick, a cooldown tick, four frames per button press. At those
// rates HIGH_WATER (24) was reached within seconds, so nearly every page turn
// dragged a 400 ms full-panel white flash behind it.
//
// Counting identity changes restores the old cadence exactly (~8 turns to a
// sleep-time clean, 24 to the in-session escape valve) without touching either
// threshold, and it correctly charges the two things that DO replace the whole
// screen: a page turn, and an overlay appearing or disappearing.
static void presentFrame() {
    uint32_t id = frameIdentity();
    if (id != s_shownId) {
        s_shownId = id;
        if (s_fastCount < INT_MAX) s_fastCount++;
    }
    msg_bridge::present();
}

void repaint() {
    drawFrame();
    presentFrame();
}

// Settle the ghosting debt: eight fields of "lighten everything" through image
// mode's fixed LUT waveform, then eight fields of the frame that was up.
//
// PUSHING THE SAME FRAME BACK IS NOT COSMETIC, and this is the one thing to
// understand before touching this function. Image mode writes the glass without
// going through — or updating — video mode's per-pixel state model, and lib/msg
// is vendored unmodified by policy so there is no hook to re-seed that model.
// A lone white push would therefore leave video believing the panel still holds
// the old picture, and the next frame would drive only the pixels that differ
// from it: everything the two frames agree on would stay white. Ending the
// image sequence on exactly the frame the model already believes is displayed
// is what keeps the two consistent. Hence the sequence below — compose, keep a
// byte copy, drive it through VIDEO so the model owns it, let it settle, then
// white-flash and put the copy back.
// `compose` is false when the caller has ALREADY composed the frame into the
// canvas — showPage() has to draw before it can know the page is displayable at
// all (that return value is the availability probe), so making it draw again
// here would render every quality page turn twice for nothing.
static void deghostFrame(bool compose) {
    uint8_t* snap = msg_bridge::scratch();
    if (!snap) {
        // Allocation failed at boot: no deghost is possible, but the frame still
        // has to reach the panel — showPage's caller is relying on this having
        // presented it. Clear the debt so we don't retry every turn.
        if (compose) drawFrame();
        presentFrame();
        s_fastCount = 0;
        return;
    }
    if (compose) drawFrame();
    memcpy(snap, msg_bridge::frame(), msg_bridge::frameBytes());
    presentFrame();          // keeps s_shownId in step; s_fastCount zeroed below
    msg_bridge::waitSettled(2000);
    memset(msg_bridge::frame(), 0xFF, msg_bridge::frameBytes());
    msg_bridge::pushImage(msg_bridge::frame(), true);    // lighten everything
    msg_bridge::pushImage(snap, false);                  // ... and the frame back
    s_fastCount = 0;
    // Sixteen fields of the whole panel being driven hard is a far bigger
    // electrical and optical disturbance than the epd_quality flash this
    // replaced — and that flash was already documented to induce phantom
    // contacts on the GT911 (the modal-open bounce guard in handleTouch exists
    // for exactly that). So arm the same swallow-until-lift latch here rather
    // than a fixed delay: a time window cannot work, because a phantom that
    // lands as a HOLD needs 500ms of contact to be reported and no window short
    // enough to be unobtrusive can cover that. Waiting for the panel to read
    // untouched once is the only test that actually terminates on reality.
    // Covers the page-turn path, which had nothing but the 350ms tap debounce.
    s_ignoreUntilClear = true;
}

void deghost() { deghostFrame(/*compose=*/true); }

bool showPage(int ring, bool quality) {
    // A modal owns the panel while it's up: refuse every redraw (background BLE
    // push, pomo phase flip) so none can clobber it. The event modal
    // (research.md §4.1) first: while a random event is on screen no background
    // push / tick can repaint the page under it.
    // event_modal::closeAndRestore() clears the flag before its own showPage.
    if (event_modal::active()) return false;
    // And the same for the combat overlay (P2.3): it owns the panel while a fight
    // is live; fight_modal clears its flag before its own closeToWorld showPage.
    if (fight_modal::active()) return false;
    // And the landmark setpiece overlay (P2.4): it owns the panel across its whole
    // run (including an interleaved fight); setpiece_modal clears its flag before
    // its own closeToWorld showPage.
    if (setpiece_modal::active()) return false;
    // Same for the Space level: it owns the panel for the whole flight and clears
    // its own flag before the showPage that restores the Starship page.
    if (space_page::active()) return false;
    pages::Page* p = pageAt(ring);
    if (!p) return false;
    bool ok = p->draw(canvas);
    if (ok) {
        // Set current FIRST — the bar reads currentRingIndex() to pick the
        // solid page dot, so bake it in before drawOnto or the dots lag.
        s_curRing = ring;
        frame_store::setCurrentName(p->name());
        // Bar + version bake into the canvas so page + chrome land in ONE frame.
        // Every redraw path (turn, push, boot) funnels through here.
        status_bar::drawOnto(canvas);
        status_bar::drawVersionOnto(canvas);
        // The canvas is already composed at this point, so tell the deghost not
        // to redo it — the draw above was the availability probe and cannot be
        // skipped, which is exactly why the deghost's own compose has to be.
        if (quality) {
            deghostFrame(/*compose=*/false);
        } else {
            presentFrame();
        }
    }
    return ok;
}

// Invert-flash a button rect as press feedback: compose the frame, flip the bits
// inside `r` in the back buffer, present that, hold it long enough to be seen,
// then compose and present the normal frame again.
//
// The old implementation inverted the CANVAS and pushed just the rect, so the
// canvas ended unchanged and the caller had to "rebound" the flash afterwards if
// its action happened not to repaint. None of that survives double buffering:
// the buffer this writes into is discarded on the next flip anyway, and
// repaint() below is what puts the panel back — so the rebound bookkeeping (and
// the push-generation counter that drove it) is deleted, not ported. An empty
// rect (pressRect's "don't flash" signal) never reaches here, but is rejected
// anyway because msg_bridge::invertRect would clip it to nothing silently.
void flashPressRect(const pages::Rect& r) {
    if (r.w <= 0 || r.h <= 0) return;
    drawFrame();
    msg_bridge::invertRect(r.x, r.y, r.w, r.h);
    msg_bridge::present();
    delay(FLASH_MS);
    repaint();
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

// Grip-graze mechanism (2026-07-17 investigation): the GT911 tracks up to 5
// simultaneous points, but examining only slot 0 (touch::detail(0)) misreads
// a handheld card. The holding hand's finger/thumb can (a) briefly graze an edge
// (<500ms, under the hold threshold) — a genuine wasClicked at edge coordinates
// that spuriously turns a page (a left-edge graze reads as "backward", the user's
// reported symptom), or (b) rest on the panel while the user's deliberate tap
// lands in slot 1+, where slot-0-only code never examines it (taps feel dead).
// Fix: scan every touch slot; among clicks that release in the same frame pick
// the most recent (largest baseMsec = the deliberate tap, not the earlier
// graze); and never turn a page on an edge-band click. Rotation was ruled out —
// coords are already rotation-corrected live (0.8.7 investigation).
// Sustained multi-finger grip guard. GT911 tracks up to 5 points; >=3 held
// continuously for >=600ms is the holding hand settling on the panel, never
// intent, so it latches once (edge-triggered) and all touch is swallowed until
// every finger lifts (s_ignoreUntilClear) — the release of that grip can't read
// as a tap or a page turn. A steady 3-finger grip keeps the count at 3; a
// momentary dip to <3 restarts the timer — acceptable.
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
        // COPY THE REGION FIRST. flashPressRect() now calls drawFrame(), which
        // calls Page::draw(), which re-runs the page's layout in place — Room and
        // Outside rebuild m_regions/m_slotCodes from epochNow() on every draw. So
        // `tbl` is the page's live table and it can be rewritten underneath us
        // during the ~130ms flash: a cooldown expiring or an unlock landing
        // repacks the slots, and the action would then fire on a layout the user
        // never saw. The old flashPressRect only inverted canvas pixels and could
        // not do this. Rare, but the fix is one copy.
        const pages::Region rg = tbl[k];
        pages::Rect pr = pg->pressRect(rg, tx, ty);
        if (pr.w > 0 && pr.h > 0) flashPressRect(pr);
        if (rg.type == 1) {
            pg->onLocalAction(rg.param, tx, ty);
            Serial.printf("[pager] local-action page=%d region=%d param=%u\n",
                          ring, k, rg.param);
        } else {
            s_tapSeq++;
            s_pendingSeq  = s_tapSeq;
            s_pendingPage = ring;
            s_pendingItem = k;
            beeper::tone(1800, 80);      // confirm the press landed on a region
            Serial.printf("[pager] region-press page=%d region=%d seq=%lu\n",
                          ring, k, (unsigned long)s_tapSeq);
        }
        // Whatever the action did or did not repaint, put a fresh frame up. The
        // old code tracked a push generation here so that an action which drew
        // nothing (a host tap, a silent cooldown reject) could rebound just the
        // flashed rect; a whole frame costs ~8 ms now, so the bookkeeping bought
        // nothing and is gone. Deliberately unconditional: an action that DID
        // repaint pays one extra frame rather than this having to guess.
        repaint();
        return true;
    }
    return false;
}

bool handleTouch() {
    int tc = touch::count();
    uint32_t nowMs = millis();

    // Space level (3b): it samples touch itself, inside its own loop, and it must
    // NOT get this function's policy — the 24 px edge band would clip the ends of
    // its control strip, the 350 ms tap debounce is four logic frames of dead
    // input, and hugging the bottom edge with a whole hand is how the level is
    // played, not a grip to latch out (research-phase3.md §8.3). It intercepts
    // first, exactly like the three modals below, for the same reason they do.
    //   Structurally this can never be reached — run() blocks the app task, so
    // appLoop is not calling handleTouch while it is up — which makes the line an
    // assertion against a future non-blocking rewrite rather than dead code.
    if (space_page::active()) return true;

    // Swallow everything until all fingers lift once — armed by a latched
    // multi-finger grip, and by deghost() (see there). Keeps that release out of
    // the normal tap paths.
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
    // grip is swallowed too. Sits before the multi-finger branch precisely to win
    // that race.
    if (event_modal::active()) {
        for (int i = 0; tc <= 1 && i < tc; i++) {
            auto t = touch::detail(i);
            if (!t.held && !t.clicked) continue;
            return event_modal::handleHold(t.x, t.y);   // coords already content-frame
        }
        return true;                               // consume anything else / multi-touch
    }

    // Modal-open bounce guard (P2.4): when a fight/setpiece modal first goes active
    // (rising edge — the frame after its ~300ms blocking begin() returns), the
    // opening tap's e-ink rebound and any phantom touch the entry epd_quality flash
    // induces on the GT911 may still be on the panel. A FIXED time window is the
    // wrong shape: M5Unified needs 500ms of continuous contact to report a hold
    // (wasHold), so no sub-500ms window can ever swallow a bounce that lands as a
    // hold, and phantom release timing is variable. Instead latch on the rising edge
    // and swallow EVERY touch routed to the modal until the panel reads fully
    // untouched (getCount()==0) once — only then are real button presses accepted.
    // Mirrors the three-finger s_ignoreUntilClear latch below. Tracked per modal so
    // a setpiece->combat handoff (beginSetpiece, a second blocking begin) re-arms.
    static bool s_fightWasActive = false;
    static bool s_spWasActive    = false;
    static bool s_modalGuard     = false;   // swallow modal touch until the first tc==0
    bool fightActive = fight_modal::active();
    bool spActive    = setpiece_modal::active();
    if ((fightActive && !s_fightWasActive) || (spActive && !s_spWasActive))
        s_modalGuard = true;                // rising edge: arm the swallow-until-lift
    s_fightWasActive = fightActive;
    s_spWasActive    = spActive;
    if (s_modalGuard && tc == 0)
        s_modalGuard = false;               // panel went untouched — release the guard

    // Combat overlay (P2.3): same ownership as the event modal — a single-finger
    // press (tap OR hold) drives fight_modal's attack/heal/flee band; page turns
    // and tab switches stay inert (this branch consumes everything and never falls
    // through). The open-bounce guard covers the flee band, which (unlike a weapon)
    // has no cooldown to otherwise absorb the rebound the instant the fight opens.
    if (fightActive) {
        if (s_modalGuard) return true;             // swallow open-bounce until first full lift
        for (int i = 0; tc <= 1 && i < tc; i++) {
            auto t = touch::detail(i);
            if (!t.held && !t.clicked) continue;
            return fight_modal::handleHold(t.x, t.y);
        }
        return true;                               // consume anything else / multi-touch
    }

    // Landmark setpiece overlay (P2.4): same ownership. Sits AFTER fight_modal so
    // that while an interleaved setpiece combat is live the fight gets the press;
    // once the fight hands back, this branch drives the narrative choice buttons.
    // The swallow-until-lift guard above stops the opening tap's rebound / entry-
    // flash phantom from landing on the house-start leave band and slamming the
    // setpiece shut the instant it opens.
    if (spActive) {
        if (s_modalGuard) return true;             // swallow open-bounce until first full lift
        for (int i = 0; tc <= 1 && i < tc; i++) {
            auto t = touch::detail(i);
            if (!t.held && !t.clicked) continue;
            return setpiece_modal::handleHold(t.x, t.y);
        }
        return true;                               // consume anything else / multi-touch
    }

    // Multi-finger grip detection (before any single-finger handling): >=3 points
    // down is a hand, not a tap — consume every such frame, and once the grip has
    // been held out latch the swallow-until-lift so its release is inert too.
    static uint32_t s_multiStart = 0;
    static bool     s_multiFired = false;
    if (tc >= 3) {
        if (s_multiStart == 0) s_multiStart = nowMs;
        if (!s_multiFired && nowMs - s_multiStart >= THREE_FINGER_MS) {
            s_multiFired = true;
            s_ignoreUntilClear = true;
        }
        return true;                               // 3 fingers down = interaction
    } else {
        s_multiStart = 0;
        s_multiFired = false;
    }

    // Long-press (M5Unified's own hold detection, default 500ms threshold)
    // selects a tap region on the current page instead of turning the page.
    // A hold that lands outside every region (or on a page with none) still
    // counts as interaction — it just selects nothing. Take the first slot
    // that reports a hold this frame. Suppressed entirely when >=2 points are
    // down, so a three-finger press-down can't fire a type=1 region hold (e.g.
    // cancel a running pomo) on its way to the switcher gesture.
    for (int i = 0; tc <= 1 && i < tc; i++) {
        auto t = touch::detail(i);
        if (!t.held) continue;
        // Coords reaching pager code are content-frame at rot 2 (full rationale on
        // the click path below) — no correction needed. dispatchRegion does the
        // hit-test + action; a hold that lands outside every region selects nothing
        // but still counts as interaction (return true either way).
        dispatchRegion(currentRingIndex(), t.x, t.y);
        return true;
    }
    // Collect clicks across all slots; when a holding-hand graze and the
    // deliberate tap release in the same frame, the deliberate tap is the more
    // recent press — the slot with the largest baseMsec.
    int chosen = -1;
    uint32_t chosenMsec = 0;
    for (int i = 0; i < tc; i++) {
        auto t = touch::detail(i);
        if (!t.clicked) continue;
        if (chosen < 0 || t.baseMsec > chosenMsec) {
            chosen = i;
            chosenMsec = t.baseMsec;
        }
    }
    if (chosen < 0) return false;
    auto t = touch::detail(chosen);
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
    int W = msg_bridge::UI_W;
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
                if (tech_page::isOpen()) tech_page::close();   // Room sub-page: ditto
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
    // debt is normally settled at sleep instead (pager::deghost). Escape valve:
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
    // Suppress every page-tick draw side effect while a modal is up — a running
    // pomo's per-second repaint would otherwise paint MM:SS over it. Least-
    // intrusive choke point (one guard covers all page ticks); the pomo service
    // keeps counting, only its VIEW repaint is held off.
    if (event_modal::active()) return;   // event modal owns the panel
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
