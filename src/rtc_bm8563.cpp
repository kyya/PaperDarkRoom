#include "rtc_bm8563.h"

#include <Arduino.h>
#include <driver/i2c.h>
#include <string.h>

// The BM8563 half of the M5.begin() teardown; rtc_bm8563.h carries the why. Every
// register access below is M5Unified's PCF8563_Class re-expressed on the legacy
// IDF 4.4 i2c driver — same registers, same masks, same rounding, same write
// order. Where this file differs from that class it is only in HOW the bytes
// reach the wire (i2c_master_write_read_device instead of M5's I2C_Class),
// never in WHICH bytes they are or in what order they go out. Read the header's
// note on why that equivalence is load-bearing before changing anything here.

namespace rtc {

// ---- wiring / bus ----------------------------------------------------------
static const i2c_port_t I2C_PORT = I2C_NUM_1;
static const int        SDA_PIN  = 41;
static const int        SCL_PIN  = 42;
static const uint32_t   I2C_HZ   = 400000;
static const uint8_t    ADDR     = 0x51;
// The panel board carries hardware pull-ups on both lines; the ESP32's internal
// ones are far too weak to hold 400 kHz edges and are left disabled, which is
// what PaperBar does on this same pair of pins.
static const TickType_t I2C_TO   = pdMS_TO_TICKS(50);

static bool s_present = false;

// SHARED BUS BRING-UP, DELIBERATELY DUPLICATED IN touch_gt911.cpp.
//
// The GT911 (0x14/0x5D) and this chip (0x51) sit on the same I2C_NUM_1, and
// whichever module's init() runs first is the one that installs the driver. The
// second install is therefore the NORMAL case, not a failure — and its return
// code cannot tell you which case you are in: IDF 4.4's driver/i2c.h documents
// exactly three returns for i2c_driver_install (ESP_OK / ESP_ERR_INVALID_ARG /
// ESP_FAIL, i2c.h:105-108) and ESP_ERR_INVALID_STATE is not among them, so a
// port somebody else already owns comes back as a plain ESP_FAIL that reads
// identically to a genuinely broken install.
//
// PaperBar shipped that bug: bar_sources.cpp gated itself on the install result,
// always initialised second, and so reported rtc=noi2c forever while touch
// worked perfectly on the same two wires (its 0.10.3 post-mortem). The fix there
// and here is to latch NOTHING from the install — the probe below is the only
// thing that actually knows whether the bus carries a transaction, and a bus
// that appears later (the other module installs it, a retry succeeds) is picked
// up by the very next read with no state to reset.
//
// This is ~15 duplicated lines rather than a new shared header on purpose: two
// modules that each bring up the bus they use, independently and idempotently,
// have no initialisation order to get wrong between them.
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

// ---- register primitives ---------------------------------------------------
//
// The PCF8563 auto-increments its register pointer, so a multi-byte read is one
// address write plus one read in a single transaction (repeated START), and a
// multi-byte write is the address byte followed by the payload.

static bool readRegs(uint8_t reg, uint8_t* out, size_t n) {
    return i2c_master_write_read_device(I2C_PORT, ADDR, &reg, 1, out, n,
                                        I2C_TO) == ESP_OK;
}

static bool writeRegs(uint8_t reg, const uint8_t* in, size_t n) {
    uint8_t w[8];                      // the longest write here is 7 payload B
    if (n > sizeof(w) - 1) return false;
    w[0] = reg;
    memcpy(&w[1], in, n);
    return i2c_master_write_to_device(I2C_PORT, ADDR, w, n + 1, I2C_TO) == ESP_OK;
}

static bool writeReg8(uint8_t reg, uint8_t value) {
    return writeRegs(reg, &value, 1);
}

// Zero on a failed read, exactly as M5Unified's readRegister8 does. Every caller
// below is a bit test whose "chip did not answer" answer is the same as its "bit
// is clear" answer, so the failure needs no separate path.
static uint8_t readReg8(uint8_t reg) {
    uint8_t v = 0;
    return readRegs(reg, &v, 1) ? v : 0;
}

// Read-modify-write, short-circuiting on the read like M5Unified's I2C_Class::
// bitOff. The short circuit matters: without it a failed read would write
// (0 & ~mask) == 0 to a control register, i.e. silently disarm the chip on a
// transient bus error.
static bool bitOff(uint8_t reg, uint8_t mask) {
    uint8_t v = 0;
    return readRegs(reg, &v, 1) && writeReg8(reg, (uint8_t)(v & ~mask));
}

static uint8_t bcd2ToByte(uint8_t value) {
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static uint8_t byteToBcd2(uint8_t value) {
    uint8_t bcdhigh = (uint8_t)(value / 10);
    return (uint8_t)((bcdhigh << 4) | (value - (bcdhigh * 10)));
}

// PCF8563_Class::begin(), including its blind first write: M5Unified's own
// comment says the TimerCam's on-board RTC sometimes fails to initialise
// otherwise, so register 0x00 (control 1) is written twice and only the SECOND
// write counts towards the verdict. 0x0E = 0x03 is the timer control register
// with the timer disabled and its clock source parked at 1/60 Hz.
//
// Those three writes ARE the probe — nothing else on this bus lives at 0x51, and
// a chip that ACKs an address write is a chip that will answer a read.
bool init() {
    i2cBringUp();
    writeReg8(0x00, 0x00);
    s_present = writeReg8(0x00, 0x00) && writeReg8(0x0E, 0x03);
    if (s_present) Serial.println("[rtc] ok");
    else           Serial.println("[rtc] no chip at 0x51 — clock unavailable");
    return s_present;
}

bool present() { return s_present; }

// Registers 0x02..0x08 are seconds, minutes, hours, day, weekday, month, year,
// and the read starts at whichever of them the caller actually wants: 0x02 when
// the time is asked for (the date then follows in the same burst), 0x05 when
// only the date is. The masks drop the flag bits that share those registers —
// VL in seconds, the century bit in month.
bool getDateTime(Date* date, Time* time) {
    uint8_t buf[7] = {0};
    int start_reg = (time != nullptr) ? 0x02 : 0x05;
    int len = ((date != nullptr) ? 4 : 0)
            + ((time != nullptr) ? 3 : 0);
    if (!s_present || len == 0 || !readRegs((uint8_t)start_reg, buf, (size_t)len))
        return false;

    int idx = 0;
    if (time) {
        time->seconds = bcd2ToByte(buf[idx++] & 0x7f);
        time->minutes = bcd2ToByte(buf[idx++] & 0x7f);
        time->hours   = bcd2ToByte(buf[idx++] & 0x3f);
    }

    if (date) {
        date->date    = bcd2ToByte(buf[idx++] & 0x3f);
        date->weekDay = bcd2ToByte(buf[idx++] & 0x07);
        date->month   = bcd2ToByte(buf[idx++] & 0x1f);
        // The century lives in bit 7 of the MONTH register — buf[idx - 1] here,
        // the byte just consumed — and selects the 1900 or 2000 base for the
        // two-digit year that follows it.
        date->year    = (uint16_t)(bcd2ToByte(buf[idx] & 0xff)
                      + ((0x80 & buf[idx - 1]) ? 1900 : 2000));
    }
    return true;
}

bool getTime(Time* time) { return getDateTime(nullptr, time); }

// The mirror image of the read, with one side effect worth stating: writing the
// seconds register with bit 7 clear also clears VL, which is what makes a clock
// that has been off its battery trustworthy again.
bool setDateTime(const Date* date, const Time* time) {
    uint8_t buf[7] = {0};

    int idx = 0;
    int reg_start = 0x05;
    if (time) {
        reg_start = 0x02;
        buf[idx++] = byteToBcd2(time->seconds);
        buf[idx++] = byteToBcd2(time->minutes);
        buf[idx++] = byteToBcd2(time->hours);
    }

    if (date) {
        buf[idx++] = byteToBcd2(date->date);
        buf[idx++] = (uint8_t)(0x07u & date->weekDay);
        buf[idx++] = (uint8_t)(byteToBcd2(date->month) + (date->year < 2000 ? 0x80 : 0));
        buf[idx++] = byteToBcd2((uint8_t)(date->year % 100));
    }
    if (!s_present || idx == 0) return false;
    return writeRegs((uint8_t)reg_start, buf, (size_t)idx);
}

// Bits 0x0C of control register 0x01 are the alarm flag (0x08) and the timer
// flag (0x04). Either one set means the INT line was pulled, which on this board
// is what re-latched the power — see power_s3.h's derivation of the wake path.
bool getIRQstatus() {
    return s_present && (0x0C & readReg8(0x01));
}

// The FLAG only. The countdown itself keeps running, which is exactly the trap
// b79a7bb fell into and why disableIRQ() below exists as a separate call.
void clearIRQ() {
    if (!s_present) return;
    bitOff(0x01, 0x0C);
}

// Alarm, timer and both INT enables, off. 0x80 written to each of the four alarm
// registers 0x09..0x0C is the PCF8563's "this field does not participate"
// encoding (bit 7 set = disabled), 0x0E = 0 stops the countdown at its source,
// and 0x01 = 0 clears the two flags and the two enable bits together.
void disableIRQ() {
    if (!s_present) return;
    static const uint8_t off[4] = {0x80, 0x80, 0x80, 0x80};
    writeRegs(0x09, off, sizeof(off));
    writeReg8(0x0E, 0);
    writeReg8(0x01, 0x00);
}

// M5Unified's rounding, kept to the digit. The chip's countdown is an 8-bit
// register clocked from a selectable source, so the reachable spans are 1..255
// of whatever unit the source gives: below 270 s the 1 Hz source (0x0E = 0x82)
// resolves to the second, at or above it the 1/60 Hz source (0x0E = 0x83)
// resolves to the minute, rounding to the nearest one. Both cap at 255 units,
// so the longest arm-able sleep is 255 minutes; asking for more silently gets
// 255. The returned value is what was actually programmed, in ms.
uint32_t setTimerIRQ(uint32_t msec) {
    if (!s_present) return 0;

    uint8_t reg_value = (uint8_t)(readReg8(0x01) & ~0x0C);

    uint32_t afterSeconds = (msec + 500) / 1000;
    if (afterSeconds == 0) {          // upstream writes `<= 0` on this unsigned
        writeReg8(0x01, (uint8_t)(reg_value & ~0x01));
        writeReg8(0x0E, 0x03);
        return 0;
    }

    uint32_t div = 1;
    uint8_t type_value = 0x82;
    if (afterSeconds < 270) {
        if (afterSeconds > 255) afterSeconds = 255;
    } else {
        div = 60;
        afterSeconds = (afterSeconds + 30) / div;
        if (afterSeconds > 255) afterSeconds = 255;
        type_value = 0x83;
    }

    writeReg8(0x0E, type_value);
    writeReg8(0x0F, (uint8_t)afterSeconds);

    // TIE (0x01) on, TI_TP (0x80) off: a level INT rather than a pulse train,
    // which is what the board's power latch needs to see.
    writeReg8(0x01, (uint8_t)((reg_value | 0x01) & ~0x80));
    return afterSeconds * div * 1000;
}

}  // namespace rtc
