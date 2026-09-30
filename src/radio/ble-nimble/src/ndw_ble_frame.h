// A LoRaWAN frame in a BLE advertisement: the air format between NDW BLE
// meters and the NDW BLE gateway that carries their frames to and from the
// network server.
//
// Connectionless, as a LoRa radio is: nothing pairs or connects. A meter
// advertises its uplink and then scans for its receive windows; the gateway
// scans for uplinks and advertises the downlinks the server sends back. The
// frame is the whole LoRaWAN PHYPayload, untouched — MIC and encryption are
// LoRaWAN's, end to end between the meter and the network server — so the
// gateway needs no key, and the server cannot tell it from a LoRa frame.
//
// It rides in the manufacturer-specific data field of a BLE 5 extended,
// non-connectable, non-scannable advertisement:
//
//   0..1   company identifier, little-endian: 0xFFFF, as the NDW beacon's
//   2      format: 0x4C ('L') an uplink, 0x44 ('D') a downlink
//   3..    the PHYPayload
//
// Extended rather than legacy advertising because a join accept with its
// CFList is 33 bytes and a downlink with MAC commands more, where a legacy
// advertisement holds 26. The format byte sits where the NDW beacon keeps its
// version (1 or 2), so a router that reads beacons passes these by.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ndw {
namespace ble {

static const uint16_t COMPANY_ID = 0xFFFF;
static const uint8_t FORMAT_UPLINK = 0x4C;
static const uint8_t FORMAT_DOWNLINK = 0x44;
static const size_t HEADER_LEN = 3;
// Longer than any frame an NDW meter sends or is sent: a data frame's 13
// bytes of overhead, 15 of FOpts, and a payload.
static const size_t MAX_FRAME = 64;

// Wraps a PHYPayload into manufacturer data. Returns its length, or 0 if the
// frame is longer than MAX_FRAME.
inline size_t wrap(uint8_t format, const uint8_t* frame, size_t len, uint8_t* out) {
  if (len == 0 || len > MAX_FRAME) return 0;
  out[0] = COMPANY_ID & 0xff;
  out[1] = COMPANY_ID >> 8;
  out[2] = format;
  for (size_t i = 0; i < len; i++) out[HEADER_LEN + i] = frame[i];
  return HEADER_LEN + len;
}

// The PHYPayload in manufacturer data of the given format, or null when it
// holds something else.
inline const uint8_t* unwrap(uint8_t format, const uint8_t* data, size_t len, size_t& frameLen) {
  if (len <= HEADER_LEN || len > HEADER_LEN + MAX_FRAME) return nullptr;
  if ((data[0] | data[1] << 8) != COMPANY_ID || data[2] != format) return nullptr;
  frameLen = len - HEADER_LEN;
  return data + HEADER_LEN;
}

}  // namespace ble
}  // namespace ndw
