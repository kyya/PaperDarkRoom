// Multi-page frame cache: PSRAM working copies + SD persistence.
//
// SD layout: /pages/0.png .. /pages/<count-1>.png + /pages/meta.json.
// meta.json carries {v, count, cur, curName, etags[]}; count/curName are
// authoritative (cur kept one hop for v1->v2 migration — see readMeta),
// etags are advisory only — each page is re-CRC'd from its PNG bytes on
// preload, so a torn file can never masquerade as a valid page. All SD writes
// go tmp+rename so a power cut can't tear them.
//
// Degradation: init(false) (no SD) → RAM-only; pages received this wake are
// pageable, everything is forgotten across deep sleep (host re-pushes).
#pragma once
#include <Arduino.h>

namespace frame_store {

constexpr int    MAX_PAGES    = 8;
constexpr size_t PAGE_BUF_MAX = 400 * 1024;

// zlib-compatible CRC32 (matches python zlib.crc32) — the page etag.
uint32_t crc32(const uint8_t* data, size_t len);

// Streaming CRC32 for callers that can't buffer the whole input (the OTA
// stream): seed with CRC32_INIT, fold chunks in, invert (~) the result to get
// the same value crc32() would return over the concatenation.
constexpr uint32_t CRC32_INIT = 0xffffffff;
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);

// Load meta + preload all pages into PSRAM. Imports a legacy /last.png
// (0.5.x firmware) as a single-page set when no meta exists.
void init(bool sdOk);

int  pageCount();                 // 0 = nothing cached
int  currentPage();               // clamped server index (STATUS/migration)

// Current-page identity by NAME (survives a host page-count change — a client
// page's name can't be shifted by adding/removing server pages). Persisted to
// meta.json ("curName"). currentName() returns "" when none/blank is stored.
void currentName(char* out, size_t cap);
void setCurrentName(const char* name);

// Store a completed page (PSRAM + SD + meta). count = page_count from the
// CTRL header; a count change drops stale pages beyond the new count.
// The stored etag is recomputed from the bytes (hostEtag only logs mismatch).
bool storePage(int idx, int count, const uint8_t* png, size_t len,
               uint32_t hostEtag);

// PSRAM copy of a page, or nullptr when missing/invalid.
const uint8_t* pagePng(int idx, size_t* len);

// Drop a page that failed to decode (drawPng) → its etag reports "-" and the
// host re-pushes it on the next sync.
void invalidate(int idx);

// Comma list for the STATUS line: "a1b2c3d4,-" ("-" = missing/invalid).
// Empty string when pageCount()==0.
void etagsHex(char* out, size_t cap);

}  // namespace frame_store
