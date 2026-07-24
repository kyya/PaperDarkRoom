#include "client_pages.h"
#include "room_page.h"
#include "outside_page.h"
#include "trade_page.h"
#include "assign_page.h"
#include "path_page.h"
#include "world_page.h"
#include "event_engine.h"
#include "fight_modal.h"

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
// AssignPage (v0.4.0) is a non-tab sub-page reached from the Outside 分工 cell;
// it stays hidden (draw() returns false) until opened, exactly like Outside/Trade
// hide until their unlock — just latched on assign_page::isOpen() instead of a
// game flag. Registered here so it occupies a stable ring slot (findable by name
// for the open/return jumps, see pager::ringIndexByName).
static AssignPage s_assign;
// PathPage (P2.1) is the other non-tab sub-page — reached from the Outside 尘土之路
// cell, gated on holding a compass, latched on path_page::isOpen(), and hidden
// (draw() returns false) until opened, exactly like AssignPage. Appended after
// assign (AssignPage precedent) so it holds a stable ring slot findable by name.
static PathPage s_path;
// WorldPage (P2.2) is the map-exploration screen — NOT a village sub-page (it
// draws its own 荒芜世界 title, no page_tabs). It hides (draw() returns false)
// until an expedition is live: available() is gated on g_world.ex.active (the
// embark / cold-boot-resume state) OR its own death frame, not a latch. Appended
// last so PathPage's doEmbark, which jumps to ringIndexByName("world"), resolves
// this stable ring slot by name.
static WorldPage s_world;
static pages::Page* s_reg[] = { &s_room, &s_outside, &s_trade, &s_assign, &s_path,
                                &s_world };

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
    // A live combat overlay (P2.3) keeps the card awake too: the enemy attacks
    // every second, so the fight must not be frozen by a deep sleep. It resolves
    // on its own (win/death) or a flee, and the victory panel self-dismisses after
    // its idle timeout, so this never pins the card awake indefinitely.
    if (fight_modal::active()) return true;
    for (int i = 0; i < count(); i++)
        if (s_reg[i]->wantsAwake()) return true;
    return false;
}

}  // namespace client_pages
