#include "ndw_radio_lora.h"

#include <Arduino.h>
#include <esp_random.h>
#include <esp_timer.h>

namespace ndw {
namespace radio {

namespace {

// LoRaWAN's public sync word, a transmit power for a bench, and 4/5 coding.
const uint8_t SYNC_WORD = 0x34;
const int8_t TX_DBM = 14;
const uint8_t CODING_RATE = 5;
const uint16_t PREAMBLE = 8;

// US915's data rates by index: DR0–4 up, DR8–13 down. A zero spreading
// factor is an index the band does not use.
struct DataRate {
  uint8_t sf;
  float bw;
};
const DataRate US915_DR[14] = {
    {10, 125}, {9, 125}, {8, 125}, {7, 125}, {8, 500}, {0, 0},   {0, 0},
    {0, 0},    {12, 500}, {11, 500}, {10, 500}, {9, 500}, {8, 500}, {7, 500},
};

// RX1's data rate for an uplink's, by RX1 offset (US915, RP002 table).
const uint8_t RX1_DR[5][4] = {{10, 9, 8, 8}, {11, 10, 9, 8}, {12, 11, 10, 9}, {13, 12, 11, 10}, {13, 13, 12, 11}};

// Sub-band 2: channels 8 to 15, 125 kHz, where the NDW gateway bridge listens.
const uint8_t SUB_BAND_FIRST = 8, SUB_BAND_COUNT = 8;

uint32_t uplinkHz(uint8_t ch) { return 902300000 + (uint32_t)ch * 200000; }

// The downlink channel that answers an uplink channel: eight at 600 kHz from
// 923.3 MHz, the uplink's modulo eight.
uint32_t rx1Hz(uint8_t ch) { return 923300000 + (uint32_t)(ch % 8) * 600000; }

// One symbol's time, in microseconds.
uint32_t symbolUs(int dr) { return (uint32_t)((1UL << US915_DR[dr].sf) * 1000.0f / US915_DR[dr].bw); }

volatile bool dio1 = false;
volatile int64_t dio1AtUs = 0;

void IRAM_ATTR onDio1() {
  dio1AtUs = esp_timer_get_time();
  dio1 = true;
}

bool configure(SX1262& chip, uint32_t hz, int dr, bool downlink) {
  if (dr < 0 || dr > 13 || !US915_DR[dr].sf) return false;
  chip.standby();
  chip.setFrequency(hz / 1e6);
  chip.setBandwidth(US915_DR[dr].bw);
  chip.setSpreadingFactor(US915_DR[dr].sf);
  chip.setCodingRate(CODING_RATE);
  chip.setSyncWord(SYNC_WORD);
  chip.setPreambleLength(PREAMBLE);
  // Downlinks go out inverted and without a CRC, so only devices hear them
  // and only gateways hear uplinks.
  chip.invertIQ(downlink);
  chip.setCRC(!downlink);
  return true;
}

// Waits for DIO1 until `untilUs`; true when it rose.
bool waitDio1(int64_t untilUs) {
  while (!dio1 && esp_timer_get_time() < untilUs) {
  }
  return dio1;
}

}  // namespace

// ---- an end device --------------------------------------------------------------

bool LoraEndDevice::begin() {
  chip_.setDio1Action(onDio1);
  chip_.setOutputPower(TX_DBM);
  return true;
}

uint64_t LoraEndDevice::send(const uint8_t* frame, size_t len, bool join) {
  bool pinned = pinned_ != ALL_CHANNELS;
  lastChannel_ = pinned ? pinned_ : SUB_BAND_FIRST + esp_random() % SUB_BAND_COUNT;
  // A join at DR0, the band's longest reach, unless kept to the one data
  // rate a single-channel gateway hears; data at DR3, with ADR off.
  lastDr_ = pinned || !join ? TEST_GATEWAY_DR : 0;
  if (!configure(chip_, uplinkHz(lastChannel_), lastDr_, false)) return 0;
  chip_.setOutputPower(TX_DBM);
  dio1 = false;
  if (chip_.startTransmit(const_cast<uint8_t*>(frame), len) != RADIOLIB_ERR_NONE) return 0;
  // Airtime at SF10 is under half a second; two is room enough.
  if (!waitDio1(esp_timer_get_time() + 2000000)) return 0;
  chip_.finishTransmit();
  // The end of the frame, from the interrupt: RX1 counts from it.
  return (uint64_t)dio1AtUs;
}

size_t LoraEndDevice::receive(const RxWindows& w, const Accept& accept, uint8_t* out, size_t cap, RxInfo& info) {
  struct Window {
    uint32_t hz;
    int dr;
    int64_t atUs;
  } windows[2] = {
      {rx1Hz(lastChannel_), RX1_DR[lastDr_ < 5 ? lastDr_ : 4][w.rx1DrOffset & 3],
       (int64_t)w.txEndUs + (int64_t)w.rxDelayS * 1000000},
      {w.rx2Hz, w.rx2Dr, (int64_t)w.txEndUs + ((int64_t)w.rxDelayS + 1) * 1000000},
  };
  for (const Window& win : windows) {
    if (!configure(chip_, win.hz, win.dr, true)) continue;
    // Opened a little early, and kept open for the preamble and some: the
    // receiver locks on as the preamble comes, and the timer stops once a
    // header is found, so a frame is received whole however long it is.
    uint32_t sym = symbolUs(win.dr);
    int64_t lead = 3 * (int64_t)sym + 2000;
    int64_t openFor = lead + (PREAMBLE + 8) * (int64_t)sym + 5000;
    while (esp_timer_get_time() < win.atUs - lead) {
    }
    dio1 = false;
    chip_.startReceive((uint32_t)(openFor / 15.625));
    // A frame found keeps the radio busy until it ends; three seconds is
    // longer than any downlink at DR8.
    if (!waitDio1(win.atUs + openFor + 3000000)) {
      chip_.standby();
      continue;
    }
    uint16_t irq = chip_.getIrqFlags();
    if (!(irq & RADIOLIB_SX126X_IRQ_RX_DONE)) {
      chip_.standby();
      continue;
    }
    size_t n = chip_.getPacketLength();
    uint8_t buf[256];
    if (n > sizeof(buf)) n = sizeof(buf);
    int16_t rc = chip_.readData(buf, n);
    info.rssi = chip_.getRSSI();
    info.snr = chip_.getSNR();
    chip_.standby();
    if (rc != RADIOLIB_ERR_NONE || n > cap || !accept(buf, n)) continue;
    lastRssi_ = info.rssi;
    lastSnr_ = info.snr;
    haveSignal_ = true;
    memcpy(out, buf, n);
    return n;
  }
  return 0;
}

void LoraEndDevice::describe(JsonObject out) {
  out["link"] = "lora";
  out["band"] = "US915";
  out["subBand"] = 2;
  if (pinned_ != ALL_CHANNELS) {
    out["channel"] = pinned_;
    out["channelMHz"] = uplinkHz(pinned_) / 1e6;
    out["dr"] = TEST_GATEWAY_DR;
  }
  if (haveSignal_) {
    out["rssi"] = lastRssi_;
    out["snr"] = lastSnr_;
  }
}

// ---- a single-channel gateway ------------------------------------------------------

namespace {

// How early the radio is set up for a downlink, and how late one may still
// go: a device's receive window opens for a few symbols, so a frame that is
// not on the air within a few milliseconds of its time is not heard.
const int64_t TX_PREPARE_US = 30000;
const int64_t TX_LATE_US = 5000;

const uint32_t LISTEN_HZ = 903900000;

bool transmitting = false;

void listen(SX1262& chip) {
  configure(chip, LISTEN_HZ, TEST_GATEWAY_DR, false);
  transmitting = false;
  dio1 = false;
  chip.startReceive();
}

struct Pending {
  bool used;
  DownlinkRequest request;
  int64_t at[2];  // when RX1 and RX2 open, on esp_timer's clock; 0 when not offered
};
Pending pending[4];
Pending* sending = nullptr;
uint32_t sendingSince = 0;

}  // namespace

bool LoraGateway::begin() {
  chip_.setDio1Action(onDio1);
  chip_.setOutputPower(TX_DBM);
  listen(chip_);
  return true;
}

bool LoraGateway::heard(Heard& out) {
  if (transmitting || !dio1) return false;
  dio1 = false;
  out.atUs = (uint64_t)dio1AtUs;
  size_t n = chip_.getPacketLength();
  if (n > sizeof(out.frame)) n = sizeof(out.frame);
  int16_t rc = chip_.readData(out.frame, n);
  out.rssi = chip_.getRSSI();
  out.snr = chip_.getSNR();
  chip_.startReceive();
  if (rc != RADIOLIB_ERR_NONE) return false;  // a CRC failure is noise, not a frame
  out.len = n;
  return true;
}

bool LoraGateway::queue(const DownlinkRequest& r) {
  if (r.deviceClass == 1) return false;  // class B needs GPS time, which this does not have
  for (auto& p : pending) {
    if (p.used) continue;
    p.request = r;
    if (r.deviceClass == 2) {
      // Class C: now, on RX2's settings.
      p.at[0] = 0;
      p.at[1] = esp_timer_get_time() + TX_PREPARE_US;
    } else {
      p.at[0] = r.hz[0] ? (int64_t)r.uplinkUs + r.rxDelayS * 1000000LL : 0;
      p.at[1] = r.hz[1] ? (int64_t)r.uplinkUs + (r.rxDelayS + 1) * 1000000LL : 0;
    }
    p.used = true;
    return true;
  }
  return false;
}

// Sends whichever downlink is due into its window, RX1 if it can still make
// it and RX2 if not. Called every loop; waits in place for the last few
// milliseconds, since a window missed by more than a few is not heard.
bool LoraGateway::service(DownlinkOutcome& done) {
  if (sending) {
    if (dio1) {
      dio1 = false;
      chip_.finishTransmit();
      done = {sending->request.diid, sending->request.devEui, sending->request.xtime, true};
      sending->used = false;
      sending = nullptr;
      listen(chip_);
      return true;
    }
    if (millis() - sendingSince > 5000) {
      done = {sending->request.diid, sending->request.devEui, sending->request.xtime, false};
      sending->used = false;
      sending = nullptr;
      listen(chip_);
      return true;
    }
    return false;
  }

  int64_t now = esp_timer_get_time();
  for (auto& p : pending) {
    if (!p.used) continue;
    for (int w = 0; w < 2; w++) {
      if (!p.at[w]) continue;
      if (now > p.at[w] + TX_LATE_US) {
        p.at[w] = 0;  // that window has gone
        continue;
      }
      if (now < p.at[w] - TX_PREPARE_US) break;  // not yet; later windows are later still
      if (!configure(chip_, p.request.hz[w], p.request.dr[w], true)) {
        p.at[w] = 0;
        continue;
      }
      chip_.setOutputPower(TX_DBM);
      while (esp_timer_get_time() < p.at[w]) {
      }
      transmitting = true;
      dio1 = false;
      if (chip_.startTransmit(p.request.pdu, p.request.len) == RADIOLIB_ERR_NONE) {
        sending = &p;
        sendingSince = millis();
        return false;
      }
      done = {p.request.diid, p.request.devEui, p.request.xtime, false};
      p.used = false;
      listen(chip_);
      return true;
    }
    if (!p.at[0] && !p.at[1]) {
      done = {p.request.diid, p.request.devEui, p.request.xtime, false};
      p.used = false;
      return true;
    }
  }
  return false;
}

void LoraGateway::describe(JsonObject out) {
  out["link"] = "lora";
  out["band"] = "US915";
  out["channel"] = TEST_GATEWAY_CHANNEL;
  out["freqMHz"] = LISTEN_HZ / 1e6;
  out["sf"] = US915_DR[TEST_GATEWAY_DR].sf;
  out["dr"] = TEST_GATEWAY_DR;
}

}  // namespace radio
}  // namespace ndw
