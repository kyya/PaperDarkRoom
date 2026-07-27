#include "preview.h"
#include "pager.h"
#include "frame_store.h"
#include "page_header.h"
#include "status_bar.h"
#include "tomato_icons.h"       // TOMATO_BIG_* / TOMATO_SM_* (client-page marks)
#include <M5Unified.h>

extern M5Canvas canvas;         // main.cpp owns the full-screen sprite

namespace preview {

static bool s_active = false;
static int  s_focus  = 0;       // ring index shown in the big pane
bool active() { return s_active; }
int  focusIndex() { return s_focus; }

// Focus-style geometry (540x960 panel). Header owns 0..HDR_BOT, the status bar
// owns the bottom BAR_H band; the body between them holds a big FOCUS thumbnail
// on the left and a vertical RAIL of mini thumbnails on the right. The rail
// lists the REACHABLE ring pages, not raw ring slots: a slot earns a cell when
// pager::ringAvailable() is true, the same predicate the status bar's page dots
// count and showPageOrNext steps over. So the ring can hold more pages than
// there are cells (closed sub-pages — Path, Assign, the tech tree — take no
// cell), and every cell maps back to a real ring index.
static const int MARGIN      = 18;                             // host pad
static const int CONTENT_TOP = 124;                            // just below rule (112)
static const int BAR_H       = 32;                             // status_bar band
static const int CONTENT_BOT = 960 - BAR_H;                    // 928
static const int CONTENT_H   = CONTENT_BOT - CONTENT_TOP;      // 804
static const int MAX_CELLS   = 6;                              // defensive clamp only
                                                               // (reachable pages <= 5)

// Big pane: 9:16 at scale 330/540 (~0.611), left column, vcentered in the body.
static const int BIG_X = MARGIN;                               // 18
static const int BIG_W = 330;
static const int BIG_H = 587;                                  // 960 * 330/540
static const int BIG_Y = CONTENT_TOP + (CONTENT_H - BIG_H) / 2;  // 232

// Rail: right column starting one gap past the big pane, mini cells at the
// panel's 9:16 ratio, horizontally centered in the rail column.
static const int RAIL_X = BIG_X + BIG_W + 12;                  // 360
static const int RAIL_W = 540 - RAIL_X - MARGIN;               // 162
static const int MINI_W = 71;
static const int MINI_H = 127;                                 // ~= 71 * 16/9
static const int MINI_X = RAIL_X + (RAIL_W - MINI_W) / 2;      // 405

// The rail's cell -> ring-index map, rebuilt on every render and hit-test
// because reachability moves with game state (a sub-page opens, a building goes
// up) and the view outlives none of it. Fills s_visible in ring order and
// returns the cell count; MAX_CELLS is a defensive clamp, never reached in
// practice (at most 5 pages are reachable at once).
static int s_visible[MAX_CELLS];
static int buildVisible() {
    int n = 0, rc = pager::ringCount();
    for (int r = 0; r < rc && n < MAX_CELLS; r++)
        if (pager::ringAvailable(r)) s_visible[n++] = r;
    return n;
}

// Rail cells are spread evenly down the body: n cells with n+1 equal gaps (a gap
// above the first, between each, and below the last), so the stack always reads
// centered whatever the cell count.
static int railGap(int n) { return n > 0 ? (CONTENT_H - n * MINI_H) / (n + 1) : 0; }
static int cellY(int i, int n) { int g = railGap(n); return CONTENT_TOP + g + i * (MINI_H + g); }

// Draw a server-page PNG scaled to fit rect (rx,ry,rw,rh), centered by explicit
// top-left (datum omitted -> top_left): with maxWidth/maxHeight==0 LGFX's
// centered datums box against the WHOLE canvas, not the scaled image, so we
// center manually against the actual scaled thumb size instead.
static void drawThumb(m5gfx::M5Canvas& c, const uint8_t* png, size_t len,
                      int rx, int ry, int rw, int rh) {
    float sx = (float)rw / 540.0f;
    float sy = (float)rh / 960.0f;
    float s  = sx < sy ? sx : sy;                     // equal ratio
    int tw = (int)(540 * s + 0.5f), th = (int)(960 * s + 0.5f);
    c.drawPng(png, len, rx + (rw - tw) / 2, ry + (rh - th) / 2, 0, 0, 0, 0, s, s);
}

// The big FOCUS pane: server page -> full 9:16 thumbnail (or a central dot when
// the page is missing from a cold cache); client page (pomo) -> white pane +
// centered 96px tomato identity mark. 1px frame either way.
static void drawBig(m5gfx::M5Canvas& c) {
    int nSrv = frame_store::pageCount();
    if (s_focus < nSrv) {
        size_t len = 0;
        const uint8_t* png = frame_store::pagePng(s_focus, &len);
        if (png) drawThumb(c, png, len, BIG_X, BIG_Y, BIG_W, BIG_H);
        else     c.fillCircle(BIG_X + BIG_W / 2, BIG_Y + BIG_H / 2, 4, TFT_BLACK);
    } else {
        c.drawBitmap(BIG_X + (BIG_W - TOMATO_BIG_W) / 2,
                     BIG_Y + (BIG_H - TOMATO_BIG_H) / 2,
                     TOMATO_BIG_FILL_BITS, TOMATO_BIG_W, TOMATO_BIG_H, TFT_BLACK);
    }
    c.drawRect(BIG_X, BIG_Y, BIG_W, BIG_H, TFT_BLACK);
}

// One rail mini cell: `slot` places it in the stack of `n` cells, `ring` is the
// ring index it stands for (the two differ once a hidden page sits between two
// reachable ones). Server page -> micro thumbnail (or a small dot when missing);
// client page -> centered 22px tomato. The FOCUS cell gets a 3px frame, every
// other cell 1px; the cell that is the CURRENT page also gets a 4px solid dot in
// its bottom-right corner (focus != current -> both markers). Both markers
// compare ring indices, so they follow the page, not the slot.
static void drawMini(m5gfx::M5Canvas& c, int slot, int ring, int n, int cur) {
    int x = MINI_X, y = cellY(slot, n);
    int nSrv = frame_store::pageCount();
    if (ring < nSrv) {
        size_t len = 0;
        const uint8_t* png = frame_store::pagePng(ring, &len);
        if (png) drawThumb(c, png, len, x, y, MINI_W, MINI_H);
        else     c.fillCircle(x + MINI_W / 2, y + MINI_H / 2, 3, TFT_BLACK);
    } else {
        c.drawBitmap(x + (MINI_W - TOMATO_SM_W) / 2, y + (MINI_H - TOMATO_SM_H) / 2,
                     TOMATO_SM_FILL_BITS, TOMATO_SM_W, TOMATO_SM_H, TFT_BLACK);
    }
    if (ring == s_focus) for (int k = 0; k < 3; k++)
        c.drawRect(x + k, y + k, MINI_W - 2 * k, MINI_H - 2 * k, TFT_BLACK);
    else c.drawRect(x, y, MINI_W, MINI_H, TFT_BLACK);
    if (ring == cur) c.fillCircle(x + MINI_W - 8, y + MINI_H - 8, 4, TFT_BLACK);
}

// Compose the entire focus view into the shared canvas (no push).
static void render() {
    canvas.fillSprite(TFT_WHITE);
    page_header::draw(canvas);
    status_bar::drawVersionOnto(canvas);        // fw version chrome, header top-right
    int n   = buildVisible();
    int cur = pager::currentRingIndex();
    drawBig(canvas);
    for (int i = 0; i < n; i++) drawMini(canvas, i, s_visible[i], n, cur);
    status_bar::drawOnto(canvas);               // clock/battery/dots bottom band
}

void enter() {
    s_active = true;
    s_focus  = pager::currentRingIndex();
    render();
    // One epd_quality full-panel push: the switcher appearing with a single
    // flash is acceptable and clears accumulated ghosting.
    auto& disp = M5.Display;
    disp.setEpdMode(epd_mode_t::epd_quality);
    canvas.pushSprite(0, 0);
    disp.setEpdMode(epd_mode_t::epd_fast);
    Serial.printf("[preview] enter ring=%d focus=%d\n", pager::ringCount(), s_focus);
}

void setFocus(int idx) {
    // `idx` is a RING index (what hitCell hands back), so accept it only when it
    // still owns a rail cell — an unreachable page has none and can't be focused.
    int n = buildVisible(), ok = 0;
    for (int i = 0; i < n; i++) if (s_visible[i] == idx) { ok = 1; break; }
    if (!ok) return;
    s_focus = idx;
    render();
    // Full-panel epd_fast re-push (not two clipped partial rects): the big pane
    // AND the rail's changed border weights land in ONE consistent update. Two
    // disjoint clip rects would need two pushes with no guaranteed simultaneity;
    // a single ~300ms fast full push is the most robust and flash-free choice.
    auto& disp = M5.Display;
    disp.setEpdMode(epd_mode_t::epd_fast);
    canvas.pushSprite(0, 0);
    Serial.printf("[preview] focus -> %d\n", idx);
}

int hitCell(int x, int y) {
    if (x >= BIG_X && x < BIG_X + BIG_W && y >= BIG_Y && y < BIG_Y + BIG_H)
        return HIT_BIG;
    int n = buildVisible();
    for (int i = 0; i < n; i++) {
        int cy = cellY(i, n);
        if (x >= MINI_X && x < MINI_X + MINI_W && y >= cy && y < cy + MINI_H)
            return s_visible[i];                   // the cell's RING index
    }
    return HIT_NONE;
}

void exit() {
    s_active = false;
    Serial.println("[preview] exit");
}

}  // namespace preview
