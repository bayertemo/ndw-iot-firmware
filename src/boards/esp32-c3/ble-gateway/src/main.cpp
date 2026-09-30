// NDW BLE Gateway, for an ESP32-C3.
//
// A LoRaWAN gateway whose radio is Bluetooth LE. NDW BLE meters advertise
// real LoRaWAN frames; this hears them and forwards each to MeterFax's
// network server as a LoRa Basics Station, and advertises what the server
// sends back into the meter's receive windows — what the NDW LoRaWAN Gateway
// does over LoRa. From the server up nothing can tell: it joins the meters,
// checks their MICs and sends them MAC commands as it would any LoRa device.
// This board holds no key and needs none.
//
// A board of three layers: the gateway itself is the core's (ndw-station),
// the hardware is the ESP32's (ndw-hal-esp32, ndw-hal-esp32-net), and the
// radio is BLE (ndw-radio-ble). Any ESP32-C3 with 4MB of flash: nothing here
// names a pin.
#include <Arduino.h>

#include "ndw_hal_esp32.h"
#include "ndw_net_esp32.h"
#include "ndw_radio_ble.h"
#include "ndw_station.h"

#ifndef GATEWAY_VERSION
#define GATEWAY_VERSION ""
#endif

using namespace ndw;

hal::Esp32 esp;
hal::Esp32Net net;
hal::Esp32Store settings("gateway");
radio::BleGateway ble;

const station::Identity IDENTITY = {"ndw-ble-gateway", GATEWAY_VERSION, "esp32-c3-ble", "ble-gateway"};

void setup() {
  // The lns command carries the whole trust file on one line.
  esp.console.begin(16384, 16384);
  settings.begin();
  net.begin();
  ble.begin();
  station::begin(IDENTITY, esp.hal, net, settings, ble);
}

void loop() {
  station::loop();
  delay(1);
}
