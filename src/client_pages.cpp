#include "client_pages.h"
#include "room_page.h"
#include "outside_page.h"
#include "trade_page.h"
#include "event_engine.h"

// A Dark Room firmware: the ring is entirely game client pages (no host-pushed
// server pages). The original two-page structure — Room + Outside, each fronted
// by the shared two-tab header (page_tabs) — is registered here; the old
// standalone Inventory page was folded into the Outside page's lower band
// (fw 0.2.2). The real Path / World pages append here as they land. Outside
// stays invisible (draw() returns false, pager skips it) until the forest is
// unlocked; Trade (v0.3.3) does the same until the trading post stands — so the
// reachable ring grows as the game unlocks, though every registered page still
// counts toward the status bar's page dots (a hidden page's dot shows but is
// simply un-turnable-to, the same pre-existing behavior Outside has had).
namespace client_pages {

static RoomPage s_room;
static OutsidePage s_outside;
static TradePage s_trade;
static pages::Page* s_reg[] = { &s_room, &s_outside, &s_trade };

int count() { return (int)(sizeof(s_reg) / sizeof(s_reg[0])); }

pages::Page* at(int i) {
    return (i >= 0 && i < count()) ? s_reg[i] : nullptr;
}

bool anyWantsAwake() {
    // An on-screen random event keeps the card awake (research.md §5.4): the
    // modal must not be deep-slept mid-choice. The engine's 2-minute idle
    // timeout (event_modal::checkTimeout) releases this so a forgotten card can
    // still sleep.
    if (events::active()) return true;
    for (int i = 0; i < count(); i++)
        if (s_reg[i]->wantsAwake()) return true;
    return false;
}

}  // namespace client_pages
