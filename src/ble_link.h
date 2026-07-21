// BLE GATT transport (CTRL/DATA/STAT/TIME_CONFIG/TAP_ACK/OTA) — protocol v2.
//
// Generic tap regions: any page can carry a table of (y0,y1) rows that a
// long-press selects; the device only ever does geometry (which region did
// y land in, on which page) and reports that fact back — it has no idea
// what a region MEANS. Resolving (page_idx, region_index) into an actual
// action (open a URL, or whatever a future host wants) is entirely host-side.
// See docs/superpowers/specs/2026-07-13-tap-regions-design.md.
//
// CTRL write:
//   4 bytes  : u32 LE total. v1 single-page semantics (page 0, no etag).
//   10 bytes : u32 LE total | u8 page_idx | u8 page_count | u32 LE etag.
//              page_idx's top bit (0x80) marks the transfer as a REGIONS
//              table (see below) instead of a page PNG; the low 7 bits are
//              still the target page index.
//   total==0 : "sync complete, nothing (more) to push" — the main loop
//              decides whether to sleep (SYNC) or stay awake (INTERACTIVE).
// DATA: chunked write-without-response stream into rxBuf (unchanged from v1).
//   A REGIONS payload is: u8 region_count | region_count * (u16 LE y0, u16 LE
//   y1) — pixel rows a long-press on that page must land inside to select
//   that region (see pager::setRegions). Sent every time a page with regions
//   is part of a session (even if the PNG itself is etag-skipped) — the
//   device is a cold boot on every wake, so anything RAM-only (like the
//   regions table) does not survive from the previous wake even though
//   frame_store's PNGs do.
// STAT: notify. STATUS line gains page=/pages=/etags= (frame_store state),
//   plus mode=int|sync (INTERACTIVE vs SYNC) and, while a long-press is
//   pending host acknowledgement, tap=<seq>:<page_idx>:<region_index>. seq
//   is a monotonic counter (one per detected long-press) — hosts dedupe on
//   it increasing rather than trusting an ack round-trip, so a lost/delayed
//   ack can never cause a double-action.
// TIME_CONFIG write (10 bytes, all u8 — local wall-clock time + quiet-hours
// window; see docs/superpowers/specs/2026-07-13-quiet-hours-design.md):
//   year_since_2000 | month | day | hour | minute | second |
//   quiet_start_hour | quiet_start_min | quiet_end_hour | quiet_end_min
//   quiet_start == quiet_end means "no quiet hours configured". Optional:
//   hosts that don't know about it simply never write this characteristic.
// OTA write (8 bytes, <II total|crc32): a valid header diverts the DATA
// characteristic's chunks from the page buffer into esp_ota_write() until
// total bytes arrive, then CRC-verifies and reboots into the new image
// (see ble_link.cpp's OTA section + docs/.../2026-07-16-ble-ota-design.md).
// TAP_ACK write (4 bytes, u32 LE seq): host has resolved & acted on the
// pending tap with this seq; pager clears it if it still matches (a stale
// ack for an already-superseded seq is a no-op). Optional — a host that
// never acks just leaves the STATUS tap= field set until the next wake
// resets it; correctness never depends on the ack landing (see above).
//
// Invariant: the host waits for the DONE notify before the next CTRL header
// (single receive buffer).
#pragma once
#include <Arduino.h>

namespace ble_link {

struct Rx {
    volatile bool     connected = false;
    volatile uint16_t mtu       = 23;
    volatile bool     secure    = false;
    volatile uint32_t connectMs = 0;
    // current transfer
    volatile uint32_t total     = 0;
    volatile uint32_t off       = 0;
    volatile uint32_t writes    = 0;
    volatile uint32_t t0        = 0;
    volatile uint32_t t1        = 0;
    volatile int      pageIdx   = 0;   // v2 header (v1 → 0)
    volatile int      pageCount = 1;   // v2 header (v1 → 1)
    volatile uint32_t pageEtag  = 0;   // host-claimed etag (v1 → 0)
    volatile bool     isRegions = false;  // page_idx's top bit — see above
    volatile bool     complete  = false;
    volatile bool     overflow  = false;
    volatile bool     syncDone  = false;
    // Host just enabled STAT notifications (CCCD write). The main loop
    // answers with an immediate STATUS instead of making the host wait out
    // the 2s cadence — the first in-loop STATUS after connect can fire
    // BEFORE the host's subscribe lands (dropped, nobody listening), and a
    // host that then times out waiting can misjudge the protocol version
    // (the tray's v1 fallback used to wipe the page set this way).
    volatile bool     statSubPending = false;
    // TIME_CONFIG (set by TimeCfgCb, consumed once by the main loop)
    volatile bool     timeCfgPending = false;
    volatile uint8_t  rtcYearSince2000 = 0;
    volatile uint8_t  rtcMonth  = 1;
    volatile uint8_t  rtcDay    = 1;
    volatile uint8_t  rtcHour   = 0;
    volatile uint8_t  rtcMinute = 0;
    volatile uint8_t  rtcSecond = 0;
    volatile uint16_t quietStartMin = 0;   // hour*60+min
    volatile uint16_t quietEndMin   = 0;   // hour*60+min
};

extern Rx rx;
extern uint8_t* rxBuf;

void init(const char* name);
void statNotify(const char* s);
void sendStatus(const char* fwVersion, bool onUsb, int rot, bool interactive);

// OTA: otaBusy() is true while an image transfer is in flight (the main loop
// must not sleep then); otaPollFinish() is called every loop iteration and, on
// the last byte, commits + reboots (never returns) or reports ota=err.
bool otaBusy();
void otaPollFinish(const char* fwVersion, bool onUsb, int rot, bool interactive);

// OTA progress (read-only) — the same s_otaOff/s_otaTotal counters STATUS's
// ota=recv token already reports. The BLE DATA callback updates them as chunks
// land; the main loop reads these to drive the bar's progress display (it does
// NOT draw from the callback). otaReceived() <= otaTotal(); both 0 until a
// header opens a transfer.
uint32_t otaReceived();
uint32_t otaTotal();

// USB-signal measurement (temporary diag): raw candidate values pushed from
// the main loop, appended to STATUS as "dchg= dsof= dtud=" (dtud =
// mounted*10 + suspended) so a host can read them over BLE when the device
// is unplugged (no serial on battery).
void setUsbDiag(int chg, int sof, int tud);

}  // namespace ble_link
