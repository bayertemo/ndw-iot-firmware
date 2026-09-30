// NDW LoRaWAN Gateway, for the Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262).
//
// A single-channel LoRaWAN gateway that speaks LoRa Basics Station to a
// network server — MeterFax's, at wss://lns.meterfax.com — over WiFi. For a
// bench: one of these and one board running NDW LoRaWAN meter fleet make a
// whole test network, without a real gateway.
//
// A board of three layers: the gateway is the core's (ndw-station, the same as
// NDW BLE Gateway's), the hardware is the ESP32's (ndw-hal-esp32,
// ndw-hal-esp32-net), and the radio is LoRa (ndw-radio-lora) on this board's
// SX1262. What is here is the board: its pins and its screen.
//
// ## One channel
//
// A real gateway has a concentrator that hears eight channels at every
// spreading factor at once. This board has one SX1262, which hears one
// channel at one spreading factor: US915 channel 8, 903.9 MHz, SF7 on
// 125 kHz (DR3). The fleet firmware keeps to exactly that when programmed
// for an NDW LoRaWAN Gateway.
#include <Arduino.h>
#include <RadioLib.h>
#include <SSD1306Wire.h>

#include "ndw_hal_esp32.h"
#include "ndw_net_esp32.h"
#include "ndw_radio_lora.h"
#include "ndw_station.h"

#ifndef GATEWAY_VERSION
#define GATEWAY_VERSION ""
#endif

using namespace ndw;

// Heltec WiFi LoRa 32 V3: SX1262 on its own SPI pins.
static const int PIN_NSS = 8, PIN_SCK = 9, PIN_MOSI = 10, PIN_MISO = 11;
static const int PIN_RST = 12, PIN_BUSY = 13, PIN_DIO1 = 14;

// The OLED: SSD1306 on I2C, powered through Vext, which is switched on low.
static const int PIN_OLED_SDA = 17, PIN_OLED_SCL = 18, PIN_OLED_RST = 21;
static const int PIN_VEXT = 36;

SX1262 chip = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, SPI);
SSD1306Wire oled(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);

hal::Esp32 esp;
hal::Esp32Net net;
hal::Esp32Store settings("gateway");
radio::LoraGateway lora(chip);

const station::Identity IDENTITY = {"ndw-lorawan-gateway", GATEWAY_VERSION, "heltec-wifi-lora-32-v3",
                                    "lorawan-gateway"};

// ---- the screen -----------------------------------------------------------------

void draw() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawString(0, 0, "NDW LoRaWAN Gateway");
  oled.drawHorizontalLine(0, 12, 128);
  oled.drawString(0, 14, station::eui().c_str());
  String wifi = String("WiFi ") + station::wifiState();
  if (station::wifiConnected()) wifi = String("WiFi ") + station::ssid().c_str();
  oled.drawString(0, 26, wifi);
  oled.drawString(0, 38, String("Server ") + station::linkState().c_str());
  station::Counters& c = station::counters();
  oled.drawString(0, 50, "up " + String(c.up) + "  down " + String(c.down) + "  903.9 SF7");
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
  // The lns command carries the whole trust file on one line.
  esp.console.begin(16384, 16384);
  startScreen();
  settings.begin();
  net.begin();

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  // 1.8 V on the TCXO, as the board wires it.
  int16_t rc = chip.begin(903.9, 125.0, 7, 5, 0x34, 14, 8, 1.8, false);
  if (rc != RADIOLIB_ERR_NONE) esp.console.writeLine("[gateway] radio.begin: " + std::to_string(rc));
  lora.begin();

  station::begin(IDENTITY, esp.hal, net, settings, lora);
  draw();
}

void loop() {
  station::loop();
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 1000) {
    draw();
    lastDraw = millis();
  }
}
