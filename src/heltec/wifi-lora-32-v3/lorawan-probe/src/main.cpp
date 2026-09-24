// NDW LoRaWAN probe for the Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262).
//
// A test device that behaves like a water meter. It joins US915 sub-band 2
// (channels 8-15 + 65) by OTAA as a LoRaWAN 1.0.x device and, every
// interval, sends MeterFax's ndw-water-v1 payload on port 10: a big-endian
// uint32 running total in decilitres, then a uint8 battery percent.
//
// ## One image, programmed over USB
//
// Nothing about a device is compiled in, so one published image serves every
// board. Its keys arrive after flashing, over the same USB cable, from a
// browser: the host writes one JSON command per line, and every answer is a
// line starting "#NDW " followed by JSON — the protocol the NDW router
// firmware speaks, so the same host code drives both.
//
//   {"cmd":"hello"}                          → eui, firmware, state
//   {"cmd":"status"}                         → everything the screen shows
//   {"cmd":"lorawan","appKey":"<32 hex>",
//    "devEui":"<16 hex>","joinEui":"<16 hex>"} → saves, then reboots and joins
//   {"cmd":"interval","seconds":60}          → how often to report
//   {"cmd":"forget"}                         → drops the keys, keeps the total
//   {"cmd":"reboot"}
//
// devEui is optional: by default it is the board's MAC widened with FF:FE,
// which is unique without anybody having to choose one. The AppKey is never
// sent back — status says only whether one is set.
//
// Everything else the firmware prints starts "[probe]" and is for people.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SSD1306Wire.h>
#include <esp_mac.h>

#ifndef PROBE_VERSION
#define PROBE_VERSION ""
#endif

// Heltec WiFi LoRa 32 V3: SX1262 on its own SPI pins.
static const int PIN_NSS = 8, PIN_SCK = 9, PIN_MOSI = 10, PIN_MISO = 11;
static const int PIN_RST = 12, PIN_BUSY = 13, PIN_DIO1 = 14;

// The OLED: SSD1306 on I2C, powered through Vext, which is switched on low.
static const int PIN_OLED_SDA = 17, PIN_OLED_SCL = 18, PIN_OLED_RST = 21;
static const int PIN_VEXT = 36;

// The battery, through a 390k/100k divider on GPIO1, switched by GPIO37. The
// switch's polarity changed between board revisions, so readBattery tries
// both rather than guessing which board this is.
static const int PIN_VBAT = 1, PIN_ADC_CTRL = 37;
static const float VBAT_DIVIDER = 4.9;

static const uint32_t JOIN_RETRY_MS = 30000;
static const uint16_t INTERVAL_MIN_S = 15, INTERVAL_MAX_S = 3600;

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, SPI);

// Sub-band 2, which is what the NDW gateway bridge serves.
LoRaWANNode node(&radio, &US915, 2);

SSD1306Wire oled(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);

Preferences store;

// What the board is, as saved in flash.
struct {
  bool provisioned = false;
  uint64_t devEui = 0;
  uint64_t joinEui = 0;
  uint8_t appKey[16] = {0};
  uint16_t intervalS = 60;
} config;

// A running total, like a meter's register: it only goes up, and survives a
// reboot and a re-provisioning. A total that restarts from zero reads to
// MeterFax as a meter running backwards, which it refuses to subtract.
uint32_t decilitres = 0;

enum State { UNPROVISIONED, JOINING, JOINED };
State state = UNPROVISIONED;

// What the screen and status show.
struct {
  String line = "starting";
  String error = "";
  uint32_t sent = 0;
  uint32_t acked = 0;
  uint32_t joinAttempts = 0;
  bool haveSignal = false;
  float rssi = 0;
  float snr = 0;
  uint16_t batteryMv = 0;
  uint8_t batteryPct = 0;
  uint32_t nextAt = 0;
} view;

// ---- small helpers -------------------------------------------------------

String firmware() {
  String v = PROBE_VERSION;
  return "ndw-lorawan-probe/" + (v.length() ? v : String("dev"));
}

const char* stateName() {
  switch (state) {
    case UNPROVISIONED: return "unprovisioned";
    case JOINING: return "joining";
    default: return "joined";
  }
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
// number tools.sh mac prints.
uint64_t macDevEui() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  uint8_t e[8] = {m[0], m[1], m[2], 0xff, 0xfe, m[3], m[4], m[5]};
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | e[i];
  return v;
}

// Which keys the saved join nonces belong to. ChirpStack refuses a reused
// DevNonce, so nonces are kept across reboots — but only for the device they
// were counted for: a board re-provisioned as a different device starts over.
uint32_t keyFingerprint() {
  uint32_t h = 2166136261u;
  auto mix = [&](const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
  };
  mix((const uint8_t*)&config.devEui, 8);
  mix((const uint8_t*)&config.joinEui, 8);
  mix(config.appKey, 16);
  return h;
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

// ---- screen --------------------------------------------------------------

void draw() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawString(0, 0, "NDW probe SB2");
  oled.setTextAlignment(TEXT_ALIGN_RIGHT);
  oled.drawString(128, 0, view.batteryMv ? String(view.batteryPct) + "%" : "bat ?");
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawHorizontalLine(0, 12, 128);

  if (state == UNPROVISIONED) {
    oled.drawString(0, 14, "Not provisioned");
    oled.drawString(0, 26, "DevEUI");
    oled.drawString(0, 38, hex64(config.devEui));
    oled.drawString(0, 50, "Program it over USB");
    oled.display();
    return;
  }

  oled.drawString(0, 14, view.line);
  oled.drawString(0, 26, "Total " + String(decilitres / 10.0, 1) + " L");
  oled.drawString(0, 38, "Sent " + String(view.sent) + "  down " + String(view.acked));
  if (view.error.length()) {
    oled.drawString(0, 50, view.error);
  } else if (view.haveSignal) {
    oled.drawString(0, 50, "RSSI " + String(view.rssi, 0) + "  SNR " + String(view.snr, 1));
  } else {
    oled.drawString(0, 50, hex64(config.devEui));
  }
  if (view.nextAt) {
    int32_t left = (int32_t)(view.nextAt - millis()) / 1000;
    oled.setTextAlignment(TEXT_ALIGN_RIGHT);
    oled.drawString(128, 26, String(left < 0 ? 0 : left) + "s");
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

// ---- configuration -------------------------------------------------------

void loadConfig() {
  config.provisioned = store.getBool("prov", false);
  config.devEui = store.getULong64("deveui", 0);
  if (!config.devEui) config.devEui = macDevEui();
  config.joinEui = store.getULong64("joineui", 0);
  store.getBytes("appkey", config.appKey, 16);
  config.intervalS = store.getUShort("interval", 60);
  if (config.intervalS < INTERVAL_MIN_S || config.intervalS > INTERVAL_MAX_S) config.intervalS = 60;
}

void saveNonces() {
  store.putBytes("nonces", node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
  store.putUInt("nkey", keyFingerprint());
}

void restoreNonces() {
  uint8_t buf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
  // Nonces saved before keys were fingerprinted carry no owner, and are kept:
  // dropping them would restart the count, and ChirpStack refuses the replay.
  if (store.isKey("nkey") && store.getUInt("nkey", 0) != keyFingerprint()) return;
  if (store.getBytes("nonces", buf, sizeof(buf)) == sizeof(buf)) {
    Serial.printf("[probe] restored nonces: %d\n", node.setBufferNonces(buf));
  }
}

// ---- the host protocol ---------------------------------------------------

void reply(JsonDocument& doc) {
  Serial.print("#NDW ");
  serializeJson(doc, Serial);
  Serial.println();
}

void refuse(const char* error, const char* detail, const char* field = nullptr) {
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
  doc["eui"] = hex64(config.devEui);
  doc["firmware"] = firmware();
  doc["state"] = stateName();
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
    out["devEui"] = hex64(config.devEui);
    out["joinEui"] = hex64(config.joinEui);
    out["provisioned"] = config.provisioned;
    out["band"] = "US915";
    out["subBand"] = 2;
    out["interval"] = config.intervalS;
    out["totalLitres"] = decilitres / 10.0;
    out["sent"] = view.sent;
    out["acked"] = view.acked;
    out["joinAttempts"] = view.joinAttempts;
    if (view.haveSignal) {
      out["rssi"] = view.rssi;
      out["snr"] = view.snr;
    }
    out["batteryPct"] = view.batteryPct;
    out["batteryMv"] = view.batteryMv;
    out["error"] = view.error;
    reply(out);
  } else if (cmd == "lorawan") {
    uint64_t devEui = macDevEui(), joinEui = 0;
    uint8_t key[16];
    const char* k = in["appKey"] | "";
    if (!parseHex(k, key, 16)) {
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
    store.putULong64("deveui", devEui);
    store.putULong64("joineui", joinEui);
    store.putBytes("appkey", key, 16);
    store.putBool("prov", true);
    JsonDocument out;
    out["ok"] = true;
    out["eui"] = hex64(devEui);
    out["state"] = "rebooting";
    reply(out);
    Serial.println("[probe] provisioned, rebooting to join");
    rebootSoon();
  } else if (cmd == "interval") {
    int s = in["seconds"] | 0;
    if (s < INTERVAL_MIN_S || s > INTERVAL_MAX_S) {
      refuse("invalid", "The interval must be 15 to 3600 seconds.", "seconds");
      return;
    }
    config.intervalS = s;
    store.putUShort("interval", s);
    JsonDocument out;
    out["ok"] = true;
    out["interval"] = s;
    reply(out);
  } else if (cmd == "forget") {
    store.remove("prov");
    store.remove("deveui");
    store.remove("joineui");
    store.remove("appkey");
    store.remove("nonces");
    store.remove("nkey");
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

// Reads whatever has arrived, and acts on each complete line. Called often:
// between joins, between uplinks and while waiting, so the host is answered
// within a few seconds even mid-cycle.
void pollSerial() {
  static String pending;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      pending.trim();
      if (pending.length()) onCommand(pending);
      pending = "";
    } else if (pending.length() < 512) {
      pending += c;
    }
  }
}

// Waits until `until`, answering the host and redrawing once a second.
void waitUntil(uint32_t until) {
  view.nextAt = until;
  uint32_t lastDraw = 0;
  while ((int32_t)(until - millis()) > 0) {
    pollSerial();
    if (millis() - lastDraw >= 1000) {
      draw();
      lastDraw = millis();
    }
    delay(20);
  }
  view.nextAt = 0;
}

// ---- LoRaWAN -------------------------------------------------------------

bool joinOnce() {
  view.joinAttempts++;
  Serial.println("[probe] join request...");
  show("joining, try " + String(view.joinAttempts));
  int16_t rc = node.activateOTAA();
  saveNonces();
  if (rc == RADIOLIB_LORAWAN_NEW_SESSION) {
    Serial.println("[probe] joined");
    view.error = "";
    state = JOINED;
    show("joined");
    return true;
  }
  Serial.printf("[probe] join failed: %d\n", rc);
  view.error = "join failed: " + String(rc);
  return false;
}

void uplink() {
  // Somewhere between half a litre and two and a half a minute: a tap left
  // running slowly, which is enough to see the point's total move.
  decilitres += 5 + (esp_random() % 21);
  store.putUInt("dl", decilitres);

  // Read before the send, not after: a transmit pulls the cell down for a
  // moment, and a reading taken then understates it.
  sampleBattery();

  uint8_t payload[5] = {
    (uint8_t)(decilitres >> 24), (uint8_t)(decilitres >> 16),
    (uint8_t)(decilitres >> 8), (uint8_t)decilitres, view.batteryPct,
  };
  uint8_t down[64];
  size_t downLen = sizeof(down);

  show("sending...");
  int16_t rc = node.sendReceive(payload, sizeof(payload), 10, down, &downLen);
  saveNonces();
  view.sent++;
  if (rc < RADIOLIB_ERR_NONE) {
    Serial.printf("[probe] uplink failed: %d\n", rc);
    view.error = "uplink failed: " + String(rc);
    show("joined");
    return;
  }
  view.error = "";
  if (rc > 0) {
    view.acked++;
    view.haveSignal = true;
    view.rssi = radio.getRSSI();
    view.snr = radio.getSNR();
  }
  Serial.printf("[probe] sent %.1f L, battery %u%%%s\n", decilitres / 10.0, view.batteryPct,
                rc > 0 ? ", downlink received" : "");
  show(rc > 0 ? "sent, gateway answered" : "sent, no answer");
}

// ---- main ----------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[probe] %s\n", firmware().c_str());

  store.begin("lorawan", false);
  loadConfig();
  decilitres = store.getUInt("dl", 0);
  sampleBattery();
  startScreen();
  Serial.printf("[probe] DevEUI %s, %s\n", hex64(config.devEui).c_str(),
                config.provisioned ? "provisioned" : "not provisioned");

  if (!config.provisioned) {
    state = UNPROVISIONED;
    draw();
    return;
  }

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  // 1.8 V on the TCXO, as the board wires it. The frequency here is replaced
  // by the LoRaWAN stack.
  int16_t rc = radio.begin(915.0, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 1.8, false);
  if (rc != RADIOLIB_ERR_NONE) {
    view.error = "radio failed: " + String(rc);
    Serial.printf("[probe] radio.begin: %d\n", rc);
  }
  // A null NwkKey is what makes RadioLib join as LoRaWAN 1.0.x, on the AppKey
  // alone — which is what the MeterFax device profiles are.
  node.beginOTAA(config.joinEui, config.devEui, nullptr, config.appKey);
  restoreNonces();
  state = JOINING;
}

void loop() {
  pollSerial();

  switch (state) {
    case UNPROVISIONED:
      waitUntil(millis() + 1000);
      break;
    case JOINING:
      if (!joinOnce()) waitUntil(millis() + JOIN_RETRY_MS);
      break;
    case JOINED:
      uplink();
      waitUntil(millis() + (uint32_t)config.intervalS * 1000);
      break;
  }
}
