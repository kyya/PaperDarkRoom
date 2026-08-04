#include "touch_gt911.h"

#include <Arduino.h>
#include <driver/i2c.h>

// The GT911 half of the M5.begin() teardown; touch_gt911.h carries the why, the
// wiring and the coordinate-frame proof. The bus bring-up, the alternating
// 0x14/0x5D probe, the 0x814F record layout and the five-point config unlock
// below are PaperBar's src/paper_s3_msg/touch_gt911.cpp — same board, same two
// pins, same chip, and every one of those four was paid for with a device-side
// bug there rather than derived from a datasheet.
//
// What is NOT carried over is PaperBar's gesture layer. It turns a press/release
// pair into a tap or a swipe because its pager is swipe-driven; pager.cpp here
// does its own hit-testing, its own edge band and its own tap debounce, and
// wants the press/release edges raw. In its place is the per-contact tracker at
// the bottom of this file, shaped to the three questions pager.cpp used to ask
// M5.Touch: how many contacts, which one just crossed the hold threshold, and
// which one just released as a tap.

namespace touch {

// ---- wiring / bus ----------------------------------------------------------
static const i2c_port_t I2C_PORT = I2C_NUM_1;
static const int SDA_PIN = 41;
static const int SCL_PIN = 42;
static const int INT_PIN = 48;
static const uint32_t I2C_HZ = 400000;
// The panel board has hardware pull-ups on both lines; the internal ones are too
// weak to be useful at 400 kHz and are left off.
static const TickType_t I2C_TO = pdMS_TO_TICKS(20);

// Both addresses the GT911 can latch at power-up. RST is not wired to the MCU,
// so which one this unit answers on is not ours to choose — probe alternately.
static const uint8_t ADDRS[2] = {0x14, 0x5D};
static const int PROBE_ROUNDS = 6;

// ---- registers -------------------------------------------------------------
static const uint16_t REG_STATUS  = 0x814E;  // bit7 = buffer ready, bits0-3 = points
// FIRST POINT RECORD STARTS AT 0x814F, NOT 0x8150.
//
// This was PaperBar's "tapping does nothing" bug. Reading from 0x8150 put x_lo
// at byte 0, so every field landed one byte late: the parsed id was really x_lo,
// the parsed x was really (x_hi, y_lo) and so inflated ~256x, and size was
// always 0. An x of 24833 fails every sane bounds test, so 100% of taps were
// rejected — silently.
//
// M5GFX agrees and is the proof this is the layout the M5PaperS3 digitizer
// actually uses: Touch_GT911::_update_data() reads the status byte from 0x814E
// and the rest from 0x814F, then takes the track id as the byte at 0x814F. The
// stride is still 8, so point N's id is at 0x814F + N*8. (Datasheets commonly
// number the first record 0x8150; this part does not.)
static const uint16_t REG_POINT0   = 0x814F;  // 8 B/point: id,x_lo,x_hi,y_lo,
                                              // y_hi,sz_lo,sz_hi,rsvd
static const uint16_t REG_PROD_ID  = 0x8140;  // ASCII "911" + NUL
static const uint16_t REG_CFG_BASE = 0x8047;  // config block, 184 B
static const uint16_t REG_CFG_NUMS = 0x804C;  // max reported contacts
static const uint16_t REG_CFG_XMAX = 0x8048;  // X max (LE), then Y, nums, sw1
static const size_t   CFG_BYTES    = 184;
static const uint8_t  TOUCH_NUMS   = 5;
static const int      POINT_BYTES  = 8;

// The digitizer's own frame, and (see touch_gt911.h) the UI's frame too — the
// config resolution is checked against these at init because a corrupt config
// block does not read as "no touch", it reads as coordinates that are
// systematically wrong, which every filter downstream then swallows in silence.
static const uint16_t TOUCH_W = 540;
static const uint16_t TOUCH_H = 960;

// ---- press-tracker thresholds ----------------------------------------------
// M5Unified's own long-press default (Touch_Class.hpp:117, _msecHold = 500) —
// the number pager.cpp's modal-bounce and grip guards were timed against.
static const uint32_t HOLD_MS = 500;
// Travel that disqualifies a contact from being either a tap or a hold. This is
// PaperBar's TAP_MOVE_MAX, not M5Unified's _flickThresh (which is 8 px): 8 px of
// slop on this glass reclassifies a firm tap as a drag. Being LOOSER than the
// old behaviour can only ever turn a shaky tap into a click, never lose one that
// used to register, so no tap pager.cpp already handles can go missing.

// TWO DELIBERATE DIVERGENCES FROM M5Unified, both in the "more permissive"
// direction, both UNVERIFIED ON HARDWARE — the pager's touch policy was tuned
// against M5.Touch's behaviour, so these are the first things to re-check on the
// device if taps feel different:
//
//   1. MOVE_MAX_PX is 30 where M5Unified's flick threshold is 8. A 10-30px
//      wobble that M5Unified classified as a flick (and the pager ignored) now
//      survives as a click, so it turns a page or fires an action.
//   2. baseMsec is the PRESS time here. M5Unified overwrites base_msec with the
//      RELEASE time when a contact ends, so two contacts released in the same
//      frame compared equal and the pager's grip-graze tiebreak (`>`) degenerated
//      to "take the first slot". Keeping the press time makes that tiebreak work
//      as its comment always claimed — arguably a bug fix, but it means the
//      behaviour proved on the device was the degenerate one.
static const int MOVE_MAX_PX = 30;

// ---- state -----------------------------------------------------------------
//
// One entry per contact the chip is currently reporting, keyed on the track_id
// it assigns — NOT on the slot index, which the chip shuffles as contacts come
// and go. Following the id is what makes a release detectable at all on a
// handheld card: the point COUNT never reaches zero while a thumb rests on the
// bezel, so "no fingers on the panel" is not an event that happens.
struct Contact {
    uint8_t  id;
    int      x, y;             // where it is now
    int      x0, y0;           // where it landed — the move test's origin
    uint32_t baseMsec;         // millis() at press
    bool     holdFired;        // the hold edge has already been delivered
    bool     moved;            // travelled past MOVE_MAX_PX at some point
    bool     held;             // crossed the hold threshold THIS pass
    bool     clicked;          // released THIS pass, having stayed a tap
};

static uint8_t s_addr = 0;     // 0 = no chip found, driver inert

static Contact s_live[MAX_POINTS];
static int     s_liveN = 0;
// Contacts that vanished from the most recent report. They are served alongside
// the live ones for exactly one pass so their release is observable, then
// dropped — see the retirement note at the top of update().
static Contact s_released[MAX_POINTS];
static int     s_releasedN = 0;

static esp_err_t regRead(uint16_t reg, uint8_t* out, size_t n) {
    // The GT911 requires the address write and the data read in ONE transaction
    // (repeated START, no STOP in between) — hence write_read_device rather than
    // a write followed by a separate read.
    const uint8_t a[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_write_read_device(I2C_PORT, s_addr, a, 2, out, n, I2C_TO);
}

static esp_err_t statusClear(uint8_t addr) {
    const uint8_t w[3] = {(uint8_t)(REG_STATUS >> 8), (uint8_t)(REG_STATUS & 0xFF), 0x00};
    return i2c_master_write_to_device(I2C_PORT, addr, w, 3, I2C_TO);
}

// SHARED BUS BRING-UP, DELIBERATELY DUPLICATED IN rtc_bm8563.cpp — the full
// story of why the install result is ignored rather than tested (IDF 4.4 reports
// a second install as a plain ESP_FAIL, indistinguishable from a broken one, and
// PaperBar shipped a release where that mistake silently killed the RTC) is in
// that file's i2cBringUp(). The short version: whichever module initialises
// first installs the driver, the second one's install fails harmlessly, and the
// probe below is the only thing that actually knows whether the bus works.
static void i2cBringUp() {
    i2c_config_t cfg = {};
    cfg.mode             = I2C_MODE_MASTER;
    cfg.sda_io_num       = SDA_PIN;
    cfg.scl_io_num       = SCL_PIN;
    cfg.sda_pullup_en    = GPIO_PULLUP_DISABLE;
    cfg.scl_pullup_en    = GPIO_PULLUP_DISABLE;
    cfg.master.clk_speed = I2C_HZ;
    i2c_param_config(I2C_PORT, &cfg);
    i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
}

// Raise the chip's reported-contact limit from the factory 2 to 5.
//
// WHY THIS IS NOT OPTIONAL: the limit is on how many contacts the chip REPORTS
// AT ALL, not on how many the firmware reads. A hand holding the card puts two
// contacts down, and at nums=2 those two fill the report — a deliberate tap then
// never appears in it, and no amount of slot scanning in pager.cpp can find a
// contact the chip was never asked to publish. That is the failure the old
// firmware fixed with M5Unified's Touch_GT911::setTouchNums(5), and it goes with
// M5.begin(); this is the same read-modify-write done by hand.
//
// The config block is VOLATILE, so this runs on every cold boot — which, on this
// card, is every boot (power_s3.h: every wake is a cold boot).
//
// Sequence is M5GFX's setTouchNums/_freshConfig: read 0x804C, and if it is not
// already 5, write it, then read the whole 184-byte block, recompute the
// checksum ((~sum)+1 over 0x8047..0x80FE) into 0x80FF, set the "config fresh"
// flag at 0x8100 and write all of it back as ONE transaction. A wrong checksum
// makes the chip reject the block and silently keep the old config.
static bool setTouchNums() {
    uint8_t nums = 0;
    if (regRead(REG_CFG_NUMS, &nums, 1) != ESP_OK) return false;
    if (nums == TOUCH_NUMS) return true;          // already unlocked

    uint8_t w[3] = {(uint8_t)(REG_CFG_NUMS >> 8), (uint8_t)(REG_CFG_NUMS & 0xFF),
                    TOUCH_NUMS};
    if (i2c_master_write_to_device(I2C_PORT, s_addr, w, 3, I2C_TO) != ESP_OK)
        return false;

    // 2 address bytes + the config block + checksum + fresh flag, contiguous:
    // 0x8047..0x8100 is 186 registers, so the buffer is 188 B.
    uint8_t cfg[2 + CFG_BYTES + 2];
    cfg[0] = (uint8_t)(REG_CFG_BASE >> 8);
    cfg[1] = (uint8_t)(REG_CFG_BASE & 0xFF);
    if (regRead(REG_CFG_BASE, &cfg[2], CFG_BYTES) != ESP_OK) return false;

    uint8_t sum = 0;
    for (size_t i = 0; i < CFG_BYTES; i++) sum += cfg[2 + i];
    cfg[2 + CFG_BYTES]     = (uint8_t)((~sum) + 1);   // 0x80FF checksum
    cfg[2 + CFG_BYTES + 1] = 0x01;                    // 0x8100 config fresh
    if (i2c_master_write_to_device(I2C_PORT, s_addr, cfg, sizeof(cfg),
                                   I2C_TO) != ESP_OK)
        return false;
    delay(10);                                        // let it latch the block

    // Trust nothing: read it back. A rejected block leaves the old value.
    if (regRead(REG_CFG_NUMS, &nums, 1) != ESP_OK) return false;
    return nums == TOUCH_NUMS;
}

bool init() {
    // 1. Wake pulse on INT. This board is reached over OTA as often as over USB
    //    (a soft reset), so the GT911 keeps whatever state the previous firmware
    //    left it in — including a 0x8040=0x05 software sleep, which no amount of
    //    polling recovers from because RST is not wired to the MCU. Driving INT
    //    low then high is the one lever left.
    pinMode(INT_PIN, OUTPUT);
    digitalWrite(INT_PIN, LOW);
    delay(58);
    digitalWrite(INT_PIN, HIGH);
    delay(2);
    // Back to an input immediately: INT is also the address-select strap, and a
    // pin left driven low would change which address the chip latches at the
    // next board power-up.
    pinMode(INT_PIN, INPUT_PULLUP);

    // 2. Bus.
    i2cBringUp();

    // 3. Probe. The write doubles as the probe and as a status clear, so a chip
    //    that came up mid-report starts from a clean slate.
    for (int r = 0; r < PROBE_ROUNDS && !s_addr; r++) {
        uint8_t a = ADDRS[r & 1];
        if (statusClear(a) == ESP_OK) s_addr = a;
        else delay(1);
    }
    if (!s_addr) {
        Serial.println("[touch] no GT911 on 0x14/0x5D — touch disabled");
        return false;
    }

    // 4. Product id, purely to prove on the log that we are talking to a GT911
    //    and not to whatever else happened to ACK.
    char id[5] = {0};
    if (regRead(REG_PROD_ID, (uint8_t*)id, 4) != ESP_OK) id[0] = 0;

    // 5. Unlock all five contacts. Not fatal if it fails — two-contact mode
    //    still works for a user who is not gripping the card — so warn and carry
    //    on rather than disabling touch over it.
    bool five = setTouchNums();
    if (!five)
        Serial.println("[touch] WARN could not unlock 5 contacts — staying at "
                       "the factory limit (a gripping hand may mask taps)");

    // 6. Config health check. setTouchNums() rewrites the whole 184-byte block to
    //    fix up the checksum, so a bug there could corrupt the RESOLUTION or the
    //    X2Y swap — and the symptom of that is not "no touch" but coordinates
    //    that are systematically wrong, which pager.cpp's edge band then swallows
    //    silently. nums=5 reading back correctly does not rule it out, so read
    //    the fields that would actually hurt and say what they are.
    //    0x8048..0x804D = xmax(2) ymax(2) nums(1) switch1(1).
    uint8_t creg[6] = {0};
    if (regRead(REG_CFG_XMAX, creg, sizeof(creg)) == ESP_OK) {
        uint16_t xmax = (uint16_t)creg[0] | ((uint16_t)creg[1] << 8);
        uint16_t ymax = (uint16_t)creg[2] | ((uint16_t)creg[3] << 8);
        Serial.printf("[touch] cfg res=%ux%u sw=0x%02x (X2Y=%d) nums=%u\n",
                      xmax, ymax, creg[5], (creg[5] >> 3) & 1, creg[4]);
        if (xmax != TOUCH_W || ymax != TOUCH_H)
            Serial.printf("[touch] WARN config resolution is %ux%u, expected "
                          "%ux%u — the config block is corrupt, coordinates "
                          "will be wrong\n", xmax, ymax, TOUCH_W, TOUCH_H);
    } else {
        Serial.println("[touch] WARN could not read back the config block");
    }

    Serial.printf("[touch] GT911 @0x%02X id=\"%s\" points=%d\n",
                  s_addr, id, five ? TOUCH_NUMS : 2);
    return true;
}

static int indexOfId(const Contact* set, int n, uint8_t id) {
    for (int i = 0; i < n; i++) if (set[i].id == id) return i;
    return -1;
}

void update(uint32_t nowMs) {
    if (!s_addr) return;

    // RETIRE THE PREVIOUS PASS'S EDGES FIRST, before the status read that may
    // bail out below. `clicked` and `held` are edges, not states: the header
    // promises a release is served for exactly ONE pass, and pager.cpp acts on
    // every clicked it sees. The GT911 only raises the buffer-ready flag when it
    // has a fresh report, so most passes return early a few lines down — and a
    // just-released contact that survived that early return would be re-served
    // on every poll until the next touch, turning one tap into a page turn per
    // loop iteration. Retiring here is what makes "one pass" mean "one update()
    // call", which is the unit the caller counts in. Note that nothing is
    // SYNTHESISED: no contact is invented and none is released here, only
    // already-delivered edges are cleared.
    s_releasedN = 0;
    for (int i = 0; i < s_liveN; i++) s_live[i].held = false;

    uint8_t status = 0;
    if (regRead(REG_STATUS, &status, 1) != ESP_OK) return;
    // Buffer not ready: the chip is mid-fill, or simply has nothing new. Return
    // WITHOUT clearing — the clear byte would throw away the frame it is writing
    // right now — and without touching the live set, because "no fresh report"
    // is not "every finger lifted". Inventing releases here is how a driver
    // fabricates taps out of a quiet bus.
    if (!(status & 0x80)) return;

    int n = status & 0x0F;
    if (n > MAX_POINTS) n = MAX_POINTS;

    uint8_t pts[MAX_POINTS * POINT_BYTES];
    bool got = (n == 0);
    if (n > 0)
        got = (regRead(REG_POINT0, pts, (size_t)n * POINT_BYTES) == ESP_OK);

    // Clearing is mandatory and unconditional from here on: leave the flag set
    // and the chip never publishes a new frame, so every later poll re-reads
    // this same coordinate set — an infinite stream of identical contacts that
    // never release.
    statusClear(s_addr);
    if (!got) return;

    // Rebuild the live set from the report, carrying each surviving contact's
    // history (where it landed, when, whether it has already fired a hold)
    // across on its track_id.
    Contact next[MAX_POINTS];
    int nextN = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t* p = pts + i * POINT_BYTES;
        uint8_t id = p[0];
        int x = (int)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        int y = (int)((uint16_t)p[3] | ((uint16_t)p[4] << 8));

        Contact c;
        int prev = indexOfId(s_live, s_liveN, id);
        if (prev < 0) {
            c.id        = id;
            c.x0        = x;
            c.y0        = y;
            c.baseMsec  = nowMs;
            c.holdFired = false;
            c.moved     = false;
            // Press, logged once. Together with the release line below this is
            // the whole raw trace of the digitizer over the cable — enough to
            // tell "the user never touched it" from "the chip saw it and
            // pager.cpp dropped it", which is the distinction that made
            // PaperBar's tap bugs diagnosable at all.
            Serial.printf("[touch] raw n=%d id=%d x=%d y=%d down\n", n, (int)id, x, y);
        } else {
            c = s_live[prev];
            // Per-axis travel, PaperBar's test. The origin is where the contact
            // LANDED, not where it was last seen, so a slow drift accumulates
            // instead of resetting itself one small step at a time.
            if (abs(x - c.x0) > MOVE_MAX_PX || abs(y - c.y0) > MOVE_MAX_PX)
                c.moved = true;
        }
        c.x       = x;
        c.y       = y;
        c.held    = false;
        c.clicked = false;

        // The hold edge: down long enough, never moved, not already fired. Same
        // three conditions M5Unified's Touch_Class applies before it promotes a
        // contact to hold_begin (Touch_Class.cpp:69-83), and like that one it is
        // an edge — true for this pass only.
        if (!c.moved && !c.holdFired &&
            (int32_t)(nowMs - c.baseMsec) >= (int32_t)HOLD_MS) {
            c.holdFired = true;
            c.held      = true;
        }
        next[nextN++] = c;
    }

    // Anything live last pass and absent from this report has RELEASED. It is a
    // click when it never became a hold and never travelled — precisely
    // M5Unified's wasClicked(), which is `state == touch_end` and therefore
    // reachable only from a contact that left the plain `touch` state (a hold or
    // a flick exits to hold_end / flick_end instead).
    for (int i = 0; i < s_liveN; i++) {
        if (indexOfId(next, nextN, s_live[i].id) >= 0) continue;
        Contact r = s_live[i];
        r.held    = false;
        r.clicked = (!r.holdFired && !r.moved);
        s_released[s_releasedN++] = r;
        Serial.printf("[touch] raw n=%d id=%d x=%d y=%d up dur=%lums clicked=%d\n",
                      n, (int)r.id, r.x, r.y,
                      (unsigned long)(nowMs - r.baseMsec), r.clicked ? 1 : 0);
    }

    for (int i = 0; i < nextN; i++) s_live[i] = next[i];
    s_liveN = nextN;
}

int count() { return s_liveN + s_releasedN; }

// Live contacts first, then the ones that released during this pass. The order
// is stable within a pass, which is what lets pager.cpp's grip-graze tiebreak
// compare two slots' baseMsec against each other meaningfully.
Detail detail(int i) {
    Detail d;
    const Contact* c = nullptr;
    if (i >= 0 && i < s_liveN)                          c = &s_live[i];
    else if (i >= s_liveN && i < s_liveN + s_releasedN) c = &s_released[i - s_liveN];
    if (!c) return d;                                   // out of range: all false

    d.x        = c->x;
    d.y        = c->y;
    d.clicked  = c->clicked;
    d.held     = c->held;
    // Live slots only. The released tail is served for exactly one more pass so
    // its edge is observable, and for THIS bit that pass is precisely when the
    // finger is no longer down.
    d.down     = (i < s_liveN);
    d.baseMsec = c->baseMsec;
    return d;
}

}  // namespace touch
