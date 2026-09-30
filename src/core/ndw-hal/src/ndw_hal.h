// What NDW firmware needs from the hardware it runs on, and nothing more.
//
// The core — the LoRaWAN stack, the meter fleet, the gateway — is written
// against these and never includes a vendor header, so it builds and is
// tested on the host, and runs on another MCU once these are implemented
// for it. src/hal/esp32 implements them over Arduino-ESP32.
//
// Strings are std::string: Arduino's String is not on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

namespace ndw {
namespace hal {

// Time since boot. Monotonic; never goes back and is never set.
class Clock {
 public:
  virtual ~Clock() {}
  virtual uint64_t micros() = 0;
  virtual uint32_t millis() { return (uint32_t)(micros() / 1000); }
  virtual void sleepMs(uint32_t ms) = 0;
};

// 32 uniform bits: the hardware's RNG, since DevNonces and keys come from it.
class Random {
 public:
  virtual ~Random() {}
  virtual uint32_t next() = 0;
};

// Blobs under short keys (at most 15 characters), in one namespace that
// survives a reboot and a reflash.
class Store {
 public:
  virtual ~Store() {}
  // The blob's size, or 0 when there is none.
  virtual size_t size(const char* key) = 0;
  // Reads exactly `len` bytes; false when the blob is missing or another size.
  virtual bool get(const char* key, void* out, size_t len) = 0;
  virtual bool put(const char* key, const void* data, size_t len) = 0;
  virtual bool remove(const char* key) = 0;
  // The keys starting with `prefix`.
  virtual std::vector<std::string> keys(char prefix) = 0;
  // Everything in the namespace.
  virtual bool clear() = 0;
};

// The host's link: the "#NDW" protocol, one JSON line each way, over USB.
class Console {
 public:
  virtual ~Console() {}
  // A whole line, when one has arrived; false otherwise. Never blocks.
  virtual bool readLine(std::string& line) = 0;
  // Writes the line and its end.
  virtual void writeLine(const std::string& line) = 0;
  // Waits until what was written has gone, before a reboot.
  virtual void flush() {}
};

// The board as a whole.
class Board {
 public:
  virtual ~Board() {}
  // Its own EUI-64: the WiFi MAC with FF:FE in the middle, unique per board.
  virtual uint64_t eui() = 0;
  virtual void reboot() = 0;
  // Its own battery, 0..100, or 255 on a board that cannot tell.
  virtual uint8_t batteryPercent() { return 255; }
};

// ---- a gateway's network ---------------------------------------------------

enum class WifiState : uint8_t { UNPROVISIONED, CONNECTING, CONNECTED, NOT_FOUND, BAD_AUTH, FAILED };

struct WifiNetwork {
  std::string ssid;
  int rssi;
  const char* auth;  // open | wep | wpa2 | wpa3 | enterprise | unknown
};

enum class SocketEvent : uint8_t { OPENED, TEXT, CLOSED };

// Why a connection that never became a websocket failed.
enum class Diagnosis : uint8_t { REFUSED, TLS, UNREACHABLE, UNEXPECTED, BAD_ADDRESS };

// WiFi, the clock it brings, a websocket, and an HTTPS request to diagnose
// one that fails. What a Basics Station gateway needs of a network stack.
class Net {
 public:
  virtual ~Net() {}

  virtual void wifiBegin(const std::string& ssid, const std::string& password, const std::string& hostname) = 0;
  virtual WifiState wifiState() = 0;
  virtual std::string ip() = 0;
  virtual std::string mac() = 0;
  virtual int rssi() = 0;
  virtual std::vector<WifiNetwork> scan() = 0;

  // Starts NTP. Unix seconds once it has answered, 0 before.
  virtual void startClock() = 0;
  virtual double unixTime() = 0;

  // One websocket at a time. `header` is a whole header line
  // ("Authorization: Bearer …"); `trust` a PEM bundle, for TLS.
  virtual void open(const std::string& host, uint16_t port, const std::string& path, bool tls,
                    const std::string& trust, const std::string& header) = 0;
  virtual void close() = 0;
  virtual void sendText(const std::string& text) = 0;
  virtual void loop() = 0;
  virtual void onSocket(std::function<void(SocketEvent, const char* text, size_t len)> handler) = 0;

  // An ordinary HTTPS GET to where a websocket failed, to say why: a refused
  // token, a certificate that does not check out, or nothing there. `detail`
  // carries the TLS library's words, or the status an unexpected answer had.
  virtual Diagnosis diagnose(const std::string& host, uint16_t port, const std::string& path,
                             const std::string& trust, const std::string& authorization, std::string& detail) = 0;
};

// Everything at once, as a board hands it to the core.
struct Hal {
  Clock& clock;
  Random& random;
  Console& console;
  Board& board;
};

}  // namespace hal
}  // namespace ndw
