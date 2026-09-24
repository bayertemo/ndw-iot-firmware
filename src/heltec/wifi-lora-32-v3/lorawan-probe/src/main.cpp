// NDW LoRaWAN meter fleet, for the Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262).
//
// A test device that behaves like a fleet of meters: up to 200 LoRaWAN
// devices over one radio, each joining by OTAA (LoRaWAN 1.0.x, US915
// sub-band 2) and reporting on its own schedule as a water, electricity or
// gas meter, with the daily rhythm real households have and the faults real
// meters show.
//
//   water  port 10  uint32 BE decilitres (running total), uint8 battery %
//                   — MeterFax's ndw-water-v1
//   power  port 11  uint32 BE watt-hours (running total), uint16 BE watts
//                   — MeterFax's ndw-power-v1
//   gas    port 12  uint32 BE decilitres (running total), uint8 battery %
//                   — MeterFax's ndw-gas-v1
//
// ## One image, programmed over USB
//
// Nothing about a device is compiled in. After flashing, a browser writes the
// fleet over the same cable, one JSON command per line; every answer is a
// line starting "#NDW " followed by JSON — the protocol the NDW router
// firmware speaks, so the same host code drives both.
//
//   {"cmd":"hello"}        → eui, firmware, kind, state — kind is
//                            "lorawan-probe", the word hosts look for
//   {"cmd":"status"}       → the fleet: size, joined, sent, anomalies, signal
//   {"cmd":"fleet-begin","count":200,"interval":900,"anomalies":20,
//    "epoch":1790000000,"tzOffset":-300}
//   {"cmd":"fleet-add","devices":[["<devEui>","<appKey>","water"|"power"|"gas",
//    "<joinEui>"?],…]}     → a chunk at a time, each answered; the kind is
//                            required, the host deciding the fleet's mix
//   {"cmd":"fleet-commit"} → saves the fleet, reboots, starts joining
//   {"cmd":"lorawan","appKey":"…","devEui":"…"?,"joinEui":"…"?}
//                          → a fleet of one water meter: the board itself
//   {"cmd":"interval","seconds":900}
//   {"cmd":"time","epoch":…,"tzOffset":…}
//   {"cmd":"forget"}       → drops the fleet
//   {"cmd":"reboot"}
//
// AppKeys are never sent back. tzOffset is minutes east of UTC, so the daily
// curves follow the host's local day.
//
// ## One radio, many devices
//
// Each device is an NVS entry holding its keys, its RadioLib nonces and
// session, and its register. Its turn loads that entry into the one RadioLib
// node — beginOTAA with its keys, then its nonces and session, which RadioLib
// restores without a join — and saves it straight back. So frame counters and
// DevNonces never go backwards across a reboot: ChirpStack refuses a reused
// DevNonce, and drops frames whose counter did.
//
// Programming the same DevEUIs again keeps their entries, so a fleet can be
// reprogrammed (a new interval, a changed mix) without every device being
// refused on rejoin. A device left out of a new fleet has its entry deleted;
// adding it back later starts its DevNonces over, and ChirpStack then needs
// that device's nonces flushed.
//
// An uplink holds the radio for about three seconds — the send and both
// receive windows — and a join about seven, so 200 devices take ten minutes
// a round once joined. The interval should be longer than that.
//
// Everything else the firmware prints starts "[probe]" and is for people.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SSD1306Wire.h>
#include <esp_mac.h>
#include <nvs.h>
#include <vector>

#ifndef PROBE_VERSION
#define PROBE_VERSION ""
#endif

// Heltec WiFi LoRa 32 V3: SX1262 on its own SPI pins.
static const int PIN_NSS = 8, PIN_SCK = 9, PIN_MOSI = 10, PIN_MISO = 11;
static const int PIN_RST = 12, PIN_BUSY = 13, PIN_DIO1 = 14;

// The OLED: SSD1306 on I2C, powered through Vext, which is switched on low.
static const int PIN_OLED_SDA = 17, PIN_OLED_SCL = 18, PIN_OLED_RST = 21;
static const int PIN_VEXT = 36;

// The board's own battery, through a 390k/100k divider on GPIO1, switched by
// GPIO37 — whose polarity changed between board revisions, so both are tried.
static const int PIN_VBAT = 1, PIN_ADC_CTRL = 37;
static const float VBAT_DIVIDER = 4.9;

static const uint16_t MAX_FLEET = 200;
static const uint32_t INTERVAL_MIN_S = 60, INTERVAL_MAX_S = 86400;

// A device whose join went unanswered tries again after this, then twice as
// long each time up to an hour: one that is not registered yet should not
// starve the rest of the radio.
static const uint32_t JOIN_BACKOFF_S = 120;
static const uint32_t JOIN_BACKOFF_MAX_S = 3600;

// Every CHECK_EVERY-th uplink of a device asks the network to acknowledge it
// (LinkCheckReq). A session the network has forgotten — the device deleted
// and re-added in ChirpStack — otherwise sends into nothing forever; after
// CHECKS_MISSED unanswered in a row the device joins again.
static const uint8_t CHECK_EVERY = 8;
static const uint8_t CHECKS_MISSED = 3;

static const uint32_t MAGIC = 0x4e445746;  // "NDWF"
static const uint16_t VERSION = 1;

enum Kind : uint8_t { WATER = 0, POWER = 1, GAS = 2 };

// What a meter is doing, for a stretch of its reports. One kind of fault:
// consumption far above the household's normal — the pattern MeterFax's
// alerts look for — for a few hours at a time, about cfg.anomalyPct of each
// device's reports in the long run.
enum Anomaly : uint8_t {
  NORMAL = 0,
  EXCESS,  // several times the usual draw, burn or demand (not HIGH: Arduino's pin level)
  ANOMALY_KINDS,
};
static const char* ANOMALY_NAMES[ANOMALY_KINDS] = {"normal", "high"};

// An episode lasts this many reports: an hour to six, at a 15-minute interval.
static const uint8_t EPISODE_MIN = 4, EPISODE_MAX = 24;

// How far above normal an episode runs.
static const float EXCESS_MIN = 4.0f, EXCESS_MAX = 10.0f;

// One device as it is kept in flash.
struct __attribute__((packed)) Record {
  uint32_t magic;
  uint16_t version;
  uint64_t devEui;
  uint64_t joinEui;
  uint8_t appKey[16];
  uint8_t kind;
  float scale;  // how big a user this household is, around 1
  uint8_t joined;
  uint8_t haveNonces;
  uint8_t haveSession;
  uint8_t nonces[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
  uint8_t session[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
  uint32_t reg;  // decilitres or watt-hours: only ever rises
  uint16_t demandW;
  uint8_t battery;
  uint8_t anomaly;
  uint16_t anomalyLeft;
  uint16_t joinFails;
  uint32_t uplinks;
  uint8_t missedChecks;
};

// What the scheduler keeps of each device, so only the one whose turn it is
// has to be in memory whole.
struct Slot {
  uint64_t devEui;
  uint32_t dueAt;   // millis
  uint32_t lastAt;  // millis of the last reading, for the use since
  uint8_t kind;
  uint8_t joined;
  uint8_t anomaly;
};

struct __attribute__((packed)) Config {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t intervalS;
  int32_t tzOffsetMin;
  uint8_t anomalyPct;
  uint32_t epoch;
};

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, SPI);

// Sub-band 2, which is what the NDW gateway bridge serves.
LoRaWANNode node(&radio, &US915, 2);

SSD1306Wire oled(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);

Config cfg = {MAGIC, VERSION, 0, 900, 0, 20, 0};
Slot* slots = nullptr;
uint16_t fleetSize = 0;
Record rec;  // the device whose turn it is

// Wall-clock time: an epoch at some millis — from the host when programmed,
// from the network's DeviceTimeAns once one arrives, or carried from the last
// save across a reboot.
uint32_t epochAt = 0, epochMillis = 0;
const char* clockSource = "none";

// A fleet being received from the host. While it is, the running fleet stops.
bool staging = false;
uint64_t* stagingList = nullptr;
uint16_t stagingExpected = 0, stagingCount = 0;
Config stagingCfg;

// What the screen and status show.
struct {
  String line = "starting";
  String error = "";
  uint32_t sent = 0;
  uint32_t acked = 0;
  bool haveSignal = false;
  float rssi = 0;
  float snr = 0;
  uint16_t batteryMv = 0;
  uint8_t batteryPct = 0;
  uint32_t lastCfgSave = 0;
} view;

// ---- small helpers -------------------------------------------------------

String firmware() {
  String v = PROBE_VERSION;
  return "ndw-lorawan-meter-fleet/" + (v.length() ? v : String("dev"));
}

String hex64(uint64_t v) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", v);
  return String(buf);
}

// Hex of exactly `bytes` bytes, colons and spaces allowed, into out.
bool parseHex(const char* s, uint8_t* out, size_t bytes) {
  size_t n = 0;
  int hi = -1;
  for (; *s; s++) {
    char c = *s;
    if (c == ':' || c == ' ' || c == '-') continue;
    int v = (c >= '0' && c <= '9') ? c - '0'
          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
          : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
    if (v < 0) return false;
    if (hi < 0) {
      hi = v;
    } else {
      if (n >= bytes) return false;
      out[n++] = (uint8_t)(hi << 4 | v);
      hi = -1;
    }
  }
  return hi < 0 && n == bytes;
}

bool parseEui(const char* s, uint64_t* out) {
  uint8_t b[8];
  if (!parseHex(s, b, 8)) return false;
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | b[i];
  *out = v;
  return true;
}

// The MAC widened with FF:FE in the middle: unique per board, and the same
// number tools.sh mac prints. The board's own identity, whatever it carries.
uint64_t boardEui() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  uint8_t e[8] = {m[0], m[1], m[2], 0xff, 0xfe, m[3], m[4], m[5]};
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | e[i];
  return v;
}

// A uniform random number in [lo, hi), from the hardware RNG.
float rnd(float lo, float hi) {
  return lo + (hi - lo) * (esp_random() / 4294967296.0f);
}

// ---- time ----------------------------------------------------------------

uint32_t epochNow() {
  return epochAt ? epochAt + (millis() - epochMillis) / 1000 : 0;
}

void setEpoch(uint32_t epoch, const char* source) {
  epochAt = epoch;
  epochMillis = millis();
  clockSource = source;
}

// Seconds into the local day, and the day of the week (0 is Sunday). Without
// any clock the fleet runs as though it started at noon on a Wednesday.
void localTime(float* hour, int* weekday) {
  uint32_t e = epochNow();
  int64_t local = e ? (int64_t)e + (int64_t)cfg.tzOffsetMin * 60
                    : 3 * 86400 + 12 * 3600 + millis() / 1000;
  int64_t day = local >= 0 ? local / 86400 : (local - 86399) / 86400;
  *hour = (float)(local - day * 86400) / 3600.0f;
  // 1 January 1970 was a Thursday.
  *weekday = (int)(((day + 4) % 7 + 7) % 7);
}

// The day of the local year, 0 to 365, for the season gas heating follows.
// Close enough without leap-year bookkeeping: a day's error moves a season by
// a day. Without any clock, early April — mid-season, neither extreme.
float dayOfYear() {
  uint32_t e = epochNow();
  if (!e) return 95;
  int64_t local = (int64_t)e + (int64_t)cfg.tzOffsetMin * 60;
  return fmodf((float)(local / 86400) , 365.2425f);
}

// ---- how meters behave ---------------------------------------------------

// Relative water use through the day: near nothing overnight, the morning's
// showers, a lull, the evening's cooking, washing and baths.
static const float WATER_HOURS[24] = {
  0.005, 0.003, 0.002, 0.002, 0.003, 0.012, 0.055, 0.095, 0.080, 0.050, 0.040, 0.040,
  0.045, 0.040, 0.035, 0.035, 0.040, 0.055, 0.075, 0.080, 0.070, 0.055, 0.035, 0.018,
};

// Electricity above the base load: low overnight, a morning bump, the long
// evening peak of cooking, lighting and screens.
static const float POWER_HOURS[24] = {
  0.25, 0.20, 0.18, 0.18, 0.20, 0.30, 0.55, 0.75, 0.65, 0.50, 0.45, 0.45,
  0.50, 0.45, 0.45, 0.50, 0.60, 0.85, 1.00, 1.00, 0.95, 0.80, 0.55, 0.35,
};

// About 350 L a day for a household, before its own scale.
static const float WATER_LITRES_PER_DAY = 350.0f;

// Gas through the day: the heating's morning run from before six, a midday
// low with the thermostat set back, the evening's heating and cooking, and a
// night setback that still burns a little. Relative, not a share.
static const float GAS_HOURS[24] = {
  0.25, 0.20, 0.20, 0.20, 0.30, 0.70, 1.00, 1.00, 0.80, 0.50, 0.35, 0.35,
  0.40, 0.35, 0.35, 0.40, 0.55, 0.85, 1.00, 0.95, 0.85, 0.70, 0.50, 0.35,
};

// About 2.5 m³ a day across a year, before the season and the household:
// heating makes the winter several times the summer, when only hot water and
// cooking burn any.
static const float GAS_LITRES_PER_DAY = 2500.0f;

float gasDaySum() {
  float s = 0;
  for (float v : GAS_HOURS) s += v;
  return s;
}

float waterDaySum() {
  float s = 0;
  for (float v : WATER_HOURS) s += v;
  return s;
}

// Starts, continues or ends a high-consumption episode. Episodes start with
// the probability that makes a device anomalous for about cfg.anomalyPct of
// its reports in the long run: a share r with episodes of mean length L starts
// one at p = r / (L·(1 − r)) per normal report.
void stepAnomaly(Record& d) {
  // A device recorded mid-fault by 0.3.0, whose faults were other kinds,
  // resumes normal: its old kind means nothing here.
  if (d.anomaly >= ANOMALY_KINDS) d.anomaly = NORMAL;
  if (d.anomaly != NORMAL) {
    if (d.anomalyLeft > 0) d.anomalyLeft--;
    if (d.anomalyLeft == 0) d.anomaly = NORMAL;
    return;
  }
  float r = min(cfg.anomalyPct, (uint8_t)90) / 100.0f;
  if (r <= 0) return;
  float meanLen = (EPISODE_MIN + EPISODE_MAX) / 2.0f;
  if (rnd(0, 1) >= r / (meanLen * (1 - r))) return;
  d.anomaly = EXCESS;
  d.anomalyLeft = EPISODE_MIN + esp_random() % (EPISODE_MAX - EPISODE_MIN + 1);
}

/** The factor an episode puts on consumption, drawn afresh each report. */
float episode(const Record& d) {
  return d.anomaly == EXCESS ? rnd(EXCESS_MIN, EXCESS_MAX) : 1.0f;
}

// Advances the register over the hours since the last reading.
void consume(Record& d, float hours) {
  float hour;
  int weekday;
  localTime(&hour, &weekday);
  int h = (int)hour % 24;
  bool weekend = weekday == 0 || weekday == 6;

  if (d.kind == WATER) {
    // Water is drawn, not trickled: a shower, a flush, a kettle filled. The
    // chance of any draw in an interval follows the hour, and a draw carries
    // what the hour would average — so the busy hours are steady and the
    // night is mostly empty intervals, as a real register reads.
    float share = WATER_HOURS[h] / waterDaySum();
    float expected = WATER_LITRES_PER_DAY * d.scale * (weekend ? 1.15f : 1.0f) * share * hours;
    float draws = share * 24 * 1.5f * hours;
    float p = 1 - expf(-draws);
    // An episode draws in every interval, not only the busy ones: a hose
    // left running, a toilet that does not stop.
    float litres = d.anomaly == EXCESS ? expected * episode(d)
                   : rnd(0, 1) < p   ? expected / p * rnd(0.5f, 1.5f)
                                     : 0;
    d.reg += (uint32_t)(litres * 10.0f + 0.5f);
    // A meter's cell loses a percent every week or so.
    if (rnd(0, 1) < hours / 168.0f && d.battery > 5) d.battery--;
  } else if (d.kind == GAS) {
    // Burnt, not drawn: a furnace cycling through the hour rather than draws
    // of a few litres, so every interval in a heating hour moves the register
    // and the spread is the cycling. The season is most of it — mid-January
    // near its peak, mid-July near the floor, northern hemisphere.
    float season = 1.0f + 0.8f * cosf(2.0f * PI * (dayOfYear() - 15) / 365.25f);
    float share = GAS_HOURS[h] / gasDaySum();
    float litres = GAS_LITRES_PER_DAY * d.scale * season * (weekend ? 1.1f : 1.0f) * share * hours *
                   rnd(0.3f, 1.7f) * episode(d);
    d.reg += (uint32_t)(litres * 10.0f + 0.5f);
    if (rnd(0, 1) < hours / 168.0f && d.battery > 5) d.battery--;
  } else {
    // A base load that never stops — the fridge, the router — the household's
    // own use through the day on top, and now and then a kettle or an oven.
    float watts = 150.0f * d.scale + 1400.0f * d.scale * (weekend ? 1.1f : 1.0f) * POWER_HOURS[h] * rnd(0.6f, 1.4f);
    if (rnd(0, 1) < 0.08f) watts += rnd(1500.0f, 3000.0f);
    watts *= episode(d);
    d.demandW = (uint16_t)min(watts, 65535.0f);
    d.reg += (uint32_t)(watts * hours + 0.5f);
  }
}

// ---- battery -------------------------------------------------------------

uint16_t readBatteryOnce(int ctrlLevel) {
  digitalWrite(PIN_ADC_CTRL, ctrlLevel);
  delay(10);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_VBAT);
  return (uint16_t)(sum / 8 * VBAT_DIVIDER);
}

// Millivolts at the cell, or 0 where nothing could be read. With the switch
// the wrong way the divider is disconnected and the pin reads near zero.
uint16_t readBattery() {
  pinMode(PIN_ADC_CTRL, OUTPUT);
  uint16_t mv = readBatteryOnce(LOW);
  if (mv < 1000) mv = readBatteryOnce(HIGH);
  return mv < 1000 ? 0 : mv;
}

// A LiPo's charge from its resting voltage: a guide rather than a gauge. On
// USB with no cell fitted the charger holds the pin near 4.2 V: full.
uint8_t batteryPercent(uint16_t mv) {
  static const uint16_t curve[][2] = {
    {4200, 100}, {4100, 90}, {4000, 80}, {3900, 65}, {3800, 50},
    {3700, 35}, {3600, 20}, {3500, 10}, {3400, 5}, {3300, 0},
  };
  if (mv >= curve[0][0]) return 100;
  for (int i = 1; i < 10; i++) {
    if (mv >= curve[i][0]) {
      uint16_t hiMv = curve[i - 1][0], loMv = curve[i][0];
      uint8_t hiPct = curve[i - 1][1], loPct = curve[i][1];
      return loPct + (uint32_t)(mv - loMv) * (hiPct - loPct) / (hiMv - loMv);
    }
  }
  return 0;
}

void sampleBattery() {
  view.batteryMv = readBattery();
  view.batteryPct = view.batteryMv ? batteryPercent(view.batteryMv) : 0;
}

// ---- fleet summary -------------------------------------------------------

uint16_t countJoined() {
  uint16_t n = 0;
  for (uint16_t i = 0; i < fleetSize; i++) n += slots[i].joined;
  return n;
}

uint16_t countKind(Kind k) {
  uint16_t n = 0;
  for (uint16_t i = 0; i < fleetSize; i++) n += slots[i].kind == k;
  return n;
}

uint16_t countAnomaly(Anomaly a) {
  uint16_t n = 0;
  for (uint16_t i = 0; i < fleetSize; i++) n += slots[i].anomaly == a;
  return n;
}

const char* stateName() {
  if (staging) return "programming";
  if (!fleetSize) return "unprovisioned";
  return countJoined() ? "running" : "joining";
}

// ---- screen --------------------------------------------------------------

void draw() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawString(0, 0, "NDW meter fleet");
  oled.setTextAlignment(TEXT_ALIGN_RIGHT);
  oled.drawString(128, 0, view.batteryMv ? String(view.batteryPct) + "%" : "bat ?");
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawHorizontalLine(0, 12, 128);

  if (staging) {
    oled.drawString(0, 14, "Programming");
    oled.drawString(0, 26, String(stagingCount) + " of " + String(stagingExpected) + " devices");
    oled.display();
    return;
  }
  if (!fleetSize) {
    oled.drawString(0, 14, "Not programmed");
    oled.drawString(0, 26, "Board EUI");
    oled.drawString(0, 38, hex64(boardEui()));
    oled.drawString(0, 50, "Program it over USB");
    oled.display();
    return;
  }

  oled.drawString(0, 14, "Fleet " + String(fleetSize) + "  joined " + String(countJoined()));
  oled.drawString(0, 26, view.line);
  oled.drawString(0, 38, "Sent " + String(view.sent) + "  faults " + String(fleetSize - countAnomaly(NORMAL)));
  if (view.error.length()) {
    oled.drawString(0, 50, view.error);
  } else if (view.haveSignal) {
    oled.drawString(0, 50, "RSSI " + String(view.rssi, 0) + "  SNR " + String(view.snr, 1));
  } else {
    oled.drawString(0, 50, "no downlink yet");
  }
  oled.display();
}

void show(const String& line) {
  view.line = line;
  draw();
}

void startScreen() {
  pinMode(PIN_VEXT, OUTPUT);
  digitalWrite(PIN_VEXT, LOW);
  delay(50);
  // The panel does not come up reliably without a reset pulse after power.
  pinMode(PIN_OLED_RST, OUTPUT);
  digitalWrite(PIN_OLED_RST, LOW);
  delay(20);
  digitalWrite(PIN_OLED_RST, HIGH);
  delay(20);
  oled.init();
  // The panel is mounted the other way up on this board.
  oled.flipScreenVertically();
  oled.setContrast(255);
}

// ---- storage -------------------------------------------------------------
//
// NVS, in a partition of its own ("fleet", in partitions.csv): a key per
// device, looked up through an index NVS keeps in RAM, written in a few
// milliseconds and spread across the partition's pages. LittleFS was tried
// first and took 0.6 s to open one file among 200, which made a boot with a
// full fleet take two minutes — long enough for a host to decide nothing
// was answering.

Preferences store;

// NVS keys are at most 15 characters, and a DevEUI in hex is 16: this is the
// same 8 bytes as "d" and 11 characters of base64url.
String recordKey(uint64_t devEui) {
  static const char* ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  char key[13] = {'d'};
  for (int i = 0; i < 10; i++) key[1 + i] = ALPHABET[(devEui >> (58 - 6 * i)) & 0x3f];
  // Ten characters carry 60 bits; the last holds the low 4, shifted up.
  key[11] = ALPHABET[(devEui & 0x0f) << 2];
  key[12] = 0;
  return String(key);
}

bool readRecord(uint64_t devEui, Record& out) {
  String key = recordKey(devEui);
  if (!store.isKey(key.c_str())) return false;
  bool ok = store.getBytes(key.c_str(), &out, sizeof(Record)) == sizeof(Record);
  return ok && out.magic == MAGIC && out.version == VERSION && out.devEui == devEui;
}

bool writeRecord(const Record& r) {
  return store.putBytes(recordKey(r.devEui).c_str(), &r, sizeof(Record)) == sizeof(Record);
}

bool writeConfig(const Config& c) {
  Config out = c;
  out.epoch = epochNow();
  view.lastCfgSave = millis();
  return store.putBytes("cfg", &out, sizeof(out)) == sizeof(out);
}

bool writeList(const uint64_t* euis, uint16_t count) {
  return store.putBytes("list", euis, count * sizeof(uint64_t)) == count * sizeof(uint64_t);
}

// Spreads the fleet's reports across the interval rather than bunching them:
// device i is due i/N of the way through, and joins go out in the same order.
void schedule() {
  uint32_t now = millis();
  for (uint16_t i = 0; i < fleetSize; i++) {
    slots[i].dueAt = now + 3000 + (uint32_t)((uint64_t)cfg.intervalS * 1000 * i / fleetSize);
    slots[i].lastAt = now;
  }
}

bool loadFleet() {
  Config c;
  if (store.getBytes("cfg", &c, sizeof(c)) != sizeof(c) || c.magic != MAGIC || c.version != VERSION ||
      c.count == 0 || c.count > MAX_FLEET) {
    return false;
  }
  uint64_t* euis = (uint64_t*)calloc(c.count, sizeof(uint64_t));
  if (store.getBytes("list", euis, c.count * sizeof(uint64_t)) != c.count * sizeof(uint64_t)) {
    free(euis);
    return false;
  }

  slots = (Slot*)calloc(c.count, sizeof(Slot));
  uint16_t n = 0;
  for (uint16_t i = 0; i < c.count; i++) {
    if (!readRecord(euis[i], rec)) {
      Serial.printf("[probe] no record for %s, skipped\n", hex64(euis[i]).c_str());
      continue;
    }
    slots[n].devEui = euis[i];
    slots[n].kind = rec.kind;
    slots[n].joined = rec.joined;
    slots[n].anomaly = rec.anomaly < ANOMALY_KINDS ? rec.anomaly : NORMAL;
    n++;
  }
  free(euis);
  cfg = c;
  fleetSize = n;
  // The clock carries on from the last save; the network corrects it.
  if (c.epoch) setEpoch(c.epoch, "saved");
  schedule();
  return n > 0;
}

// Deletes the records of devices no longer in the fleet — including any a
// programming left behind when it was abandoned before its commit.
void prune(const uint64_t* keep, uint16_t count) {
  std::vector<String> drop;
  nvs_iterator_t it = nvs_entry_find("fleet", "fleet", NVS_TYPE_BLOB);
  while (it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    if (info.key[0] == 'd') {
      bool kept = false;
      for (uint16_t i = 0; i < count && !kept; i++) kept = recordKey(keep[i]) == info.key;
      if (!kept) drop.push_back(String(info.key));
    }
    it = nvs_entry_next(it);
  }
  for (const String& key : drop) store.remove(key.c_str());
}

// Firmware 0.2.0 kept the fleet in LittleFS. A board still carrying one has
// it copied here once — slowly, at LittleFS's pace, which is the reason for
// the move — so its devices keep their DevNonces and sessions, and the old
// files are then wiped.
void importLittleFs() {
  if (!LittleFS.begin(false)) return;
  File f = LittleFS.open("/fleet.cfg", "r");
  Config c;
  bool ok = f && f.read((uint8_t*)&c, sizeof(c)) == sizeof(c) && c.magic == MAGIC && c.version == VERSION &&
            c.count > 0 && c.count <= MAX_FLEET;
  if (f) f.close();
  if (ok) {
    show("upgrading storage");
    Serial.printf("[probe] moving %u devices out of LittleFS, once\n", c.count);
    uint64_t* euis = (uint64_t*)calloc(c.count, sizeof(uint64_t));
    f = LittleFS.open("/fleet.lst", "r");
    ok = f && f.read((uint8_t*)euis, c.count * sizeof(uint64_t)) == c.count * sizeof(uint64_t);
    if (f) f.close();
    for (uint16_t i = 0; ok && i < c.count; i++) {
      f = LittleFS.open("/d/" + hex64(euis[i]), "r");
      if (f && f.read((uint8_t*)&rec, sizeof(Record)) == sizeof(Record) && rec.devEui == euis[i]) writeRecord(rec);
      if (f) f.close();
      if (i % 20 == 19) show("upgrading " + String(i + 1) + "/" + String(c.count));
    }
    if (ok) {
      epochAt = 0;
      ok = writeList(euis, c.count) && store.putBytes("cfg", &c, sizeof(c)) == sizeof(c);
    }
    free(euis);
  }
  LittleFS.end();
  if (ok) {
    LittleFS.format();
    Serial.println("[probe] storage upgraded");
  }
}

// The single-device probe before that kept its keys in Preferences, in the
// default NVS partition. A board still carrying them becomes a fleet of one
// with the same keys, nonces and register — so it rejoins on the next
// DevNonce, rather than one ChirpStack has already seen, and its total
// carries on rather than restarting.
void migrate() {
  Preferences old;
  if (!old.begin("lorawan", false)) return;
  if (!old.getBool("prov", false)) {
    old.end();
    return;
  }
  memset(&rec, 0, sizeof(rec));
  rec.magic = MAGIC;
  rec.version = VERSION;
  rec.devEui = old.getULong64("deveui", 0);
  if (!rec.devEui) rec.devEui = boardEui();
  rec.joinEui = old.getULong64("joineui", 0);
  old.getBytes("appkey", rec.appKey, 16);
  rec.kind = WATER;
  rec.scale = 1.0f;
  rec.reg = old.getUInt("dl", 0);
  rec.battery = 100;
  rec.haveNonces = old.getBytes("nonces", rec.nonces, sizeof(rec.nonces)) == sizeof(rec.nonces);

  Config c = {MAGIC, VERSION, 1, old.getUShort("interval", 60), 0, 0, 0};
  if (c.intervalS < INTERVAL_MIN_S) c.intervalS = INTERVAL_MIN_S;
  if (writeRecord(rec) && writeList(&rec.devEui, 1) && writeConfig(c)) {
    old.clear();
    Serial.printf("[probe] kept %s from the single-device firmware\n", hex64(rec.devEui).c_str());
  }
  old.end();
}

// ---- LoRaWAN, one device at a time --------------------------------------

// Loads rec into the one RadioLib node: its keys, then the nonces and
// session it had, so a joined device resumes rather than joining again.
//
// beginOTAA does not clear the previous device's session, and restored nonces
// carry an "active" flag that makes activateOTAA report a restore whether or
// not a session was loaded — so the session is cleared first, and again for
// a device that should join.
void load() {
  node.clearSession();
  node.beginOTAA(rec.joinEui, rec.devEui, nullptr, rec.appKey);
  if (rec.haveNonces && node.setBufferNonces(rec.nonces) != RADIOLIB_ERR_NONE) rec.haveNonces = false;
  if (rec.joined && rec.haveSession && rec.haveNonces && node.setBufferSession(rec.session) == RADIOLIB_ERR_NONE) {
    return;
  }
  rec.joined = false;
  rec.haveSession = false;
  node.clearSession();
}

void keep() {
  memcpy(rec.nonces, node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  rec.haveNonces = true;
  if (rec.joined) {
    memcpy(rec.session, node.getBufferSession(), RADIOLIB_LORAWAN_SESSION_BUF_SIZE);
    rec.haveSession = true;
  }
  if (!writeRecord(rec)) view.error = "flash write failed";
}

void join(Slot& s, uint16_t i) {
  show("joining #" + String(i + 1));
  int16_t rc = node.activateOTAA();
  if (rc == RADIOLIB_LORAWAN_NEW_SESSION) {
    rec.joined = true;
    rec.joinFails = 0;
    rec.missedChecks = 0;
    s.dueAt = millis() + 5000;
    Serial.printf("[probe] #%u %s joined\n", i + 1, hex64(rec.devEui).c_str());
    view.error = "";
  } else {
    rec.joinFails++;
    uint32_t wait = min(JOIN_BACKOFF_MAX_S, JOIN_BACKOFF_S << min<uint16_t>(rec.joinFails - 1, 10));
    s.dueAt = millis() + wait * 1000;
    Serial.printf("[probe] #%u %s join failed: %d, again in %lus\n", i + 1, hex64(rec.devEui).c_str(), rc,
                  (unsigned long)wait);
    view.error = "#" + String(i + 1) + " join " + String(rc);
  }
  s.joined = rec.joined;
  keep();
}

void report(Slot& s, uint16_t i) {
  uint32_t now = millis();
  float hours = (now - s.lastAt) / 3600000.0f;
  s.lastAt = now;
  s.dueAt = now + cfg.intervalS * 1000;

  stepAnomaly(rec);
  consume(rec, hours);
  s.anomaly = rec.anomaly;

  uint8_t payload[6] = {(uint8_t)(rec.reg >> 24), (uint8_t)(rec.reg >> 16), (uint8_t)(rec.reg >> 8), (uint8_t)rec.reg};
  size_t len;
  uint8_t port;
  if (rec.kind == WATER || rec.kind == GAS) {
    // A fleet of one is this board, so it reports this board's battery.
    if (fleetSize == 1) {
      sampleBattery();
      rec.battery = view.batteryPct;
    }
    payload[4] = rec.battery;
    len = 5;
    port = rec.kind == GAS ? 12 : 10;
  } else {
    payload[4] = rec.demandW >> 8;
    payload[5] = rec.demandW;
    len = 6;
    port = 11;
  }

  // Ask the network the time until it has answered once: the daily curves
  // need the local hour, and a board that rebooted away from the browser has
  // only the clock it last saved.
  bool askTime = strcmp(clockSource, "network") != 0;
  if (askTime) node.sendMacCommandReq(RADIOLIB_LORAWAN_MAC_DEVICE_TIME);
  bool check = ++rec.uplinks % CHECK_EVERY == 0;
  if (check) node.sendMacCommandReq(RADIOLIB_LORAWAN_MAC_LINK_CHECK);

  show("sending #" + String(i + 1));
  uint8_t down[64];
  size_t downLen = sizeof(down);
  int16_t rc = node.sendReceive(payload, len, port, down, &downLen);
  view.sent++;

  if (rc < RADIOLIB_ERR_NONE) {
    Serial.printf("[probe] #%u uplink failed: %d\n", i + 1, rc);
    view.error = "#" + String(i + 1) + " uplink " + String(rc);
  } else {
    view.error = "";
    if (rc > 0) {
      view.acked++;
      view.haveSignal = true;
      view.rssi = radio.getRSSI();
      view.snr = radio.getSNR();
    }
    if (askTime) {
      uint32_t unix = 0;
      if (node.getMacDeviceTimeAns(&unix, nullptr, true) == RADIOLIB_ERR_NONE && unix > 1600000000UL) {
        setEpoch(unix, "network");
        Serial.printf("[probe] network time %lu\n", (unsigned long)unix);
      }
    }
  }
  if (check) {
    uint8_t margin, gateways;
    if (rc > 0 && node.getMacLinkCheckAns(&margin, &gateways) == RADIOLIB_ERR_NONE) {
      rec.missedChecks = 0;
    } else if (++rec.missedChecks >= CHECKS_MISSED) {
      // The network has stopped answering this device: join again, which
      // either works or backs off like any other unanswered join.
      Serial.printf("[probe] #%u %s unanswered %u times, rejoining\n", i + 1, hex64(rec.devEui).c_str(),
                    rec.missedChecks);
      rec.joined = false;
      rec.haveSession = false;
      rec.missedChecks = 0;
      s.joined = false;
      s.dueAt = millis() + 2000;
    }
  }
  keep();
  show("sent #" + String(i + 1));
}

// ---- the host protocol ---------------------------------------------------

void reply(JsonDocument& doc) {
  Serial.print("#NDW ");
  serializeJson(doc, Serial);
  Serial.println();
}

void refuse(const char* error, const String& detail, const char* field = nullptr) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = error;
  doc["detail"] = detail;
  if (field) {
    JsonObject f = doc["faults"].to<JsonArray>().add<JsonObject>();
    f["field"] = field;
    f["message"] = detail;
  }
  reply(doc);
}

void rebootSoon() {
  Serial.flush();
  delay(300);
  ESP.restart();
}

void describe(JsonDocument& doc) {
  doc["ok"] = true;
  doc["eui"] = hex64(boardEui());
  doc["firmware"] = firmware();
  // What this is, for a host deciding what to offer: a board answering as a
  // LoRaWAN probe takes a fleet; a router does not.
  doc["kind"] = "lorawan-probe";
  doc["state"] = stateName();
  doc["fleet"] = fleetSize;
  doc["maxFleet"] = MAX_FLEET;
  JsonArray caps = doc["capabilities"].to<JsonArray>();
  caps.add("lorawan");
  caps.add("fleet");
}

void setClock(JsonDocument& in, Config& c) {
  uint32_t epoch = in["epoch"] | 0;
  if (epoch > 1600000000UL) setEpoch(epoch, "host");
  if (in["tzOffset"].is<int>()) c.tzOffsetMin = constrain(in["tzOffset"].as<int>(), -14 * 60, 14 * 60);
}

// A device as programmed: the keys it was given, and if the board already
// held this DevEUI with the same keys, everything it had learned — its
// DevNonces above all, since ChirpStack refuses one it has seen.
void stage(uint64_t devEui, uint64_t joinEui, const uint8_t* key, Kind kind) {
  Record old;
  bool same = readRecord(devEui, old) && old.joinEui == joinEui && memcmp(old.appKey, key, 16) == 0;
  if (same && old.kind == kind) return;  // already right as it is

  memset(&rec, 0, sizeof(rec));
  rec.magic = MAGIC;
  rec.version = VERSION;
  rec.devEui = devEui;
  rec.joinEui = joinEui;
  memcpy(rec.appKey, key, 16);
  rec.kind = kind;
  rec.scale = rnd(0.5f, 1.6f);
  rec.battery = (uint8_t)rnd(70, 101);
  // Registers do not start at zero on real meters: a few years of use.
  rec.reg = kind == WATER ? (uint32_t)rnd(2e5f, 2e6f) : kind == GAS ? (uint32_t)rnd(2e5f, 2e7f) : (uint32_t)rnd(5e6f, 4e7f);
  if (same) {
    // The keys are the same device's; only what it pretends to be changed.
    memcpy(rec.nonces, old.nonces, sizeof(rec.nonces));
    memcpy(rec.session, old.session, sizeof(rec.session));
    rec.haveNonces = old.haveNonces;
    rec.haveSession = old.haveSession;
    rec.joined = old.joined;
  }
  writeRecord(rec);
}

bool commit(const uint64_t* euis, uint16_t count, Config& c) {
  c.count = count;
  if (!writeList(euis, count) || !writeConfig(c)) {
    refuse("storage", "The fleet could not be written to flash.");
    return false;
  }
  prune(euis, count);
  JsonDocument out;
  out["ok"] = true;
  out["fleet"] = count;
  out["state"] = "rebooting";
  reply(out);
  Serial.printf("[probe] fleet of %u saved, rebooting to join\n", count);
  rebootSoon();
  return true;
}

bool intervalOk(uint32_t s) {
  return s >= INTERVAL_MIN_S && s <= INTERVAL_MAX_S;
}

void onCommand(const String& line) {
  JsonDocument in;
  if (deserializeJson(in, line)) {
    refuse("bad-json", "That line is not JSON.");
    return;
  }
  String cmd = in["cmd"] | "";

  if (cmd == "hello") {
    JsonDocument out;
    describe(out);
    reply(out);
  } else if (cmd == "status") {
    JsonDocument out;
    describe(out);
    out["band"] = "US915";
    out["subBand"] = 2;
    out["interval"] = cfg.intervalS;
    out["anomalies"] = cfg.anomalyPct;
    out["joined"] = countJoined();
    out["water"] = countKind(WATER);
    out["power"] = countKind(POWER);
    out["gas"] = countKind(GAS);
    JsonObject faults = out["faults"].to<JsonObject>();
    for (int a = EXCESS; a < ANOMALY_KINDS; a++) faults[ANOMALY_NAMES[a]] = countAnomaly((Anomaly)a);
    out["sent"] = view.sent;
    out["acked"] = view.acked;
    out["clock"] = clockSource;
    if (epochAt) out["epoch"] = epochNow();
    if (view.haveSignal) {
      out["rssi"] = view.rssi;
      out["snr"] = view.snr;
    }
    out["batteryPct"] = view.batteryPct;
    out["error"] = view.error;
    reply(out);
  } else if (cmd == "fleet-begin") {
    uint16_t count = in["count"] | 0;
    uint32_t interval = in["interval"] | 900;
    int anomalies = in["anomalies"] | 20;
    if (count < 1 || count > MAX_FLEET) {
      refuse("invalid", "A fleet is 1 to " + String(MAX_FLEET) + " devices.", "count");
      return;
    }
    if (!intervalOk(interval)) {
      refuse("invalid", "The interval must be 60 to 86400 seconds.", "interval");
      return;
    }
    if (anomalies < 0 || anomalies > 90) {
      refuse("invalid", "Anomalies must be 0 to 90 percent.", "anomalies");
      return;
    }
    free(stagingList);
    stagingList = (uint64_t*)calloc(count, sizeof(uint64_t));
    stagingExpected = count;
    stagingCount = 0;
    stagingCfg = cfg;
    stagingCfg.intervalS = interval;
    stagingCfg.anomalyPct = anomalies;
    setClock(in, stagingCfg);
    // The running fleet stops until the new one is committed, or the board
    // restarts on the old one.
    staging = true;
    draw();
    JsonDocument out;
    out["ok"] = true;
    out["expected"] = count;
    reply(out);
  } else if (cmd == "fleet-add") {
    if (!staging) {
      refuse("order", "Send fleet-begin first.");
      return;
    }
    for (JsonArray row : in["devices"].as<JsonArray>()) {
      String which = "Device " + String(stagingCount + 1) + ": ";
      if (stagingCount >= stagingExpected) {
        refuse("invalid", "More devices than fleet-begin announced.");
        return;
      }
      uint64_t devEui, joinEui = 0;
      uint8_t key[16];
      // Each device says what it is: the host decides the fleet's mix.
      String kind = row[2] | "";
      if (!parseEui(row[0] | "", &devEui)) {
        refuse("invalid", which + "the DevEUI must be 16 hex characters.", "devEui");
        return;
      }
      if (!parseHex(row[1] | "", key, 16)) {
        refuse("invalid", which + "the AppKey must be 32 hex characters.", "appKey");
        return;
      }
      const char* j = row[3] | "";
      if (*j && !parseEui(j, &joinEui)) {
        refuse("invalid", which + "the JoinEUI must be 16 hex characters.", "joinEui");
        return;
      }
      if (kind != "water" && kind != "power" && kind != "gas") {
        refuse("invalid", which + "the kind must be water, power or gas.", "kind");
        return;
      }
      for (uint16_t k = 0; k < stagingCount; k++) {
        if (stagingList[k] == devEui) {
          refuse("invalid", which + "the DevEUI " + hex64(devEui) + " is already in the fleet.", "devEui");
          return;
        }
      }
      stage(devEui, joinEui, key, kind == "power" ? POWER : kind == "gas" ? GAS : WATER);
      stagingList[stagingCount++] = devEui;
    }
    draw();
    JsonDocument out;
    out["ok"] = true;
    out["received"] = stagingCount;
    reply(out);
  } else if (cmd == "fleet-commit") {
    if (!staging || stagingCount != stagingExpected) {
      refuse("incomplete", "Received " + String(stagingCount) + " of " + String(stagingExpected) + " devices.");
      return;
    }
    commit(stagingList, stagingCount, stagingCfg);
  } else if (cmd == "lorawan") {
    uint64_t devEui = boardEui(), joinEui = 0;
    uint8_t key[16];
    if (!parseHex(in["appKey"] | "", key, 16)) {
      refuse("invalid", "The AppKey must be 32 hex characters.", "appKey");
      return;
    }
    const char* d = in["devEui"] | "";
    if (*d && !parseEui(d, &devEui)) {
      refuse("invalid", "The DevEUI must be 16 hex characters.", "devEui");
      return;
    }
    const char* j = in["joinEui"] | "";
    if (*j && !parseEui(j, &joinEui)) {
      refuse("invalid", "The JoinEUI must be 16 hex characters.", "joinEui");
      return;
    }
    Config c = cfg;
    setClock(in, c);
    stage(devEui, joinEui, key, WATER);
    // Answered before the reboot with the DevEUI it will join as.
    JsonDocument out;
    out["ok"] = true;
    out["eui"] = hex64(devEui);
    out["state"] = "rebooting";
    c.count = 1;
    if (!writeList(&devEui, 1) || !writeConfig(c)) {
      refuse("storage", "The keys could not be written to flash.");
      return;
    }
    prune(&devEui, 1);
    reply(out);
    rebootSoon();
  } else if (cmd == "interval") {
    uint32_t s = in["seconds"] | 0;
    if (!intervalOk(s)) {
      refuse("invalid", "The interval must be 60 to 86400 seconds.", "seconds");
      return;
    }
    cfg.intervalS = s;
    if (fleetSize) writeConfig(cfg);
    JsonDocument out;
    out["ok"] = true;
    out["interval"] = s;
    reply(out);
  } else if (cmd == "time") {
    setClock(in, cfg);
    if (fleetSize) writeConfig(cfg);
    JsonDocument out;
    out["ok"] = true;
    out["clock"] = clockSource;
    reply(out);
  } else if (cmd == "forget") {
    store.clear();
    JsonDocument out;
    out["ok"] = true;
    out["state"] = "rebooting";
    reply(out);
    rebootSoon();
  } else if (cmd == "reboot") {
    JsonDocument out;
    out["ok"] = true;
    out["state"] = "rebooting";
    reply(out);
    rebootSoon();
  } else {
    refuse("unknown-command", "That command is not one this firmware knows.");
  }
}

// Reads whatever has arrived, and acts on each complete line. Called between
// every radio operation, so the host is answered within seconds even while
// the fleet runs.
void pollSerial() {
  static String pending;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      pending.trim();
      if (pending.length()) onCommand(pending);
      pending = "";
    } else if (pending.length() < 8192) {
      pending += c;
    }
  }
}

// ---- main ----------------------------------------------------------------

void setup() {
  // A fleet-add chunk is a few kilobytes on one line, sent faster than a
  // radio operation takes, so the receive buffer holds a whole one.
  Serial.setRxBufferSize(8192);
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[probe] %s, %u bytes a device\n", firmware().c_str(), sizeof(Record));

  sampleBattery();
  startScreen();

  if (!store.begin("fleet", false, "fleet")) {
    view.error = "no fleet storage";
    Serial.println("[probe] the fleet NVS partition did not open");
  }
  if (!store.isKey("cfg")) importLittleFs();
  if (!store.isKey("cfg")) migrate();

  if (!loadFleet()) {
    Serial.printf("[probe] board %s, not programmed\n", hex64(boardEui()).c_str());
    draw();
    return;
  }
  Serial.printf("[probe] fleet of %u (%u water, %u power, %u gas), %u joined, every %lus\n", fleetSize,
                countKind(WATER), countKind(POWER), countKind(GAS), countJoined(), (unsigned long)cfg.intervalS);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  // 1.8 V on the TCXO, as the board wires it. The frequency here is replaced
  // by the LoRaWAN stack.
  int16_t rc = radio.begin(915.0, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 1.8, false);
  if (rc != RADIOLIB_ERR_NONE) {
    view.error = "radio failed: " + String(rc);
    Serial.printf("[probe] radio.begin: %d\n", rc);
  }
  show("starting");
}

void loop() {
  pollSerial();

  if (staging || !fleetSize) {
    delay(20);
    return;
  }

  // The clock is saved now and then, so a board restarted away from the
  // browser and out of the network's reach still keeps roughly the day.
  if (epochAt && millis() - view.lastCfgSave > 10UL * 60 * 1000) writeConfig(cfg);

  // The device most overdue, joined or not.
  uint32_t now = millis();
  int32_t best = -1, lateness = -1;
  for (uint16_t i = 0; i < fleetSize; i++) {
    int32_t late = (int32_t)(now - slots[i].dueAt);
    if (late >= 0 && late > lateness) {
      lateness = late;
      best = i;
    }
  }
  if (best < 0) {
    static uint32_t lastDraw = 0;
    if (now - lastDraw > 1000) {
      draw();
      lastDraw = now;
    }
    delay(20);
    return;
  }

  Slot& s = slots[best];
  if (!readRecord(s.devEui, rec)) {
    view.error = "#" + String(best + 1) + " record lost";
    s.dueAt = now + cfg.intervalS * 1000;
    return;
  }
  load();
  if (rec.joined) {
    node.activateOTAA();  // restores, without anything on the air
    report(s, best);
  } else {
    join(s, best);
  }
}
