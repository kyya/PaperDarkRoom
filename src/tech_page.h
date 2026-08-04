// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Tech-tree (科技树) page — a read-only sub-page of the VILLAGE (Outside),
// reached from its 科技树 action cell, the one directly after 伐木. It was the
// Room's sub-page until v0.14, when the user asked for the entry to move
// ("科技树的按钮请你挪到小镇里面伐木的后面一个按钮"); the page body is unchanged,
// but its tab highlight and its 返回 target followed the entry to the village. Player feedback: nobody could tell the game HAS a growth
// line ("骨枪 looked like the ceiling", "never knew a 水壶 was craftable"),
// because the Room only ever offers the craftables you can nearly afford
// (room.js craftUnlocked). This page deliberately breaks that reveal-as-you-go
// rule and lays the four upgrade ladders out in full, from the very first
// action, so the ladder itself is discoverable:
//   武器 拳头 → 骨枪 → 铁剑 → 钢剑 → 步枪 · 护甲 皮甲 → 铁甲 → 钢甲
//   水容器 水壶 → 水桶 → 水罐 · 携带 双肩包 → 篷车 → 车队
// Every row's name and cost is read from the ONE CRAFT table (game_data.h), so
// this page can never drift from what the Room actually charges.
//
// Built on the AssignPage sub-page model: an s_active latch gates draw()/
// available() (an un-opened tech tree is a skipped ring slot, exactly like the
// closed AssignPage), it reuses the SHARED tab header (page_tabs, 生火间 lit —
// it is the village's sub-page, not a 4th tab), and returns via a 「返回」band that
// jumps back to "room". It carries no tick(): nothing on it can change while it
// is on screen (it has no action but 返回), so there is nothing to repaint.
//
// Layout (540x960): tab header(0..72) · four sections from y=84, each a 24px
// heading + one 30px row per entry (a filled/hollow owned marker, the 24px
// name, and the right-aligned cost) · a one-line footnote just above the band
// (the 前哨站 water tip — the fix for the "渴死" complaint) · the 「返回」band at
// 836..916, clear of the 928 status bar. 4 headings + 14 rows fit one screen, so
// unlike the Room grid this page needs no 更多 pagination. KEEPS its 返回 band
// (unlike AssignPage, v0.10.3): the tree already fits with 12px to spare above
// the status bar, so dropping 返回 would buy back a slot nothing here needs —
// see assign_page.h for why THAT page's budget was different. Don't "fix" this
// for consistency; there's nothing to fix.
//
// Naming (§8.3 glyph closure): the page is 科技树 — 科 and 技 are absent from
// strings_zh.h (the upstream translation table the closure is normally scanned
// out of), but gen_cjk_font.py now carries an explicit FIRMWARE_LITERAL_CHARS
// registry for firmware-only UI literals that need a glyph outside that table.
// 科技 is its first entry, so both codepoints are baked into cjk_font12.h same
// as every closure glyph — no runtime fallback, no tofu risk.
#pragma once
#include "page.h"

namespace tech_page {
// Open/close the tech-tree page (flip the visibility latch). The caller pairs
// each with a pager::showPage to actually navigate (open -> this page's ring
// index; close -> back to the Outside page).
void open();
void close();
bool isOpen();
}  // namespace tech_page

class TechPage : public pages::Page {
public:
    const char* name() const override { return "tech"; }
    bool draw(m5gfx::M5Canvas& canvas) override;   // false unless open
    bool available() const override;               // == tech_page::isOpen()
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;  // sole action: 返回
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;
    // No tick()/wantsAwake: a read-only page with nothing that changes on screen.

private:
    // The 「返回」band is the only touch region on the page.
    mutable pages::Region m_regions[1];
    mutable int           m_regionCount = 0;
};
