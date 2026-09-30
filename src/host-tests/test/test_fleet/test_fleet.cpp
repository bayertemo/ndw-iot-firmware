// The fleet engine, radio-free: programmed over its host protocol, then run
// against a fake radio whose other end is a small network server — one that
// joins a device, reads its uplinks and answers with what ChirpStack would.
//
// What is pinned is the device's side of LoRaWAN over time: it joins, it
// reports with the counter moving on, it takes the network's time, and it
// joins again once its link checks go unanswered.
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include <utils/Cryptography.h>

#include "ndw_fleet.h"
#include "ndw_lorawan.h"
#include "ndw_text.h"

using namespace ndw;

// ---- fake hardware -----------------------------------------------------------

struct FakeClock : hal::Clock {
  uint64_t us = 1000000;
  uint64_t micros() override { return us; }
  void sleepMs(uint32_t ms) override { us += (uint64_t)ms * 1000; }
};

struct FakeRandom : hal::Random {
  uint32_t state = 7;
  uint32_t next() override { return state = state * 1664525u + 1013904223u; }
};

struct FakeStore : hal::Store {
  std::map<std::string, std::vector<uint8_t>> blobs;
  size_t size(const char* key) override { return blobs.count(key) ? blobs[key].size() : 0; }
  bool get(const char* key, void* out, size_t len) override {
    if (!blobs.count(key) || blobs[key].size() != len) return false;
    memcpy(out, blobs[key].data(), len);
    return true;
  }
  bool put(const char* key, const void* data, size_t len) override {
    blobs[key].assign((const uint8_t*)data, (const uint8_t*)data + len);
    return true;
  }
  bool remove(const char* key) override { return blobs.erase(key) > 0; }
  std::vector<std::string> keys(char prefix) override {
    std::vector<std::string> out;
    for (auto& kv : blobs)
      if (kv.first[0] == prefix) out.push_back(kv.first);
    return out;
  }
  bool clear() override {
    blobs.clear();
    return true;
  }
};

struct FakeConsole : hal::Console {
  std::vector<std::string> in, out;
  bool readLine(std::string& line) override {
    if (in.empty()) return false;
    line = in.front();
    in.erase(in.begin());
    return true;
  }
  void writeLine(const std::string& line) override { out.push_back(line); }
};

struct FakeBoard : hal::Board {
  int reboots = 0;
  uint64_t eui() override { return 0xaca704fffebce8f0ULL; }
  void reboot() override { reboots++; }
};

// ---- a network server at the other end of the radio ---------------------------

static uint8_t APP_KEY[16];
static const uint32_t DEV_ADDR = 0x01a2b3c4;
static const uint32_t UNIX_TIME = 1790000000;

struct Network {
  bool answerJoins = true;
  bool answerLinkChecks = true;
  int joins = 0;
  std::vector<uint32_t> fCnts;
  lorawan::Session session;
  uint32_t fCntDown = 0;
  std::vector<uint8_t> reply;  // what goes down in the next receive

  static void aes(const uint8_t* key, uint8_t* in, size_t len, uint8_t* out, bool decrypt) {
    static uint8_t k[16];
    memcpy(k, key, 16);
    RadioLibAES128Instance.init(k);
    if (decrypt) RadioLibAES128Instance.decryptECB(in, len, out);
    else RadioLibAES128Instance.encryptECB(in, len, out);
  }
  static uint32_t cmac(const uint8_t* key, const uint8_t* msg, size_t len) {
    static uint8_t k[16], buf[300];
    memcpy(k, key, 16);
    memcpy(buf, msg, len);
    RadioLibAES128Instance.init(k);
    uint8_t out[16];
    RadioLibAES128Instance.generateCMAC(buf, len, out);
    return out[0] | out[1] << 8 | out[2] << 16 | (uint32_t)out[3] << 24;
  }
  static void block(uint8_t* b, uint8_t first, uint8_t dir, uint32_t addr, uint32_t fcnt, uint8_t last) {
    memset(b, 0, 16);
    b[0] = first;
    b[5] = dir;
    memcpy(b + 6, &addr, 4);
    memcpy(b + 10, &fcnt, 4);
    b[15] = last;
  }

  void onJoinRequest(const uint8_t* f) {
    joins++;
    if (!answerJoins) return;
    uint16_t devNonce = f[17] | f[18] << 8;
    // AppNonce, NetID, DevAddr, DLSettings (RX2 DR8), RxDelay 1, then the MIC.
    uint8_t plain[17] = {0x20, 0x11, 0x22, 0x33, 0x13, 0x00, 0x00};
    memcpy(plain + 7, &DEV_ADDR, 4);
    plain[11] = 0x08;
    plain[12] = 0x01;
    uint32_t mic = cmac(APP_KEY, plain, 13);
    memcpy(plain + 13, &mic, 4);
    reply.assign(17, 0);
    reply[0] = 0x20;
    aes(APP_KEY, plain + 1, 16, reply.data() + 1, true);
    // The session the device will derive.
    lorawan::JoinAccept a;
    TEST_ASSERT_TRUE(lorawan::parseJoinAccept(reply.data(), 17, APP_KEY, devNonce, a, session));
    fCntDown = 0;
  }

  void onUplink(const uint8_t* f, size_t n) {
    uint32_t addr;
    memcpy(&addr, f + 1, 4);
    TEST_ASSERT_EQUAL_HEX32(DEV_ADDR, addr);
    uint8_t b0[16 + 64];
    uint32_t fcnt = f[6] | f[7] << 8;
    block(b0, 0x49, 0, addr, fcnt, (uint8_t)(n - 4));
    memcpy(b0 + 16, f, n - 4);
    uint32_t mic;
    memcpy(&mic, f + n - 4, 4);
    TEST_ASSERT_EQUAL_HEX32(cmac(session.nwkSKey, b0, 16 + n - 4), mic);
    fCnts.push_back(fcnt);

    // Answers what was asked, in FOpts: DeviceTimeAns and LinkCheckAns.
    uint8_t foptsLen = f[5] & 0x0f;
    std::vector<uint8_t> answers;
    for (int i = 0; i < foptsLen; i++) {
      uint8_t cid = f[8 + i];
      if (cid == lorawan::DEVICE_TIME) {
        uint32_t gps = UNIX_TIME - 315964800 + 18;
        answers.insert(answers.end(), {0x0D, (uint8_t)gps, (uint8_t)(gps >> 8), (uint8_t)(gps >> 16),
                                       (uint8_t)(gps >> 24), 0});
      } else if (cid == lorawan::LINK_CHECK && answerLinkChecks) {
        answers.insert(answers.end(), {0x02, 10, 1});
      }
    }
    if (answers.empty()) return;
    // An unconfirmed data down, MAC commands in FOpts, no port.
    std::vector<uint8_t> d = {0x60};
    d.insert(d.end(), (uint8_t*)&DEV_ADDR, (uint8_t*)&DEV_ADDR + 4);
    d.push_back((uint8_t)answers.size());
    d.push_back((uint8_t)fCntDown);
    d.push_back((uint8_t)(fCntDown >> 8));
    d.insert(d.end(), answers.begin(), answers.end());
    uint8_t mb[16 + 64];
    block(mb, 0x49, 1, DEV_ADDR, fCntDown, (uint8_t)d.size());
    memcpy(mb + 16, d.data(), d.size());
    uint32_t dmic = cmac(session.nwkSKey, mb, 16 + d.size());
    d.insert(d.end(), (uint8_t*)&dmic, (uint8_t*)&dmic + 4);
    fCntDown++;
    reply = d;
  }
};

struct FakeRadio : radio::EndDeviceRadio {
  FakeClock* clock;
  Network* net;
  uint8_t pinned = 0xff;
  bool begin() override { return true; }
  uint64_t send(const uint8_t* f, size_t n, bool join) override {
    clock->us += 50000;
    net->reply.clear();
    if (join) net->onJoinRequest(f);
    else net->onUplink(f, n);
    return clock->us;
  }
  size_t receive(const radio::RxWindows& w, const radio::Accept& accept, uint8_t* out, size_t cap,
                 radio::RxInfo& info) override {
    clock->us = w.txEndUs + (uint64_t)w.rxDelayS * 1000000 + 100000;
    if (net->reply.empty() || !accept(net->reply.data(), net->reply.size())) return 0;
    memcpy(out, net->reply.data(), net->reply.size());
    info.rssi = -40;
    info.snr = 8;
    return net->reply.size();
  }
  void keepToChannel(uint8_t ch) override { pinned = ch; }
  void describe(JsonObject out) override { out["link"] = "fake"; }
};

// ---- the fleet, programmed and run ---------------------------------------------

static FakeClock clk;
static FakeRandom rng;
static FakeConsole console;
static FakeBoard board;
static FakeStore store;
static Network network;
static FakeRadio fakeRadio;
static hal::Hal H{clk, rng, console, board};
static const fleet::Identity ID = {"ndw-test-fleet", "", "lorawan-probe", "test"};

static JsonDocument lastReply() {
  JsonDocument doc;
  for (auto it = console.out.rbegin(); it != console.out.rend(); ++it) {
    if (it->rfind("#NDW ", 0) == 0) {
      deserializeJson(doc, it->substr(5));
      break;
    }
  }
  return doc;
}

static void command(const std::string& line) {
  console.in.push_back(line);
  fleet::loop();
}

// Programs a fleet of one water meter, and boots again as the commit would.
static void programOne() {
  store.clear();
  board.reboots = 0;
  network = Network();
  fakeRadio.clock = &clk;
  fakeRadio.net = &network;
  fleet::begin(ID, H, store, fakeRadio);
  command("{\"cmd\":\"fleet-begin\",\"count\":1,\"interval\":60,\"anomalies\":0,\"epoch\":1780000000,\"tzOffset\":0}");
  command("{\"cmd\":\"fleet-add\",\"devices\":[[\"02a1b2c3d4e5f607\",\"" + text::hexBytes(APP_KEY, 16) +
          "\",\"water\"]]}");
  command("{\"cmd\":\"fleet-commit\"}");
  TEST_ASSERT_EQUAL_INT(1, board.reboots);
  board.reboots = 0;
  fleet::begin(ID, H, store, fakeRadio);
}

static void runFor(uint32_t seconds) {
  uint64_t until = clk.us + (uint64_t)seconds * 1000000;
  while (clk.us < until) fleet::loop();
}

void setUp(void) { text::parseHex("2b7e151628aed2a6abf7158809cf4f3c", APP_KEY, 16); }
void tearDown(void) {}

void test_joins_then_reports_with_the_counter_moving_on(void) {
  programOne();
  runFor(5 * 60);
  TEST_ASSERT_EQUAL_INT(1, network.joins);
  TEST_ASSERT_TRUE(network.fCnts.size() >= 4);
  for (size_t i = 0; i < network.fCnts.size(); i++) TEST_ASSERT_EQUAL_UINT32(i, network.fCnts[i]);
  TEST_ASSERT_EQUAL_STRING("running", fleet::view().state);
}

void test_takes_the_network_time(void) {
  programOne();
  runFor(3 * 60);
  command("{\"cmd\":\"status\"}");
  JsonDocument s = lastReply();
  TEST_ASSERT_EQUAL_STRING("network", s["clock"] | "");
  TEST_ASSERT_TRUE((s["epoch"] | 0u) >= UNIX_TIME);
}

void test_joins_again_after_three_unanswered_link_checks(void) {
  programOne();
  network.answerLinkChecks = false;
  // A check every 8th uplink, a minute apart: three missed take 24 minutes.
  runFor(30 * 60);
  TEST_ASSERT_EQUAL_INT(2, network.joins);
}

void test_backs_off_while_its_joins_go_unanswered(void) {
  programOne();
  network.answerJoins = false;
  runFor(10 * 60);
  // 3 s after boot, then 2 and 4 minutes after each; the next, 8 minutes on, is past ten.
  TEST_ASSERT_EQUAL_INT(3, network.joins);
  TEST_ASSERT_EQUAL_STRING("joining", fleet::view().state);
}

void test_reprogramming_the_same_keys_keeps_the_session(void) {
  programOne();
  runFor(3 * 60);
  size_t sent = network.fCnts.size();
  command("{\"cmd\":\"fleet-begin\",\"count\":1,\"interval\":120,\"anomalies\":0}");
  command("{\"cmd\":\"fleet-add\",\"devices\":[[\"02a1b2c3d4e5f607\",\"" + text::hexBytes(APP_KEY, 16) +
          "\",\"water\"]]}");
  command("{\"cmd\":\"fleet-commit\"}");
  fleet::begin(ID, H, store, fakeRadio);
  runFor(5 * 60);
  TEST_ASSERT_EQUAL_INT(1, network.joins);
  TEST_ASSERT_EQUAL_UINT32(sent, network.fCnts[sent]);
}

void test_keeps_the_channel_it_was_given(void) {
  programOne();
  command("{\"cmd\":\"channel\",\"channel\":8}");
  TEST_ASSERT_EQUAL_UINT8(8, fakeRadio.pinned);
  fleet::begin(ID, H, store, fakeRadio);
  TEST_ASSERT_EQUAL_UINT8(8, fakeRadio.pinned);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_joins_then_reports_with_the_counter_moving_on);
  RUN_TEST(test_takes_the_network_time);
  RUN_TEST(test_joins_again_after_three_unanswered_link_checks);
  RUN_TEST(test_backs_off_while_its_joins_go_unanswered);
  RUN_TEST(test_reprogramming_the_same_keys_keeps_the_session);
  RUN_TEST(test_keeps_the_channel_it_was_given);
  return UNITY_END();
}
