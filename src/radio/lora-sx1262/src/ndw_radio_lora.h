// NDW's LoRa radio: US915 through an SX1262, over RadioLib's PHY. An end
// device's, keeping Class A's receive windows to the microsecond, and a
// single-channel gateway's.
//
// RadioLib is the chip's driver here and nothing more: the LoRaWAN above it
// is the core's (ndw-lorawan), the same as over BLE. So this knows US915 —
// its channels, data rates and the downlink channel that answers each uplink
// channel — and when a window opens; it does not know what a frame means.
//
// The board makes the SX1262 on its own pins and begins it; this takes it
// from there.
#pragma once

#include <RadioLib.h>
#include <stdint.h>

#include "ndw_radio.h"

namespace ndw {
namespace radio {

// US915 channel 8, 903.9 MHz — the first of sub-band 2 — at DR3, SF7 on
// 125 kHz: what the NDW LoRaWAN Gateway's one radio hears, and what an end
// device keeps to for it.
static const uint8_t TEST_GATEWAY_CHANNEL = 8;
static const uint8_t TEST_GATEWAY_DR = 3;
static const uint8_t ALL_CHANNELS = 0xff;

class LoraEndDevice : public EndDeviceRadio {
 public:
  explicit LoraEndDevice(SX1262& chip) : chip_(chip) {}
  bool begin() override;
  uint64_t send(const uint8_t* frame, size_t len, bool join) override;
  size_t receive(const RxWindows& windows, const Accept& accept, uint8_t* out, size_t cap, RxInfo& info) override;
  // Sub-band 2 (channels 8 to 15), a channel at random for each frame; or,
  // for a single-channel gateway, one channel at DR3 for everything.
  void keepToChannel(uint8_t channel) override { pinned_ = channel; }
  uint8_t channel() override { return pinned_; }
  void describe(JsonObject out) override;

 private:
  SX1262& chip_;
  uint8_t pinned_ = ALL_CHANNELS;
  uint8_t lastChannel_ = 8, lastDr_ = 3;
  float lastRssi_ = 0, lastSnr_ = 0;
  bool haveSignal_ = false;
};

class LoraGateway : public GatewayRadio {
 public:
  explicit LoraGateway(SX1262& chip) : chip_(chip) {}
  bool begin() override;
  bool heard(Heard& out) override;
  bool queue(const DownlinkRequest& request) override;
  bool service(DownlinkOutcome& done) override;
  void describe(JsonObject out) override;
  uint32_t reportHz() override { return 903900000; }
  uint8_t reportDr() override { return TEST_GATEWAY_DR; }
  const char* listening() override { return "903.9 MHz SF7"; }

 private:
  SX1262& chip_;
};

}  // namespace radio
}  // namespace ndw
