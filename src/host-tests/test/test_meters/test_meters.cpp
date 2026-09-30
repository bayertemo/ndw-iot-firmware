// How the simulated meters behave, on the host: the payloads MeterFax's NDW
// Simulator codec reads, and registers that only ever rise.
#include <unity.h>

#include "ndw_meters.h"

using namespace ndw::meters;

static uint32_t state = 12345;
static uint32_t lcg() { return state = state * 1664525u + 1013904223u; }

void setUp(void) {
  state = 12345;
  setRandom(lcg);
}
void tearDown(void) {}

void test_water_is_port_10_five_bytes_big_endian(void) {
  Meter m = {};
  m.kind = WATER;
  m.reg = 0x01020304;
  m.battery = 87;
  uint8_t out[6], port;
  TEST_ASSERT_EQUAL_UINT(5, encode(m, out, port));
  TEST_ASSERT_EQUAL_UINT8(10, port);
  const uint8_t want[5] = {1, 2, 3, 4, 87};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 5);
}

void test_power_is_port_11_with_demand(void) {
  Meter m = {};
  m.kind = POWER;
  m.reg = 0x0a0b0c0d;
  m.demandW = 0x1234;
  uint8_t out[6], port;
  TEST_ASSERT_EQUAL_UINT(6, encode(m, out, port));
  TEST_ASSERT_EQUAL_UINT8(11, port);
  const uint8_t want[6] = {0x0a, 0x0b, 0x0c, 0x0d, 0x12, 0x34};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, out, 6);
}

void test_gas_is_port_12(void) {
  Meter m = {};
  m.kind = GAS;
  uint8_t out[6], port;
  TEST_ASSERT_EQUAL_UINT(5, encode(m, out, port));
  TEST_ASSERT_EQUAL_UINT8(12, port);
}

void test_seed_starts_a_few_years_in(void) {
  for (int k = WATER; k <= GAS; k++) {
    Meter m;
    seed(m, (Kind)k);
    TEST_ASSERT_TRUE(m.reg > 0);
    TEST_ASSERT_TRUE(m.scale >= 0.5f && m.scale < 1.6f);
    TEST_ASSERT_TRUE(m.battery >= 70 && m.battery <= 100);
    TEST_ASSERT_EQUAL_UINT8(NORMAL, m.anomaly);
  }
}

void test_registers_only_rise_over_a_week(void) {
  Clock clock = {1790000000, -300, 0};
  for (int k = WATER; k <= GAS; k++) {
    Meter m;
    seed(m, (Kind)k);
    uint32_t before = m.reg;
    for (int i = 0; i < 7 * 96; i++) {
      uint32_t last = m.reg;
      stepAnomaly(m, 20);
      consume(m, 0.25f, clock);
      clock.epoch += 900;
      TEST_ASSERT_TRUE(m.reg >= last);
    }
    TEST_ASSERT_TRUE(m.reg > before);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_water_is_port_10_five_bytes_big_endian);
  RUN_TEST(test_power_is_port_11_with_demand);
  RUN_TEST(test_gas_is_port_12);
  RUN_TEST(test_seed_starts_a_few_years_in);
  RUN_TEST(test_registers_only_rise_over_a_week);
  return UNITY_END();
}
