#include "frame_store.h"
#include <SD.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

namespace frame_store {

struct Page {
    uint8_t* buf   = nullptr;
    size_t   len   = 0;
    uint32_t etag  = 0;
    bool     valid = false;
};

static Page s_pages[MAX_PAGES];
static int  s_count = 0;
static int  s_cur   = 0;
static bool s_sd    = false;
static char s_curName[24] = {0};

static const char* DIR      = "/.darkroom/pages";
static const char* META     = "/.darkroom/pages/meta.json";
static const char* META_TMP = "/.darkroom/pages/meta.tmp";

// Nibble-table CRC32, zlib polynomial (reflected). Deliberately NOT the ROM
// esp_rom_crc32_le — this is 16 words and provably python-zlib compatible.
static const uint32_t CRC32_T[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};

uint32_t crc32(const uint8_t* data, size_t len) {
    return ~crc32_update(0xffffffff, data, len);
}

// Streaming half of crc32(): fold more bytes into a running state. Seed with
// CRC32_INIT, feed chunks, invert the result (~) to get the final CRC — the
// OTA path uses this because it never buffers the whole image (frame_store.h).
uint32_t crc32_update(uint32_t c, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        c ^= data[i];
        c = (c >> 4) ^ CRC32_T[c & 15];
        c = (c >> 4) ^ CRC32_T[c & 15];
    }
    return c;
}

static uint8_t* pageAlloc(size_t len) {
    uint8_t* b = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!b) b = (uint8_t*)malloc(len);
    return b;
}

static void pagePath(int idx, char* out, size_t cap) {
    snprintf(out, cap, "%s/%d.png", DIR, idx);
}

static bool writeAtomic(const char* path, const char* tmp, const char* bak,
                        const uint8_t* data, size_t len) {
    File f = SD.open(tmp, FILE_WRITE);
    if (!f) return false;
    size_t wr = f.write(data, len);
    f.close();
    if (wr != len) {
        SD.remove(tmp);
        return false;
    }
    const bool hadOld = SD.exists(path);
    if (hadOld) {
        SD.remove(bak);
        if (!SD.rename(path, bak)) {
            SD.remove(tmp);
            return false;
        }
    }
    if (!SD.rename(tmp, path)) {
        SD.remove(tmp);
        if (hadOld) {
            SD.remove(path);
            SD.rename(bak, path);
        }
        return false;
    }
    return true;
}

static bool writeMeta() {
    if (!s_sd) return false;
    char buf[512];
    size_t o = 0;
    int n = snprintf(buf, sizeof(buf),
                     "{\"v\":2,\"count\":%d,\"cur\":%d,\"curName\":\"%s\",\"etags\":[",
                     s_count, s_cur, s_curName);
    if (n < 0 || (size_t)n >= sizeof(buf)) return false;
    o = (size_t)n;
    for (int i = 0; i < s_count; i++) {
        n = snprintf(buf + o, sizeof(buf) - o, "%s\"%08lx\"",
                     i ? "," : "", s_pages[i].valid
                         ? (unsigned long)s_pages[i].etag : 0ul);
        if (n < 0 || (size_t)n >= sizeof(buf) - o) return false;
        o += (size_t)n;
    }
    n = snprintf(buf + o, sizeof(buf) - o, "]}");
    if (n < 0 || (size_t)n >= sizeof(buf) - o) return false;
    o += (size_t)n;
    return writeAtomic(META, META_TMP, "/.darkroom/pages/meta.bak",
                       (const uint8_t*)buf, o);
}

// Only count/cur are trusted from meta; etags are recomputed on preload.
static bool readMeta() {
    File f = SD.open(META, FILE_READ);
    if (!f) return false;
    char buf[512];
    size_t n = f.read((uint8_t*)buf, sizeof(buf) - 1);
    f.close();
    buf[n] = 0;
    int count = 0, cur = 0;
    const char* p = strstr(buf, "\"count\":");
    if (!p || sscanf(p, "\"count\":%d", &count) != 1) return false;
    p = strstr(buf, "\"cur\":");
    if (!p || sscanf(p, "\"cur\":%d", &cur) != 1) return false;
    if (count < 1 || count > MAX_PAGES) return false;
    s_count = count;
    s_cur = (cur >= 0 && cur < count) ? cur : 0;
    // v2: a curName wins. v1 (no curName): synth "srv:<cur>" so a device
    // upgrading from 0.9.x restores the same server page it slept on.
    s_curName[0] = 0;
    p = strstr(buf, "\"curName\":\"");
    if (p) {
        p += 11;                                   // past the key + quote
        size_t i = 0;
        while (*p && *p != '"' && i < sizeof(s_curName) - 1) s_curName[i++] = *p++;
        s_curName[i] = 0;
    }
    if (!s_curName[0])
        snprintf(s_curName, sizeof(s_curName), "srv:%d", s_cur);
    return true;
}

static void preloadPage(int i) {
    char path[24];
    pagePath(i, path, sizeof(path));
    File f = SD.open(path, FILE_READ);
    if (!f) return;
    size_t len = f.size();
    if (len == 0 || len > PAGE_BUF_MAX) { f.close(); return; }
    uint8_t* buf = pageAlloc(len);
    if (!buf) { f.close(); return; }
    if (f.read(buf, len) != len) { free(buf); f.close(); return; }
    f.close();
    s_pages[i].buf = buf;
    s_pages[i].len = len;
    s_pages[i].etag = crc32(buf, len);
    s_pages[i].valid = true;
}

void init(bool sdOk) {
    s_sd = sdOk;
    if (!s_sd) return;
    // One-time cleanup: earlier firmware (since removed) legacy-imported a
    // dashboard 0.5.x /last.png fossil into our page store, and that import
    // persisted to SD. If the fossil is still sitting on the card, our store
    // may carry that contamination — wipe both so the device reboots to a
    // clean empty store. Once /last.png is gone this branch never runs again.
    if (SD.exists("/last.png")) {
        SD.remove("/last.png");
        for (int i = 0; i < MAX_PAGES; i++) {
            char path[24];
            pagePath(i, path, sizeof(path));
            SD.remove(path);
        }
        SD.remove(META);
        SD.remove(META_TMP);
        for (int i = 0; i < MAX_PAGES; i++) {
            if (s_pages[i].buf) free(s_pages[i].buf);
            s_pages[i] = Page{};
        }
        s_count = 0;
        s_cur = 0;
        s_curName[0] = 0;
        Serial.println("[store] wiped legacy /last.png contamination");
    }
    if (!SD.exists(DIR)) SD.mkdir(DIR);
    if (readMeta()) {
        for (int i = 0; i < s_count; i++) preloadPage(i);
        Serial.printf("[store] meta ok: %d page(s), cur=%d\n", s_count, s_cur);
        return;
    }
    // No meta (fresh SD, or just wiped above) — empty store, normal first boot.
}

int pageCount() { return s_count; }

int currentPage() {
    if (s_cur < 0 || s_cur >= s_count) return 0;
    return s_cur;
}

void currentName(char* out, size_t cap) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "%s", s_curName);
}

void setCurrentName(const char* name) {
    if (!name) return;
    if (strcmp(name, s_curName) == 0) return;      // flash-wear guard
    snprintf(s_curName, sizeof(s_curName), "%s", name);
    writeMeta();
}

bool storePage(int idx, int count, const uint8_t* png, size_t len,
               uint32_t hostEtag) {
    if (idx < 0 || count < 1 || count > MAX_PAGES || idx >= count ||
        len == 0 || len > PAGE_BUF_MAX)
        return false;
    // Page-set shrank: drop stale pages beyond the new count.
    if (count != s_count) {
        for (int i = count; i < s_count; i++) {
            if (s_pages[i].buf) free(s_pages[i].buf);
            s_pages[i] = Page{};
            if (s_sd) {
                char path[24];
                pagePath(i, path, sizeof(path));
                SD.remove(path);
            }
        }
        s_count = count;
        if (s_cur >= count) s_cur = 0;
    }
    uint8_t* buf = pageAlloc(len);
    if (!buf) return false;
    memcpy(buf, png, len);
    uint32_t et = crc32(buf, len);
    if (hostEtag && et != hostEtag)
        Serial.printf("[store] WARN page %d crc %08lx != host %08lx\n",
                      idx, (unsigned long)et, (unsigned long)hostEtag);
    if (s_pages[idx].buf) free(s_pages[idx].buf);
    s_pages[idx].buf = buf;
    s_pages[idx].len = len;
    s_pages[idx].etag = et;
    s_pages[idx].valid = true;
    if (s_sd) {
        char tmp[32], bak[32], path[32];
        snprintf(tmp, sizeof(tmp), "%s/%d.tmp", DIR, idx);
        snprintf(bak, sizeof(bak), "%s/%d.bak", DIR, idx);
        pagePath(idx, path, sizeof(path));
        if (!writeAtomic(path, tmp, bak, buf, len)) {
            Serial.printf("[store] page %d save failed\n", idx);
            return false;
        }
        if (!writeMeta()) {
            Serial.println("[store] metadata save failed");
            return false;
        }
    }
    return true;
}

const uint8_t* pagePng(int idx, size_t* len) {
    if (idx < 0 || idx >= s_count || !s_pages[idx].valid) return nullptr;
    if (len) *len = s_pages[idx].len;
    return s_pages[idx].buf;
}

void invalidate(int idx) {
    if (idx < 0 || idx >= s_count || !s_pages[idx].valid) return;
    if (s_pages[idx].buf) free(s_pages[idx].buf);
    s_pages[idx] = Page{};
    writeMeta();
    Serial.printf("[store] page %d invalidated\n", idx);
}

void etagsHex(char* out, size_t cap) {
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < s_count && n + 11 < cap; i++) {
        if (i) out[n++] = ',';
        if (s_pages[i].valid)
            n += snprintf(out + n, cap - n, "%08lx",
                          (unsigned long)s_pages[i].etag);
        else
            out[n++] = '-';
        out[n] = 0;
    }
}

}  // namespace frame_store
