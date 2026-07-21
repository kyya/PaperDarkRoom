// Placeholder "A Dark Room" game page (Phase 1 skeleton). A single firmware-
// rendered client page — no host push, no server pages in the ring. Its only
// job right now is to prove the whole chain is alive end to end: the pager
// draws it, the status bar + version bake in, a tap opens the interactive wake
// window, and the device deep-sleeps and wakes back onto it. Replaced by the
// real Room module (fire / wood / builder / craftables) in a later task.
#pragma once
#include "page.h"

class RoomPage : public pages::Page {
public:
    const char* name() const override { return "room"; }
    bool draw(m5gfx::M5Canvas& canvas) override;
    const pages::Region* regions(int* n) const override;
    // tick / onLocalAction / wantsAwake keep the Page defaults: no time axis,
    // no local actions yet, and it sleeps (earns nothing offline to stay up for).
};
