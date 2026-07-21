#include "ble_link.h"
#include "frame_store.h"
#include "pager.h"
#include "status_bar.h"
#include "pomo.h"
#include <M5Unified.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <nvs.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLESecurity.h>
#include <BLE2902.h>
#include <string.h>

// Wipe stored bonds on boot. Default OFF (keep bonds -> silent reconnect).
#ifndef BLE_CLEAR_BONDS
#define BLE_CLEAR_BONDS 0
#endif

#define SVC_UUID  "e3e30001-1111-2222-3333-444455556666"
#define CTRL_UUID "e3e30002-1111-2222-3333-444455556666"
#define DATA_UUID "e3e30003-1111-2222-3333-444455556666"
#define STAT_UUID "e3e30004-1111-2222-3333-444455556666"
#define TIME_UUID "e3e30005-1111-2222-3333-444455556666"
#define TAP_ACK_UUID "e3e30006-1111-2222-3333-444455556666"
#define OTA_UUID  "e3e30007-1111-2222-3333-444455556666"

extern bool g_interactive;  // main.cpp — true only for a button/tap/USB wake,
                            // not a silent background RTC-timer sync (see
                            // SrvCb::onConnect below)
extern bool g_sdOk;         // main.cpp — SD mount result at boot; surfaced in
                            // STATUS's sd= token (a cold boot with sd=0 means
                            // the PSRAM-only cache dies every deep sleep)

namespace ble_link {

Rx rx;
uint8_t* rxBuf = nullptr;

static BLECharacteristic* statChar = nullptr;

void statNotify(const char* s) {
    if (!statChar || !rx.connected) return;
    statChar->setValue((uint8_t*)s, strlen(s));
    statChar->notify();
}

static uint32_t u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---- OTA firmware update -------------------------------------------------
// One extra characteristic (OTA_UUID) carries an 8-byte header <II total|crc32
// (zlib CRC32, frame_store's flavor). After a valid header DATA chunks are
// diverted from the page buffer straight into esp_ota_write() — no whole-image
// buffering. The stream/write happens in DataCb (it must keep up with the
// link); the terminal steps (CRC verify, esp_ota_end, set_boot, NVS rollback
// marker, reboot) run in the main loop via otaPollFinish(), so the ota=ok
// STATUS notify has a moment to flush before esp_restart(). A disconnect
// mid-transfer aborts and returns DATA routing to normal — the half-written
// partition is inert (otadata was never repointed). See
// docs/superpowers/specs/2026-07-16-ble-ota-design.md.
enum OtaState { OTA_NONE, OTA_ACTIVE, OTA_OK, OTA_ERR };
static volatile OtaState      s_ota        = OTA_NONE;
static esp_ota_handle_t       s_otaHandle  = 0;
static const esp_partition_t* s_otaPart    = nullptr;
static volatile uint32_t      s_otaTotal   = 0;
static volatile uint32_t      s_otaOff     = 0;
static uint32_t               s_otaCrcWant = 0;
static uint32_t               s_otaCrcRun  = frame_store::CRC32_INIT;
static volatile bool          s_otaDone    = false;   // last byte in / write err
static volatile bool          s_otaWriteErr = false;
static char                   s_otaErr[24] = {0};

// Stamp the app-level rollback markers (NVS namespace "ota") just before we
// boot into the fresh image: pending=1, boots=0. Early setup() increments
// boots each cold boot and, at BLE-ready, clears them — three straight failed
// boots flip back to the old partition (see main.cpp, and the spec).
static void otaMarkPending() {
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "pending", 1);
    nvs_set_u8(h, "boots", 0);
    nvs_commit(h);
    nvs_close(h);
}

static void otaFail(const char* reason) {
    strncpy(s_otaErr, reason, sizeof(s_otaErr) - 1);
    s_otaErr[sizeof(s_otaErr) - 1] = 0;
    s_ota = OTA_ERR;
    Serial.printf("[ota] err:%s\n", s_otaErr);
}

class CtrlCb : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        std::string v = c->getValue();
        if (v.size() < 4) return;
        const uint8_t* p = (const uint8_t*)v.data();
        uint32_t total = u32le(p);
        if (total == 0) {          // sync complete (v1 hosts use this to skip)
            rx.syncDone = true;
            return;
        }
        rx.total    = total;
        rx.off      = 0;
        rx.writes   = 0;
        rx.complete = false;
        rx.overflow = (total > frame_store::PAGE_BUF_MAX);
        rx.t0       = millis();
        if (v.size() >= 10) {      // v2: total | page_idx | page_count | etag
            rx.isRegions = (p[4] & 0x80) != 0;   // top bit — see ble_link.h
            rx.pageIdx   = p[4] & 0x7F;
            rx.pageCount = p[5];
            rx.pageEtag  = u32le(p + 6);
        } else {                   // v1: bare length = single page 0
            rx.isRegions = false;
            rx.pageIdx   = 0;
            rx.pageCount = 1;
            rx.pageEtag  = 0;
        }
        Serial.printf("[ble] CTRL %s%d/%d total=%lu etag=%08lx%s\n",
                      rx.isRegions ? "regions@" : "page ",
                      rx.pageIdx, rx.pageCount, (unsigned long)total,
                      (unsigned long)rx.pageEtag,
                      rx.overflow ? " (REJECT)" : "");
    }
};

// TIME_CONFIG write (10 bytes, all u8): local wall-clock time + quiet-hours
// window. Only captures bytes into rx.*; the main loop does the actual
// M5.Rtc.setDateTime + NVS work (same division as CtrlCb/DataCb).
class TimeCfgCb : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        std::string v = c->getValue();
        if (v.size() < 10) return;
        const uint8_t* p = (const uint8_t*)v.data();
        rx.rtcYearSince2000 = p[0];
        rx.rtcMonth         = p[1];
        rx.rtcDay           = p[2];
        rx.rtcHour          = p[3];
        rx.rtcMinute        = p[4];
        rx.rtcSecond        = p[5];
        rx.quietStartMin    = (uint16_t)p[6] * 60 + p[7];
        rx.quietEndMin      = (uint16_t)p[8] * 60 + p[9];
        rx.timeCfgPending   = true;
        Serial.printf("[ble] TIME_CONFIG 20%02u-%02u-%02u %02u:%02u:%02u quiet=%02u:%02u-%02u:%02u\n",
                      rx.rtcYearSince2000, rx.rtcMonth, rx.rtcDay,
                      rx.rtcHour, rx.rtcMinute, rx.rtcSecond,
                      p[6], p[7], p[8], p[9]);
    }
};

// TAP_ACK write (4 bytes, u32 LE seq): host has acted on this pending tap —
// clear it if it's still current (a stale ack for an already-superseded seq
// is a no-op, see pager::ackTap). Not on the correctness-critical path: a
// host that never writes this just leaves STATUS's tap= field set until the
// next wake resets it (hosts dedupe on seq increasing, not on this ack).
class TapAckCb : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        std::string v = c->getValue();
        if (v.size() < 4) return;
        pager::ackTap(u32le((const uint8_t*)v.data()));
    }
};

// OTA header write (8 bytes, <II total|crc32): open the passive OTA partition
// and switch DATA into esp_ota_write mode. esp_ota_begin(OTA_SIZE_UNKNOWN)
// erases lazily per-sector inside esp_ota_write (no multi-second up-front
// erase stall in this callback), spreading the cost across the stream.
class OtaCb : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        std::string v = c->getValue();
        if (v.size() < 8) return;
        const uint8_t* p = (const uint8_t*)v.data();
        uint32_t total = u32le(p);
        uint32_t crc   = u32le(p + 4);
        if (s_ota == OTA_ACTIVE) esp_ota_abort(s_otaHandle);  // restart cleanly
        s_otaPart = esp_ota_get_next_update_partition(NULL);
        if (!s_otaPart) { otaFail("nopart"); return; }
        esp_err_t e = esp_ota_begin(s_otaPart, OTA_SIZE_UNKNOWN, &s_otaHandle);
        if (e != ESP_OK) { otaFail("begin"); return; }
        s_otaTotal    = total;
        s_otaOff      = 0;
        s_otaCrcWant  = crc;
        s_otaCrcRun   = frame_store::CRC32_INIT;
        s_otaDone     = false;
        s_otaWriteErr = false;
        s_ota         = OTA_ACTIVE;
        Serial.printf("[ota] begin total=%lu crc=%08lx -> %s\n",
                      (unsigned long)total, (unsigned long)crc,
                      s_otaPart->label);
    }
};

class DataCb : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic* c) override {
        std::string v = c->getValue();
        size_t n = v.size();
        // OTA takes precedence over the page buffer: stream chunks straight
        // into flash, fold the running CRC, and flag the last byte for the
        // main loop to finalize. A write failure ends the transfer too.
        if (s_ota == OTA_ACTIVE) {
            if (n == 0 || s_otaOff >= s_otaTotal) return;
            if (s_otaOff + n > s_otaTotal) n = s_otaTotal - s_otaOff;  // clamp
            if (esp_ota_write(s_otaHandle, v.data(), n) != ESP_OK) {
                s_otaWriteErr = true;
                s_otaDone = true;
                return;
            }
            s_otaCrcRun = frame_store::crc32_update(s_otaCrcRun,
                                                    (const uint8_t*)v.data(), n);
            s_otaOff += n;
            if (s_otaOff >= s_otaTotal) s_otaDone = true;
            return;
        }
        if (n == 0 || rx.overflow || rx.complete) return;
        if (rx.off + n > frame_store::PAGE_BUF_MAX || rx.off + n > rx.total) {
            rx.overflow = true;
            return;
        }
        memcpy(rxBuf + rx.off, v.data(), n);
        rx.off += n;
        rx.writes++;
        if (rx.off >= rx.total) {
            rx.t1 = millis();
            rx.complete = true;
        }
    }
};

// STAT CCCD write: the host enabling notifications is the ground truth for
// "someone is listening now" — flag it so the main loop answers with an
// immediate STATUS (see Rx::statSubPending). Only captures the bit, same
// callbacks-capture/loop-acts division as every other callback here.
class StatCccdCb : public BLEDescriptorCallbacks {
    void onWrite(BLEDescriptor* d) override {
        uint8_t* v = d->getValue();
        if (d->getLength() >= 2 && (v[0] & 0x01)) rx.statSubPending = true;
    }
};

class SrvCb : public BLEServerCallbacks {
    void onConnect(BLEServer*) override {
        rx.connected = true;
        rx.connectMs = millis();
        Serial.println("[ble] connected");
        // Chime only on a human-initiated wake — a silent background SYNC
        // (RTC-timer) connect must stay silent, or the device would beep on
        // every unattended sync all day. tone() hands off to the speaker's
        // own task and returns immediately, so this can't stall the BLE
        // stack (see Speaker_Class.cpp's spk_task).
        if (g_interactive) M5.Speaker.tone(1800, 80);
        status_bar::draw();    // unconditional — a SYNC connect is real too,
                               // and unlike the chime a passive status bar
                               // isn't disruptive during a background sync
    }
    void onDisconnect(BLEServer*) override {
        rx.connected = false;
        rx.mtu = 23;
        // Abort an in-flight OTA: drop the handle, return DATA to normal. The
        // half-written partition is inert (otadata never repointed), and the
        // main loop's sleep path resumes now that otaBusy() is false again.
        if (s_ota == OTA_ACTIVE) {
            esp_ota_abort(s_otaHandle);
            s_ota = OTA_NONE;
            s_otaOff = 0;
            Serial.println("[ota] aborted — disconnect mid-transfer");
        }
        Serial.println("[ble] disconnected — re-advertising");
        BLEDevice::startAdvertising();
        // Redraw the real current page to clear the BLE glyph (no-ops if no
        // page exists yet — showPage draws the bar only on success), then
        // repaint the bar unconditionally so the glyph is erased even on a
        // blank/pageless screen (a page redraw would otherwise wipe the bar).
        pager::showPage(pager::currentRingIndex(), false);
        status_bar::draw();
    }
    void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* p) override {
        rx.mtu = p->mtu.mtu;
        Serial.printf("[ble] mtu=%u\n", rx.mtu);
    }
};

// Just Works (IO_CAP_NONE): only the link-encrypted result matters.
class SecCb : public BLESecurityCallbacks {
    uint32_t onPassKeyRequest() override { return 0; }
    void onPassKeyNotify(uint32_t) override {}
    bool onSecurityRequest() override { return true; }
    bool onConfirmPIN(uint32_t) override { return true; }
    void onAuthenticationComplete(esp_ble_auth_cmpl_t c) override {
        rx.secure = c.success;
        Serial.printf("[ble] auth %s\n", c.success ? "ok (encrypted)" : "FAIL");
    }
};

#if BLE_CLEAR_BONDS
static void clearBonds() {
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) return;
    esp_ble_bond_dev_t* list =
        (esp_ble_bond_dev_t*)malloc(n * sizeof(esp_ble_bond_dev_t));
    if (!list) return;
    esp_ble_get_bond_device_list(&n, list);
    for (int i = 0; i < n; i++) esp_ble_remove_bond_device(list[i].bd_addr);
    free(list);
    Serial.printf("[ble] cleared %d bond(s)\n", n);
}
#endif

void init(const char* name) {
    rxBuf = (uint8_t*)heap_caps_malloc(frame_store::PAGE_BUF_MAX,
                                       MALLOC_CAP_SPIRAM);
    if (!rxBuf) rxBuf = (uint8_t*)malloc(frame_store::PAGE_BUF_MAX);
    Serial.printf("[ble] rx buf %s\n", rxBuf ? "ok" : "FAILED");

    BLEDevice::init(name);
    BLEDevice::setMTU(517);
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
    BLEDevice::setSecurityCallbacks(new SecCb());

    BLEServer* server = BLEDevice::createServer();
    server->setCallbacks(new SrvCb());

    // Handle budget — MUST be explicit. The GATT table needs 1 handle (service
    // decl) + 2 per characteristic + 1 per descriptor. Six chars (CTRL/DATA/
    // STAT/TIME/TAP_ACK/OTA) + STAT's CCCD = 1 + 6*2 + 1 = 14, and create
    // service's DEFAULT numHandles is only 15 — too tight, so this must be set.
    //
    // History / the two red herrings: for a while the OTA characteristic (the
    // 6th) appeared "missing" from the discovered table on the host, and the
    // handle budget looked like the culprit — but that was WRONG on two counts.
    // (1) The real cause of the three "OTA char not found" scares was the
    // WINDOWS GATT CACHE: `pnputil` removing the PnP node does NOT clear the
    // cached GATT database, so a stale table kept hiding e3e30007; a full
    // Settings > Bluetooth > Remove device (GUI) is what actually flushes it,
    // and after that the char enumerated fine. (2) On-panel diag confirmed the
    // char was built correctly all along (handle 53, add_char status 0) even at
    // 32 handles — so 32 was already sufficient. 48 is kept purely as defensive
    // headroom for future characteristics. inst_id=0 (single service instance).
    BLEService* svc = server->createService(BLEUUID(SVC_UUID), 48, 0);

    BLECharacteristic* ctrl = svc->createCharacteristic(
        CTRL_UUID, BLECharacteristic::PROPERTY_WRITE);
    ctrl->setCallbacks(new CtrlCb());
    ctrl->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);

    BLECharacteristic* data = svc->createCharacteristic(
        DATA_UUID,
        BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
    data->setCallbacks(new DataCb());
    data->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);

    statChar = svc->createCharacteristic(
        STAT_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    BLE2902* statCccd = new BLE2902();
    statCccd->setCallbacks(new StatCccdCb());
    statChar->addDescriptor(statCccd);

    BLECharacteristic* timeCfg = svc->createCharacteristic(
        TIME_UUID, BLECharacteristic::PROPERTY_WRITE);
    timeCfg->setCallbacks(new TimeCfgCb());
    timeCfg->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);

    BLECharacteristic* tapAck = svc->createCharacteristic(
        TAP_ACK_UUID, BLECharacteristic::PROPERTY_WRITE);
    tapAck->setCallbacks(new TapAckCb());
    tapAck->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);

    BLECharacteristic* ota = svc->createCharacteristic(
        OTA_UUID, BLECharacteristic::PROPERTY_WRITE);
    ota->setCallbacks(new OtaCb());
    ota->setAccessPermissions(ESP_GATT_PERM_WRITE_ENCRYPTED);

    svc->start();

    BLESecurity* sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    sec->setCapability(ESP_IO_CAP_NONE);
    sec->setKeySize(16);
    sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
#if BLE_CLEAR_BONDS
    clearBonds();
#endif

    BLEAdvertising* adv = BLEDevice::getAdvertising();
    adv->addServiceUUID(SVC_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
    adv->setMaxPreferred(0x12);
    BLEAdvertisementData scanRsp;
    scanRsp.setName(name);
    adv->setScanResponseData(scanRsp);

    BLEDevice::startAdvertising();
    Serial.printf("[ble] advertising as '%s' (encrypted / Just Works)\n", name);
}

static int s_dchg = -1, s_dsof = -1, s_dtud = -1;

void setUsbDiag(int chg, int sof, int tud) {
    s_dchg = chg; s_dsof = sof; s_dtud = tud;
}

void sendStatus(const char* fwVersion, bool onUsb, int rot, bool interactive) {
    char etags[96];
    frame_store::etagsHex(etags, sizeof(etags));
    int bat = (int)M5.Power.getBatteryLevel();
    int mv  = (int)M5.Power.getBatteryVoltage();
    int ma  = (int)M5.Power.getBatteryCurrent();
    char s[320];
    int n = snprintf(s, sizeof(s), "STATUS fw=%s usb=%d rot=%d mtu=%u",
                     fwVersion, onUsb ? 1 : 0, rot, rx.mtu);
    if (bat >= 0 && bat <= 100)
        n += snprintf(s + n, sizeof(s) - n, " bat=%d", bat);
    if (mv > 0) n += snprintf(s + n, sizeof(s) - n, " mv=%d", mv);
    n += snprintf(s + n, sizeof(s) - n, " ma=%d", ma);
    n += snprintf(s + n, sizeof(s) - n, " page=%d pages=%d etags=%s",
                  pager::currentRingIndex(), frame_store::pageCount(),
                  etags[0] ? etags : "-");
    n += snprintf(s + n, sizeof(s) - n, " dchg=%d dsof=%d dtud=%d",
                  s_dchg, s_dsof, s_dtud);
    // Cache-hole diagnostics: sd= (SD mount at boot — sd=0 => PSRAM-only cache
    // dies every deep sleep, the prime suspect for frequent holes) and skips=
    // (page-skip events this wake, showPageOrNext stepping over missing pages).
    n += snprintf(s + n, sizeof(s) - n, " sd=%d skips=%u",
                  g_sdOk ? 1 : 0, (unsigned)pager::skipCount());
    n += snprintf(s + n, sizeof(s) - n, " mode=%s", interactive ? "int" : "sync");
    if (pomo::active())
        n += snprintf(s + n, sizeof(s) - n, " pomo=%s:%d",
                      pomo::inBreak() ? "break" : "work",
                      pomo::remainingMinutes());
    if (pager::hasPendingTap())
        n += snprintf(s + n, sizeof(s) - n, " tap=%lu:%d:%d",
                      (unsigned long)pager::pendingTapSeq(),
                      pager::pendingTapPage(), pager::pendingTapItem());
    // OTA progress/result token — omitted entirely when no OTA has run this
    // wake, so back-compat hosts see the exact old line (per-token parsing).
    if (s_ota == OTA_ACTIVE)
        n += snprintf(s + n, sizeof(s) - n, " ota=recv:%lu",
                      (unsigned long)s_otaOff);
    else if (s_ota == OTA_OK)
        n += snprintf(s + n, sizeof(s) - n, " ota=ok");
    else if (s_ota == OTA_ERR)
        n += snprintf(s + n, sizeof(s) - n, " ota=err:%s", s_otaErr);
    statNotify(s);
    Serial.printf("[ble] -> %s\n", s);
}

bool otaBusy() { return s_ota == OTA_ACTIVE; }

// Read-only progress accessors for the bar's OTA display (see ble_link.h).
uint32_t otaReceived() { return s_otaOff; }
uint32_t otaTotal()    { return s_otaTotal; }

// Main-loop hook: finish an OTA once its last byte has landed (s_otaDone).
// On success this commits the new image and reboots (never returns); on any
// failure it reports ota=err over STATUS and returns DATA routing to normal.
void otaPollFinish(const char* fwVersion, bool onUsb, int rot, bool interactive) {
    if (!s_otaDone) return;
    s_otaDone = false;
    if (s_ota != OTA_ACTIVE) return;

    if (s_otaWriteErr) {
        esp_ota_abort(s_otaHandle);
        otaFail("write");
        sendStatus(fwVersion, onUsb, rot, interactive);
        return;
    }
    uint32_t got = ~s_otaCrcRun;
    if (got != s_otaCrcWant) {
        Serial.printf("[ota] crc %08lx != want %08lx\n",
                      (unsigned long)got, (unsigned long)s_otaCrcWant);
        esp_ota_abort(s_otaHandle);
        otaFail("crc");
        sendStatus(fwVersion, onUsb, rot, interactive);
        return;
    }
    esp_err_t e = esp_ota_end(s_otaHandle);   // consumes the handle (validates)
    if (e != ESP_OK) {
        otaFail("verify");
        sendStatus(fwVersion, onUsb, rot, interactive);
        return;
    }
    if (esp_ota_set_boot_partition(s_otaPart) != ESP_OK) {
        otaFail("setboot");
        sendStatus(fwVersion, onUsb, rot, interactive);
        return;
    }
    otaMarkPending();
    s_ota = OTA_OK;
    sendStatus(fwVersion, onUsb, rot, interactive);   // emits ota=ok
    Serial.println("[ota] committed — rebooting into new image");
    delay(400);                                        // let the notify flush
    Serial.flush();
    delay(50);
    esp_restart();
}

}  // namespace ble_link
