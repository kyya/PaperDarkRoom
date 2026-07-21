// Quiet-hours containment math — pure functions, no I2C/NVS dependency, so
// the logic can be traced by hand without hardware. See
// docs/superpowers/specs/2026-07-13-quiet-hours-design.md.
#pragma once
#include <Arduino.h>

namespace quiet_hours {

// start==end means "disabled" (never suppresses a wake).
inline bool enabled(uint16_t startMin, uint16_t endMin) {
    return startMin != endMin;
}

// Is nowMin inside [startMin, endMin)? Handles the midnight-wrap case
// (start > end, e.g. a hypothetical 22:00-06:00 window) even though the
// default 02:00-09:00 doesn't need it — nearly free to handle generally.
inline bool inWindow(uint16_t nowMin, uint16_t startMin, uint16_t endMin) {
    if (!enabled(startMin, endMin)) return false;
    if (startMin < endMin) return nowMin >= startMin && nowMin < endMin;
    return nowMin >= startMin || nowMin < endMin;
}

// Seconds from now (nowMin:nowSec) until endMin. Assumes inWindow() is true
// (caller's responsibility) so endMin is always "ahead" once the wrap is
// accounted for.
inline uint32_t secondsUntilEnd(uint16_t nowMin, uint8_t nowSec, uint16_t endMin) {
    uint32_t minsUntil = (endMin >= nowMin)
        ? (uint32_t)(endMin - nowMin)
        : (uint32_t)(24 * 60 - nowMin + endMin);   // wrapped past midnight
    uint32_t secs = minsUntil * 60;
    return secs > nowSec ? secs - nowSec : 0;
}

}  // namespace quiet_hours
