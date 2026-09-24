// Copy to credentials.h (ignored by git) and fill in. The same three values
// go into MeterFax (Devices → Add device) or ChirpStack when the device is
// registered; the join fails with nothing more specific than a timeout if any
// of them differ.
#pragma once
#include <stdint.h>

// All zeros is fine: ChirpStack does not use it to route a LoRaWAN 1.0.x join.
static const uint64_t JOIN_EUI = 0x0000000000000000;

// Any unique EUI-64. The board's MAC, widened with FF:FE in the middle, is a
// convenient one: `tools.sh mac` prints it.
static const uint64_t DEV_EUI = 0x0000000000000000;

// 16 random bytes: `openssl rand -hex 16`, then written out as below.
static uint8_t appKey[16] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
