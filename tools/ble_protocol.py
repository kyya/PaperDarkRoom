"""BLE GATT protocol constants for the M5PaperS3 A Dark Room firmware.

Trimmed copy of dashboard-fw's daemon/ble_protocol.py, kept in sync BY HAND (not
import) since this is a separate repo now. Only the constants tools/ble_ota.py
actually needs are here — the game firmware only ever registers OTA/DATA/STAT
(+ the always-present SERVICE/CTRL/TAP_ACK/TIME_CONFIG characteristics it
inherits from the reused ble_link.cpp, kept for cross-flash compatibility; see
docs/research.md §3.2). The authoritative definition lives in the firmware:
src/ble_link.cpp.

Changing the protocol (adding/renaming a characteristic, changing the device
name) means updating both this file and src/ble_link.cpp.
"""
from __future__ import annotations

# CTRL: frame-header characteristic, also carries debug game commands — a write
# of ASCII "adr:give <res> <amount>" is intercepted by the firmware (see
# tools/adr_cmd.py + src/ble_link.cpp's CtrlCb).
CTRL_UUID = "e3e30002-1111-2222-3333-444455556666"
DATA_UUID = "e3e30003-1111-2222-3333-444455556666"
STAT_UUID = "e3e30004-1111-2222-3333-444455556666"
# OTA firmware update: 8-byte header <II total|crc32 (zlib CRC32), then the
# image streams over DATA_UUID (see tools/ble_ota.py + the firmware).
OTA_UUID = "e3e30007-1111-2222-3333-444455556666"

DEVICE_NAME = "M5PaperS3"
