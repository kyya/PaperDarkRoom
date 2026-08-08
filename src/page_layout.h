// Shared page layout authority: the one content-margin constant every
// firmware-rendered page and modal measures its columns/rules/buttons from, so
// the whole ring keeps a single horizontal rhythm (host pad = 24, matching the
// daemon's pixel pages). Per-page button geometry stays local to each page.
#pragma once

constexpr int PAD = 24;
