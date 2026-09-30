#include "ndw_net_esp32.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <sys/time.h>
#include <time.h>

namespace ndw {
namespace hal {

namespace {

WebSocketsClient ws;
std::function<void(SocketEvent, const char*, size_t)> handler;

// The socket keeps pointers into these, so they outlive the call that set them.
std::string trustHeld, headerHeld;

// Why the last attempt ended, from the driver: tells a wrong passphrase from
// a network out of range, which are fixed differently.
volatile uint8_t wifiReason = 0;
uint32_t wifiSince = 0;

const char* authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN:
      return "open";
    case WIFI_AUTH_WEP:
      return "wep";
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "wpa2";
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "wpa3";
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return "enterprise";
    default:
      return "unknown";
  }
}

void onEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (!handler) return;
  switch (type) {
    case WStype_CONNECTED:
      handler(SocketEvent::OPENED, nullptr, 0);
      break;
    case WStype_TEXT:
      handler(SocketEvent::TEXT, (const char*)payload, length);
      break;
    case WStype_DISCONNECTED:
      handler(SocketEvent::CLOSED, nullptr, 0);
      break;
    default:
      break;
  }
}

}  // namespace

void Esp32Net::begin() {
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) wifiReason = info.wifi_sta_disconnected.reason;
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      wifiReason = 0;
      Serial.printf("[gateway] on %s as %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }
  });
  WiFi.mode(WIFI_STA);
  ws.onEvent(onEvent);
}

void Esp32Net::wifiBegin(const std::string& ssid, const std::string& password, const std::string& hostname) {
  wifiReason = 0;
  wifiSince = millis();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostname.c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), password.c_str());
}

WifiState Esp32Net::wifiState() {
  if (WiFi.status() == WL_CONNECTED) return WifiState::CONNECTED;
  switch (wifiReason) {
    case 0:
      return WifiState::CONNECTING;
    case WIFI_REASON_NO_AP_FOUND:
      return WifiState::NOT_FOUND;
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE:
      return WifiState::BAD_AUTH;
    default:
      // Anything else while it keeps retrying reads as trying, until it has
      // been at it long enough to call a failure.
      return millis() - wifiSince > 30000 ? WifiState::FAILED : WifiState::CONNECTING;
  }
}

std::string Esp32Net::ip() { return WiFi.localIP().toString().c_str(); }

std::string Esp32Net::mac() { return WiFi.macAddress().c_str(); }

int Esp32Net::rssi() { return WiFi.RSSI(); }

std::vector<WifiNetwork> Esp32Net::scan() {
  std::vector<WifiNetwork> out;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) out.push_back({WiFi.SSID(i).c_str(), WiFi.RSSI(i), authName(WiFi.encryptionType(i))});
  WiFi.scanDelete();
  return out;
}

void Esp32Net::startClock() { configTime(0, 0, "pool.ntp.org", "time.google.com"); }

double Esp32Net::unixTime() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  // Before NTP has answered the clock reads 1970.
  return tv.tv_sec > 1600000000 ? tv.tv_sec + tv.tv_usec / 1e6 : 0;
}

void Esp32Net::open(const std::string& host, uint16_t port, const std::string& path, bool tls,
                    const std::string& trust, const std::string& header) {
  trustHeld = trust;
  headerHeld = header;
  // The library will not connect until this long has passed since its last
  // failure — which begin() sets to zero, so a long interval also stops the
  // first attempt: an hour here once meant no connection for the first hour
  // after boot. Short, so a new socket connects at once; the retries that
  // matter are the station's, which does not run the library between them.
  ws.setReconnectInterval(1000);
  ws.setExtraHeaders(headerHeld.c_str());
  if (tls) {
    ws.beginSslWithCA(host.c_str(), port, path.c_str(), trustHeld.c_str(), "");
  } else {
    ws.begin(host.c_str(), port, path.c_str(), "");
  }
  // A frame every half minute keeps the connection alive through Cloudflare
  // and nginx, and notices a dead one within a minute.
  ws.enableHeartbeat(30000, 10000, 2);
}

void Esp32Net::close() { ws.disconnect(); }

void Esp32Net::sendText(const std::string& text) { ws.sendTXT(text.c_str(), text.size()); }

void Esp32Net::loop() { ws.loop(); }

void Esp32Net::onSocket(std::function<void(SocketEvent, const char*, size_t)> h) { handler = h; }

Diagnosis Esp32Net::diagnose(const std::string& host, uint16_t port, const std::string& path,
                             const std::string& trust, const std::string& authorization, std::string& detail) {
  WiFiClientSecure client;
  client.setCACert(trust.c_str());
  client.setTimeout(10);
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, host.c_str(), port, path.c_str(), true)) return Diagnosis::BAD_ADDRESS;
  http.addHeader("Authorization", authorization.c_str());
  int code = http.GET();
  http.end();
  if (code == 401 || code == 403) return Diagnosis::REFUSED;
  if (code < 0) {
    char buf[96] = "";
    if (client.lastError(buf, sizeof(buf)) != 0) {
      detail = buf;
      return Diagnosis::TLS;
    }
    return Diagnosis::UNREACHABLE;
  }
  detail = std::to_string(code);
  return Diagnosis::UNEXPECTED;
}

}  // namespace hal
}  // namespace ndw
