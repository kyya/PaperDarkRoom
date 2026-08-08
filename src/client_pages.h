// Client-page registry: firmware-rendered pages appended at the ring tail.
// Adding a client page = implement pages::Page + one line here.
#pragma once
#include "page.h"

namespace client_pages {

int count();                 // number of registered client pages
pages::Page* at(int i);      // 0..count-1, else nullptr

// Power-axis aggregate over the registry (the "services keep the device
// awake" gate, page-independent) plus the live overlays (event / fight /
// setpiece modals), which must not be deep-slept mid-choice.
bool anyWantsAwake();

}  // namespace client_pages
