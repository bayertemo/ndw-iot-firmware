// A gateway's network on an ESP32: WiFi, NTP, a websocket (links2004's
// WebSockets) and an HTTPS request to diagnose one that fails. Apart from
// ndw-hal-esp32 so an end device, which has no network, does not build it.
#pragma once

#include "ndw_hal.h"

namespace ndw {
namespace hal {

class Esp32Net : public Net {
 public:
  // Registers for WiFi's events. Call once, from setup().
  void begin();

  void wifiBegin(const std::string& ssid, const std::string& password, const std::string& hostname) override;
  WifiState wifiState() override;
  std::string ip() override;
  std::string mac() override;
  int rssi() override;
  std::vector<WifiNetwork> scan() override;

  void startClock() override;
  double unixTime() override;

  void open(const std::string& host, uint16_t port, const std::string& path, bool tls, const std::string& trust,
            const std::string& header) override;
  void close() override;
  void sendText(const std::string& text) override;
  void loop() override;
  void onSocket(std::function<void(SocketEvent, const char*, size_t)> handler) override;

  Diagnosis diagnose(const std::string& host, uint16_t port, const std::string& path, const std::string& trust,
                     const std::string& authorization, std::string& detail) override;
};

}  // namespace hal
}  // namespace ndw
