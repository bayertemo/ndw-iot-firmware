// A LoRa Basics Station gateway, whatever its radio and whatever it runs on:
// the link to the network server, the settings, and the "#NDW" host protocol
// flash.meterfax.com programs a gateway with.
//
// The same for the NDW LoRaWAN Gateway, whose radio is an SX1262, and the
// NDW BLE Gateway, whose radio is Bluetooth: each board hands this its
// hardware (ndw_hal.h) and its radio (ndw_radio.h), and from here up nothing
// can tell them apart.
//
// ## Basics Station, briefly
//
// The gateway asks <url>/router-info which address to use, opens a websocket
// there, says its version, and gets a router_config. Each frame heard goes up
// as a jreq or updf message, carrying "xtime" — the radio's microsecond clock
// at the end of the frame, its top byte a number for this connection. A
// downlink comes back as a dnmsg carrying that same xtime and a delay, and
// the radio sends it into the device's window. The token rides on both
// connections as an Authorization header, which is what MeterFax checks.
//
// ## The host protocol
//
// One JSON command per line; every answer is a line starting "#NDW "
// followed by JSON:
//
//   {"cmd":"hello"}   → eui, firmware, kind, state
//   {"cmd":"status"}  → WiFi (state, ssid, ip, mac, rssi), the server link
//                       (lns: url, state, error, up, down, late), and the
//                       radio's own block
//   {"cmd":"scan"}    → the WiFi networks it can hear
//   {"cmd":"wifi","ssid":"…","password":"…"}
//                     → joins, and answers with where that got
//   {"cmd":"lns","url":"wss://…","trust":"-----BEGIN CERTIFICATE-----…",
//    "token":"Authorization: Bearer …"}
//                     → the server, the file to trust it by, and the
//                       gateway's token; connects straight away
//   {"cmd":"forget"}  → drops everything and reboots
//   {"cmd":"reboot"}
//
// The token is never sent back, only whether there is one and its last four
// characters. A refusal is {"ok":false,"error":…,"detail":…,"faults":[…]}.
// Everything else printed starts "[gateway]" and is for people.
#pragma once

#include <stdint.h>

#include <string>

#include "ndw_hal.h"
#include "ndw_radio.h"

namespace ndw {
namespace station {

// What differs between gateways, besides the radio and the hardware.
struct Identity {
  // "ndw-lorawan-gateway", say: the firmware reports "<name>/<version>".
  const char* name;
  // The release, or "" for a local build, which reports "dev".
  const char* version;
  // Basics Station's model field.
  const char* model;
  // The host protocol's kind, for flash.meterfax.com to decide what to offer.
  const char* kind;
};

// Counted for the status reply and a screen.
struct Counters {
  uint32_t up = 0;
  uint32_t down = 0;
  uint32_t late = 0;
};

// Loads the settings, starts WiFi and the clock. Call from setup(), with the
// radio begun.
void begin(const Identity& identity, hal::Hal& hal, hal::Net& net, hal::Store& settings, radio::GatewayRadio& radio);

// Reads the host's commands, keeps the server link going, and moves frames
// both ways. Every loop, as often as it can run.
void loop();

Counters& counters();

// For a screen.
std::string eui();
std::string firmware();
const char* wifiState();
std::string ssid();
bool wifiConnected();
// The link's state in a word, or its error when it has one.
std::string linkState();

}  // namespace station
}  // namespace ndw
