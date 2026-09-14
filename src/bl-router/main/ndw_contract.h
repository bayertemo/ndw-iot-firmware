/*
 * The GATT contract, shared with the console.
 *
 * These UUIDs and payload shapes must match
 * ndw-iot-console/src/app/core/ble/contract.ts exactly. There is no build-time
 * link between the two files, so a change on either side that is not mirrored
 * here shows up as a console that connects and then finds no characteristic —
 * a failure with a technician already standing at the bench.
 *
 * The console writes the UUIDs lowercase, 128-bit, in the usual 8-4-4-4-12
 * text form. NimBLE wants them as little-endian byte arrays, which is why the
 * macro below reverses them.
 */

#pragma once

#include "host/ble_uuid.h"

/*
 * 6e5d0001-b5a3-f393-e0a9-e50e24dcca9e and friends, byte-reversed.
 *
 * BLE_UUID128_INIT takes bytes least-significant first, so the text form is
 * written back to front. Getting this wrong produces a service that advertises
 * fine and matches nothing — the console's filter simply never sees it.
 */
#define NDW_UUID128(last_byte)                                                                  \
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,    \
                     (last_byte), 0x00, 0x5d, 0x6e)

/* Advertised, and what the console filters its device chooser on. */
#define NDW_SERVICE_UUID NDW_UUID128(0x01)

/* Read: the sixteen ASCII hex characters of this box's EUI. */
#define NDW_EUI_UUID NDW_UUID128(0x02)

/* Write: {"ssid":"...","password":"..."} as UTF-8 JSON. */
#define NDW_WIFI_UUID NDW_UUID128(0x03)

/* Notify: how the join went, once the radio has tried. */
#define NDW_STATUS_UUID NDW_UUID128(0x04)

/*
 * Status values pushed on the notify characteristic.
 *
 * Single ASCII words rather than a numeric code: the console shows these to a
 * person, and a string that is already readable cannot be mistranslated by a
 * lookup table that drifts out of sync.
 */
#define NDW_STATUS_CONNECTING "connecting"
#define NDW_STATUS_CONNECTED  "connected"
#define NDW_STATUS_BAD_AUTH   "bad-auth"
#define NDW_STATUS_NOT_FOUND  "not-found"
#define NDW_STATUS_FAILED     "failed"

/* Sixteen hex characters, no separators, lowercase. */
#define NDW_EUI_LEN 16
