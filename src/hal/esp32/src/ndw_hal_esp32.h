// NDW's hardware layer on an ESP32, over Arduino-ESP32: the clock, the RNG,
// NVS, the USB serial port and the board. Any ESP32 of the Arduino core's —
// the S3 on a Heltec V3, a C3 — since none of it names a pin.
#pragma once

#include <Arduino.h>
#include <Preferences.h>

#include <string>

#include "ndw_hal.h"

namespace ndw {
namespace hal {

class Esp32Clock : public Clock {
 public:
  uint64_t micros() override;
  uint32_t millis() override;
  void sleepMs(uint32_t ms) override;
};

// The hardware RNG: true randomness once the radio is on, as it is in every
// NDW build before a key or nonce is drawn.
class Esp32Random : public Random {
 public:
  uint32_t next() override;
};

// A Preferences namespace, in the default NVS partition or one of its own.
class Esp32Store : public Store {
 public:
  // `partition` null for the default "nvs".
  Esp32Store(const char* ns, const char* partition = nullptr) : ns_(ns), partition_(partition) {}
  // Opens it; false when the partition is missing from the table.
  bool begin();
  size_t size(const char* key) override;
  bool get(const char* key, void* out, size_t len) override;
  bool put(const char* key, const void* data, size_t len) override;
  bool remove(const char* key) override;
  std::vector<std::string> keys(char prefix) override;
  bool clear() override;

 private:
  const char* ns_;
  const char* partition_;
  Preferences prefs_;
};

// The USB serial port: Serial, which is the C3's own USB with
// ARDUINO_USB_CDC_ON_BOOT, and the S3's UART bridge on the Heltec.
class Esp32Console : public Console {
 public:
  // `rxBuffer` sized for the longest line the host sends at once: a fleet-add
  // chunk, or the gateway's lns command with its trust file.
  void begin(size_t rxBuffer, size_t maxLine);
  bool readLine(std::string& line) override;
  void writeLine(const std::string& line) override;
  void flush() override;

 private:
  std::string pending_;
  size_t maxLine_ = 8192;
};

class Esp32Board : public Board {
 public:
  uint64_t eui() override;
  void reboot() override;
};

// Everything above, made once.
struct Esp32 {
  Esp32Clock clock;
  Esp32Random random;
  Esp32Console console;
  Esp32Board board;
  Hal hal{clock, random, console, board};
};

}  // namespace hal
}  // namespace ndw
