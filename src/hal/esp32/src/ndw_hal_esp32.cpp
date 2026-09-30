#include "ndw_hal_esp32.h"

#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <nvs.h>

namespace ndw {
namespace hal {

uint64_t Esp32Clock::micros() { return (uint64_t)esp_timer_get_time(); }

uint32_t Esp32Clock::millis() { return ::millis(); }

void Esp32Clock::sleepMs(uint32_t ms) { delay(ms); }

uint32_t Esp32Random::next() { return esp_random(); }

// ---- NVS ----------------------------------------------------------------------

bool Esp32Store::begin() { return prefs_.begin(ns_, false, partition_); }

size_t Esp32Store::size(const char* key) { return prefs_.isKey(key) ? prefs_.getBytesLength(key) : 0; }

bool Esp32Store::get(const char* key, void* out, size_t len) {
  return prefs_.isKey(key) && prefs_.getBytesLength(key) == len && prefs_.getBytes(key, out, len) == len;
}

bool Esp32Store::put(const char* key, const void* data, size_t len) {
  return prefs_.putBytes(key, data, len) == len;
}

bool Esp32Store::remove(const char* key) { return prefs_.remove(key); }

std::vector<std::string> Esp32Store::keys(char prefix) {
  std::vector<std::string> out;
  nvs_iterator_t it = nvs_entry_find(partition_ ? partition_ : "nvs", ns_, NVS_TYPE_BLOB);
  while (it) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    if (info.key[0] == prefix) out.push_back(info.key);
    it = nvs_entry_next(it);
  }
  return out;
}

bool Esp32Store::clear() { return prefs_.clear(); }

// ---- the console ------------------------------------------------------------------

void Esp32Console::begin(size_t rxBuffer, size_t maxLine) {
  maxLine_ = maxLine;
  Serial.setRxBufferSize(rxBuffer);
  Serial.begin(115200);
  delay(200);
}

bool Esp32Console::readLine(std::string& line) {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (pending_.empty()) continue;
      line.swap(pending_);
      pending_.clear();
      return true;
    }
    if (pending_.size() < maxLine_) pending_ += c;
  }
  return false;
}

void Esp32Console::writeLine(const std::string& line) {
  Serial.write((const uint8_t*)line.data(), line.size());
  Serial.println();
}

void Esp32Console::flush() { Serial.flush(); }

// ---- the board ----------------------------------------------------------------------

uint64_t Esp32Board::eui() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  uint8_t e[8] = {m[0], m[1], m[2], 0xff, 0xfe, m[3], m[4], m[5]};
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | e[i];
  return v;
}

void Esp32Board::reboot() { ESP.restart(); }

}  // namespace hal
}  // namespace ndw
