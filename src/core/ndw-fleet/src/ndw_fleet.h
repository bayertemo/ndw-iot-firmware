// A fleet of simulated meters on one board: up to 200 LoRaWAN devices, each
// joining by OTAA (LoRaWAN 1.0.3, Class A) and reporting on its own schedule
// as a water, electricity or gas meter (ndw-meters), with the daily rhythm
// real households have and the faults real meters show.
//
// Whatever the radio: NDW LoRaWAN meter fleet sends over LoRa, NDW BLE meter
// fleet over Bluetooth, and both are this, the same LoRaWAN stack
// (ndw-lorawan) over a different ndw_radio.h. A device joins, asks the
// network the time until it has been answered, checks every 8th uplink that
// the network still hears it and joins again after 3 checks unanswered, and
// answers whatever MAC commands come down.
//
// ## One image, programmed over USB
//
// Nothing about a device is compiled in. A host writes the fleet over the
// cable, one JSON command per line; every answer is a line starting "#NDW "
// followed by JSON:
//
//   {"cmd":"hello"}        → eui, firmware, kind, state, fleet, maxFleet
//   {"cmd":"status"}       → the fleet: size, joined, sent, anomalies, clock,
//                            and the radio's own fields
//   {"cmd":"fleet-begin","count":200,"interval":900,"anomalies":20,
//    "epoch":1790000000,"tzOffset":-300,"channel":8?}
//   {"cmd":"fleet-add","devices":[["<devEui>","<appKey>","water"|"power"|"gas",
//    "<joinEui>"?],…]}     → a chunk at a time, each answered
//   {"cmd":"fleet-commit"} → saves the fleet, reboots, starts joining
//   {"cmd":"lorawan","appKey":"…","devEui":"…"?,"joinEui":"…"?}
//                          → a fleet of one water meter: the board itself
//   {"cmd":"interval","seconds":900}
//   {"cmd":"channel","channel":8|null}
//                          → keeps the fleet to one channel (or frees it), for
//                            a radio that has channels
//   {"cmd":"time","epoch":…,"tzOffset":…}
//   {"cmd":"forget"}       → drops the fleet
//   {"cmd":"reboot"}
//
// AppKeys are never sent back. tzOffset is minutes east of UTC, so the daily
// curves follow the host's local day.
//
// ## Many devices, one radio
//
// Each device is a record in the store holding its keys, its DevNonce, its
// session and its register. Its turn loads that record, sends, keeps its
// receive windows, and saves it straight back — with the counter or the
// DevNonce already moved on before anything goes on the air, so neither ever
// repeats, whenever the board stops.
//
// Programming the same DevEUIs with the same keys again keeps their records,
// so a fleet can be reprogrammed (a new interval, a changed mix) without every
// device joining again.
#pragma once

#include <stdint.h>

#include <string>

#include "ndw_hal.h"
#include "ndw_radio.h"

namespace ndw {
namespace fleet {

// What differs between fleets, besides the radio and the hardware.
struct Identity {
  // "ndw-lorawan-meter-fleet", say: the firmware reports "<name>/<version>".
  const char* name;
  // The release, or "" for a local build, which reports "dev".
  const char* version;
  // The host protocol's kind: "lorawan-probe" over LoRa, "ble-fleet" over BLE.
  const char* kind;
  // What the log lines start with, "[probe]" or "[fleet]".
  const char* tag;
};

static const uint16_t MAX_FLEET = 200;

// What a screen would show, kept current.
struct View {
  std::string line = "starting";
  std::string error;
  const char* state = "starting";
  uint16_t fleet = 0;
  uint16_t joined = 0;
  uint16_t faults = 0;
  uint32_t sent = 0;
  uint32_t acked = 0;
  bool haveSignal = false;
  float rssi = 0;
  float snr = 0;
  bool staging = false;
  uint16_t stagingCount = 0;
  uint16_t stagingExpected = 0;
  uint64_t boardEui = 0;
};

// Opens the store, loads the fleet, and starts the radio's channel setting.
// `store` is the fleet's own namespace. Call from setup(), with the radio
// begun.
void begin(const Identity& identity, hal::Hal& hal, hal::Store& store, radio::EndDeviceRadio& radio);

// Answers the host, and gives the most overdue device its turn. Every loop.
void loop();

const View& view();

}  // namespace fleet
}  // namespace ndw
