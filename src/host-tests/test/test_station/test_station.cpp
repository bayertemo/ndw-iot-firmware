// The gateway core, radio-free and network-free: programmed over its host
// protocol, then taken through Basics Station against a fake network stack —
// discovery, the mux connection, router_config — with a frame up and a
// downlink back.
#include <string.h>
#include <unity.h>

#include <map>
#include <string>
#include <vector>

#include "ndw_station.h"

using namespace ndw;

struct FakeClock : hal::Clock {
  uint64_t us = 1000000;
  uint64_t micros() override { return us; }
  void sleepMs(uint32_t ms) override { us += (uint64_t)ms * 1000; }
};

struct FakeRandom : hal::Random {
  uint32_t next() override { return 0x42; }
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
  std::vector<std::string> keys(char) override { return {}; }
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
  uint64_t eui() override { return 0xaca704fffebce8f0ULL; }
  void reboot() override {}
};

struct FakeNet : hal::Net {
  std::function<void(hal::SocketEvent, const char*, size_t)> handler;
  std::vector<std::string> opened;  // host:port/path
  std::vector<std::string> sent;
  std::string header;
  void wifiBegin(const std::string&, const std::string&, const std::string&) override {}
  hal::WifiState wifiState() override { return hal::WifiState::CONNECTED; }
  std::string ip() override { return "10.0.0.2"; }
  std::string mac() override { return "AC:A7:04:BC:E8:F0"; }
  int rssi() override { return -50; }
  std::vector<hal::WifiNetwork> scan() override { return {}; }
  void startClock() override {}
  double unixTime() override { return 1790000000.0; }
  void open(const std::string& host, uint16_t port, const std::string& path, bool, const std::string&,
            const std::string& h) override {
    opened.push_back(host + ":" + std::to_string(port) + path);
    header = h;
  }
  void close() override {}
  void sendText(const std::string& text) override { sent.push_back(text); }
  void loop() override {}
  void onSocket(std::function<void(hal::SocketEvent, const char*, size_t)> h) override { handler = h; }
  hal::Diagnosis diagnose(const std::string&, uint16_t, const std::string&, const std::string&,
                          const std::string&, std::string&) override {
    return hal::Diagnosis::REFUSED;
  }
  void deliver(const std::string& text) { handler(hal::SocketEvent::TEXT, text.c_str(), text.size()); }
};

struct FakeGatewayRadio : radio::GatewayRadio {
  std::vector<radio::Heard> toHear;
  std::vector<radio::DownlinkRequest> queued;
  bool begin() override { return true; }
  bool heard(radio::Heard& out) override {
    if (toHear.empty()) return false;
    out = toHear.front();
    toHear.erase(toHear.begin());
    return true;
  }
  bool queue(const radio::DownlinkRequest& r) override {
    queued.push_back(r);
    return true;
  }
  bool service(radio::DownlinkOutcome&) override { return false; }
  void describe(JsonObject out) override { out["link"] = "fake"; }
  uint32_t reportHz() override { return 903900000; }
  uint8_t reportDr() override { return 3; }
  const char* listening() override { return "a fake"; }
};

static FakeClock clk;
static FakeRandom rng;
static FakeConsole console;
static FakeBoard board;
static FakeStore store;
static FakeNet net;
static FakeGatewayRadio gw;
static hal::Hal H{clk, rng, console, board};
static const station::Identity ID = {"ndw-test-gateway", "", "test-model", "ble-gateway"};

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

static JsonDocument lastSent() {
  JsonDocument doc;
  deserializeJson(doc, net.sent.back());
  return doc;
}

void setUp(void) {}
void tearDown(void) {}

void test_programs_and_connects_through_discovery(void) {
  station::begin(ID, H, net, store, gw);
  console.in.push_back("{\"cmd\":\"hello\"}");
  station::loop();
  TEST_ASSERT_EQUAL_STRING("ble-gateway", lastReply()["kind"] | "");

  console.in.push_back("{\"cmd\":\"wifi\",\"ssid\":\"Bench\",\"password\":\"secret\"}");
  console.in.push_back(
      "{\"cmd\":\"lns\",\"url\":\"wss://lns.example.com\",\"trust\":\"-----BEGIN CERTIFICATE-----x\","
      "\"token\":\"abc123\"}");
  station::loop();
  JsonDocument lns = lastReply();
  TEST_ASSERT_TRUE(lns["lns"]["token"] | false);
  TEST_ASSERT_EQUAL_STRING("c123", lns["lns"]["tokenHint"] | "");

  // WiFi is up and the clock set: the next pass asks router-info.
  station::loop();
  TEST_ASSERT_EQUAL_STRING("lns.example.com:443/router-info", net.opened.back().c_str());
  TEST_ASSERT_EQUAL_STRING("Authorization: Bearer abc123", net.header.c_str());
  net.handler(hal::SocketEvent::OPENED, nullptr, 0);
  TEST_ASSERT_EQUAL_STRING("ac-a7-04-ff-fe-bc-e8-f0", lastSent()["router"] | "");

  net.deliver("{\"router\":\"ac-a7-04-ff-fe-bc-e8-f0\",\"uri\":\"wss://lns.example.com/traffic/x\"}");
  station::loop();
  TEST_ASSERT_EQUAL_STRING("lns.example.com:443/traffic/x", net.opened.back().c_str());
  net.handler(hal::SocketEvent::OPENED, nullptr, 0);
  TEST_ASSERT_EQUAL_STRING("version", lastSent()["msgtype"] | "");
  TEST_ASSERT_EQUAL_STRING("test-model", lastSent()["model"] | "");

  net.deliver("{\"msgtype\":\"router_config\",\"region\":\"US902\"}");
  TEST_ASSERT_EQUAL_STRING("connected", station::linkState().c_str());
}

void test_forwards_a_frame_where_the_radio_reports_it(void) {
  radio::Heard h = {};
  // An unconfirmed data up: DevAddr 01020304, FCnt 9, port 10, 5 bytes.
  const uint8_t frame[] = {0x40, 0x04, 0x03, 0x02, 0x01, 0x00, 0x09, 0x00, 0x0a,
                           0x11, 0x22, 0x33, 0x44, 0x55, 0xde, 0xad, 0xbe, 0xef};
  memcpy(h.frame, frame, sizeof frame);
  h.len = sizeof frame;
  h.atUs = 123456789;
  h.rssi = -60;
  gw.toHear.push_back(h);
  station::loop();
  JsonDocument up = lastSent();
  TEST_ASSERT_EQUAL_STRING("updf", up["msgtype"] | "");
  TEST_ASSERT_EQUAL_INT(0x01020304, up["DevAddr"] | 0);
  TEST_ASSERT_EQUAL_INT(9, up["FCnt"] | 0);
  TEST_ASSERT_EQUAL_INT(10, up["FPort"] | 0);
  TEST_ASSERT_EQUAL_STRING("1122334455", up["FRMPayload"] | "");
  TEST_ASSERT_EQUAL_INT(3, up["DR"] | 0);
  TEST_ASSERT_EQUAL_UINT32(903900000, up["Freq"] | 0u);
  uint64_t xtime = up["upinfo"]["xtime"] | (uint64_t)0;
  TEST_ASSERT_EQUAL_UINT8(0x43, xtime >> 56);  // this connection's session byte
  TEST_ASSERT_EQUAL_UINT64(123456789, xtime & 0xffffffffffffULL);
  TEST_ASSERT_EQUAL_INT(1, station::counters().up);
}

void test_hands_a_downlink_to_the_radio_with_its_window(void) {
  uint64_t xtime = (uint64_t)0x43 << 56 | 123456789;
  net.deliver("{\"msgtype\":\"dnmsg\",\"DevEui\":\"02-a1-b2-c3-d4-e5-f6-07\",\"dC\":0,\"diid\":7,"
              "\"pdu\":\"6004030201\",\"RxDelay\":5,\"RX1DR\":13,\"RX1Freq\":923300000,"
              "\"RX2DR\":8,\"RX2Freq\":923300000,\"xtime\":" +
              std::to_string(xtime) + "}");
  TEST_ASSERT_EQUAL_UINT(1, gw.queued.size());
  const radio::DownlinkRequest& d = gw.queued.back();
  TEST_ASSERT_EQUAL_UINT(5, d.len);
  TEST_ASSERT_EQUAL_UINT32(7, d.diid);
  TEST_ASSERT_EQUAL_INT(5, d.rxDelayS);
  TEST_ASSERT_EQUAL_UINT64(123456789, d.uplinkUs);
  TEST_ASSERT_EQUAL_INT(13, d.dr[0]);
  TEST_ASSERT_EQUAL_UINT32(923300000, d.hz[1]);

  // One for another connection is not this one's to send.
  net.deliver("{\"msgtype\":\"dnmsg\",\"dC\":0,\"diid\":8,\"pdu\":\"60\",\"xtime\":5}");
  TEST_ASSERT_EQUAL_UINT(1, gw.queued.size());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_programs_and_connects_through_discovery);
  RUN_TEST(test_forwards_a_frame_where_the_radio_reports_it);
  RUN_TEST(test_hands_a_downlink_to_the_radio_with_its_window);
  return UNITY_END();
}
