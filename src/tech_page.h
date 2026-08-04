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
// rule and lays the growth ladders out in full, from the very first action, so
// the ladder itself is discoverable. It carries TWO static pages:
//   装备 (page 0) 武器 拳头 → 骨枪 → 铁剑 → 钢剑 → 步枪 · 护甲 皮甲 → 铁甲 → 钢甲
//                 水容器 水壶 → 水桶 → 水罐 · 携带 双肩包 → 篷车 → 车队
//   建筑 (page 1) 陷阱 → 货车 → 小屋 → 狩猎小屋 → 贸易站 → 制革屋 → 熏肉房 →
//                 工坊 → 炼钢坊 → 军械坊
// The 建筑 page answers the follow-up complaint to the one that created this
// page ("科技树里面没有建筑相关的"): the village's build order was the ONE ladder
// still hidden behind craftUnlocked. Every row's name and cost is read from the
// ONE CRAFT table (game_data.h), so this page can never drift from what the Room
// actually charges — including the count-scaling wood surcharge the 陷阱/小屋
// rows carry (woodIncrPerN), which the equipment rows never do.
//
// Built on the AssignPage sub-page model: an s_active latch gates draw()/
// available() (an un-opened tech tree is a skipped ring slot, exactly like the
// closed AssignPage), it reuses the SHARED tab header (page_tabs, 生火间 lit —
// it is the village's sub-page, not a 4th tab), and returns via a 「返回」band that
// jumps back to "room". It carries no tick(): nothing on it can change while it
// is on screen unless the player presses something, and both bands repaint
// through the pager themselves, so there is nothing for a time axis to do.
//
// Layout (540x960), shared by both pages: tab header(0..72) · sections from
// y=84, each a 24px heading (32px block) + one 30px row per entry (a filled/
// hollow owned marker, the 24px name, and the right-aligned cost) · the band row
// at 836..916, clear of the 928 status bar.
//   page 0: 4 headings + 14 rows -> 84 + 4*(32+12) + 14*30 = 680, then the
//           one-line 前哨站 water footnote at 792 (the fix for the "渴死"
//           complaint) just above the band.
//   page 1: 1 heading + 10 rows -> 84 + 32 + 10*30 = 416, no footnote.
// Both budgets end far above the band, so NEITHER page needs the Room grid's
// 更多 pagination — the split into two pages is by SUBJECT, not by overflow, and
// both entry sets are fixed tables that can never grow at runtime. KEEPS its
// 返回 band (unlike AssignPage, v0.10.3): both ladders already fit with 12px to
// spare above the status bar, so dropping 返回 would buy back a slot nothing here
// needs — and the band row has to exist regardless, since the paging band shares
// it. See assign_page.h for why THAT page's budget was different. Don't "fix"
// this for consistency; there's nothing to fix.
//
// Paging (v0.14): the band row is TWO side-by-side 240px bands in the
// room_page/outside_page two-column geometry — 「建筑」/「装备」 (whichever page
// you are NOT on) on the left, 「返回」 on the right. They share ONE Region: a
// Region is a y-band only, so the horizontal split is resolved from the press x
// against 540/2, exactly as the Room grid picks its column. The page index lives
// in a file-static s_page that open() resets to 0, so entering the tech tree
// always lands on 装备 — and that holds for EVERY exit, including the ones that
// bypass the 返回 band (a tab tap, or the page-turn fallback, both of which reach
// pager.cpp's closeSubPageLatches): they all call close(), and open() is the only
// door back in. A flip repaints the WHOLE page (pager::showPage on the current
// ring index) rather than pushing a sub-rect: every row on screen changes, so
// there is no sub-rect worth computing.
//
// Naming (§8.3 glyph closure): the page is 科技树 — 科 and 技 are absent from
// strings_zh.h (the upstream translation table the closure is normally scanned
// out of), but gen_cjk_font.py now carries an explicit FIRMWARE_LITERAL_CHARS
// registry for firmware-only UI literals that need a glyph outside that table.
// 科技 is its first entry, so both codepoints are baked into cjk_font12.h same
// as every closure glyph — no runtime fallback, no tofu risk. The two paging
// labels needed NO such registration: 建/筑 are already in the closure via
// 建筑物/建造者 (the same route the Outside page's 建筑 legend takes) and 装/备
// via 装备精良, all four verified present in cjk_font12.h.
#pragma once
#include "page.h"

namespace tech_page {
// Open/close the tech-tree page (flip the visibility latch). The caller pairs
// each with a pager::showPage to actually navigate (open -> this page's ring
// index; close -> back to the Outside page). open() also rewinds to page 0
// (装备), so the doorway always opens on the same view.
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
    void onLocalAction(uint8_t param, int x, int y) override;  // flip page / 返回
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;
    // No tick()/wantsAwake: a read-only page with nothing that changes on screen.

private:
    // The band row is the only touch region on the page — one y-band carrying
    // both side-by-side bands, split on x (see the Paging note above).
    mutable pages::Region m_regions[1];
    mutable int           m_regionCount = 0;
};
