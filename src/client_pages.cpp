#include "client_pages.h"
#include "room_page.h"
#include "outside_page.h"

// A Dark Room firmware: the ring is entirely game client pages (no host-pushed
// server pages). Phase 1 registers Room then Outside; the real Path / World
// pages append here as they land. Outside stays invisible (draw() returns
// false, pager skips it) until the forest is unlocked.
namespace client_pages {

static RoomPage s_room;
static OutsidePage s_outside;
static pages::Page* s_reg[] = { &s_room, &s_outside };

int count() { return (int)(sizeof(s_reg) / sizeof(s_reg[0])); }

pages::Page* at(int i) {
    return (i >= 0 && i < count()) ? s_reg[i] : nullptr;
}

bool anyWantsAwake() {
    for (int i = 0; i < count(); i++)
        if (s_reg[i]->wantsAwake()) return true;
    return false;
}

}  // namespace client_pages
