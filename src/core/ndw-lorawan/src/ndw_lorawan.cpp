#include "ndw_lorawan.h"

#include <string.h>

#include <utils/Cryptography.h>

namespace ndw {
namespace lorawan {

namespace {

const uint8_t DIR_UP = 0x00, DIR_DOWN = 0x01;

void putLe32(uint8_t* p, uint32_t v) {
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

uint32_t le24(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }

// The A and B blocks share a layout (spec 4.3.3 and 4.4): a leading byte,
// four zeros, the direction, DevAddr, the 32-bit FCnt, a zero, and a last
// byte that is the block index for A and the message length for B.
void block(uint8_t* b, uint8_t first, uint8_t dir, uint32_t devAddr, uint32_t fCnt, uint8_t last) {
  memset(b, 0, 16);
  b[0] = first;
  b[5] = dir;
  putLe32(b + 6, devAddr);
  putLe32(b + 10, fCnt);
  b[15] = last;
}

// RadioLib's AES keeps a pointer to its key, so the key is copied somewhere
// that outlives the call.
uint8_t keyCopy[16];

void useKey(const uint8_t* key) {
  memcpy(keyCopy, key, 16);
  RadioLibAES128Instance.init(keyCopy);
}

// FRMPayload's cipher: XOR with AES(key, A_i), i from 1. The same both ways.
void crypt(const uint8_t* key, uint8_t dir, uint32_t devAddr, uint32_t fCnt, const uint8_t* in, size_t len, uint8_t* out) {
  useKey(key);
  uint8_t a[16], s[16];
  for (size_t i = 0; i < len; i += 16) {
    block(a, 0x01, dir, devAddr, fCnt, (uint8_t)(i / 16 + 1));
    RadioLibAES128Instance.encryptECB(a, 16, s);
    for (size_t j = 0; j < 16 && i + j < len; j++) out[i + j] = in[i + j] ^ s[j];
  }
}

// The first four bytes of CMAC(key, [B0 |] msg).
uint32_t mic(const uint8_t* key, const uint8_t* b0, const uint8_t* msg, size_t len) {
  uint8_t buf[16 + 256];
  size_t n = 0;
  if (b0) {
    memcpy(buf, b0, 16);
    n = 16;
  }
  memcpy(buf + n, msg, len);
  useKey(key);
  uint8_t cmac[16];
  RadioLibAES128Instance.generateCMAC(buf, n + len, cmac);
  return le32(cmac);
}

uint32_t dataMic(const uint8_t* nwkSKey, uint8_t dir, uint32_t devAddr, uint32_t fCnt, const uint8_t* msg, size_t len) {
  uint8_t b0[16];
  block(b0, 0x49, dir, devAddr, fCnt, (uint8_t)len);
  return mic(nwkSKey, b0, msg, len);
}

// Seconds from the Unix epoch to the GPS one, and the leap seconds GPS time
// has gained since: DeviceTimeAns counts in GPS time.
const uint32_t GPS_EPOCH = 315964800;
const uint32_t LEAP_SECONDS = 18;

}  // namespace

size_t buildUplink(const Session& session, const Uplink& up, uint8_t* out, size_t cap) {
  if (up.fPort == 0 || up.fPort > 223 || up.fOptsLen > 15) return 0;
  const size_t total = OVERHEAD + up.fOptsLen + up.len;
  if (total > cap || total - 4 > 255) return 0;

  uint8_t* p = out;
  *p++ = UNCONFIRMED_UP << 5;
  putLe32(p, session.devAddr);
  p += 4;
  *p++ = (up.ack ? 0x20 : 0x00) | up.fOptsLen;  // ADR off
  *p++ = up.fCnt;
  *p++ = up.fCnt >> 8;
  if (up.fOptsLen) memcpy(p, up.fOpts, up.fOptsLen);
  p += up.fOptsLen;
  *p++ = up.fPort;
  crypt(session.appSKey, DIR_UP, session.devAddr, up.fCnt, up.payload, up.len, p);
  p += up.len;

  putLe32(p, dataMic(session.nwkSKey, DIR_UP, session.devAddr, up.fCnt, out, p - out));
  return total;
}

size_t buildUplink(const Session& session, uint32_t fCnt, uint8_t fPort, const uint8_t* payload, size_t len,
                   uint8_t* out, size_t cap) {
  Uplink up = {fCnt, fPort, payload, len, nullptr, 0, false};
  return buildUplink(session, up, out, cap);
}

size_t buildJoinRequest(uint64_t joinEui, uint64_t devEui, uint16_t devNonce, const uint8_t appKey[16], uint8_t* out) {
  out[0] = JOIN_REQUEST << 5;
  // Both EUIs go on the air least significant byte first.
  for (int i = 0; i < 8; i++) {
    out[1 + i] = joinEui >> (8 * i);
    out[9 + i] = devEui >> (8 * i);
  }
  out[17] = devNonce;
  out[18] = devNonce >> 8;
  putLe32(out + 19, mic(appKey, nullptr, out, 19));
  return JOIN_REQUEST_LEN;
}

bool parseJoinAccept(const uint8_t* frame, size_t len, const uint8_t appKey[16], uint16_t devNonce, JoinAccept& accept,
                     Session& session) {
  // 17 bytes, or 33 with a CFList.
  if ((len != 17 && len != 33) || mtypeOf(frame) != JOIN_ACCEPT) return false;

  // The network encrypts a join accept with AES decryption, so the device
  // reads it with encryption: its one AES direction.
  uint8_t plain[33];
  plain[0] = frame[0];
  useKey(appKey);
  RadioLibAES128Instance.encryptECB(const_cast<uint8_t*>(frame + 1), len - 1, plain + 1);
  if (mic(appKey, nullptr, plain, len - 4) != le32(plain + len - 4)) return false;

  accept.joinNonce = le24(plain + 1);
  accept.netId = le24(plain + 4);
  session.devAddr = le32(plain + 7);
  accept.rx1DrOffset = (plain[11] >> 4) & 0x07;
  accept.rx2Dr = plain[11] & 0x0f;
  accept.rxDelay = plain[12] & 0x0f;

  // NwkSKey and AppSKey: AES(AppKey, 0x01 or 0x02 | JoinNonce | NetID | DevNonce | 0…).
  uint8_t in[16] = {0};
  memcpy(in + 1, plain + 1, 6);  // JoinNonce and NetID, as they arrived
  in[7] = devNonce;
  in[8] = devNonce >> 8;
  useKey(appKey);
  in[0] = 0x01;
  RadioLibAES128Instance.encryptECB(in, 16, session.nwkSKey);
  in[0] = 0x02;
  RadioLibAES128Instance.encryptECB(in, 16, session.appSKey);
  return true;
}

bool parseDownlink(const uint8_t* frame, size_t len, const Session& session, uint32_t lastFCnt, bool haveLast,
                   Downlink& out) {
  MType type = mtypeOf(frame);
  if ((type != UNCONFIRMED_DOWN && type != CONFIRMED_DOWN) || len < 12) return false;
  if (le32(frame + 1) != session.devAddr) return false;

  uint8_t fctrl = frame[5];
  size_t foptsLen = fctrl & 0x0f;
  if (8 + foptsLen + 4 > len) return false;

  // The 16 bits on the air, extended past the last counter seen.
  uint16_t low = frame[6] | frame[7] << 8;
  uint32_t fCnt = (lastFCnt & 0xffff0000u) | low;
  if (haveLast && fCnt <= lastFCnt) fCnt += 0x10000;
  if (haveLast && fCnt <= lastFCnt) return false;

  if (dataMic(session.nwkSKey, DIR_DOWN, session.devAddr, fCnt, frame, len - 4) != le32(frame + len - 4)) return false;

  out.confirmed = type == CONFIRMED_DOWN;
  out.ack = fctrl & 0x20;
  out.fPending = fctrl & 0x10;
  out.fCnt = fCnt;
  out.fOptsLen = foptsLen;
  memcpy(out.fOpts, frame + 8, foptsLen);
  size_t rest = len - 8 - foptsLen - 4;
  out.fPort = rest ? frame[8 + foptsLen] : -1;
  out.len = rest > 1 ? rest - 1 : 0;
  if (out.len > sizeof(out.payload)) return false;
  // Port 0 carries MAC commands, under the network key.
  const uint8_t* key = out.fPort == 0 ? session.nwkSKey : session.appSKey;
  crypt(key, DIR_DOWN, session.devAddr, fCnt, frame + 9 + foptsLen, out.len, out.payload);
  return true;
}

void handleMac(const uint8_t* c, size_t len, uint8_t battery, int8_t snr, MacResult& out) {
  memset(&out, 0, sizeof(out));
  auto answer = [&](const uint8_t* bytes, size_t n) {
    if (out.answersLen + n > sizeof(out.answers)) return;
    memcpy(out.answers + out.answersLen, bytes, n);
    out.answersLen += n;
  };
  size_t i = 0;
  while (i < len) {
    uint8_t cid = c[i++];
    // Each command's length after its CID, as the network sends it.
    size_t n;
    switch (cid) {
      case 0x02: n = 2; break;  // LinkCheckAns
      case 0x03: n = 4; break;  // LinkADRReq
      case 0x04: n = 1; break;  // DutyCycleReq
      case 0x05: n = 4; break;  // RXParamSetupReq
      case 0x06: n = 0; break;  // DevStatusReq
      case 0x07: n = 5; break;  // NewChannelReq
      case 0x08: n = 1; break;  // RXTimingSetupReq
      case 0x09: n = 1; break;  // TxParamSetupReq
      case 0x0A: n = 4; break;  // DlChannelReq
      case 0x0D: n = 5; break;  // DeviceTimeAns
      default: return;
    }
    if (i + n > len) return;
    const uint8_t* a = c + i;
    i += n;
    switch (cid) {
      case 0x02:
        out.linkCheck = true;
        out.margin = a[0];
        out.gateways = a[1];
        break;
      case 0x03: {
        const uint8_t ans[] = {0x03, 0x07};  // power, data rate and channel mask all acknowledged
        answer(ans, 2);
        break;
      }
      case 0x04: {
        const uint8_t ans[] = {0x04};
        answer(ans, 1);
        break;
      }
      case 0x05: {
        const uint8_t ans[] = {0x05, 0x07};
        answer(ans, 2);
        break;
      }
      case 0x06: {
        // The margin is six bits, signed.
        int8_t m = snr < -32 ? -32 : snr > 31 ? 31 : snr;
        const uint8_t ans[] = {0x06, battery, (uint8_t)(m & 0x3f)};
        answer(ans, 3);
        break;
      }
      case 0x07: {
        const uint8_t ans[] = {0x07, 0x03};
        answer(ans, 2);
        break;
      }
      case 0x08: {
        const uint8_t ans[] = {0x08};
        answer(ans, 1);
        break;
      }
      case 0x09: {
        const uint8_t ans[] = {0x09};
        answer(ans, 1);
        break;
      }
      case 0x0A: {
        const uint8_t ans[] = {0x0A, 0x03};
        answer(ans, 2);
        break;
      }
      case 0x0D:
        out.deviceTime = true;
        out.unixTime = le32(a) + GPS_EPOCH - LEAP_SECONDS;
        break;
    }
  }
}

}  // namespace lorawan
}  // namespace ndw
