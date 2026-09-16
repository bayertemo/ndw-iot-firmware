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
 * only part of an advertisement that carries arbitrary bytes. Two versions
 * are on the air:
 *
 *   0..1   company identifier, little-endian
 *   2      format version, 1 or 2
 *   3      device kind
 *   4..11  EUI-64, big-endian so it reads the same as the printed identifier
 *   12..15 counter, little-endian
 *   16..19 uptime in seconds, little-endian
 *   20     battery percent, 0xff when unknown
 *
 * Version 1 is 24 bytes and sends everything in the clear, with 21..23
 * reserved and zero. Version 2 is 25 bytes: 16..20 are encrypted under the
 * device's key and a four-byte authentication tag follows at 21..24. The
 * field positions and meanings are identical; only their confidentiality
 * differs, so one decoder reads both once the bytes are in hand.
 *
 * Header 0..15 travels in the clear in both, and that is load-bearing rather
 * than an omission. Three different readers need part of it before any key is
 * available:
 *
 *   - a router filters on the company identifier and version holding no key
 *     at all, which is what lets it relay for sensors it has never met;
 *   - the platform needs the EUI to know whose key to try before trying one;
 *   - the counter is the nonce, so it cannot itself be encrypted — and both
 *     the router's repeat suppression and the platform's replay window key on
 *     it, neither of which can wait for a decryption that may never succeed.
 *
 * Clear does not mean unprotected. The whole header is fed to CCM as
 * associated data, so editing any of it — swapping an EUI, rewinding a
 * counter — invalidates the tag. It can be read by anyone and changed by
 * nobody.
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

/**
 * Bumped when the layout above changes incompatibly.
 *
 * Version 1 is what an unprovisioned board sends: it has no key, so it has
 * nothing to encrypt with, and a board that went silent until somebody
 * claimed it would look broken on the bench. Version 2 is what the same board
 * sends once a key has been written to it over the cable.
 */
#define NDW_BEACON_VERSION 1
#define NDW_BEACON_VERSION_SEALED 2

/** What kind of device is speaking. */
#define NDW_KIND_TEST_BUTTON 0x01

/** Total manufacturer-data length, including the company identifier. */
#define NDW_BEACON_LEN 24

/**
 * Version 2: the same fields, with a tag where the reserved bytes were.
 *
 * 25 of the 29 bytes BLE allows for manufacturer data. The four that remain
 * are why the tag is four bytes and not eight — a real limit of the budget
 * rather than a considered security margin.
 */
#define NDW_BEACON_SEALED_LEN 25

/** Bytes 0..15: authenticated as associated data, but not encrypted. */
#define NDW_CLEAR_LEN 16

/**
 * The CCM nonce: the EUI and the counter, twelve bytes at offset 4.
 *
 * CCM requires a value that never repeats under one key, and this is exactly
 * that — the EUI makes it unique per device and the counter, which is
 * monotonic and persisted across reboots, makes it unique per frame. Both are
 * already on the wire, so the nonce costs no bytes.
 *
 * It deliberately excludes the company, version and kind bytes at 0..3. Those
 * are constant for a device, so they would contribute nothing to uniqueness —
 * and they are authenticated anyway, since the whole sixteen-byte header goes
 * in as associated data.
 */
#define NDW_OFF_NONCE 4
#define NDW_NONCE_LEN 12

/** Bytes 16..20 encrypted: uptime and battery. */
#define NDW_SEALED_LEN 5

/** Where the tag sits, and how much of it travels. */
#define NDW_OFF_TAG 21
#define NDW_TAG_LEN 4

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
