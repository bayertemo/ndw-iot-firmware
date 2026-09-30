// The frame builder against lora-packet's frames for the same fields and keys.
//
// lora-packet (npm) is an independent implementation of the same
// specification, and the one MeterFax's own tooling could check a frame with.
// A frame that matches it byte for byte, MIC included, is one ChirpStack
// accepts: the MIC is the only thing the server checks before decrypting.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "ndw_lorawan.h"

using ndw::lorawan::Session;
using ndw::lorawan::buildUplink;

static size_t hex(const char* s, uint8_t* out) {
  size_t n = strlen(s) / 2;
  for (size_t i = 0; i < n; i++) {
    unsigned v;
    sscanf(s + 2 * i, "%2x", &v);
    out[i] = v;
  }
  return n;
}

static void check(const char* devAddr, const char* nwk, const char* app, uint32_t fCnt, uint8_t port,
                  const char* payload, const char* expected) {
  Session s;
  uint8_t a[4];
  hex(devAddr, a);
  s.devAddr = (uint32_t)a[0] << 24 | a[1] << 16 | a[2] << 8 | a[3];
  hex(nwk, s.nwkSKey);
  hex(app, s.appSKey);
  uint8_t in[64], want[80], got[80];
  size_t len = hex(payload, in);
  size_t wantLen = hex(expected, want);
  size_t n = buildUplink(s, fCnt, port, in, len, got, sizeof got);
  TEST_ASSERT_EQUAL_UINT(wantLen, n);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got, wantLen);
}

// A water reading: port 10, five bytes, the first frame.
void test_water_reading(void) {
  check("01a2b3c4", "2b7e151628aed2a6abf7158809cf4f3c", "000102030405060708090a0b0c0d0e0f", 1, 10, "0000303905",
        "40c4b3a2010001000addff55ae422332ba18");
}

// A counter past 16 bits: only the low half is on the air, all of it is in the MIC.
void test_counter_past_16_bits(void) {
  check("00c0ffee", "ffeeddccbbaa99887766554433221100", "0f0e0d0c0b0a09080706050403020100", 0x12345, 11,
        "00012345ab00", "40eeffc0000045230bf931e5e1763e29a0d0e4");
}

// A payload longer than one AES block, so the keystream moves on to A_2.
void test_two_blocks(void) {
  check("01000001", "11111111111111111111111111111111", "22222222222222222222222222222222", 7, 12,
        "000102030405060708090a0b0c0d0e0f1011",
        "40010000010007000c2db880b7375458392abacdee8449038585667f7dd451");
}

void test_refuses_port_zero_and_a_small_buffer(void) {
  Session s = {};
  uint8_t in[6] = {0}, out[32];
  TEST_ASSERT_EQUAL_UINT(0, buildUplink(s, 1, 0, in, 6, out, sizeof out));
  TEST_ASSERT_EQUAL_UINT(0, buildUplink(s, 1, 224, in, 6, out, sizeof out));
  TEST_ASSERT_EQUAL_UINT(0, buildUplink(s, 1, 10, in, 6, out, 18));
  TEST_ASSERT_EQUAL_UINT(19, buildUplink(s, 1, 10, in, 6, out, 19));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_water_reading);
  RUN_TEST(test_counter_past_16_bits);
  RUN_TEST(test_two_blocks);
  RUN_TEST(test_refuses_port_zero_and_a_small_buffer);
  return UNITY_END();
}
