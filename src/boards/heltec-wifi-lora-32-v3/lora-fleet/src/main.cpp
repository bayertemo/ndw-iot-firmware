// NDW LoRaWAN meter fleet, for the Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262).
//
// Up to 200 simulated water, electricity and gas meters on one board and one
// radio, each joining by OTAA and reporting as a LoRaWAN Class A device (US915
// sub-band 2), with a household's daily rhythm and the faults real meters
// show.
//
// A board of three layers: the fleet is the core's (ndw-fleet, the same as
// NDW BLE meter fleet's), the hardware is the ESP32's (ndw-hal-esp32), and the
// radio is LoRa (ndw-radio-lora) on this board's SX1262. What is here is the
// board: its pins, its battery, and its screen.
#include <Arduino.h>
#include <RadioLib.h>
#include <SSD1306Wire.h>

#include "ndw_fleet.h"
#include "ndw_hal_esp32.h"
#include "ndw_radio_lora.h"
#include "ndw_text.h"

#ifndef PROBE_VERSION
#define PROBE_VERSION ""
#endif

using namespace ndw;

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

SX1262 chip = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, SPI);
SSD1306Wire oled(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);

// ---- the battery -----------------------------------------------------------------

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
      {3700, 35},  {3600, 20}, {3500, 10}, {3400, 5},  {3300, 0},
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

// The ESP32's board, with this one's battery: a fleet of one reports it.
class HeltecBoard : public hal::Esp32Board {
 public:
  uint8_t batteryPercent() override {
    uint16_t mv = readBattery();
    return mv ? ::batteryPercent(mv) : 255;
  }
};

hal::Esp32Clock clk;
hal::Esp32Random rng;
hal::Esp32Console console;
HeltecBoard board;
hal::Hal H{clk, rng, console, board};
// The fleet's own NVS partition; see partitions.csv.
hal::Esp32Store store("fleet", "fleet");
radio::LoraEndDevice lora(chip);

const fleet::Identity IDENTITY = {"ndw-lorawan-meter-fleet", PROBE_VERSION, "lorawan-probe", "probe"};

// ---- the screen -----------------------------------------------------------------

void draw() {
  const fleet::View& v = fleet::view();
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawString(0, 0, "NDW meter fleet");
  oled.drawHorizontalLine(0, 12, 128);

  if (v.staging) {
    oled.drawString(0, 14, "Programming");
    oled.drawString(0, 26, String(v.stagingCount) + " of " + String(v.stagingExpected) + " devices");
  } else if (!v.fleet) {
    oled.drawString(0, 14, "Not programmed");
    oled.drawString(0, 26, "Board EUI");
    oled.drawString(0, 38, text::hex64(v.boardEui).c_str());
    oled.drawString(0, 50, "Program it over USB");
  } else {
    oled.drawString(0, 14, "Fleet " + String(v.fleet) + "  joined " + String(v.joined));
    oled.drawString(0, 26, v.line.c_str());
    oled.drawString(0, 38, "Sent " + String(v.sent) + "  faults " + String(v.faults));
    if (v.error.size()) {
      oled.drawString(0, 50, v.error.c_str());
    } else if (v.haveSignal) {
      oled.drawString(0, 50, "RSSI " + String(v.rssi, 0) + "  SNR " + String(v.snr, 1));
    } else {
      oled.drawString(0, 50, "no downlink yet");
    }
  }
  oled.display();
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

// ---- main ----------------------------------------------------------------------

void setup() {
  // A fleet-add chunk is a few kilobytes on one line, sent faster than a
  // radio operation takes, so the receive buffer holds a whole one.
  console.begin(8192, 8192);
  startScreen();
  if (!store.begin()) console.writeLine("[probe] the fleet NVS partition did not open");

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  // 1.8 V on the TCXO, as the board wires it. The rest the radio layer sets
  // for each frame.
  int16_t rc = chip.begin(915.0, 125.0, 9, 7, 0x34, 14, 8, 1.8, false);
  if (rc != RADIOLIB_ERR_NONE) console.writeLine("[probe] radio.begin: " + std::to_string(rc));
  lora.begin();

  fleet::begin(IDENTITY, H, store, lora);
  draw();
}

void loop() {
  fleet::loop();
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 1000) {
    draw();
    lastDraw = millis();
  }
}
