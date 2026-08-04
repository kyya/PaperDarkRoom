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
// a 4th tab) rather than a self-drawn title (v0.4.1). It shipped with a trailing
// 「返回」band through v0.10.2; v0.10.3 dropped it (see below) — the tab header
// and the pager's page-turn fallback are this page's only exits now.
//
// Visibility: unlike the Outside/Trade pages (gated purely on a game flag), this
// page is gated on an explicit s_active latch — draw() returns false (so the
// pager's showPageOrNext skips its ring slot) whenever it is NOT open, exactly
// as an un-unlocked page is skipped. open()/close() flip the latch; the Outside
// 分工 cell opens it + showPage()s to it. outsideUnlocked also gates it (no
// assigning before the forest exists). Layout (540x960, all text via tr() /
// closure-safe literals):
//   tab header(0..72) · 人口 X/Y + 伐木者 xN info(80) · one 80px full-width band
//   per UNLOCKED job (120 + n*90, a 36px name + a 24px "xN" count with the shared
//   right-hand two-column stepper — assignWorker(job, ±1) from the fine column
//   and ±10 from the coarse one, truncated to the idle/assigned count). NO
//   trailing 返回 band (v0.10.3 — see below): the job-band list is the whole page.
//
// v0.10.2 fix (user-reported): P1 shipped with <=6 assignable jobs (miners were
// P2), so the original MAX_JOBS=6 comfortably covered every unlocked job AND
// doubled as this page's one-screen render cap. Once P2 added the 3 miner/
// steelworker/armourer jobs (9 assignable jobs total), that single constant fed
// STRAIGHT into the engine query (`unlockedJobs(m_jobs, MAX_JOBS)`), so any job
// past the 6th in enum order (hunter/trapper/tanner/charcutier/iron miner/coal
// miner) was silently dropped from the query itself — no row, no stepper,
// nothing to press for 炼钢工人 (steelworker) et al., even though the Outside
// page's 工人 fieldset (a JOB_COUNT-sized, never-truncated buffer) kept showing
// the true "炼钢工人 x0" cell. The fix split the two roles the old constant
// conflated: the engine query runs into a JOB_COUNT-sized scratch inside
// layoutBands() (game_state.cpp's unlockedJobs() can never return more than
// JOB_COUNT-1 real jobs, so that buffer structurally cannot truncate), and
// MAX_BANDS is ONLY the page's own render capacity — paginated the
// trade_page.cpp's layoutBands way once (if ever) the unlocked list outgrows
// one screen.
//
// v0.10.3 (user pushback on the v0.10.2 pagination): "那个页面没有其他按钮,为什么
// 不能放完整的工人调整?" — fair: this page has NO other content competing for the
// budget the way path/tech's fixed rows do (see below), so a dropped 返回 band
// buys back exactly one more slot, and 8+1=9 covers every real job with zero
// pagination in the case that actually happens today. Verified against upstream
// (outside.js): there never WAS a 返回 button — the worker rows sit directly in
// the Outside page with no sub-page navigation at all, so this page's 返回 band
// was always a port-only addition, not a parity requirement. Dropping it is not
// "removing navigation": the shared tab header (page_tabs) stays clickable on
// EVERY page including this one, and a short tap anywhere this page draws
// nothing (the empty band below the last job, or the 72..120 info-row gutter)
// falls through to the pager's page-turn fallback — both are real, already-
// existing exits, not new ones. pager.cpp's handleTouch was fixed alongside this
// (closeSubPageLatches) so BOTH exits — a tab tap AND a page-turn miss — close
// this page's latch; before that fix only the tab tap did, and the (now-removed)
// 返回 band was covering for the gap. See pager.cpp for the shared fix (it
// applies to path/tech too, which keep their own 返回 bands for a DIFFERENT
// reason: tech's tree already fits its page with room to spare — dropping 返回
// there buys back nothing worth having — and path's budget is spent on its own
// fixed 护甲/水 readout + 出发 band below the item window, so reclaiming 返回's
// 80px there does not unlock "fit everything on one page" the way it does here).
// A future job add that pushes the true total past 9 is exactly what MAX_BANDS'
// pagination is still there for — it just doesn't fire today.
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
    // One page shows at most MAX_BANDS job/"更多" slots — NO trailing 返回 band
    // (v0.10.3, see the file header) — so MAX_BANDS IS the region count. The
    // vertical-budget derivation (assign_page.cpp): BAND_TOP=120, 90px pitch,
    // MAX_BANDS=9 job/更多 slots end at 120 + 8*90 + 80 = 920 < 928; a 10th slot
    // would end at 1010, past the status bar. param is the band index:
    // 0..m_slotCount-1 index m_slotCodes (a Job id, or the A_MORE sentinel).
    // Within a real job band the press x picks the stepper column (±1 / ±10)
    // and the y picks the half (▲ / ▼) — see stepper.h; a 更多 band takes the
    // whole-band press instead (see pressRect). The engine query itself is NOT
    // bounded by MAX_BANDS — layoutBands() (assign_page.cpp) queries the full
    // unlocked-job list into a JOB_COUNT-sized local scratch first and only
    // THEN slices out one page's worth, so a job can never again vanish from
    // the query the way the old MAX_JOBS=6 cap did (see the file header). With
    // today's real max of 9 assignable jobs (JOB_COUNT-1), MAX_BANDS=9 means
    // total<=MAX_BANDS ALWAYS holds — layoutBands' pagination branch is dead
    // code today, kept only as a guard against a future job add pushing the
    // total past 9 (see the file header's v0.10.3 note).
    static constexpr int MAX_BANDS = 9;
    mutable pages::Region m_regions[MAX_BANDS];
    mutable int           m_regionCount = 0;
    mutable uint8_t       m_slotCodes[MAX_BANDS];  // Job id per band, or A_MORE
    mutable int           m_slotCount = 0;
    int                   m_page = 0;       // which batch of unlocked jobs is shown
    uint32_t              m_lastSig = 0;   // tick()'s content baseline; onLocalAction
                                           // re-syncs it after its showPage so an
                                           // assignment doesn't force a second full
                                           // redraw next tick (see assign_page.cpp)
};
