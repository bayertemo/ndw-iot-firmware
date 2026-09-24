// LoRaWAN probe: joins US915 sub-band 2 (channels 8-15 + 65) by OTAA as a
// LoRaWAN 1.0.x device, then reports as a water meter every minute.
//
// The payload is MeterFax's ndw-water-v1 format, which its decode rule reads
// when the device is on the ChirpStack profile of that name: port 10, a
// uint32 big-endian running total in decilitres, then a uint8 battery %.
//
// LoRaWAN 1.0.x because the ChirpStack tenant's profiles are 1.0.3: RadioLib
// takes a null NwkKey to mean 1.0.x, joining on the AppKey alone.
//
// The OLED shows what the serial log does, so the probe can be carried away
// from a laptop to see where the gateway still hears it.
#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SSD1306Wire.h>

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

// Which device this is, and the key it joins with. Kept out of this file,
// in include/credentials.h, which git ignores: this repository is public,
// and an AppKey in it would let anyone impersonate the device. Copy
// include/credentials.example.h to start one.
#if __has_include("credentials.h")
#include "credentials.h"
#else
#error "No include/credentials.h. Copy include/credentials.example.h and fill it in."
#endif

static const uint32_t UPLINK_EVERY_MS = 60000;

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, SPI);

// Sub-band 2, which is what the WisGate listens on.
LoRaWANNode node(&radio, &US915, 2);

SSD1306Wire oled(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);

Preferences store;

// A running total, like a meter's register: it only goes up, and survives a
// reboot. A total that restarts from zero reads to MeterFax as a meter
// running backwards, which it refuses to subtract.
uint32_t decilitres = 0;

// What the screen shows, kept here so any stage can change one line and redraw.
struct {
  String state = "starting";
  uint32_t sent = 0;
  uint32_t acked = 0;
  bool haveSignal = false;
  float rssi = 0;
  float snr = 0;
  String error = "";
  uint32_t nextAt = 0;
  uint16_t batteryMv = 0;
  uint8_t batteryPct = 0;
} view;

uint16_t readBatteryOnce(int ctrlLevel) {
  digitalWrite(PIN_ADC_CTRL, ctrlLevel);
  delay(10);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogReadMilliVolts(PIN_VBAT);
  }
  return (uint16_t)(sum / 8 * VBAT_DIVIDER);
}

// Millivolts at the cell, or 0 where nothing could be read. With the switch
// the wrong way the divider is disconnected and the pin reads near zero, so a
// reading under a volt means "try the other level".
uint16_t readBattery() {
  pinMode(PIN_ADC_CTRL, OUTPUT);
  uint16_t mv = readBatteryOnce(LOW);
  if (mv < 1000) {
    mv = readBatteryOnce(HIGH);
  }
  return mv < 1000 ? 0 : mv;
}

// A LiPo's charge from its resting voltage. The curve is flat through the
// middle, so this is a guide rather than a gauge — and on USB with no cell
// fitted the charger holds the pin near 4.2 V, which reads as full.
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

void draw() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);

  oled.drawString(0, 0, "MeterFax probe SB2");
  oled.setTextAlignment(TEXT_ALIGN_RIGHT);
  oled.drawString(128, 0, view.batteryMv ? String(view.batteryPct) + "%" : "bat ?");
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawHorizontalLine(0, 12, 128);

  oled.drawString(0, 14, view.state);
  oled.drawString(0, 26, "Total " + String(decilitres / 10.0, 1) + " L");
  oled.drawString(0, 38, "Sent " + String(view.sent) + "  down " + String(view.acked));

  if (view.error.length()) {
    oled.drawString(0, 50, view.error);
  } else if (view.haveSignal) {
    oled.drawString(0, 50, "RSSI " + String(view.rssi, 0) + "  SNR " + String(view.snr, 1));
  } else {
    oled.drawString(0, 50, "no downlink yet");
  }

  if (view.nextAt) {
    int32_t left = (int32_t)(view.nextAt - millis()) / 1000;
    oled.setTextAlignment(TEXT_ALIGN_RIGHT);
    oled.drawString(128, 26, String(left < 0 ? 0 : left) + "s");
  }

  oled.display();
}

void showState(const String& s) {
  view.state = s;
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
  draw();
}

// ChirpStack refuses a join that reuses a DevNonce, and RadioLib starts from
// zero on every boot unless its nonces are kept — so every reboot after the
// first would be rejected without this.
void saveNonces() {
  store.putBytes("nonces", node.getBufferNonces(), RADIOLIB_LORAWAN_NONCES_BUF_SIZE);
}

void restoreNonces() {
  uint8_t buf[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
  if (store.getBytes("nonces", buf, sizeof(buf)) == sizeof(buf)) {
    int16_t state = node.setBufferNonces(buf);
    Serial.printf("[probe] restored nonces: %d\n", state);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n[probe] LoRaWAN probe, US915 sub-band 2");
  Serial.printf("[probe] DevEUI %016llx\n", DEV_EUI);

  sampleBattery();
  startScreen();

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);

  // 1.8 V on the TCXO, as the board wires it. The frequency here is replaced
  // by the LoRaWAN stack.
  int16_t state = radio.begin(915.0, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8, 1.8, false);
  Serial.printf("[probe] radio.begin: %d\n", state);
  if (state != RADIOLIB_ERR_NONE) {
    view.error = "radio failed: " + String(state);
    draw();
    while (true) { delay(1000); }
  }

  store.begin("lorawan", false);
  decilitres = store.getUInt("dl", 0);
  Serial.printf("[probe] register starts at %.1f L\n", decilitres / 10.0);

  state = node.beginOTAA(JOIN_EUI, DEV_EUI, nullptr, appKey);
  Serial.printf("[probe] beginOTAA: %d\n", state);
  restoreNonces();

  // Keep asking: each attempt is heard or not, and an unregistered device is
  // still visible in ChirpStack's LoRaWAN frames tab as a join request.
  for (uint32_t attempt = 1;; attempt++) {
    Serial.println("[probe] join request...");
    showState("joining, try " + String(attempt));
    state = node.activateOTAA();
    saveNonces();
    if (state == RADIOLIB_LORAWAN_NEW_SESSION) {
      Serial.println("[probe] JOINED");
      view.error = "";
      showState("joined");
      break;
    }
    Serial.printf("[probe] join failed: %d, retrying in 30s\n", state);
    view.error = "join failed: " + String(state);
    view.nextAt = millis() + 30000;
    while ((int32_t)(view.nextAt - millis()) > 0) {
      draw();
      delay(1000);
    }
    view.nextAt = 0;
  }
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
    (uint8_t)(decilitres >> 8), (uint8_t)decilitres,
    view.batteryPct,
  };
  uint8_t down[64];
  size_t downLen = sizeof(down);

  showState("sending...");
  int16_t state = node.sendReceive(payload, sizeof(payload), 10, down, &downLen);
  saveNonces();
  view.sent++;

  if (state < RADIOLIB_ERR_NONE) {
    Serial.printf("[probe] uplink failed: %d\n", state);
    view.error = "uplink failed: " + String(state);
    showState("joined");
    return;
  }

  view.error = "";
  if (state > 0) {
    // The signal of the downlink, as the probe heard it: how well the gateway
    // reaches here, which is the other half of whether this spot works.
    view.acked++;
    view.haveSignal = true;
    view.rssi = radio.getRSSI();
    view.snr = radio.getSNR();
  }
  Serial.printf("[probe] sent %.1f L, battery %u%% (%u mV)%s\n", decilitres / 10.0, view.batteryPct, view.batteryMv, state > 0 ? ", downlink received" : "");
  showState(state > 0 ? "sent, gateway answered" : "sent, no answer");
}

void loop() {
  uplink();

  view.nextAt = millis() + UPLINK_EVERY_MS;
  while ((int32_t)(view.nextAt - millis()) > 0) {
    draw();
    delay(1000);
  }
}
