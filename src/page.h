// The Page interface: the pager holds Page* and never special-cases a page
// kind. ServerPage (pager.cpp) wraps a host-pushed PNG in frame_store;
// ClientPages (client_pages registry) are firmware-rendered. The ring is
// [ServerPage × pageCount()] + [client pages], client pages tail-fixed.
// See docs/superpowers/specs/2026-07-20-client-pages-design.md.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace m5gfx { class M5Canvas; }

namespace pages {

// A touch region: a y-band [y0,y1) with type/param — the exact shape the BLE
// REGIONS wire format delivers (type 0 = host action, type 1 = firmware-local,
// param = pomodoro minutes). ClientPages return a firmware-constant table.
struct Region { uint16_t y0, y1; uint8_t type, param; };

// A partial-refresh target (see pager::partialRefresh). Whole-panel is
// {0,0,540,960}; a client page passes just the sub-rect it repainted.
struct Rect { int x, y, w, h; };

// FAST = e-ink fast/DU-class update of the rect (ghosting accrues, charged to
// the existing debt counter). FASTEST = even quicker DU-class update for
// sub-second cadences (e.g. the pomodoro countdown tick); same debt accounting
// as FAST. QUALITY = grayscale-clean update (clears local ghosting, resets
// the debt).
enum class RefreshMode { FAST, FASTEST, QUALITY };

class Page {
public:
    virtual ~Page() = default;

    // Stable identity — persisted as the current page ("srv:0", "pomo").
    virtual const char* name() const = 0;

    // Full-page paint into the shared canvas. Returns false when the page
    // could not paint (a missing/torn server PNG) so the pager can skip it;
    // client pages always return true.
    virtual bool draw(m5gfx::M5Canvas& canvas) = 0;

    // Touch table (y-bands). *n receives the count; nullptr/0 = no regions.
    virtual const Region* regions(int* n) const = 0;

    // Firmware-local action: the pager calls this when a long-press lands on
    // one of THIS page's type=1 regions (see pager::handleTouch), passing the
    // region's param. The action is consumed on-device, never reported to the
    // host. Default: no-op (server pages carry no firmware-local behavior).
    virtual void onLocalAction(uint8_t param) { (void)param; }

    // Time axis — called every loop() pass ONLY while this page is current
    // (pager::tickCurrent). May partial-refresh a sub-rect. Default: no-op.
    virtual void tick(uint32_t nowMs) { (void)nowMs; }

    // Power axis — true keeps the device awake even off this page. Default off.
    virtual bool wantsAwake() const { return false; }
};

}  // namespace pages
