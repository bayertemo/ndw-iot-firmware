#include "ndw_radio_ble.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "ndw_ble_frame.h"

#if !CONFIG_BT_NIMBLE_EXT_ADV
#error "NDW's BLE radio needs -DCONFIG_BT_NIMBLE_EXT_ADV=1 in the board's build_flags"
#endif

namespace ndw {
namespace radio {

namespace {

// Each frame is advertised for this long, every 20 ms: a dozen copies or so,
// so one gets through a scanner sharing its radio with WiFi.
const uint32_t ADVERTISE_MS = 300;
const uint16_t ADVERTISE_INTERVAL = 32;  // 0.625 ms units

// How long a device listens after RX1 would have opened: RX1 and RX2, a
// second each, less nothing — it is the device's promise the gateway keeps.
const int64_t LISTEN_AFTER_RX1_US = 2000000;

// The gateway advertises a downlink until the device stops listening, less
// the time the device's own advertising took before the gateway heard it.
const int64_t GATEWAY_AFTER_RX1_US = 1700000;
// And at most this long: the device takes the first copy it hears.
const uint32_t DOWNLINK_MAX_MS = 1500;

// The gateway scans 60 ms of every 80, leaving the rest to WiFi; a device,
// with no WiFi, scans all the time it listens.
const uint16_t GATEWAY_SCAN_INTERVAL_MS = 80, GATEWAY_SCAN_WINDOW_MS = 60;
const uint16_t DEVICE_SCAN_MS = 30;

// A frame seen again within this long is a repeat of the same broadcast.
const uint32_t REPEAT_MS = 10000;

bool started = false;

void startBle() {
  if (started) return;
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  started = true;
}

NimBLEExtAdvertising* advertiser() { return NimBLEDevice::getAdvertising(); }

bool startAdvertising(uint8_t format, const uint8_t* frame, size_t len) {
  uint8_t data[ble::HEADER_LEN + ble::MAX_FRAME];
  size_t n = ble::wrap(format, frame, len, data);
  if (!n) return false;
  NimBLEExtAdvertisement ad;
  ad.setLegacyAdvertising(false);
  ad.setConnectable(false);
  ad.setScannable(false);
  ad.setMinInterval(ADVERTISE_INTERVAL);
  ad.setMaxInterval(ADVERTISE_INTERVAL);
  ad.setManufacturerData(std::string((const char*)data, n));
  return advertiser()->setInstanceData(0, ad) && advertiser()->start(0);
}

// A frame as the scanner hands it over, on NimBLE's task: copied into a
// queue for whoever is waiting on it.
struct Caught {
  uint8_t frame[ble::MAX_FRAME];
  uint8_t len;
  int8_t rssi;
  int64_t atUs;
};

class Catcher : public NimBLEAdvertisedDeviceCallbacks {
 public:
  uint8_t format = ble::FORMAT_UPLINK;
  QueueHandle_t queue = nullptr;
  uint32_t dropped = 0;

  void onResult(NimBLEAdvertisedDevice* device) override {
    if (!queue || !device->haveManufacturerData()) return;
    std::string data = device->getManufacturerData();
    size_t len;
    const uint8_t* frame = ble::unwrap(format, (const uint8_t*)data.data(), data.size(), len);
    if (!frame) return;
    Caught c;
    c.atUs = esp_timer_get_time();
    memcpy(c.frame, frame, len);
    c.len = len;
    c.rssi = device->getRSSI();
    if (xQueueSend(queue, &c, 0) != pdTRUE) dropped++;
  }
};
Catcher catcher;

// Stops and restarts the scan without NimBLE-Arduino's stop() and a fresh
// start(), both of which clear its results list from this task while
// NimBLE's own may still be delivering one — the race that hung a fleet that
// stopped and started around every uplink. The host call is safe from any
// task, and a continued start clears nothing; with no results kept, there is
// nothing to clear.
void cancelScan() { ble_gap_disc_cancel(); }

void resumeScan() {
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (!scan->isScanning()) scan->start(0, nullptr, true);
}

void startScanning(uint8_t format, uint16_t intervalMs, uint16_t windowMs) {
  if (!catcher.queue) catcher.queue = xQueueCreate(32, sizeof(Caught));
  catcher.format = format;
  NimBLEScan* scan = NimBLEDevice::getScan();
  // Every advertisement, repeats included: the controller's own filter goes
  // by address, and one board sends a whole fleet's frames from one.
  scan->setAdvertisedDeviceCallbacks(&catcher, true);
  scan->setActiveScan(false);  // a broadcast has no scan response
  scan->setInterval(intervalMs);
  scan->setWindow(windowMs);
  scan->setMaxResults(0);  // nothing kept: each is handled as it arrives
  scan->start(0, nullptr, false);
}

}  // namespace

// ---- an end device ------------------------------------------------------------

// A device scans only while it listens: scanning while it advertises takes
// the one radio from its own uplinks, and a gateway hears almost none of
// them. So the scan is set up once, and cancelled and resumed around each
// receive window (cancelScan, resumeScan).
bool BleEndDevice::begin() {
  startBle();
  startScanning(ble::FORMAT_DOWNLINK, DEVICE_SCAN_MS, DEVICE_SCAN_MS);
  cancelScan();
  return true;
}

uint64_t BleEndDevice::send(const uint8_t* frame, size_t len, bool join) {
  (void)join;
  txStartUs_ = esp_timer_get_time();
  if (!startAdvertising(ble::FORMAT_UPLINK, frame, len)) return 0;
  delay(ADVERTISE_MS);
  advertiser()->stop(0);
  sent_++;
  return esp_timer_get_time();
}

size_t BleEndDevice::receive(const RxWindows& w, const Accept& accept, uint8_t* out, size_t cap, RxInfo& info) {
  // From the uplink until RX2 would have closed, measured from when the
  // advertising began: the gateway may have heard the first copy.
  int64_t until = (int64_t)txStartUs_ + (int64_t)w.rxDelayS * 1000000 + LISTEN_AFTER_RX1_US;
  Caught c;
  // Anything caught before the uplink was sent is not its answer.
  while (xQueueReceive(catcher.queue, &c, 0) == pdTRUE) {
  }
  resumeScan();
  size_t got = 0;
  while (!got && esp_timer_get_time() < until) {
    if (xQueueReceive(catcher.queue, &c, pdMS_TO_TICKS(20)) != pdTRUE) continue;
    heard_++;
    if (c.len > cap || !accept(c.frame, c.len)) continue;
    memcpy(out, c.frame, c.len);
    info.rssi = c.rssi;
    info.snr = 0;
    got = c.len;
  }
  cancelScan();
  return got;
}

void BleEndDevice::describe(JsonObject out) {
  out["link"] = "ble";
  out["sent"] = sent_;
  out["heard"] = heard_;
}

// ---- a gateway -------------------------------------------------------------------

namespace {

struct Seen {
  uint32_t hash;
  uint32_t at;
};
Seen seen[64];
uint8_t seenNext = 0;

uint32_t fnv1a(const uint8_t* b, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
  return h;
}

// The first copy of each broadcast goes up; the rest are dropped here. Left
// to the server, a copy that arrived after its deduplication window would
// read as a replayed counter.
bool isRepeat(const uint8_t* frame, size_t len) {
  uint32_t h = fnv1a(frame, len), now = millis();
  for (const Seen& s : seen)
    if (s.at && s.hash == h && now - s.at < REPEAT_MS) return true;
  seen[seenNext] = {h, now ? now : 1};
  seenNext = (seenNext + 1) % (sizeof(seen) / sizeof(seen[0]));
  return false;
}

struct Pending {
  bool used;
  DownlinkRequest request;
  int64_t untilUs;  // when the device stops listening
};
Pending pending[4];
Pending* sending = nullptr;
uint32_t sendingUntilMs = 0;

struct {
  uint32_t heard = 0;    // frames off the air, repeats included
  uint32_t repeats = 0;  // copies of one already sent
} stats;

}  // namespace

bool BleGateway::begin() {
  startBle();
  startScanning(ble::FORMAT_UPLINK, GATEWAY_SCAN_INTERVAL_MS, GATEWAY_SCAN_WINDOW_MS);
  return true;
}

bool BleGateway::heard(Heard& out) {
  Caught c;
  while (catcher.queue && xQueueReceive(catcher.queue, &c, 0) == pdTRUE) {
    stats.heard++;
    if (isRepeat(c.frame, c.len)) {
      stats.repeats++;
      continue;
    }
    memcpy(out.frame, c.frame, c.len);
    out.len = c.len;
    out.atUs = (uint64_t)c.atUs;
    out.rssi = c.rssi;
    out.snr = 9.0f;  // BLE has none; ChirpStack reads it for ADR, which these devices do not use
    return true;
  }
  // The scan stops itself now and then (a controller reset, a WiFi scan);
  // it is started again rather than left off, unless it was paused.
  if (!paused_) resumeScan();
  return false;
}

bool BleGateway::queue(const DownlinkRequest& request) {
  // Class A only: B and C listen at times BLE devices do not.
  if (request.deviceClass != 0 || request.len > ble::MAX_FRAME) return false;
  for (auto& p : pending) {
    if (p.used) continue;
    p.request = request;
    p.untilUs = (int64_t)request.uplinkUs + (int64_t)request.rxDelayS * 1000000 + GATEWAY_AFTER_RX1_US;
    p.used = true;
    return true;
  }
  return false;
}

bool BleGateway::service(DownlinkOutcome& done) {
  if (sending) {
    if ((int32_t)(millis() - sendingUntilMs) < 0) return false;
    advertiser()->stop(0);
    done = {sending->request.diid, sending->request.devEui, sending->request.xtime, true};
    sending->used = false;
    sending = nullptr;
    return true;
  }
  int64_t now = esp_timer_get_time();
  for (auto& p : pending) {
    if (!p.used) continue;
    // Sent the moment it is known, for as long as the device can still be
    // listening; one that came after that is late.
    if (now >= p.untilUs || !startAdvertising(ble::FORMAT_DOWNLINK, p.request.pdu, p.request.len)) {
      done = {p.request.diid, p.request.devEui, p.request.xtime, false};
      p.used = false;
      return true;
    }
    uint32_t forMs = (uint32_t)((p.untilUs - now) / 1000);
    sendingUntilMs = millis() + (forMs < DOWNLINK_MAX_MS ? forMs : DOWNLINK_MAX_MS);
    sending = &p;
    return false;
  }
  return false;
}

void BleGateway::pause(bool paused) {
  paused_ = paused;
  if (paused) {
    cancelScan();
  } else {
    resumeScan();
  }
}

void BleGateway::describe(JsonObject out) {
  out["link"] = "ble";
  out["band"] = "US915";
  out["channel"] = 8;
  out["freqMHz"] = reportHz() / 1e6;
  out["dr"] = reportDr();
  out["heard"] = stats.heard;
  out["repeats"] = stats.repeats;
  out["dropped"] = catcher.dropped;
}

}  // namespace radio
}  // namespace ndw
