// Client-page registry: firmware-rendered pages appended at the ring tail.
// Adding a client page = implement pages::Page + one line here. Empty today
// (the pomo ClientPage is registered in Task 5).
#pragma once
#include "page.h"

namespace client_pages {

int count();                 // number of registered client pages
pages::Page* at(int i);      // 0..count-1, else nullptr

// Power-axis aggregate over the registry (the "services keep the device
// awake" gate, page-independent). Today: pomo active once Task 5 registers it.
bool anyWantsAwake();

}  // namespace client_pages
