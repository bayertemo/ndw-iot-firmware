// NDW's BLE radio: LoRaWAN frames carried in BLE 5 extended advertisements
// (ndw_ble_frame.h), over NimBLE-Arduino. The end device's and the gateway's.
//
// Nothing pairs or connects, as nothing does over LoRa. A device advertises
// its uplink, then scans for its downlink until its RX2 would have closed; a
// gateway scans for uplinks and advertises each downlink from the moment the
// server sends it until the device stops listening. The microsecond timing
// LoRa's receive windows need has no meaning here: listening for the whole of
// both windows keeps Class A's promise — one answer, after an uplink, within
// RX1 and RX2 — without it.
//
// A board builds with -DCONFIG_BT_NIMBLE_EXT_ADV=1, which NimBLE itself has
// to be compiled with.
#pragma once

#include <stdint.h>

#include "ndw_radio.h"

namespace ndw {
namespace radio {

class BleEndDevice : public EndDeviceRadio {
 public:
  bool begin() override;
  uint64_t send(const uint8_t* frame, size_t len, bool join) override;
  size_t receive(const RxWindows& windows, const Accept& accept, uint8_t* out, size_t cap, RxInfo& info) override;
  void describe(JsonObject out) override;

 private:
  uint64_t txStartUs_ = 0;
  uint32_t sent_ = 0, heard_ = 0;
};

class BleGateway : public GatewayRadio {
 public:
  bool begin() override;
  bool heard(Heard& out) override;
  bool queue(const DownlinkRequest& request) override;
  bool service(DownlinkOutcome& done) override;
  void describe(JsonObject out) override;
  // Every frame is reported where the NDW LoRaWAN Gateway listens — US915
  // channel 8, 903.9 MHz, DR3 — which MeterFax's region accepts.
  uint32_t reportHz() override { return 903900000; }
  uint8_t reportDr() override { return 3; }
  const char* listening() override { return "Bluetooth LE"; }
  void pause(bool paused) override;

 private:
  bool paused_ = false;
};

}  // namespace radio
}  // namespace ndw
