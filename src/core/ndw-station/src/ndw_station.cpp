#include "ndw_station.h"

#include <stdio.h>
#include <string.h>

#include <ArduinoJson.h>

#include "ndw_text.h"

namespace ndw {
namespace station {

using hal::Diagnosis;
using hal::SocketEvent;
using hal::WifiState;
using text::hex64;
using text::startsWith;
using text::trim;

namespace {

const Identity* id = nullptr;
hal::Hal* H = nullptr;
hal::Net* net = nullptr;
hal::Store* store = nullptr;
radio::GatewayRadio* radio = nullptr;
Counters counts;

// How long a connection may take to become a websocket before it is called
// failed and diagnosed. A websocket library reports a failed TCP or TLS
// connection to nobody, so without this a gateway that cannot connect says
// "connecting" for ever.
const uint32_t OPEN_TIMEOUT_MS = 20000;

// How long to wait for NTP before connecting anyway. Until the clock is set
// it reads 1970, and every certificate looks not yet valid.
const uint32_t CLOCK_WAIT_MS = 30000;

// A failed connection is tried again after 2 s, then twice as long each time
// up to 30 s: soon enough that a gateway is back within moments of the
// server or the network returning, and not so often it hammers either.
const uint32_t RETRY_FIRST_MS = 2000;
const uint32_t RETRY_MAX_MS = 30000;

// The trust file and a token, as bounds on what is accepted over USB.
const size_t TRUST_MAX = 12000;
const size_t TOKEN_MAX = 512;

// A line from the host: the lns command carries the whole trust file.
const size_t HOST_LINE_MAX = 16384;

// What it was told over USB, kept in the settings store.
struct {
  std::string ssid;
  std::string password;
  std::string url;    // wss://host[:port]
  std::string trust;  // PEM
  std::string token;  // the whole header line: "Authorization: Bearer …"
} cfg;

// ---- small helpers -----------------------------------------------------------

uint32_t now() { return H->clock.millis(); }

void say(const std::string& line) { H->console.writeLine("[gateway] " + line); }

std::string firmwareName() {
  std::string v = id->version;
  return std::string(id->name) + "/" + (v.size() ? v : std::string("dev"));
}

int32_t le32(const uint8_t* b) {
  return (int32_t)((uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
}

// An EUI from the bytes as they are on the air, least significant first.
uint64_t euiOnAir(const uint8_t* b) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; i--) v = v << 8 | b[i];
  return v;
}

// The token as it goes on the wire, whichever way it was pasted: the whole
// line MeterFax shows, just "Bearer …", or only the token itself.
std::string tokenHeader(std::string t) {
  t = trim(t);
  if (startsWith(t, "Authorization:")) return t;
  if (startsWith(t, "Bearer ")) return "Authorization: " + t;
  return "Authorization: Bearer " + t;
}

// The value half of it, for a request that takes name and value apart.
std::string tokenValue() {
  size_t colon = cfg.token.find(':');
  return trim(colon == std::string::npos ? cfg.token : cfg.token.substr(colon + 1));
}

// ws[s]://host[:port][/…] into its parts.
bool parseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path, bool& tls) {
  size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) return false;
  std::string scheme = url.substr(0, schemeEnd);
  tls = scheme == "wss";
  if (!tls && scheme != "ws") return false;
  std::string rest = url.substr(schemeEnd + 3);
  size_t slash = rest.find('/');
  std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
  path = slash == std::string::npos ? "/" : rest.substr(slash);
  size_t colon = authority.find(':');
  host = colon == std::string::npos ? authority : authority.substr(0, colon);
  port = colon == std::string::npos ? (tls ? 443 : 80) : (uint16_t)atoi(authority.substr(colon + 1).c_str());
  return host.size() > 0 && port > 0;
}

// ---- settings ----------------------------------------------------------------

std::string readString(const char* key, size_t max) {
  size_t n = store->size(key);
  if (!n || n > max) return "";
  std::string s(n, '\0');
  if (!store->get(key, &s[0], n)) return "";
  return s;
}

void writeString(const char* key, const std::string& value) { store->put(key, value.data(), value.size()); }

void loadSettings() {
  cfg.ssid = readString("ssid", 64);
  cfg.password = readString("pass", 128);
  cfg.url = readString("url", 256);
  cfg.token = readString("token", TOKEN_MAX + 32);
  cfg.trust = readString("trust", TRUST_MAX);
}

bool haveServer() { return cfg.url.size() && cfg.trust.size() && cfg.token.size(); }

// ---- WiFi --------------------------------------------------------------------

void startWifi() {
  if (!cfg.ssid.size()) return;
  net->wifiBegin(cfg.ssid, cfg.password, "ndw-gw-" + hex64(H->board.eui()).substr(10));
}

const char* wifiStateName() {
  if (!cfg.ssid.size()) return "unprovisioned";
  switch (net->wifiState()) {
    case WifiState::CONNECTED:
      return "connected";
    case WifiState::NOT_FOUND:
      return "not-found";
    case WifiState::BAD_AUTH:
      return "bad-auth";
    case WifiState::FAILED:
      return "failed";
    default:
      return "connecting";
  }
}

bool wifiUp() { return net->wifiState() == WifiState::CONNECTED; }

// ---- the server link -----------------------------------------------------------

enum Link : uint8_t { L_UNSET, L_NO_WIFI, L_DISCOVERING, L_CONNECTING, L_CONNECTED, L_ERROR };
const char* LINK_NAMES[] = {"unset", "waiting-for-wifi", "discovering", "connecting", "connected", "error"};

struct {
  Link state = L_UNSET;
  // Why not, in one word: tls | refused | unreachable | discovery | region |
  // dropped | failed. Each points at a different fix.
  std::string error;
  std::string detail;
  bool discovery = true;  // this socket is the router-info one
  bool opened = false;    // it got as far as a websocket
  bool configured = false;
  std::string muxHost, muxPath;
  uint16_t muxPort = 443;
  bool muxTls = true;
  bool pendingMux = false;
  bool closing = false;  // a close this asked for, not one to report
  uint32_t retryAt = 0;
  uint32_t openedAt = 0;  // when this socket was started, for OPEN_TIMEOUT_MS
  uint32_t wifiAt = 0;    // when WiFi came up, for CLOCK_WAIT_MS
  uint32_t backoffMs = RETRY_FIRST_MS;
  uint8_t session = 0;  // top byte of xtime: a downlink from an older connection is refused
} server;

// Closes the socket without the close reading as a failure.
void closeSocket() {
  server.closing = true;
  net->close();
  server.closing = false;
}

void linkFailed(const char* error, const std::string& detail) {
  closeSocket();
  server.state = L_ERROR;
  server.error = error;
  server.detail = detail;
  server.configured = false;
  server.retryAt = now() + server.backoffMs;
  server.backoffMs = server.backoffMs * 2 < RETRY_MAX_MS ? server.backoffMs * 2 : RETRY_MAX_MS;
  say("server: " + std::string(error) + " — " + detail + ", again in " +
      std::to_string((server.retryAt - now()) / 1000) + "s");
}

// Why a connection failed before it became a websocket: the socket says only
// "closed" for a wrong trust file, a refused token and a server that is not
// there alike; an ordinary HTTPS request to the same place tells them apart.
void diagnose(const std::string& host, uint16_t port, const std::string& path) {
  std::string detail;
  switch (net->diagnose(host, port, path, cfg.trust, tokenValue(), detail)) {
    case Diagnosis::REFUSED:
      linkFailed("refused", "The server turned the token away. Check it, and that this gateway is added in MeterFax.");
      break;
    case Diagnosis::TLS:
      linkFailed("tls", "The server's certificate did not check out against the trust file: " + detail);
      break;
    case Diagnosis::UNREACHABLE:
      linkFailed("unreachable", "The server could not be reached from this network.");
      break;
    case Diagnosis::BAD_ADDRESS:
      linkFailed("failed", "The server address could not be used.");
      break;
    default:
      linkFailed("failed", "The server answered " + detail + " where a websocket was expected.");
  }
}

void openSocket(const std::string& host, uint16_t port, const std::string& path, bool tls) {
  closeSocket();
  server.opened = false;
  server.openedAt = now();
  net->open(host, port, path, tls, cfg.trust, cfg.token);
}

void startDiscovery() {
  std::string host, path;
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
  say("asking " + host + ":" + std::to_string(port) + "/router-info where to connect");
  openSocket(host, port, "/router-info", tls);
}

void startMux() {
  server.discovery = false;
  server.state = L_CONNECTING;
  server.session = (uint8_t)(H->random.next() & 0xff) | 1;
  say("connecting to " + server.muxHost + ":" + std::to_string(server.muxPort) + server.muxPath);
  openSocket(server.muxHost, server.muxPort, server.muxPath, server.muxTls);
}

void sendToServer(JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  net->sendText(out);
}

// ---- frames up -----------------------------------------------------------------

void addUpinfo(JsonDocument& doc, const radio::Heard& h) {
  doc["DR"] = radio->reportDr();
  doc["Freq"] = radio->reportHz();
  JsonObject up = doc["upinfo"].to<JsonObject>();
  up["rctx"] = 0;
  up["xtime"] = ((uint64_t)server.session << 56) | (h.atUs & 0xffffffffffffULL);
  up["gpstime"] = 0;
  up["fts"] = -1;
  up["rssi"] = h.rssi;
  up["snr"] = h.snr;
  // Before NTP has answered the server takes 0 as "no time" and uses its own.
  up["rxtime"] = net->unixTime();
}

// A frame off the air, as Basics Station takes it apart: a join request or a
// data frame, field by field. Anything else is not LoRaWAN this carries.
void forward(const radio::Heard& h) {
  const uint8_t* b = h.frame;
  size_t n = h.len;
  if (server.state != L_CONNECTED || !server.configured || n < 1) return;
  uint8_t mtype = b[0] >> 5;
  JsonDocument doc;
  if (mtype == 0 && n == 23) {
    doc["msgtype"] = "jreq";
    doc["MHdr"] = b[0];
    doc["JoinEui"] = text::dashed(euiOnAir(b + 1));
    doc["DevEui"] = text::dashed(euiOnAir(b + 9));
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
    doc["FOpts"] = text::hexBytes(b + 8, foptsLen);
    doc["FPort"] = rest ? b[8 + foptsLen] : -1;
    doc["FRMPayload"] = rest > 1 ? text::hexBytes(b + 9 + foptsLen, rest - 1) : std::string("");
    doc["MIC"] = le32(b + n - 4);
  } else {
    return;
  }
  addUpinfo(doc, h);
  sendToServer(doc);
  counts.up++;
}

// ---- frames down -----------------------------------------------------------------

// A dnmsg, handed to the radio with what it needs to find the device's window.
void onDownlink(JsonDocument& in) {
  radio::DownlinkRequest d;
  d.deviceClass = in["dC"] | 0;
  d.xtime = in["xtime"] | (uint64_t)0;
  if (d.deviceClass == 0 && (d.xtime >> 56) != server.session) {
    say("dropping a downlink meant for an earlier connection");
    return;
  }
  if (!text::unhex(in["pdu"] | "", d.pdu, sizeof(d.pdu), d.len)) return;
  // Unsigned and 32 bits: ChirpStack's ids run past what a signed int holds,
  // and read as one they came back 0, acknowledging nothing.
  d.diid = in["diid"].as<uint32_t>();
  d.devEui = in["DevEui"] | "";
  d.uplinkUs = d.xtime & 0xffffffffffffULL;
  d.rxDelayS = in["RxDelay"] | 1;
  if (d.rxDelayS < 1) d.rxDelayS = 1;
  d.hz[0] = in["RX1Freq"] | 0;
  d.dr[0] = in["RX1Freq"].is<uint32_t>() ? (in["RX1DR"] | -1) : -1;
  d.hz[1] = in["RX2Freq"] | 0;
  d.dr[1] = in["RX2Freq"].is<uint32_t>() ? (in["RX2DR"] | -1) : -1;
  if (!radio->queue(d)) counts.late++;
}

void serviceRadio() {
  radio::Heard h;
  while (radio->heard(h)) forward(h);
  radio::DownlinkOutcome done;
  while (radio->service(done)) {
    if (!done.sent) {
      counts.late++;
      continue;
    }
    counts.down++;
    JsonDocument ack;
    ack["msgtype"] = "dntxed";
    ack["diid"] = done.diid;
    ack["DevEui"] = done.devEui;
    ack["rctx"] = 0;
    ack["xtime"] = done.xtime;
    ack["txtime"] = 0;
    ack["gpstime"] = 0;
    if (server.state == L_CONNECTED) sendToServer(ack);
  }
}

// ---- websocket events ------------------------------------------------------------

void onMessage(const char* text, size_t len) {
  JsonDocument in;
  if (deserializeJson(in, text, len)) return;

  if (server.discovery) {
    std::string err = in["error"] | "";
    std::string uri = in["uri"] | "";
    if (err.size() || !uri.size()) {
      linkFailed("discovery", err.size() ? err : std::string("The server did not say where to connect."));
      return;
    }
    bool tls;
    std::string host, path;
    uint16_t port;
    if (!parseUrl(uri, host, port, path, tls)) {
      linkFailed("discovery", "The server named an address this cannot use: " + uri);
      return;
    }
    // TLS whenever the configured address was: a bridge behind a proxy that
    // ends TLS can name its own ws:// address, and the token must not travel
    // in the clear because of it.
    bool configuredTls;
    std::string h, p;
    uint16_t pt;
    parseUrl(cfg.url, h, pt, p, configuredTls);
    server.muxHost = host;
    server.muxPort = port;
    server.muxPath = path;
    server.muxTls = tls || configuredTls;
    server.pendingMux = true;  // not from inside the socket's callback
    return;
  }

  std::string type = in["msgtype"] | "";
  if (type == "router_config") {
    std::string region = in["region"] | "";
    if (!startsWith(region, "US")) {
      linkFailed("region", "The server runs " + region + "; this gateway only knows US915.");
      return;
    }
    server.configured = true;
    server.state = L_CONNECTED;
    server.backoffMs = RETRY_FIRST_MS;
    say("connected, " + region + ", listening on " + radio->listening());
  } else if (type == "dnmsg") {
    onDownlink(in);
  }
}

void onSocket(SocketEvent type, const char* payload, size_t length) {
  switch (type) {
    case SocketEvent::OPENED:
      server.opened = true;
      if (server.discovery) {
        JsonDocument q;
        q["router"] = text::dashed(H->board.eui());
        sendToServer(q);
      } else {
        JsonDocument v;
        v["msgtype"] = "version";
        v["station"] = firmwareName();
        v["firmware"] = firmwareName();
        v["package"] = "";
        v["model"] = id->model;
        v["protocol"] = 2;
        v["features"] = "";
        sendToServer(v);
      }
      break;
    case SocketEvent::TEXT:
      onMessage(payload, length);
      break;
    case SocketEvent::CLOSED:
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
  }
}

void serviceLink() {
  if (!haveServer()) {
    server.state = L_UNSET;
    return;
  }
  if (!wifiUp()) {
    if (server.state != L_NO_WIFI && server.state != L_UNSET) closeSocket();
    server.state = L_NO_WIFI;
    server.wifiAt = 0;
    server.configured = false;
    return;
  }
  if (server.state == L_NO_WIFI || server.state == L_UNSET) {
    if (!server.wifiAt) server.wifiAt = now();
    // The clock first: TLS checks the certificate's dates against it.
    bool clock = net->unixTime() > 1600000000;
    if (!clock && now() - server.wifiAt < CLOCK_WAIT_MS) return;
    if (!clock) say("no time from NTP yet; connecting anyway");
    server.backoffMs = RETRY_FIRST_MS;
    startDiscovery();
    return;
  }
  if ((server.state == L_DISCOVERING || server.state == L_CONNECTING) && !server.opened && !server.pendingMux &&
      now() - server.openedAt > OPEN_TIMEOUT_MS) {
    say("no websocket after 20s, finding out why");
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
    std::string host, path;
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
  if (server.state == L_ERROR) {
    // Waiting out the backoff, with the socket still.
    if ((int32_t)(now() - server.retryAt) >= 0) startDiscovery();
    return;
  }
  net->loop();
}

// ---- the host protocol ---------------------------------------------------------

void reply(JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  H->console.writeLine("#NDW " + out);
}

void refuse(const char* error, const std::string& detail, const char* field = nullptr) {
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
  doc["eui"] = hex64(H->board.eui());
  doc["firmware"] = firmwareName();
  // What this is, for a host deciding what to offer.
  doc["kind"] = id->kind;
  doc["state"] = wifiStateName();
  JsonArray caps = doc["capabilities"].to<JsonArray>();
  caps.add("wifi");
  caps.add("lns");
}

// The WiFi half in the shape the router firmware reported, so the same host
// code reads every gateway; the server link beside it.
void status(JsonDocument& doc) {
  describe(doc);
  doc["ssid"] = cfg.ssid;
  doc["ip"] = wifiUp() ? net->ip() : std::string("");
  doc["mac"] = net->mac();
  doc["rssi"] = wifiUp() ? net->rssi() : 0;
  JsonObject l = doc["lns"].to<JsonObject>();
  l["url"] = cfg.url;
  l["trust"] = cfg.trust.size() > 0;
  l["token"] = cfg.token.size() > 0;
  if (cfg.token.size() >= 4) l["tokenHint"] = cfg.token.substr(cfg.token.size() - 4);
  l["state"] = server.state == L_ERROR && server.error == "diagnosing" ? "connecting" : LINK_NAMES[server.state];
  l["error"] = server.state == L_ERROR && server.error != "diagnosing" ? server.error : std::string("");
  l["detail"] = server.state == L_ERROR ? server.detail : std::string("");
  l["up"] = counts.up;
  l["down"] = counts.down;
  l["late"] = counts.late;
  radio->describe(doc["radio"].to<JsonObject>());
}

void rebootSoon() {
  H->console.flush();
  H->clock.sleepMs(300);
  H->board.reboot();
}

void onCommand(const std::string& line) {
  JsonDocument in;
  if (deserializeJson(in, line)) {
    refuse("bad-json", "That line is not JSON.");
    return;
  }
  std::string cmd = in["cmd"] | "";

  if (cmd == "hello") {
    JsonDocument out;
    describe(out);
    reply(out);
  } else if (cmd == "status") {
    JsonDocument out;
    status(out);
    reply(out);
  } else if (cmd == "scan") {
    JsonDocument out;
    out["ok"] = true;
    JsonArray nets = out["networks"].to<JsonArray>();
    // A radio sharing the antenna stands aside: a WiFi scan dwells on every
    // channel, and against a BLE scan most of that time it heard nothing.
    radio->pause(true);
    std::vector<hal::WifiNetwork> found = net->scan();
    radio->pause(false);
    for (const hal::WifiNetwork& n : found) {
      JsonObject o = nets.add<JsonObject>();
      o["ssid"] = n.ssid;
      o["rssi"] = n.rssi;
      o["auth"] = n.auth;
    }
    reply(out);
  } else if (cmd == "wifi") {
    std::string ssid = in["ssid"] | "";
    std::string password = in["password"] | "";
    if (!ssid.size() || ssid.size() > 32) {
      refuse("invalid", "Choose a network.", "ssid");
      return;
    }
    if (password.size() > 63) {
      refuse("invalid", "A WiFi passphrase is at most 63 characters.", "password");
      return;
    }
    cfg.ssid = ssid;
    cfg.password = password;
    writeString("ssid", ssid);
    writeString("pass", password);
    startWifi();
    // Answers with where the join got, so the page can say which of a wrong
    // passphrase and an absent network it was.
    uint32_t until = now() + 30000;
    while ((int32_t)(until - now()) > 0) {
      if (strcmp(wifiStateName(), "connecting") != 0) break;
      H->clock.sleepMs(100);
    }
    JsonDocument out;
    status(out);
    reply(out);
  } else if (cmd == "lns") {
    std::string url = trim(in["url"] | "");
    std::string trust = in["trust"] | "";
    std::string token = trim(in["token"] | "");
    std::string host, path;
    uint16_t port;
    bool tls;
    if (!parseUrl(url, host, port, path, tls) || !tls) {
      refuse("invalid", "The server address is a wss:// URL.", "url");
      return;
    }
    if (trust.find("-----BEGIN CERTIFICATE-----") == std::string::npos || trust.size() > TRUST_MAX) {
      refuse("invalid", "That file is not a trust file: it holds no certificate.", "trust");
      return;
    }
    if (!token.size() || token.size() > TOKEN_MAX) {
      refuse("invalid", "Paste the gateway's token from MeterFax.", "token");
      return;
    }
    // A path on the address is dropped: Basics Station asks /router-info.
    cfg.url = "wss://" + host + (port == 443 ? std::string("") : ":" + std::to_string(port));
    cfg.token = tokenHeader(token);
    closeSocket();
    cfg.trust = trust;
    writeString("url", cfg.url);
    writeString("token", cfg.token);
    writeString("trust", cfg.trust);
    server.state = L_UNSET;  // connects on the next pass, if WiFi is up
    counts = Counters();
    JsonDocument out;
    status(out);
    reply(out);
  } else if (cmd == "forget") {
    store->clear();
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

void pollConsole() {
  std::string line;
  while (H->console.readLine(line)) {
    line = trim(line);
    if (line.size() && line.size() <= HOST_LINE_MAX) onCommand(line);
  }
}

}  // namespace

void begin(const Identity& identity, hal::Hal& hal, hal::Net& n, hal::Store& settings, radio::GatewayRadio& r) {
  id = &identity;
  H = &hal;
  net = &n;
  store = &settings;
  radio = &r;
  say(firmwareName() + ", EUI " + hex64(H->board.eui()));

  loadSettings();
  net->onSocket(onSocket);
  startWifi();
  // The clock is for rxtime and TLS; the server has its own when this has none.
  net->startClock();

  if (!cfg.ssid.size()) say("no WiFi yet: program it from flash.meterfax.com");
  if (!haveServer()) say("no server yet: program it from flash.meterfax.com");
}

void loop() {
  pollConsole();
  serviceRadio();
  serviceLink();
  serviceRadio();
}

Counters& counters() { return counts; }

std::string eui() { return hex64(H->board.eui()); }

std::string firmware() { return firmwareName(); }

const char* wifiState() { return wifiStateName(); }

std::string ssid() { return cfg.ssid; }

bool wifiConnected() { return wifiUp(); }

std::string linkState() {
  if (server.state == L_ERROR) return server.error;
  return LINK_NAMES[server.state];
}

}  // namespace station
}  // namespace ndw
