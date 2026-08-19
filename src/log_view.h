// Shared newest-on-top log stream (Room + Trade). Renders GameState::log
// through tr() so only strings_zh.h glyphs reach the 12px face.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "page.h"

namespace m5gfx { class M5Canvas; }
namespace adr { struct LogEntry; }

namespace log_view {

constexpr int SCALE = 2;                     // 12px face x2 = 24px
constexpr int LINEH = 12 * SCALE + 12 * SCALE / 4;   // 30

using Keep = bool (*)(const adr::LogEntry&);

void format(const adr::LogEntry& e, char* out, size_t cap);
uint32_t sig(Keep keep = nullptr);
void draw(m5gfx::M5Canvas& c, int x, int top, int w, int bottom,
          Keep keep = nullptr);
pages::Rect areaRect(int top, int bottom);
void pushBand(int top, int bottom);

}  // namespace log_view
