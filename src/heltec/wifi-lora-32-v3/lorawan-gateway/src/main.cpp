// NDW LoRaWAN Gateway, for the Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262).
//
// A single-channel LoRaWAN gateway that speaks LoRa Basics Station to a
// network server — MeterFax's, at wss://lns.meterfax.com — over WiFi. For a
// bench: one of these and one board running NDW LoRaWAN meter fleet make a
// whole test network, without a real gateway.
//
// ## One channel
//
// A real gateway has a concentrator that hears eight channels at every
// spreading factor at once. This board has one SX1262, which hears one
// channel at one spreading factor: US915 channel 8, 903.9 MHz, SF7 on
// 125 kHz (DR3). The fleet firmware keeps to exactly that when programmed
// for an NDW LoRaWAN Gateway. The router_config the server sends describes a whole
// sub-band; this takes the region from it and listens on its one channel.
//
// ## Programmed over USB
//
// Nothing is compiled in. After flashing, flash.meterfax.com's Program tab
// sends one JSON command per line; every answer is a line starting "#NDW "
// followed by JSON — the protocol the other NDW firmware speaks.
//
//   {"cmd":"hello"}   → eui, firmware, kind ("lorawan-gateway"), state
//   {"cmd":"status"}  → WiFi (state, ssid, ip, mac, rssi) and the server
//                       link (lns: url, state, error, up, down, late)
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
//
// ## Basics Station, briefly
//
// The gateway asks <url>/router-info which address to use, opens a websocket
// there, says its version, and gets a router_config. Each frame heard goes up
// as a jreq or updf message, carrying "xtime" — this board's microsecond
// clock at the end of the frame. A downlink comes back as a dnmsg carrying
// that same xtime and a delay, so the gateway itself works out when RX1 (and
// RX2 after it) opens, and transmits into it. The token rides on both
// connections as an Authorization header, which is what MeterFax checks.
//
// Everything else the firmware prints starts "[gateway]" and is for people.
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <RadioLib.h>
#include <SSD1306Wire.h>
#include <WebSocketsClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <time.h>

#ifndef GATEWAY_VERSION
#define GATEWAY_VERSION ""
#endif

// Heltec WiFi LoRa 32 V3: SX1262 on its own SPI pins.
static const int PIN_NSS = 8, PIN_SCK = 9, PIN_MOSI = 10, PIN_MISO = 11;
static const int PIN_RST = 12, PIN_BUSY = 13, PIN_DIO1 = 14;

// The OLED: SSD1306 on I2C, powered through Vext, which is switched on low.
static const int PIN_OLED_SDA = 17, PIN_OLED_SCL = 18, PIN_OLED_RST = 21;
static const int PIN_VEXT = 36;

// What it listens on: US915 channel 8 at DR3, which the fleet firmware keeps
// to when programmed for an NDW LoRaWAN Gateway. Change one, change both.
static const float LISTEN_MHZ = 903.9;
static const uint32_t LISTEN_HZ = 903900000;
static const uint8_t LISTEN_SF = 7;
static const float LISTEN_BW = 125.0;
static const uint8_t LISTEN_DR = 3;

// LoRaWAN's public sync word, and a transmit power for a bench.
static const uint8_t LORAWAN_SYNC_WORD = 0x34;
static const int8_t TX_DBM = 14;

// US915's data rates by index, as Basics Station numbers them: DR0–4 up,
// DR8–13 down. A zero spreading factor is an index the band does not use.
struct DataRate {
  uint8_t sf;
  float bw;
};
static const DataRate US915_DR[14] = {
    {10, 125}, {9, 125}, {8, 125}, {7, 125}, {8, 500}, {0, 0},   {0, 0},
    {0, 0},    {12, 500}, {11, 500}, {10, 500}, {9, 500}, {8, 500}, {7, 500},
};

// How early the radio is set up for a downlink, and how late one may still
// go: a device's receive window opens for a few symbols, so a frame that is
// not on the air within a few milliseconds of its time is not heard.
static const int64_t TX_PREPARE_US = 30000;
static const int64_t TX_LATE_US = 5000;

// How long a connection may take to become a websocket before it is called
// failed and diagnosed. The websocket library reports a failed TCP or TLS
// connection to nobody — it only logs it — so without this a gateway that
// cannot connect says "connecting" for ever.
static const uint32_t OPEN_TIMEOUT_MS = 20000;

// How long to wait for NTP before connecting anyway. Until the clock is set
// it reads 1970, and every certificate looks not yet valid.
static const uint32_t CLOCK_WAIT_MS = 30000;

// A failed connection is tried again after 2 s, then twice as long each time
// up to 30 s: soon enough that a gateway is back within moments of the
// server or the network returning, and not so often it hammers either.
static const uint32_t RETRY_FIRST_MS = 2000;
static const uint32_t RETRY_MAX_MS = 30000;

// The trust file and a token, as bounds on what is accepted over USB.
static const size_t TRUST_MAX = 12000;
static const size_t TOKEN_MAX = 512;

SX1262 radio = new Module(PIN_NSS, PIN_DIO1, PIN_RST, PIN_BUSY, SPI);
SSD1306Wire oled(0x3c, PIN_OLED_SDA, PIN_OLED_SCL, GEOMETRY_128_64);
WebSocketsClient ws;
Preferences prefs;

// What it was told over USB, kept in NVS under "gateway".
struct {
  String ssid;
  String password;
  String url;    // wss://host[:port]
  String trust;  // PEM; the websocket client keeps a pointer into this
  String token;  // the whole header line: "Authorization: Bearer …"
} cfg;

// ---- small helpers -------------------------------------------------------

String firmware() {
  String v = GATEWAY_VERSION;
  return "ndw-lorawan-gateway/" + (v.length() ? v : String("dev"));
}

uint64_t boardEui() {
  uint8_t m[6];
  esp_read_mac(m, ESP_MAC_WIFI_STA);
  uint8_t e[8] = {m[0], m[1], m[2], 0xff, 0xfe, m[3], m[4], m[5]};
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = v << 8 | e[i];
  return v;
}

String hex64(uint64_t v) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%016llx", v);
  return String(buf);
}

// An EUI the way Basics Station writes one: eight pairs, dashes between.
String dashed(const uint8_t* be) {
  char buf[24];
  snprintf(buf, sizeof(buf), "%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x", be[0], be[1], be[2], be[3], be[4], be[5],
           be[6], be[7]);
  return String(buf);
}

String dashed64(uint64_t v) {
  uint8_t be[8];
  for (int i = 0; i < 8; i++) be[i] = v >> (56 - 8 * i);
  return dashed(be);
}

String hexBytes(const uint8_t* b, size_t n) {
  static const char* digits = "0123456789abcdef";
  String out;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    out += digits[b[i] >> 4];
    out += digits[b[i] & 15];
  }
  return out;
}

bool unhex(const String& s, uint8_t* out, size_t max, size_t& len) {
  if (s.length() % 2 || s.length() / 2 > max) return false;
  len = s.length() / 2;
  for (size_t i = 0; i < len; i++) {
    char h = s[2 * i], l = s[2 * i + 1];
    auto v = [](char c) -> int {
      return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    if (v(h) < 0 || v(l) < 0) return false;
    out[i] = v(h) << 4 | v(l);
  }
  return true;
}

int32_t le32(const uint8_t* b) {
  return (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
}

// The token as it goes on the wire, whichever way it was pasted: the whole
// line MeterFax shows, just "Bearer …", or only the token itself.
String tokenHeader(String t) {
  t.trim();
  if (t.startsWith("Authorization:")) return t;
  if (t.startsWith("Bearer ")) return "Authorization: " + t;
  return "Authorization: Bearer " + t;
}

// The value half of it, for HTTPClient, which takes name and value apart.
String tokenValue() {
  int colon = cfg.token.indexOf(':');
  String v = cfg.token.substring(colon + 1);
  v.trim();
  return v;
}

// wss://host[:port][/…] into its parts. Only wss: the token must not cross
// the network in the clear.
bool parseUrl(const String& url, String& host, uint16_t& port, String& path, bool& tls) {
  int schemeEnd = url.indexOf("://");
  if (schemeEnd < 0) return false;
  String scheme = url.substring(0, schemeEnd);
  tls = scheme == "wss";
  if (!tls && scheme != "ws") return false;
  String rest = url.substring(schemeEnd + 3);
  int slash = rest.indexOf('/');
  String authority = slash < 0 ? rest : rest.substring(0, slash);
  path = slash < 0 ? "/" : rest.substring(slash);
  int colon = authority.indexOf(':');
  host = colon < 0 ? authority : authority.substring(0, colon);
  port = colon < 0 ? (tls ? 443 : 80) : authority.substring(colon + 1).toInt();
  return host.length() > 0 && port > 0;
}

// ---- settings ------------------------------------------------------------

void loadSettings() {
  prefs.begin("gateway", false);
  cfg.ssid = prefs.getString("ssid", "");
  cfg.password = prefs.getString("pass", "");
  cfg.url = prefs.getString("url", "");
  cfg.token = prefs.getString("token", "");
  // A blob, not a string: NVS strings stop at 4000 bytes, and a trust file
  // holding a chain is more.
  size_t n = prefs.getBytesLength("trust");
  cfg.trust = "";
  if (n > 0 && n <= TRUST_MAX) {
    char* buf = (char*)malloc(n + 1);
    if (buf) {
      prefs.getBytes("trust", buf, n);
      buf[n] = 0;
      cfg.trust = buf;
      free(buf);
    }
  }
}

bool haveServer() {
  return cfg.url.length() && cfg.trust.length() && cfg.token.length();
}

// ---- WiFi ----------------------------------------------------------------

// Why the last attempt ended, from the driver: tells a wrong passphrase from
// a network out of range, which are fixed differently.
volatile uint8_t wifiReason = 0;
uint32_t wifiSince = 0;

void startWifi() {
  if (!cfg.ssid.length()) return;
  wifiReason = 0;
  wifiSince = millis();
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(("ndw-gw-" + hex64(boardEui()).substring(10)).c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
}

const char* wifiState() {
  if (!cfg.ssid.length()) return "unprovisioned";
  if (WiFi.status() == WL_CONNECTED) return "connected";
  switch (wifiReason) {
    case 0:
      return "connecting";
    case WIFI_REASON_NO_AP_FOUND:
      return "not-found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_EXPIRE:
      return "bad-auth";
    default:
      // Anything else while it keeps retrying reads as trying, until it has
      // been at it long enough to call a failure.
      return millis() - wifiSince > 30000 ? "failed" : "connecting";
  }
}

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

// ---- the radio -----------------------------------------------------------

volatile bool dio1 = false;
volatile int64_t dio1AtUs = 0;

void IRAM_ATTR onDio1() {
  dio1AtUs = esp_timer_get_time();
  dio1 = true;
}

bool transmitting = false;

void listenForUplinks() {
  radio.standby();
  radio.setFrequency(LISTEN_MHZ);
  radio.setBandwidth(LISTEN_BW);
  radio.setSpreadingFactor(LISTEN_SF);
  radio.setCodingRate(5);
  radio.setSyncWord(LORAWAN_SYNC_WORD);
  radio.setPreambleLength(8);
  radio.invertIQ(false);
  radio.setCRC(true);  // uplinks carry a CRC; downlinks do not
  transmitting = false;
  dio1 = false;
  radio.startReceive();
}

// ---- the server link -----------------------------------------------------

enum Link : uint8_t { L_UNSET, L_NO_WIFI, L_DISCOVERING, L_CONNECTING, L_CONNECTED, L_ERROR };
static const char* LINK_NAMES[] = {"unset", "waiting-for-wifi", "discovering", "connecting", "connected", "error"};

struct {
  Link state = L_UNSET;
  // Why not, in one word: tls | refused | unreachable | discovery | region |
  // dropped | failed. Each points at a different fix.
  String error;
  String detail;
  bool discovery = true;  // this socket is the router-info one
  bool opened = false;    // it got as far as a websocket
  bool configured = false;
  String muxHost, muxPath;
  uint16_t muxPort = 443;
  bool muxTls = true;
  bool pendingMux = false;
  bool closing = false;  // a close this asked for, not one to report
  uint32_t retryAt = 0;
  uint32_t openedAt = 0;  // when this socket was started, for OPEN_TIMEOUT_MS
  uint32_t wifiAt = 0;    // when WiFi came up, for CLOCK_WAIT_MS
  uint32_t backoffMs = RETRY_FIRST_MS;
  uint8_t session = 0;  // top byte of xtime: a downlink from an older connection is refused
  uint32_t up = 0, down = 0, late = 0;
} server;

// Closes the socket without the close reading as a failure: the library
// reports it through the same callback, and synchronously.
void closeSocket() {
  server.closing = true;
  ws.disconnect();
  server.closing = false;
}

void linkFailed(const char* error, const String& detail) {
  closeSocket();
  server.state = L_ERROR;
  server.error = error;
  server.detail = detail;
  server.configured = false;
  server.retryAt = millis() + server.backoffMs;
  server.backoffMs = min<uint32_t>(server.backoffMs * 2, RETRY_MAX_MS);
  Serial.printf("[gateway] server: %s — %s, again in %lus\n", error, detail.c_str(),
                (unsigned long)((server.retryAt - millis()) / 1000));
}

// Why a connection failed before it became a websocket. The websocket
// library says only "disconnected", which is the same event for a wrong
// trust file, a refused token and a server that is not there; an ordinary
// HTTPS request to the same place tells them apart.
void diagnose(const String& host, uint16_t port, const String& path) {
  WiFiClientSecure client;
  client.setCACert(cfg.trust.c_str());
  client.setTimeout(10);
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, host, port, path, true)) {
    linkFailed("failed", "The server address could not be used.");
    return;
  }
  http.addHeader("Authorization", tokenValue());
  int code = http.GET();
  http.end();
  if (code == 401 || code == 403) {
    linkFailed("refused", "The server turned the token away. Check it, and that this gateway is added in MeterFax.");
  } else if (code < 0) {
    char buf[96] = "";
    int tlsError = client.lastError(buf, sizeof(buf));
    if (tlsError != 0) {
      linkFailed("tls", String("The server's certificate did not check out against the trust file: ") + buf);
    } else {
      linkFailed("unreachable", "The server could not be reached from this network.");
    }
  } else {
    linkFailed("failed", "The server answered " + String(code) + " where a websocket was expected.");
  }
}

void openSocket(const String& host, uint16_t port, const String& path, bool tls) {
  closeSocket();
  server.opened = false;
  server.openedAt = millis();
  // The library's own reconnect is switched off (an hour is its "never"):
  // it retries silently, hiding the failure this diagnoses. The retries are
  // this firmware's — straight away, then backing off from 2 s to 30 s.
  ws.setReconnectInterval(3600000);
  ws.setExtraHeaders(cfg.token.c_str());
  if (tls) {
    ws.beginSslWithCA(host.c_str(), port, path.c_str(), cfg.trust.c_str(), "");
  } else {
    ws.begin(host.c_str(), port, path.c_str(), "");
  }
  // A frame every half minute keeps the connection alive through Cloudflare
  // and nginx, and notices a dead one within a minute.
  ws.enableHeartbeat(30000, 10000, 2);
}

void startDiscovery() {
  String host, path;
  uint16_t port;
  bool tls;
  if (!parseUrl(cfg.url, host, port, path, tls)) {
    linkFailed("failed", "The server address is not a wss:// URL.");
    return;
  }
  server.discovery = true;
  server.configured = false;
  server.state = L_DISCOVERING;
  server.error = "";
  server.detail = "";
  Serial.printf("[gateway] asking %s:%u/router-info where to connect\n", host.c_str(), port);
  openSocket(host, port, "/router-info", tls);
}

void startMux() {
  server.discovery = false;
  server.state = L_CONNECTING;
  server.session = (uint8_t)(esp_random() & 0xff) | 1;
  Serial.printf("[gateway] connecting to %s:%u%s\n", server.muxHost.c_str(), server.muxPort, server.muxPath.c_str());
  openSocket(server.muxHost, server.muxPort, server.muxPath, server.muxTls);
}

void sendToServer(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  ws.sendTXT(out);
}

// ---- downlinks -----------------------------------------------------------

struct Downlink {
  bool used;
  uint8_t pdu[255];
  uint8_t len;
  uint32_t diid;
  String devEui;
  uint64_t xtime;
  int64_t at[2];  // when RX1 and RX2 open, on esp_timer's clock; 0 when not offered
  uint32_t freq[2];
  int dr[2];
};
Downlink queue[4];

// A dnmsg: when to send it is the uplink's xtime plus the delay.
void onDownlink(JsonDocument& in) {
  int dc = in["dC"] | 0;
  if (dc == 1) return;  // class B needs GPS time, which this does not have
  uint64_t xtime = in["xtime"] | (uint64_t)0;
  if (dc == 0 && (xtime >> 56) != server.session) {
    Serial.println("[gateway] dropping a downlink meant for an earlier connection");
    return;
  }
  Downlink* d = nullptr;
  for (auto& q : queue)
    if (!q.used) d = &q;
  if (!d) {
    server.late++;
    Serial.println("[gateway] downlink queue full, dropping one");
    return;
  }
  size_t len = 0;
  if (!unhex(in["pdu"] | "", d->pdu, sizeof(d->pdu), len)) return;
  d->len = len;
  d->diid = in["diid"] | 0;
  d->devEui = in["DevEui"] | "";
  d->xtime = xtime;
  int64_t base = (int64_t)(xtime & 0xffffffffffffULL);
  int delayS = in["RxDelay"] | 1;
  if (delayS < 1) delayS = 1;
  if (dc == 2) {
    // Class C: now, on RX2's settings.
    d->at[0] = 0;
    d->at[1] = esp_timer_get_time() + TX_PREPARE_US;
  } else {
    d->at[0] = in["RX1Freq"].is<uint32_t>() ? base + delayS * 1000000LL : 0;
    d->at[1] = in["RX2Freq"].is<uint32_t>() ? base + (delayS + 1) * 1000000LL : 0;
  }
  d->freq[0] = in["RX1Freq"] | 0;
  d->dr[0] = in["RX1DR"] | -1;
  d->freq[1] = in["RX2Freq"] | 0;
  d->dr[1] = in["RX2DR"] | -1;
  d->used = true;
}

bool configureTx(uint32_t hz, int dr) {
  if (dr < 0 || dr > 13 || !US915_DR[dr].sf) return false;
  radio.standby();
  radio.setFrequency(hz / 1e6);
  radio.setBandwidth(US915_DR[dr].bw);
  radio.setSpreadingFactor(US915_DR[dr].sf);
  radio.setCodingRate(5);
  radio.setSyncWord(LORAWAN_SYNC_WORD);
  radio.setPreambleLength(8);
  radio.invertIQ(true);  // downlinks go out inverted, so only devices hear them
  radio.setCRC(false);
  radio.setOutputPower(TX_DBM);
  return true;
}

Downlink* sending = nullptr;
uint32_t sendingSince = 0;

// Sends whichever downlink is due into its window, RX1 if it can still make
// it and RX2 if not. Called every loop; waits in place for the last few
// milliseconds, since a window missed by more than a few is not heard.
void serviceDownlinks() {
  if (sending) {
    if (dio1) {
      dio1 = false;
      radio.finishTransmit();
      server.down++;
      JsonDocument ack;
      ack["msgtype"] = "dntxed";
      ack["diid"] = sending->diid;
      ack["DevEui"] = sending->devEui;
      ack["rctx"] = 0;
      ack["xtime"] = sending->xtime;
      ack["txtime"] = 0;
      ack["gpstime"] = 0;
      if (server.state == L_CONNECTED) sendToServer(ack);
      sending->used = false;
      sending = nullptr;
      listenForUplinks();
    } else if (millis() - sendingSince > 5000) {
      Serial.println("[gateway] a downlink never finished sending");
      sending->used = false;
      sending = nullptr;
      listenForUplinks();
    }
    return;
  }

  int64_t now = esp_timer_get_time();
  for (auto& d : queue) {
    if (!d.used) continue;
    for (int w = 0; w < 2; w++) {
      if (!d.at[w]) continue;
      if (now > d.at[w] + TX_LATE_US) {
        d.at[w] = 0;  // that window has gone
        continue;
      }
      if (now < d.at[w] - TX_PREPARE_US) break;  // not yet; later windows are later still
      if (!configureTx(d.freq[w], d.dr[w])) {
        d.at[w] = 0;
        continue;
      }
      while (esp_timer_get_time() < d.at[w]) {
      }
      transmitting = true;
      dio1 = false;
      if (radio.startTransmit(d.pdu, d.len) == RADIOLIB_ERR_NONE) {
        sending = &d;
        sendingSince = millis();
      } else {
        d.used = false;
        listenForUplinks();
      }
      return;
    }
    if (!d.at[0] && !d.at[1]) {
      server.late++;
      Serial.printf("[gateway] downlink %lu missed both windows\n", (unsigned long)d.diid);
      d.used = false;
    }
  }
}

// ---- uplinks -------------------------------------------------------------

double rxTime() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  // Before NTP has answered the clock reads 1970; the server takes 0 as "no
  // time" and uses its own.
  return tv.tv_sec > 1600000000 ? tv.tv_sec + tv.tv_usec / 1e6 : 0;
}

void addUpinfo(JsonDocument& doc, int64_t atUs, float rssi, float snr) {
  doc["DR"] = LISTEN_DR;
  doc["Freq"] = LISTEN_HZ;
  JsonObject up = doc["upinfo"].to<JsonObject>();
  up["rctx"] = 0;
  up["xtime"] = ((uint64_t)server.session << 56) | ((uint64_t)atUs & 0xffffffffffffULL);
  up["gpstime"] = 0;
  up["fts"] = -1;
  up["rssi"] = rssi;
  up["snr"] = snr;
  up["rxtime"] = rxTime();
}

// A frame off the air, as Basics Station takes it apart: a join request or a
// data frame, field by field. Anything else is not LoRaWAN this carries.
void forward(const uint8_t* b, size_t n, int64_t atUs, float rssi, float snr) {
  if (server.state != L_CONNECTED || !server.configured || n < 1) return;
  uint8_t mtype = b[0] >> 5;
  JsonDocument doc;
  if (mtype == 0 && n == 23) {
    uint8_t joinEui[8], devEui[8];
    for (int i = 0; i < 8; i++) {
      joinEui[i] = b[8 - i];
      devEui[i] = b[16 - i];
    }
    doc["msgtype"] = "jreq";
    doc["MHdr"] = b[0];
    doc["JoinEui"] = dashed(joinEui);
    doc["DevEui"] = dashed(devEui);
    doc["DevNonce"] = b[17] | b[18] << 8;
    doc["MIC"] = le32(b + 19);
  } else if ((mtype == 2 || mtype == 4) && n >= 12) {
    uint8_t fctrl = b[5];
    size_t foptsLen = fctrl & 0x0f;
    if (8 + foptsLen + 4 > n) return;
    size_t rest = n - 8 - foptsLen - 4;
    doc["msgtype"] = "updf";
    doc["MHdr"] = b[0];
    doc["DevAddr"] = le32(b + 1);
    doc["FCtrl"] = fctrl;
    doc["FCnt"] = b[6] | b[7] << 8;
    doc["FOpts"] = hexBytes(b + 8, foptsLen);
    doc["FPort"] = rest ? b[8 + foptsLen] : -1;
    doc["FRMPayload"] = rest > 1 ? hexBytes(b + 9 + foptsLen, rest - 1) : String("");
    doc["MIC"] = le32(b + n - 4);
  } else {
    return;
  }
  addUpinfo(doc, atUs, rssi, snr);
  sendToServer(doc);
  server.up++;
}

void serviceRadio() {
  if (transmitting || !dio1) return;
  dio1 = false;
  int64_t at = dio1AtUs;
  uint8_t buf[256];
  size_t n = radio.getPacketLength();
  if (n > sizeof(buf)) n = sizeof(buf);
  int16_t rc = radio.readData(buf, n);
  float rssi = radio.getRSSI(), snr = radio.getSNR();
  radio.startReceive();
  if (rc != RADIOLIB_ERR_NONE) return;  // a CRC failure is noise, not a frame
  forward(buf, n, at, rssi, snr);
}

// ---- websocket events ----------------------------------------------------

void onMessage(const char* text, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, text, len)) return;

  if (server.discovery) {
    String err = in["error"] | "";
    String uri = in["uri"] | "";
    String path;
    if (err.length() || !uri.length()) {
      linkFailed("discovery", err.length() ? err : String("The server did not say where to connect."));
      return;
    }
    bool tls;
    String host;
    uint16_t port;
    if (!parseUrl(uri, host, port, path, tls)) {
      linkFailed("discovery", "The server named an address this cannot use: " + uri);
      return;
    }
    // TLS whenever the configured address was: a bridge behind a proxy that
    // ends TLS can name its own ws:// address, and the token must not travel
    // in the clear because of it.
    bool configuredTls;
    String h, p;
    uint16_t pt;
    parseUrl(cfg.url, h, pt, p, configuredTls);
    server.muxHost = host;
    server.muxPort = port;
    server.muxPath = path;
    server.muxTls = tls || configuredTls;
    server.pendingMux = true;  // not from inside the library's callback
    return;
  }

  String type = in["msgtype"] | "";
  if (type == "router_config") {
    String region = in["region"] | "";
    if (!region.startsWith("US")) {
      linkFailed("region", "The server runs " + region + "; this gateway only knows US915.");
      return;
    }
    server.configured = true;
    server.state = L_CONNECTED;
    server.backoffMs = RETRY_FIRST_MS;
    Serial.printf("[gateway] connected, %s, listening on %.1f MHz SF%u\n", region.c_str(), LISTEN_MHZ, LISTEN_SF);
  } else if (type == "dnmsg") {
    onDownlink(in);
  }
}

void onSocket(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      server.opened = true;
      if (server.discovery) {
        JsonDocument q;
        q["router"] = dashed64(boardEui());
        sendToServer(q);
      } else {
        JsonDocument v;
        v["msgtype"] = "version";
        v["station"] = firmware();
        v["firmware"] = firmware();
        v["package"] = "";
        v["model"] = "heltec-wifi-lora-32-v3";
        v["protocol"] = 2;
        v["features"] = "";
        sendToServer(v);
      }
      break;
    case WStype_TEXT:
      onMessage((const char*)payload, length);
      break;
    case WStype_DISCONNECTED:
      if (server.closing || server.pendingMux || server.state == L_ERROR) break;  // ours, on purpose
      if (!server.opened) {
        // Never became a websocket: find out why, off this callback.
        server.state = L_ERROR;
        server.error = "diagnosing";
        server.retryAt = 0;
      } else {
        linkFailed("dropped", "The connection closed.");
      }
      break;
    default:
      break;
  }
}

void serviceLink() {
  if (!haveServer()) {
    server.state = L_UNSET;
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (server.state != L_NO_WIFI && server.state != L_UNSET) closeSocket();
    server.state = L_NO_WIFI;
    server.wifiAt = 0;
    server.configured = false;
    return;
  }
  if (server.state == L_NO_WIFI || server.state == L_UNSET) {
    if (!server.wifiAt) server.wifiAt = millis();
    // The clock first: TLS checks the certificate's dates against it.
    if (time(nullptr) < 1600000000 && millis() - server.wifiAt < CLOCK_WAIT_MS) return;
    if (time(nullptr) < 1600000000) Serial.println("[gateway] no time from NTP yet; connecting anyway");
    server.backoffMs = RETRY_FIRST_MS;
    startDiscovery();
    return;
  }
  if ((server.state == L_DISCOVERING || server.state == L_CONNECTING) && !server.opened && !server.pendingMux &&
      millis() - server.openedAt > OPEN_TIMEOUT_MS) {
    Serial.println("[gateway] no websocket after 20s, finding out why");
    closeSocket();
    server.state = L_ERROR;
    server.error = "diagnosing";
  }
  if (server.pendingMux) {
    server.pendingMux = false;
    startMux();
    return;
  }
  if (server.state == L_ERROR && server.error == "diagnosing") {
    String host, path;
    uint16_t port;
    bool tls;
    if (server.discovery) {
      parseUrl(cfg.url, host, port, path, tls);
      path = "/router-info";
    } else {
      host = server.muxHost;
      port = server.muxPort;
      path = server.muxPath;
    }
    diagnose(host, port, path);
    return;
  }
  if (server.state == L_ERROR && (int32_t)(millis() - server.retryAt) >= 0) {
    startDiscovery();
    return;
  }
  ws.loop();
}

// ---- the screen ----------------------------------------------------------

void draw() {
  oled.clear();
  oled.setFont(ArialMT_Plain_10);
  oled.setTextAlignment(TEXT_ALIGN_LEFT);
  oled.drawString(0, 0, "NDW LoRaWAN Gateway");
  oled.drawHorizontalLine(0, 12, 128);
  oled.drawString(0, 14, hex64(boardEui()));
  String wifi = String("WiFi ") + wifiState();
  if (WiFi.status() == WL_CONNECTED) wifi = "WiFi " + cfg.ssid;
  oled.drawString(0, 26, wifi);
  String link = LINK_NAMES[server.state];
  if (server.state == L_ERROR) link = server.error;
  oled.drawString(0, 38, "Server " + link);
  oled.drawString(0, 50, "up " + String(server.up) + "  down " + String(server.down) + "  903.9 SF7");
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

// ---- the host protocol ---------------------------------------------------

void reply(JsonDocument& doc) {
  Serial.print("#NDW ");
  serializeJson(doc, Serial);
  Serial.println();
}

void refuse(const char* error, const String& detail, const char* field = nullptr) {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = error;
  doc["detail"] = detail;
  if (field) {
    JsonObject f = doc["faults"].to<JsonArray>().add<JsonObject>();
    f["field"] = field;
    f["message"] = detail;
  }
  reply(doc);
}

void describe(JsonDocument& doc) {
  doc["ok"] = true;
  doc["eui"] = hex64(boardEui());
  doc["firmware"] = firmware();
  // What this is, for a host deciding what to offer.
  doc["kind"] = "lorawan-gateway";
  doc["state"] = wifiState();
  JsonArray caps = doc["capabilities"].to<JsonArray>();
  caps.add("wifi");
  caps.add("lns");
}

// The WiFi half in the shape the router firmware reports, so the same host
// code reads both; the server link beside it.
void status(JsonDocument& doc) {
  describe(doc);
  doc["ssid"] = cfg.ssid;
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("");
  doc["mac"] = WiFi.macAddress();
  doc["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  JsonObject l = doc["lns"].to<JsonObject>();
  l["url"] = cfg.url;
  l["trust"] = cfg.trust.length() > 0;
  l["token"] = cfg.token.length() > 0;
  if (cfg.token.length() >= 4) l["tokenHint"] = cfg.token.substring(cfg.token.length() - 4);
  l["state"] = server.state == L_ERROR && server.error == "diagnosing" ? "connecting" : LINK_NAMES[server.state];
  l["error"] = server.state == L_ERROR && server.error != "diagnosing" ? server.error : String("");
  l["detail"] = server.state == L_ERROR ? server.detail : String("");
  l["up"] = server.up;
  l["down"] = server.down;
  l["late"] = server.late;
  JsonObject radioInfo = doc["radio"].to<JsonObject>();
  radioInfo["band"] = "US915";
  radioInfo["channel"] = 8;
  radioInfo["freqMHz"] = LISTEN_MHZ;
  radioInfo["sf"] = LISTEN_SF;
  radioInfo["dr"] = LISTEN_DR;
}

void rebootSoon() {
  Serial.flush();
  delay(300);
  ESP.restart();
}

void onCommand(const String& line) {
  JsonDocument in;
  if (deserializeJson(in, line)) {
    refuse("bad-json", "That line is not JSON.");
    return;
  }
  String cmd = in["cmd"] | "";

  if (cmd == "hello") {
    JsonDocument out;
    describe(out);
    reply(out);
  } else if (cmd == "status") {
    JsonDocument out;
    status(out);
    reply(out);
  } else if (cmd == "scan") {
    int n = WiFi.scanNetworks();
    JsonDocument out;
    out["ok"] = true;
    JsonArray nets = out["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      JsonObject net = nets.add<JsonObject>();
      net["ssid"] = WiFi.SSID(i);
      net["rssi"] = WiFi.RSSI(i);
      net["auth"] = authName(WiFi.encryptionType(i));
    }
    WiFi.scanDelete();
    reply(out);
  } else if (cmd == "wifi") {
    String ssid = in["ssid"] | "";
    String password = in["password"] | "";
    if (!ssid.length() || ssid.length() > 32) {
      refuse("invalid", "Choose a network.", "ssid");
      return;
    }
    if (password.length() > 63) {
      refuse("invalid", "A WiFi passphrase is at most 63 characters.", "password");
      return;
    }
    cfg.ssid = ssid;
    cfg.password = password;
    prefs.putString("ssid", ssid);
    prefs.putString("pass", password);
    startWifi();
    // Answers with where the join got, so the page can say which of a wrong
    // passphrase and an absent network it was.
    uint32_t until = millis() + 30000;
    while (millis() < until) {
      String s = wifiState();
      if (s != "connecting") break;
      delay(100);
    }
    JsonDocument out;
    status(out);
    reply(out);
  } else if (cmd == "lns") {
    String url = in["url"] | "";
    String trust = in["trust"] | "";
    String token = in["token"] | "";
    url.trim();
    token.trim();
    String host, path;
    uint16_t port;
    bool tls;
    if (!parseUrl(url, host, port, path, tls) || !tls) {
      refuse("invalid", "The server address is a wss:// URL.", "url");
      return;
    }
    if (trust.indexOf("-----BEGIN CERTIFICATE-----") < 0 || trust.length() > TRUST_MAX) {
      refuse("invalid", "That file is not a trust file: it holds no certificate.", "trust");
      return;
    }
    if (!token.length() || token.length() > TOKEN_MAX) {
      refuse("invalid", "Paste the gateway's token from MeterFax.", "token");
      return;
    }
    // A path on the address is dropped: Basics Station asks /router-info.
    cfg.url = "wss://" + host + (port == 443 ? String("") : ":" + String(port));
    cfg.token = tokenHeader(token);
    closeSocket();
    cfg.trust = trust;
    prefs.putString("url", cfg.url);
    prefs.putString("token", cfg.token);
    prefs.putBytes("trust", cfg.trust.c_str(), cfg.trust.length());
    server.state = L_UNSET;  // connects on the next pass, if WiFi is up
    server.up = server.down = server.late = 0;
    JsonDocument out;
    status(out);
    reply(out);
  } else if (cmd == "forget") {
    prefs.clear();
    JsonDocument out;
    out["ok"] = true;
    out["state"] = "rebooting";
    reply(out);
    rebootSoon();
  } else if (cmd == "reboot") {
    JsonDocument out;
    out["ok"] = true;
    out["state"] = "rebooting";
    reply(out);
    rebootSoon();
  } else {
    refuse("unknown-command", "That command is not one this firmware knows.");
  }
}

// Reads whatever has arrived, and acts on each complete line. The trust file
// makes the lns line several kilobytes, so the buffer is sized for it.
void pollSerial() {
  static String pending;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      pending.trim();
      if (pending.length()) onCommand(pending);
      pending = "";
    } else if (pending.length() < 16384) {
      pending += c;
    }
  }
}

// ---- main ----------------------------------------------------------------

void setup() {
  Serial.setRxBufferSize(16384);
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[gateway] %s, EUI %s\n", firmware().c_str(), hex64(boardEui()).c_str());

  startScreen();
  loadSettings();

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) wifiReason = info.wifi_sta_disconnected.reason;
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      wifiReason = 0;
      Serial.printf("[gateway] on %s as %s\n", cfg.ssid.c_str(), WiFi.localIP().toString().c_str());
    }
  });
  WiFi.mode(WIFI_STA);
  startWifi();
  // The clock is for rxtime only; the server has its own when this has none.
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  ws.onEvent(onSocket);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_NSS);
  // 1.8 V on the TCXO, as the board wires it.
  int16_t rc = radio.begin(LISTEN_MHZ, LISTEN_BW, LISTEN_SF, 5, LORAWAN_SYNC_WORD, TX_DBM, 8, 1.8, false);
  if (rc != RADIOLIB_ERR_NONE) Serial.printf("[gateway] radio.begin: %d\n", rc);
  radio.setDio1Action(onDio1);
  listenForUplinks();

  if (!cfg.ssid.length()) Serial.println("[gateway] no WiFi yet: program it from flash.meterfax.com");
  if (!haveServer()) Serial.println("[gateway] no server yet: program it from flash.meterfax.com");
  draw();
}

void loop() {
  pollSerial();
  serviceRadio();
  serviceDownlinks();
  serviceLink();
  serviceDownlinks();

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw > 1000) {
    draw();
    lastDraw = millis();
  }
}
