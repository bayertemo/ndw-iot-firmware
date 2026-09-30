// LoRaWAN 1.0.3 Class A, built without a radio.
//
// For a device that speaks LoRaWAN over another link — NDW's BLE meters
// advertise the frames a LoRa radio would have sent, and hear the network's
// answers the same way, through a gateway that forwards both untouched. So
// this builds and reads exactly the PHYPayloads of the specification: the
// network server joins the device, checks its MICs and decrypts its frames as
// it would any LoRa device's, and cannot tell the difference.
//
// What a Class A end device needs and no more: a join request, the join
// accept and the session keys it yields, data uplinks with their MAC
// commands, and data downlinks. The radio's part — channels, data rates,
// timing — is the link's business, not this.
//
// AES comes from RadioLib's portable implementation, which also gives the
// CMAC the pinned Arduino core leaves out of mbedtls. Nothing here touches a
// radio, so it builds and is tested on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ndw {
namespace lorawan {

// A session: what a join gives a device, or what an ABP device is programmed
// with.
struct Session {
  uint32_t devAddr;
  uint8_t nwkSKey[16];
  uint8_t appSKey[16];
};

// MHDR, DevAddr, FCtrl, FCnt, FPort and the MIC: a data frame's fixed bytes.
static const size_t OVERHEAD = 13;

// A join request is always this long.
static const size_t JOIN_REQUEST_LEN = 23;

// MType, the top three bits of MHDR.
enum MType : uint8_t {
  JOIN_REQUEST = 0,
  JOIN_ACCEPT = 1,
  UNCONFIRMED_UP = 2,
  UNCONFIRMED_DOWN = 3,
  CONFIRMED_UP = 4,
  CONFIRMED_DOWN = 5,
};

inline MType mtypeOf(const uint8_t* frame) { return (MType)(frame[0] >> 5); }

// A data uplink, beyond its session.
struct Uplink {
  // The full 32-bit counter: its low 16 bits go on the air, and all 32 go
  // into the encryption and the MIC, as the server reconstructs them.
  uint32_t fCnt;
  // 1..223, an application port. A frame with only MAC commands still
  // carries its reading here, so port 0 is not used.
  uint8_t fPort;
  const uint8_t* payload;
  size_t len;
  // MAC commands, up to 15 bytes, in the clear in FOpts.
  const uint8_t* fOpts;
  uint8_t fOptsLen;
  // Acknowledges a confirmed downlink.
  bool ack;
};

// Builds an unconfirmed data uplink into `out`, and returns its length; 0
// when the port is out of range, FOpts are over 15 bytes, or the frame would
// not fit `cap`. ADR is off.
size_t buildUplink(const Session& session, const Uplink& up, uint8_t* out, size_t cap);

// The same, with no MAC commands: the shape an ABP meter sends.
size_t buildUplink(const Session& session, uint32_t fCnt, uint8_t fPort, const uint8_t* payload, size_t len,
                   uint8_t* out, size_t cap);

// A join request: the JoinEUI and DevEUI as numbers (most significant byte
// first, as printed), and the device's next DevNonce. Always 23 bytes.
size_t buildJoinRequest(uint64_t joinEui, uint64_t devEui, uint16_t devNonce, const uint8_t appKey[16], uint8_t* out);

// What a join accept says, besides the session it yields.
struct JoinAccept {
  uint32_t joinNonce;
  uint32_t netId;
  uint8_t rx1DrOffset;
  uint8_t rx2Dr;
  uint8_t rxDelay;
};

// Reads a join accept meant for the device that sent `devNonce` under
// `appKey`: decrypts it, checks its MIC, and derives the session. False for a
// frame that is not a join accept, or not this device's.
bool parseJoinAccept(const uint8_t* frame, size_t len, const uint8_t appKey[16], uint16_t devNonce, JoinAccept& accept,
                     Session& session);

// A data downlink, decrypted.
struct Downlink {
  bool confirmed;
  bool ack;
  bool fPending;
  uint32_t fCnt;  // reconstructed to 32 bits
  uint8_t fOpts[15];
  uint8_t fOptsLen;
  int16_t fPort;  // -1 when there is none
  uint8_t payload[222];
  size_t len;
};

// Reads a data downlink for this session. `lastFCnt` is the counter of the
// last downlink accepted, if `haveLast`; a frame whose counter is not beyond
// it is refused, as is one for another DevAddr or with a MIC that fails.
bool parseDownlink(const uint8_t* frame, size_t len, const Session& session, uint32_t lastFCnt, bool haveLast,
                   Downlink& out);

// ---- MAC commands ---------------------------------------------------------

// Commands a device asks with.
static const uint8_t LINK_CHECK = 0x02;
static const uint8_t DEVICE_TIME = 0x0D;

// What the device has to say back, and what it learned, from the commands in
// a downlink.
struct MacResult {
  uint8_t answers[15];
  uint8_t answersLen;
  bool linkCheck;  // a LinkCheckAns arrived
  uint8_t margin;
  uint8_t gateways;
  bool deviceTime;  // a DeviceTimeAns arrived
  uint32_t unixTime;
};

// Answers the network's commands as a Class A device that takes whatever it
// is told: every setting acknowledged, since the link beneath carries no
// channels or data rates for it to refuse. `battery` is 1..254 (0 on mains,
// 255 unknown), `snr` the downlink's, for DevStatusAns. Commands past one this
// does not know are skipped, as their length is unknown.
void handleMac(const uint8_t* commands, size_t len, uint8_t battery, int8_t snr, MacResult& out);

}  // namespace lorawan
}  // namespace ndw
