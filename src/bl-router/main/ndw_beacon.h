/*
 * The NDW beacon: what a sensor broadcasts and a router overhears.
 *
 * Copied verbatim between src/bl-test-button and src/bl-router. They are
 * separate ESP-IDF projects with no shared component, so the duplication is
 * deliberate — and it is a wire format, which means a change has to be made
 * in both places or the router silently stops understanding the sensor.
 *
 * Connectionless. The sensor never pairs and is never connected to — it
 * advertises, and any router in range reads the advertisement. That is the
 * right shape for the thing being modelled: a meter sends one small reading
 * occasionally and should spend the rest of its life asleep, and a connection
 * per reading would cost more energy than the reading is worth. It also means
 * one router hears every sensor around it without knowing any of them first.
 *
 * The payload rides in the manufacturer-specific data field, which is the
 * only part of an advertisement that carries arbitrary bytes. It is 24 bytes:
 *
 *   0..1   company identifier, little-endian
 *   2      format version
 *   3      device kind
 *   4..11  EUI-64, big-endian so it reads the same as the printed identifier
 *   12..15 counter, little-endian
 *   16..19 uptime in seconds, little-endian
 *   20     battery percent, 0xff when unknown
 *   21..23 reserved, zero
 *
 * Little-endian throughout except the EUI, which is stored big-endian on
 * purpose: it is an identifier people read and type, and reversing it in the
 * payload would mean every log and every bug report disagreed with the label
 * on the box.
 */

#pragma once

#include <stdint.h>

/*
 * Company identifier.
 *
 * 0xFFFF is the value the Bluetooth SIG reserves for testing and internal
 * use, which is exactly what this is. Shipping hardware needs a real
 * assignment — until then, using someone else's would make our frames
 * indistinguishable from theirs to anyone scanning.
 */
#define NDW_COMPANY_ID 0xFFFF

/** Bumped when the layout below changes incompatibly. */
#define NDW_BEACON_VERSION 1

/** What kind of device is speaking. */
#define NDW_KIND_TEST_BUTTON 0x01

/** Total manufacturer-data length, including the company identifier. */
#define NDW_BEACON_LEN 24

/** Offsets into the manufacturer-data field. */
#define NDW_OFF_COMPANY 0
#define NDW_OFF_VERSION 2
#define NDW_OFF_KIND 3
#define NDW_OFF_EUI 4
#define NDW_OFF_COUNTER 12
#define NDW_OFF_UPTIME 16
#define NDW_OFF_BATTERY 20

/** Battery percent when the device has no way to measure it. */
#define NDW_BATTERY_UNKNOWN 0xFF
