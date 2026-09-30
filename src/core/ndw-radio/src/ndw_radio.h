// What the core asks of a radio: an end device's, to send LoRaWAN frames and
// hear the answers in its receive windows, and a gateway's, to hear frames
// and send the network's answers into those windows.
//
// The radio is the link, not LoRaWAN: which channel and data rate, how a
// receive window is kept, what a frame looks like on the air. The core's
// LoRaWAN stack is the same above every one. src/radio has two:
//
//   lora-sx1262   LoRa, through an SX1262: US915 channels, RX1 and RX2 to the
//                 microsecond, as the specification has them
//   ble-nimble    Bluetooth LE advertising, through NimBLE: no channels, and a
//                 receive window kept by listening from the uplink until RX2
//                 would have closed
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>

#include <ArduinoJson.h>

namespace ndw {
namespace radio {

// ---- an end device's --------------------------------------------------------

// Class A's receive windows after one uplink.
struct RxWindows {
  uint64_t txEndUs;    // when the uplink finished, on the radio's clock
  uint8_t rxDelayS;    // RX1 opens this long after it: 1 for data, 5 for a join accept
  uint8_t rx1DrOffset;
  uint32_t rx2Hz;
  uint8_t rx2Dr;
};

struct RxInfo {
  float rssi;
  float snr;
};

// Called with each frame heard in a window; true for the one this device is
// waiting for, which ends the listening. False for another device's.
using Accept = std::function<bool(const uint8_t* frame, size_t len)>;

class EndDeviceRadio {
 public:
  virtual ~EndDeviceRadio() {}
  virtual bool begin() = 0;
  // Sends a frame. `join` says it is a join request, which some bands send at
  // another data rate. The end of the transmission, on the radio's clock, or
  // 0 when it did not go.
  virtual uint64_t send(const uint8_t* frame, size_t len, bool join) = 0;
  // Keeps the windows after that uplink. The accepted frame's length, copied
  // into `out`, or 0 when both windows passed without one.
  virtual size_t receive(const RxWindows& windows, const Accept& accept, uint8_t* out, size_t cap, RxInfo& info) = 0;
  // One channel to keep to (a single-channel gateway's), or 0xff for the
  // band's. A radio without channels ignores it.
  virtual void keepToChannel(uint8_t channel) { (void)channel; }
  virtual uint8_t channel() { return 0xff; }
  // What it says about itself in status: the band, the channel, the link.
  virtual void describe(JsonObject out) = 0;
};

// ---- a gateway's -------------------------------------------------------------

// A frame off the air.
struct Heard {
  uint8_t frame[256];
  size_t len;
  uint64_t atUs;  // the end of the frame, on the radio's clock
  float rssi;
  float snr;
};

// A dnmsg, as the core read it: what to send, and where the device listens.
struct DownlinkRequest {
  uint8_t pdu[256];
  size_t len;
  uint32_t diid;
  std::string devEui;
  uint64_t xtime;      // the uplink's, as it went up
  uint64_t uplinkUs;   // its low 48 bits: the uplink's end on the radio's clock
  int rxDelayS;
  int deviceClass;     // 0 A, 1 B, 2 C
  // RX1 and RX2: frequency and data rate, 0 and -1 where not offered.
  uint32_t hz[2];
  int dr[2];
};

// How a downlink ended, for the core to acknowledge or count.
struct DownlinkOutcome {
  uint32_t diid;
  std::string devEui;
  uint64_t xtime;
  bool sent;
};

class GatewayRadio {
 public:
  virtual ~GatewayRadio() {}
  virtual bool begin() = 0;
  // The next frame heard, if any. Called every loop.
  virtual bool heard(Heard& out) = 0;
  // Takes a downlink to send into its windows. False when it cannot: a queue
  // full, a class this radio does not serve, a frame too long.
  virtual bool queue(const DownlinkRequest& request) = 0;
  // Time-critical work — a transmission due now — and the next finished
  // downlink, if any. Called every loop, as often as it can be.
  virtual bool service(DownlinkOutcome& done) = 0;
  // The status reply's "radio" block.
  virtual void describe(JsonObject out) = 0;
  // Where every frame is reported, for a radio that has no channels of its
  // own: the band's frequency and data rate the server accepts. A LoRa radio
  // reports where it heard the frame.
  virtual uint32_t reportHz() = 0;
  virtual uint8_t reportDr() = 0;
  // For the log line on connecting: "903.9 MHz SF7", "Bluetooth LE".
  virtual const char* listening() = 0;
};

}  // namespace radio
}  // namespace ndw
