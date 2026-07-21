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

static const char* DIR      = "/pages";
static const char* META     = "/pages/meta.json";
static const char* META_TMP = "/pages/meta.tmp";
static const char* LEGACY   = "/last.png";   // 0.5.x single-frame persistence

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

static bool writeMeta() {
    if (!s_sd) return false;
    File f = SD.open(META_TMP, FILE_WRITE);
    if (!f) return false;
    f.printf("{\"v\":2,\"count\":%d,\"cur\":%d,\"curName\":\"%s\",\"etags\":[",
             s_count, s_cur, s_curName);
    for (int i = 0; i < s_count; i++) {
        if (s_pages[i].valid)
            f.printf("%s\"%08lx\"", i ? "," : "", (unsigned long)s_pages[i].etag);
        else
            f.printf("%s\"-\"", i ? "," : "");
    }
    f.print("]}");
    f.close();
    SD.remove(META);
    return SD.rename(META_TMP, META);
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
    if (!SD.exists(DIR)) SD.mkdir(DIR);
    if (readMeta()) {
        for (int i = 0; i < s_count; i++) preloadPage(i);
        Serial.printf("[store] meta ok: %d page(s), cur=%d\n", s_count, s_cur);
        return;
    }
    // Legacy import: 0.5.x left /last.png — adopt it as a single-page set so
    // the first boot after reflash isn't blank.
    File f = SD.open(LEGACY, FILE_READ);
    if (!f) return;
    size_t len = f.size();
    if (len == 0 || len > PAGE_BUF_MAX) { f.close(); return; }
    uint8_t* buf = pageAlloc(len);
    if (!buf) { f.close(); return; }
    if (f.read(buf, len) != len) { free(buf); f.close(); return; }
    f.close();
    s_count = 1;
    s_cur = 0;
    s_pages[0].buf = buf;
    s_pages[0].len = len;
    s_pages[0].etag = crc32(buf, len);
    s_pages[0].valid = true;
    char path[24];
    pagePath(0, path, sizeof(path));
    File out = SD.open(path, FILE_WRITE);
    if (out) { out.write(buf, len); out.close(); }
    writeMeta();
    Serial.printf("[store] imported legacy /last.png (%u B)\n", (unsigned)len);
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
        char tmp[24], path[24];
        snprintf(tmp, sizeof(tmp), "%s/%d.tmp", DIR, idx);
        pagePath(idx, path, sizeof(path));
        File f = SD.open(tmp, FILE_WRITE);
        if (f) {
            f.write(buf, len);
            f.close();
            SD.remove(path);
            SD.rename(tmp, path);
        }
        writeMeta();
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
