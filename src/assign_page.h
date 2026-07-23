// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Assign (worker-assignment) page — a standalone sub-page split off the Outside
// page in v0.4.0. Assigning villagers to jobs used to be a ▲/▼ stepper grid
// crammed into the middle of the Outside page (v0.3.x); it outgrew the shared
// vertical budget once the inventory box wanted the room, so it became its own
// full-height page reached by the Outside page's 分工 action cell. It reuses the
// SHARED tab header (page_tabs, 村落 tab lit — it is the village's sub-page, not
// a 4th tab) rather than a self-drawn title (v0.4.1), and returns via a 「返回」
// band.
//
// Visibility: unlike the Outside/Trade pages (gated purely on a game flag), this
// page is gated on an explicit s_active latch — draw() returns false (so the
// pager's showPageOrNext skips its ring slot) whenever it is NOT open, exactly
// as an un-unlocked page is skipped. open()/close() flip the latch; the Outside
// 分工 cell opens it + showPage()s to it, its 返回 band closes it + showPage()s
// back to the village. outsideUnlocked also gates it (no assigning before the
// forest exists). Layout (540x960, all text via tr() / closure-safe literals):
//   tab header(0..72) · 人口 X/Y + 伐木者 xN info(80) · one 80px full-width band
//   per UNLOCKED job (120 + n*90, a 36px name + a 24px "xN" count with a
//   right-hand ▲/▼ stepper — assignWorker(job, ±1)) · a 「返回」band. P1 unlocks
//   <=6 assignable jobs (miners are P2), so 6*90 + 80 = 620px from y=120 ends at
//   740 < 928 status bar — one page, no pagination (a P2 job add would need it).
// tick() settles the offline economy and repaints on change while open.
#pragma once
#include "page.h"

namespace assign_page {
// Open/close the assignment page (flip the visibility latch). The caller pairs
// each with a pager::showPage to actually navigate (open -> to this page's ring
// index; close -> back to the Outside page).
void open();
void close();
bool isOpen();
}  // namespace assign_page

class AssignPage : public pages::Page {
public:
    const char* name() const override { return "assign"; }
    bool draw(m5gfx::M5Canvas& canvas) override;   // false unless open + outsideUnlocked
    bool available() const override;               // open + outsideUnlocked (see .cpp)
    const pages::Region* regions(int* n) const override;
    void onLocalAction(uint8_t param, int x, int y) override;  // param=band index; y picks ▲/▼
    pages::Rect pressRect(const pages::Region& rg, int x, int y) const override;  // exact button
    void tick(uint32_t nowMs) override;
    // wantsAwake stays false: the economy accrues offline via settle() on wake.

private:
    // <=6 unlocked P1 jobs + a trailing 返回 band = 7 regions. param is the band
    // index: 0..m_jobCount-1 are job bands (m_jobs maps them to Job ids), and the
    // index == m_jobCount is the 返回 band. The press y picks the ▲/▼ half within
    // a job band; x is unused (full-width bands).
    static constexpr int MAX_JOBS = 6;
    mutable pages::Region m_regions[MAX_JOBS + 1];
    mutable int           m_regionCount = 0;
    mutable uint8_t       m_jobs[MAX_JOBS];     // Job id per visible band
    mutable int           m_jobCount = 0;
};
