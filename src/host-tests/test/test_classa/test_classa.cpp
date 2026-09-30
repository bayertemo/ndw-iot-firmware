// Class A against lora-packet's frames for the same fields and keys: the
// join, the session it yields, and data both ways with MAC commands.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "ndw_lorawan.h"

using namespace ndw::lorawan;

static size_t hex(const char* s, uint8_t* out) {
  size_t n = strlen(s) / 2;
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    sscanf(s + 2 * i, "%2x", &v);
    out[i] = v;
  }
  return n;
}

static uint8_t appKey[16];
static const uint16_t DEV_NONCE = 0x0102;

void setUp(void) { hex("2b7e151628aed2a6abf7158809cf4f3c", appKey); }
void tearDown(void) {}

static Session joined() {
  Session s;
  s.devAddr = 0x01020304;
  hex("b4acdf1f3e7dc6401f9d7898e3542117", s.nwkSKey);
  hex("5398c7730eb07e19a16362564e7fa100", s.appSKey);
  return s;
}

void test_join_request(void) {
  uint8_t want[23], got[23];
  hex("00000000000000000007f6e5d4c3b2a1020201ceadd7e1", want);
  TEST_ASSERT_EQUAL_UINT(23, buildJoinRequest(0, 0x02a1b2c3d4e5f607ULL, DEV_NONCE, appKey, got));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, 23);
}

static void checkAccept(const char* frameHex) {
  uint8_t frame[33];
  size_t n = hex(frameHex, frame);
  JoinAccept a;
  Session s;
  TEST_ASSERT_TRUE(parseJoinAccept(frame, n, appKey, DEV_NONCE, a, s));
  TEST_ASSERT_EQUAL_HEX32(0x01020304, s.devAddr);
  TEST_ASSERT_EQUAL_HEX32(0xa1b2c3, a.joinNonce);
  TEST_ASSERT_EQUAL_HEX32(0x000013, a.netId);
  TEST_ASSERT_EQUAL_UINT8(8, a.rx2Dr);
  TEST_ASSERT_EQUAL_UINT8(1, a.rxDelay);
  Session want = joined();
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want.nwkSKey, s.nwkSKey, 16);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want.appSKey, s.appSKey, 16);
}

void test_join_accept(void) { checkAccept("20bf2b05521ff03692b0dd69fb0e13cdaa"); }

// US915 may add a CFList (a channel mask): 33 bytes, read the same.
void test_join_accept_with_cflist(void) {
  checkAccept("2090418971ba6748f40d4a65a7ad834d0a9ebfe214f89106dba649b8ab3f552fb0");
}

void test_join_accept_for_another_device(void) {
  uint8_t frame[17];
  hex("20bf2b05521ff03692b0dd69fb0e13cdaa", frame);
  uint8_t other[16] = {1};
  JoinAccept a;
  Session s;
  TEST_ASSERT_FALSE(parseJoinAccept(frame, 17, other, DEV_NONCE, a, s));
}

void test_uplink_with_mac_commands_and_ack(void) {
  Session s = joined();
  uint8_t payload[5], fopts[2] = {DEVICE_TIME, LINK_CHECK}, want[40], got[40];
  hex("0000303905", payload);
  size_t wantLen = hex("40040302012209000d020a4addbb1c444e93072a", want);
  Uplink up = {9, 10, payload, 5, fopts, 2, true};
  TEST_ASSERT_EQUAL_UINT(wantLen, buildUplink(s, up, got, sizeof got));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, wantLen);
}

void test_downlink(void) {
  Session s = joined();
  uint8_t frame[40];
  size_t n = hex("a004030201240500020a010601a5cc5fadbd89", frame);
  Downlink d;
  TEST_ASSERT_TRUE(parseDownlink(frame, n, s, 0, false, d));
  TEST_ASSERT_TRUE(d.confirmed);
  TEST_ASSERT_TRUE(d.ack);
  TEST_ASSERT_EQUAL_UINT32(5, d.fCnt);
  TEST_ASSERT_EQUAL_INT16(1, d.fPort);
  const uint8_t cafe[2] = {0xca, 0xfe};
  TEST_ASSERT_EQUAL_UINT(2, d.len);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(cafe, d.payload, 2);
  TEST_ASSERT_EQUAL_UINT8(4, d.fOptsLen);

  // Seen once, it is not taken again.
  TEST_ASSERT_FALSE(parseDownlink(frame, n, s, 5, true, d));
  // A flipped bit fails the MIC.
  frame[10] ^= 1;
  TEST_ASSERT_FALSE(parseDownlink(frame, n, s, 0, false, d));
}

void test_mac_answers(void) {
  // LinkCheckAns, DevStatusReq, LinkADRReq, DeviceTimeAns.
  uint8_t cmds[] = {0x02, 0x0a, 0x01, 0x06, 0x03, 0x30, 0xff, 0x00, 0x01, 0x0d, 0x10, 0x27, 0x00, 0x50, 0x00};
  MacResult r;
  handleMac(cmds, sizeof cmds, 200, -3, r);
  TEST_ASSERT_TRUE(r.linkCheck);
  TEST_ASSERT_EQUAL_UINT8(10, r.margin);
  TEST_ASSERT_EQUAL_UINT8(1, r.gateways);
  TEST_ASSERT_TRUE(r.deviceTime);
  TEST_ASSERT_EQUAL_UINT32(0x50002710u + 315964800u - 18u, r.unixTime);
  const uint8_t want[] = {0x06, 200, (uint8_t)(-3 & 0x3f), 0x03, 0x07};
  TEST_ASSERT_EQUAL_UINT8(sizeof want, r.answersLen);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, r.answers, sizeof want);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_join_request);
  RUN_TEST(test_join_accept);
  RUN_TEST(test_join_accept_with_cflist);
  RUN_TEST(test_join_accept_for_another_device);
  RUN_TEST(test_uplink_with_mac_commands_and_ack);
  RUN_TEST(test_downlink);
  RUN_TEST(test_mac_answers);
  return UNITY_END();
}
