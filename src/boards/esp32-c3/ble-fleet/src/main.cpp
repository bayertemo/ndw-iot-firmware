// NDW BLE meter fleet, for an ESP32-C3.
//
// The NDW LoRaWAN meter fleet with Bluetooth LE for its radio: up to 200
// simulated water, electricity and gas meters, each joining by OTAA and
// reporting as a LoRaWAN Class A device, its frames advertised over BLE for
// an NDW BLE Gateway nearby to carry to and from the network server. The
// same stack, the same meters and the same host protocol as the LoRa fleet;
// only the radio differs.
//
// A board of three layers: the fleet is the core's (ndw-fleet), the hardware
// is the ESP32's (ndw-hal-esp32), and the radio is BLE (ndw-radio-ble). Any
// ESP32-C3 with 4MB of flash: nothing here names a pin.
#include <Arduino.h>

#include "ndw_fleet.h"
#include "ndw_hal_esp32.h"
#include "ndw_radio_ble.h"

#ifndef FLEET_VERSION
#define FLEET_VERSION ""
#endif

using namespace ndw;

hal::Esp32 esp;
// The fleet's own NVS partition, as on the Heltec fleet; see partitions.csv.
hal::Esp32Store store("fleet", "fleet");
radio::BleEndDevice ble;

const fleet::Identity IDENTITY = {"ndw-ble-meter-fleet", FLEET_VERSION, "ble-fleet", "fleet"};

void setup() {
  // A fleet-add chunk is a few kilobytes on one line.
  esp.console.begin(8192, 8192);
  if (!store.begin()) esp.console.writeLine("[fleet] the fleet NVS partition did not open");
  ble.begin();
  fleet::begin(IDENTITY, esp.hal, store, ble);
  // A hang costs a reboot, not a board that goes quiet.
  hal::startWatchdog(30);
}

void loop() { fleet::loop(); }
