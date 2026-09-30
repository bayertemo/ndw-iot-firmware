#include "ndw_fleet.h"

#include <stdlib.h>
#include <string.h>

#include <ArduinoJson.h>

#include "ndw_lorawan.h"
#include "ndw_meters.h"
#include "ndw_text.h"

namespace ndw {
namespace fleet {

using meters::ANOMALY_KINDS;
using meters::ANOMALY_NAMES;
using meters::EXCESS;
using meters::GAS;
using meters::Kind;
using meters::NORMAL;
using meters::POWER;
using meters::WATER;
using text::hex64;

namespace {

const uint32_t INTERVAL_MIN_S = 60, INTERVAL_MAX_S = 86400;

// A device whose join went unanswered tries again after this, then twice as
// long each time up to an hour: one that is not registered yet should not
// starve the rest of the radio.
const uint32_t JOIN_BACKOFF_S = 120;
const uint32_t JOIN_BACKOFF_MAX_S = 3600;

// Every CHECK_EVERY-th uplink of a device asks the network to acknowledge it
// (LinkCheckReq). A session the network has forgotten — the device deleted
// and re-added in ChirpStack — otherwise sends into nothing forever; after
// CHECKS_MISSED unanswered in a row the device joins again.
const uint8_t CHECK_EVERY = 8;
const uint8_t CHECKS_MISSED = 3;

// US915's RX2, until a join accept says otherwise.
const uint32_t RX2_HZ = 923300000;
const uint8_t RX2_DR = 8;

const uint32_t MAGIC = 0x4e445752;  // "NDWR"
const uint16_t VERSION = 1;

const uint8_t ALL_CHANNELS = 0xff;

// One device as it is kept in the store.
struct __attribute__((packed)) Record {
  uint32_t magic;
  uint16_t version;
  uint64_t devEui;
  uint64_t joinEui;
  uint8_t appKey[16];
  // The meter.
  uint8_t kind;
  float scale;
  uint32_t reg;
  uint16_t demandW;
  uint8_t battery;
  uint8_t anomaly;
  uint16_t anomalyLeft;
  // LoRaWAN.
  uint8_t joined;
  uint16_t devNonce;  // the next join's
  uint32_t devAddr;
  uint8_t nwkSKey[16];
  uint8_t appSKey[16];
  uint32_t fCntUp;  // the next uplink's
  uint32_t fCntDown;
  uint8_t haveDown;
  uint8_t rx1DrOffset;
  uint8_t rx2Dr;
  uint8_t rxDelay;
  uint8_t answers[15];  // MAC answers owed, sent with the next uplink
  uint8_t answersLen;
  uint8_t ackDue;       // the last downlink was confirmed
  uint16_t joinFails;
  uint32_t uplinks;
  uint8_t missedChecks;
};

// What the scheduler keeps of each device, so only the one whose turn it is
// has to be in memory whole.
struct Slot {
  uint64_t devEui;
  uint32_t dueAt;   // millis
  uint32_t lastAt;  // millis of the last reading, for the use since
  uint8_t kind;
  uint8_t joined;
  uint8_t anomaly;
};

struct __attribute__((packed)) Config {
  uint32_t magic;
  uint16_t version;
  uint16_t count;
  uint32_t intervalS;
  int32_t tzOffsetMin;
  uint8_t anomalyPct;
  uint32_t epoch;
};

const Identity* id = nullptr;
hal::Hal* H = nullptr;
hal::Store* store = nullptr;
radio::EndDeviceRadio* radio = nullptr;

Config cfg = {MAGIC, VERSION, 0, 900, 0, 20, 0};
Slot* slots = nullptr;
uint16_t fleetSize = 0;
Record rec;  // the device whose turn it is
View view_;
uint32_t lastCfgSave = 0;

// Wall-clock time: an epoch at some millis — from the host when programmed,
// from the network's DeviceTimeAns once one arrives, or carried from the last
// save across a reboot.
uint32_t epochAt = 0, epochMillis = 0;
const char* clockSource = "none";

// A fleet being received from the host. While it is, the running fleet stops.
bool staging = false;
uint8_t stagingChannel = ALL_CHANNELS;
uint64_t* stagingList = nullptr;
uint16_t stagingExpected = 0, stagingCount = 0;
Config stagingCfg;

// ---- small helpers -----------------------------------------------------------

uint32_t now() { return H->clock.millis(); }

void say(const std::string& line) { H->console.writeLine(std::string("[") + id->tag + "] " + line); }

std::string firmwareName() {
  std::string v = id->version;
  return std::string(id->name) + "/" + (v.size() ? v : std::string("dev"));
}

uint32_t nextRandom() { return H->random.next(); }

std::string numbered(uint16_t i) { return "#" + std::to_string(i + 1); }

// ---- time --------------------------------------------------------------------

uint32_t epochNow() { return epochAt ? epochAt + (now() - epochMillis) / 1000 : 0; }

void setEpoch(uint32_t epoch, const char* source) {
  epochAt = epoch;
  epochMillis = now();
  clockSource = source;
}

meters::Clock clockNow() { return {epochNow(), cfg.tzOffsetMin, now() / 1000}; }

// ---- a record's meter, for ndw-meters, and back --------------------------------

meters::Meter meterOf(const Record& d) {
  return {d.kind, d.scale, d.reg, d.demandW, d.battery, d.anomaly, d.anomalyLeft};
}

void keepMeter(Record& d, const meters::Meter& m) {
  d.kind = m.kind;
  d.scale = m.scale;
  d.reg = m.reg;
  d.demandW = m.demandW;
  d.battery = m.battery;
  d.anomaly = m.anomaly;
  d.anomalyLeft = m.anomalyLeft;
}

lorawan::Session sessionOf(const Record& d) {
  lorawan::Session s;
  s.devAddr = d.devAddr;
  memcpy(s.nwkSKey, d.nwkSKey, 16);
  memcpy(s.appSKey, d.appSKey, 16);
  return s;
}

// ---- fleet summary -------------------------------------------------------------

uint16_t countJoined() {
  uint16_t n = 0;
  for (uint16_t i = 0; i < fleetSize; i++) n += slots[i].joined;
  return n;
}

uint16_t countKind(Kind k) {
  uint16_t n = 0;
  for (uint16_t i = 0; i < fleetSize; i++) n += slots[i].kind == k;
  return n;
}

uint16_t countAnomaly(uint8_t a) {
  uint16_t n = 0;
  for (uint16_t i = 0; i < fleetSize; i++) n += slots[i].anomaly == a;
  return n;
}

const char* stateName() {
  if (staging) return "programming";
  if (!fleetSize) return "unprovisioned";
  return countJoined() ? "running" : "joining";
}

void refreshView() {
  view_.state = stateName();
  view_.fleet = fleetSize;
  view_.joined = countJoined();
  view_.faults = fleetSize - countAnomaly(NORMAL);
  view_.staging = staging;
  view_.stagingCount = stagingCount;
  view_.stagingExpected = stagingExpected;
}

void show(const std::string& line) {
  view_.line = line;
  refreshView();
}

// ---- storage -------------------------------------------------------------------

// Store keys are at most 15 characters, and a DevEUI in hex is 16: this is the
// same 8 bytes as "d" and 11 characters of base64url.
std::string recordKey(uint64_t devEui) {
  static const char* ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  char key[13] = {'d'};
  for (int i = 0; i < 10; i++) key[1 + i] = ALPHABET[(devEui >> (58 - 6 * i)) & 0x3f];
  // Ten characters carry 60 bits; the last holds the low 4, shifted up.
  key[11] = ALPHABET[(devEui & 0x0f) << 2];
  key[12] = 0;
  return key;
}

bool readRecord(uint64_t devEui, Record& out) {
  return store->get(recordKey(devEui).c_str(), &out, sizeof(Record)) && out.magic == MAGIC &&
         out.version == VERSION && out.devEui == devEui;
}

bool writeRecord(const Record& r) { return store->put(recordKey(r.devEui).c_str(), &r, sizeof(Record)); }

bool writeConfig(const Config& c) {
  Config out = c;
  out.epoch = epochNow();
  lastCfgSave = now();
  return store->put("cfg", &out, sizeof(out));
}

bool writeList(const uint64_t* euis, uint16_t count) {
  return store->put("list", euis, count * sizeof(uint64_t));
}

// Spreads the fleet's reports across the interval rather than bunching them:
// device i is due i/N of the way through, and joins go out in the same order.
void schedule() {
  uint32_t t = now();
  for (uint16_t i = 0; i < fleetSize; i++) {
    slots[i].dueAt = t + 3000 + (uint32_t)((uint64_t)cfg.intervalS * 1000 * i / fleetSize);
    slots[i].lastAt = t;
  }
}

bool loadFleet() {
  Config c;
  if (!store->get("cfg", &c, sizeof(c)) || c.magic != MAGIC || c.version != VERSION || c.count == 0 ||
      c.count > MAX_FLEET) {
    return false;
  }
  uint64_t* euis = (uint64_t*)calloc(c.count, sizeof(uint64_t));
  if (!store->get("list", euis, c.count * sizeof(uint64_t))) {
    free(euis);
    return false;
  }
  slots = (Slot*)calloc(c.count, sizeof(Slot));
  uint16_t n = 0;
  for (uint16_t i = 0; i < c.count; i++) {
    if (!readRecord(euis[i], rec)) {
      say("no record for " + hex64(euis[i]) + ", skipped");
      continue;
    }
    slots[n].devEui = euis[i];
    slots[n].kind = rec.kind;
    slots[n].joined = rec.joined;
    slots[n].anomaly = rec.anomaly < ANOMALY_KINDS ? rec.anomaly : NORMAL;
    n++;
  }
  free(euis);
  cfg = c;
  fleetSize = n;
  // The clock carries on from the last save; the network corrects it.
  if (c.epoch) setEpoch(c.epoch, "saved");
  schedule();
  return n > 0;
}

// Deletes the records of devices no longer in the fleet — including any a
// programming left behind when it was abandoned before its commit.
void prune(const uint64_t* keep, uint16_t count) {
  for (const std::string& key : store->keys('d')) {
    bool kept = false;
    for (uint16_t i = 0; i < count && !kept; i++) kept = recordKey(keep[i]) == key;
    if (!kept) store->remove(key.c_str());
  }
}

// ---- LoRaWAN, one device at a time ---------------------------------------------

// The battery a DevStatusAns reports: 1 to 254, 255 unknown.
uint8_t devStatusBattery(uint8_t pct) {
  if (pct > 100) return 255;
  uint8_t b = (uint8_t)((uint32_t)pct * 254 / 100);
  return b ? b : 1;
}

void join(Slot& s, uint16_t i) {
  show("joining " + numbered(i));
  uint16_t nonce = rec.devNonce;
  // The DevNonce moves on in the store before the request is on the air:
  // ChirpStack refuses one it has seen, whenever the board stopped.
  rec.devNonce = nonce + 1;
  if (!writeRecord(rec)) {
    view_.error = "flash write failed";
    return;
  }

  uint8_t frame[lorawan::JOIN_REQUEST_LEN];
  lorawan::buildJoinRequest(rec.joinEui, rec.devEui, nonce, rec.appKey, frame);
  uint64_t txEnd = radio->send(frame, sizeof(frame), true);

  lorawan::JoinAccept accept;
  lorawan::Session session;
  bool joined = false;
  if (txEnd) {
    radio::RxWindows windows = {txEnd, 5, 0, RX2_HZ, RX2_DR};
    radio::RxInfo info;
    uint8_t buf[64];
    joined = radio->receive(
                 windows,
                 [&](const uint8_t* f, size_t n) {
                   return lorawan::mtypeOf(f) == lorawan::JOIN_ACCEPT &&
                          lorawan::parseJoinAccept(f, n, rec.appKey, nonce, accept, session);
                 },
                 buf, sizeof(buf), info) > 0;
  }

  if (joined) {
    rec.joined = 1;
    rec.devAddr = session.devAddr;
    memcpy(rec.nwkSKey, session.nwkSKey, 16);
    memcpy(rec.appSKey, session.appSKey, 16);
    rec.fCntUp = 0;
    rec.fCntDown = 0;
    rec.haveDown = 0;
    rec.rx1DrOffset = accept.rx1DrOffset;
    rec.rx2Dr = accept.rx2Dr;
    rec.rxDelay = accept.rxDelay ? accept.rxDelay : 1;
    rec.answersLen = 0;
    rec.ackDue = 0;
    rec.joinFails = 0;
    rec.missedChecks = 0;
    s.dueAt = now() + 5000;
    say(numbered(i) + " " + hex64(rec.devEui) + " joined");
    view_.error = "";
  } else {
    rec.joinFails++;
    uint32_t shift = rec.joinFails - 1 < 10 ? rec.joinFails - 1 : 10;
    uint32_t wait = JOIN_BACKOFF_S << shift;
    if (wait > JOIN_BACKOFF_MAX_S) wait = JOIN_BACKOFF_MAX_S;
    s.dueAt = now() + wait * 1000;
    say(numbered(i) + " " + hex64(rec.devEui) + " join unanswered, again in " + std::to_string(wait) + "s");
    view_.error = numbered(i) + " join unanswered";
  }
  s.joined = rec.joined;
  if (!writeRecord(rec)) view_.error = "flash write failed";
}

void report(Slot& s, uint16_t i) {
  uint32_t t = now();
  float hours = (t - s.lastAt) / 3600000.0f;
  s.lastAt = t;
  s.dueAt = t + cfg.intervalS * 1000;

  meters::Meter m = meterOf(rec);
  meters::stepAnomaly(m, cfg.anomalyPct);
  meters::consume(m, hours, clockNow());
  // A fleet of one is this board, so it reports this board's battery.
  uint8_t own = H->board.batteryPercent();
  if (m.kind != POWER && fleetSize == 1 && own <= 100) m.battery = own;
  keepMeter(rec, m);
  s.anomaly = rec.anomaly;

  uint8_t payload[6];
  uint8_t port;
  size_t len = meters::encode(m, payload, port);

  // FOpts: what is owed from the last downlink, then the device's own asks —
  // the time until the network has answered once (the daily curves need the
  // local hour), and every CHECK_EVERY-th uplink a link check.
  uint8_t fopts[15];
  uint8_t foptsLen = rec.answersLen;
  memcpy(fopts, rec.answers, foptsLen);
  bool askTime = strcmp(clockSource, "network") != 0 && foptsLen < 15;
  if (askTime) fopts[foptsLen++] = lorawan::DEVICE_TIME;
  bool check = ++rec.uplinks % CHECK_EVERY == 0 && foptsLen < 15;
  if (check) fopts[foptsLen++] = lorawan::LINK_CHECK;

  lorawan::Session session = sessionOf(rec);
  lorawan::Uplink up = {rec.fCntUp, port, payload, len, fopts, foptsLen, rec.ackDue != 0};
  uint8_t frame[64];
  size_t n = lorawan::buildUplink(session, up, frame, sizeof(frame));

  // The counter moves on in the store before the frame is on the air, so no
  // number is ever used twice, whenever the board stops.
  rec.fCntUp++;
  rec.answersLen = 0;
  rec.ackDue = 0;
  if (!writeRecord(rec)) {
    view_.error = "flash write failed";
    return;
  }

  show("sending " + numbered(i));
  uint64_t txEnd = n ? radio->send(frame, n, false) : 0;
  if (!txEnd) {
    view_.error = numbered(i) + " not sent";
    say(numbered(i) + " uplink could not be sent");
    return;
  }
  view_.sent++;
  view_.error = "";

  radio::RxWindows windows = {txEnd, rec.rxDelay ? rec.rxDelay : (uint8_t)1, rec.rx1DrOffset, RX2_HZ,
                              rec.rx2Dr ? rec.rx2Dr : RX2_DR};
  radio::RxInfo info;
  lorawan::Downlink down;
  uint8_t buf[256];
  bool heard = radio->receive(
                   windows,
                   [&](const uint8_t* f, size_t fl) {
                     return lorawan::parseDownlink(f, fl, session, rec.fCntDown, rec.haveDown != 0, down);
                   },
                   buf, sizeof(buf), info) > 0;

  bool checked = false;
  if (heard) {
    view_.acked++;
    view_.haveSignal = true;
    view_.rssi = info.rssi;
    view_.snr = info.snr;
    rec.fCntDown = down.fCnt;
    rec.haveDown = 1;
    rec.ackDue = down.confirmed;
    // MAC commands ride in FOpts, or in port 0's payload.
    const uint8_t* cmds = down.fPort == 0 ? down.payload : down.fOpts;
    size_t cmdsLen = down.fPort == 0 ? down.len : down.fOptsLen;
    lorawan::MacResult mac;
    lorawan::handleMac(cmds, cmdsLen, devStatusBattery(rec.battery), (int8_t)info.snr, mac);
    memcpy(rec.answers, mac.answers, mac.answersLen);
    rec.answersLen = mac.answersLen;
    if (mac.linkCheck) checked = true;
    if (mac.deviceTime && mac.unixTime > 1600000000UL) {
      setEpoch(mac.unixTime, "network");
      say("network time " + std::to_string(mac.unixTime));
    }
  }
  if (check) {
    if (checked) {
      rec.missedChecks = 0;
    } else if (++rec.missedChecks >= CHECKS_MISSED) {
      // The network has stopped answering this device: join again, which
      // either works or backs off like any other unanswered join.
      say(numbered(i) + " " + hex64(rec.devEui) + " unanswered " + std::to_string(rec.missedChecks) +
          " times, rejoining");
      rec.joined = 0;
      rec.missedChecks = 0;
      s.joined = 0;
      s.dueAt = now() + 2000;
    }
  }
  if (!writeRecord(rec)) view_.error = "flash write failed";
  show("sent " + numbered(i));
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

void rebootSoon() {
  H->console.flush();
  H->clock.sleepMs(300);
  H->board.reboot();
}

void describe(JsonDocument& doc) {
  doc["ok"] = true;
  doc["eui"] = hex64(H->board.eui());
  doc["firmware"] = firmwareName();
  // What this is, for a host deciding what to offer: a board answering as a
  // fleet takes one; a gateway does not.
  doc["kind"] = id->kind;
  doc["state"] = stateName();
  doc["fleet"] = fleetSize;
  doc["maxFleet"] = MAX_FLEET;
  JsonArray caps = doc["capabilities"].to<JsonArray>();
  caps.add("lorawan");
  caps.add("fleet");
}

void setClock(JsonDocument& in, Config& c) {
  uint32_t epoch = in["epoch"] | 0;
  if (epoch > 1600000000UL) setEpoch(epoch, "host");
  if (in["tzOffset"].is<int>()) {
    int tz = in["tzOffset"].as<int>();
    c.tzOffsetMin = tz < -14 * 60 ? -14 * 60 : tz > 14 * 60 ? 14 * 60 : tz;
  }
}

// A device as programmed: the keys it was given, and if the board already
// held this DevEUI with the same keys, everything it had learned — its
// DevNonce above all, since ChirpStack refuses one it has seen.
void stage(uint64_t devEui, uint64_t joinEui, const uint8_t* key, Kind kind) {
  Record old;
  bool same = readRecord(devEui, old) && old.joinEui == joinEui && memcmp(old.appKey, key, 16) == 0;
  if (same && old.kind == kind) return;  // already right as it is

  if (same) {
    // The keys are the same device's; only what it pretends to be changed.
    rec = old;
  } else {
    memset(&rec, 0, sizeof(rec));
    rec.magic = MAGIC;
    rec.version = VERSION;
    rec.devEui = devEui;
    rec.joinEui = joinEui;
    memcpy(rec.appKey, key, 16);
    // A DevNonce from a random start: a DevEUI programmed before, under
    // other firmware, may have used the low ones.
    rec.devNonce = (uint16_t)nextRandom();
  }
  meters::Meter m;
  meters::seed(m, kind);
  keepMeter(rec, m);
  writeRecord(rec);
}

bool commit(const uint64_t* euis, uint16_t count, Config& c) {
  c.count = count;
  // Its own key rather than a Config field: Config is read back whole and
  // checked by size, so a new field would make every saved fleet unreadable.
  if (!writeList(euis, count) || !writeConfig(c) || !store->put("chan", &stagingChannel, 1)) {
    refuse("storage", "The fleet could not be written to flash.");
    return false;
  }
  prune(euis, count);
  JsonDocument out;
  out["ok"] = true;
  out["fleet"] = count;
  out["state"] = "rebooting";
  reply(out);
  say("fleet of " + std::to_string(count) + " saved, rebooting to join");
  rebootSoon();
  return true;
}

bool intervalOk(uint32_t s) { return s >= INTERVAL_MIN_S && s <= INTERVAL_MAX_S; }

// A channel as the host sends it: 0 to 63, or null for the whole band.
bool readChannel(JsonDocument& in, uint8_t& ch) {
  ch = ALL_CHANNELS;
  if (in["channel"].is<int>()) {
    int v = in["channel"].as<int>();
    if (v < 0 || v > 63) {
      refuse("invalid", "The channel must be 0 to 63, one of US915's 125 kHz channels.", "channel");
      return false;
    }
    ch = (uint8_t)v;
  } else if (!in["channel"].isNull()) {
    refuse("invalid", "The channel is a number, or null for the whole sub-band.", "channel");
    return false;
  }
  return true;
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
    describe(out);
    // The radio's fields at the top, where hosts have always read them: the
    // band, the channel kept to, the last downlink's signal.
    radio->describe(out.as<JsonObject>());
    out["interval"] = cfg.intervalS;
    out["anomalies"] = cfg.anomalyPct;
    out["joined"] = countJoined();
    out["water"] = countKind(WATER);
    out["power"] = countKind(POWER);
    out["gas"] = countKind(GAS);
    JsonObject faults = out["faults"].to<JsonObject>();
    for (int a = EXCESS; a < ANOMALY_KINDS; a++) faults[ANOMALY_NAMES[a]] = countAnomaly(a);
    out["sent"] = view_.sent;
    out["acked"] = view_.acked;
    out["clock"] = clockSource;
    if (epochAt) out["epoch"] = epochNow();
    if (view_.haveSignal) {
      out["rssi"] = view_.rssi;
      out["snr"] = view_.snr;
    }
    uint8_t own = H->board.batteryPercent();
    out["batteryPct"] = own <= 100 ? own : 0;
    out["error"] = view_.error;
    reply(out);
  } else if (cmd == "fleet-begin") {
    uint16_t count = in["count"] | 0;
    uint32_t interval = in["interval"] | 900;
    int anomalies = in["anomalies"] | 20;
    if (count < 1 || count > MAX_FLEET) {
      refuse("invalid", "A fleet is 1 to " + std::to_string(MAX_FLEET) + " devices.", "count");
      return;
    }
    if (!intervalOk(interval)) {
      refuse("invalid", "The interval must be 60 to 86400 seconds.", "interval");
      return;
    }
    if (anomalies < 0 || anomalies > 90) {
      refuse("invalid", "Anomalies must be 0 to 90 percent.", "anomalies");
      return;
    }
    uint8_t ch;
    if (!readChannel(in, ch)) return;
    free(stagingList);
    stagingList = (uint64_t*)calloc(count, sizeof(uint64_t));
    stagingExpected = count;
    stagingCount = 0;
    stagingCfg = cfg;
    stagingCfg.intervalS = interval;
    stagingCfg.anomalyPct = anomalies;
    setClock(in, stagingCfg);
    stagingChannel = ch;
    // The running fleet stops until the new one is committed, or the board
    // restarts on the old one.
    staging = true;
    refreshView();
    JsonDocument out;
    out["ok"] = true;
    out["expected"] = count;
    reply(out);
  } else if (cmd == "fleet-add") {
    if (!staging) {
      refuse("order", "Send fleet-begin first.");
      return;
    }
    for (JsonArray row : in["devices"].as<JsonArray>()) {
      std::string which = "Device " + std::to_string(stagingCount + 1) + ": ";
      if (stagingCount >= stagingExpected) {
        refuse("invalid", "More devices than fleet-begin announced.");
        return;
      }
      uint64_t devEui, joinEui = 0;
      uint8_t key[16];
      // Each device says what it is: the host decides the fleet's mix.
      std::string kind = row[2] | "";
      if (!text::parseEui(row[0] | "", &devEui)) {
        refuse("invalid", which + "the DevEUI must be 16 hex characters.", "devEui");
        return;
      }
      if (!text::parseHex(row[1] | "", key, 16)) {
        refuse("invalid", which + "the AppKey must be 32 hex characters.", "appKey");
        return;
      }
      const char* j = row[3] | "";
      if (*j && !text::parseEui(j, &joinEui)) {
        refuse("invalid", which + "the JoinEUI must be 16 hex characters.", "joinEui");
        return;
      }
      if (kind != "water" && kind != "power" && kind != "gas") {
        refuse("invalid", which + "the kind must be water, power or gas.", "kind");
        return;
      }
      for (uint16_t k = 0; k < stagingCount; k++) {
        if (stagingList[k] == devEui) {
          refuse("invalid", which + "the DevEUI " + hex64(devEui) + " is already in the fleet.", "devEui");
          return;
        }
      }
      stage(devEui, joinEui, key, kind == "power" ? POWER : kind == "gas" ? GAS : WATER);
      stagingList[stagingCount++] = devEui;
    }
    refreshView();
    JsonDocument out;
    out["ok"] = true;
    out["received"] = stagingCount;
    reply(out);
  } else if (cmd == "fleet-commit") {
    if (!staging || stagingCount != stagingExpected) {
      refuse("incomplete",
             "Received " + std::to_string(stagingCount) + " of " + std::to_string(stagingExpected) + " devices.");
      return;
    }
    commit(stagingList, stagingCount, stagingCfg);
  } else if (cmd == "lorawan") {
    uint64_t devEui = H->board.eui(), joinEui = 0;
    uint8_t key[16];
    if (!text::parseHex(in["appKey"] | "", key, 16)) {
      refuse("invalid", "The AppKey must be 32 hex characters.", "appKey");
      return;
    }
    const char* d = in["devEui"] | "";
    if (*d && !text::parseEui(d, &devEui)) {
      refuse("invalid", "The DevEUI must be 16 hex characters.", "devEui");
      return;
    }
    const char* j = in["joinEui"] | "";
    if (*j && !text::parseEui(j, &joinEui)) {
      refuse("invalid", "The JoinEUI must be 16 hex characters.", "joinEui");
      return;
    }
    Config c = cfg;
    setClock(in, c);
    stage(devEui, joinEui, key, WATER);
    c.count = 1;
    if (!writeList(&devEui, 1) || !writeConfig(c)) {
      refuse("storage", "The keys could not be written to flash.");
      return;
    }
    prune(&devEui, 1);
    // Answered before the reboot with the DevEUI it will join as.
    JsonDocument out;
    out["ok"] = true;
    out["eui"] = hex64(devEui);
    out["state"] = "rebooting";
    reply(out);
    rebootSoon();
  } else if (cmd == "interval") {
    uint32_t s = in["seconds"] | 0;
    if (!intervalOk(s)) {
      refuse("invalid", "The interval must be 60 to 86400 seconds.", "seconds");
      return;
    }
    cfg.intervalS = s;
    if (fleetSize) writeConfig(cfg);
    JsonDocument out;
    out["ok"] = true;
    out["interval"] = s;
    reply(out);
  } else if (cmd == "channel") {
    // Keeps the fleet already on the board to one channel, or frees it: the
    // devices and their keys stay as they are, so a fleet already imported
    // into MeterFax need not be made again to serve a single-channel gateway.
    uint8_t ch;
    if (!readChannel(in, ch)) return;
    if (!store->put("chan", &ch, 1)) {
      refuse("storage", "The channel could not be written to flash.");
      return;
    }
    radio->keepToChannel(ch);
    JsonDocument out;
    out["ok"] = true;
    radio->describe(out.as<JsonObject>());
    if (ch == ALL_CHANNELS) out["channel"] = nullptr;
    reply(out);
  } else if (cmd == "time") {
    setClock(in, cfg);
    if (fleetSize) writeConfig(cfg);
    JsonDocument out;
    out["ok"] = true;
    out["clock"] = clockSource;
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
    line = text::trim(line);
    if (line.size()) onCommand(line);
  }
}

}  // namespace

void begin(const Identity& identity, hal::Hal& hal, hal::Store& s, radio::EndDeviceRadio& r) {
  id = &identity;
  H = &hal;
  store = &s;
  radio = &r;
  // As a boot finds it: nothing staged, no clock, nothing loaded.
  staging = false;
  free(stagingList);
  stagingList = nullptr;
  stagingExpected = stagingCount = 0;
  free(slots);
  slots = nullptr;
  fleetSize = 0;
  epochAt = epochMillis = 0;
  clockSource = "none";
  cfg = {MAGIC, VERSION, 0, 900, 0, 20, 0};
  view_ = View();
  view_.boardEui = H->board.eui();
  meters::setRandom([] { return H->random.next(); });
  say(firmwareName() + ", " + std::to_string(sizeof(Record)) + " bytes a device");

  uint8_t ch = ALL_CHANNELS;
  store->get("chan", &ch, 1);
  radio->keepToChannel(ch);

  if (!loadFleet()) {
    say("board " + hex64(H->board.eui()) + ", not programmed");
    refreshView();
    return;
  }
  say("fleet of " + std::to_string(fleetSize) + " (" + std::to_string(countKind(WATER)) + " water, " +
      std::to_string(countKind(POWER)) + " power, " + std::to_string(countKind(GAS)) + " gas), " +
      std::to_string(countJoined()) + " joined, every " + std::to_string(cfg.intervalS) + "s");
  show("starting");
}

void loop() {
  pollConsole();

  if (staging || !fleetSize) {
    refreshView();
    H->clock.sleepMs(20);
    return;
  }

  // The clock is saved now and then, so a board restarted away from the
  // browser and out of the network's reach still keeps roughly the day.
  if (epochAt && now() - lastCfgSave > 10UL * 60 * 1000) writeConfig(cfg);

  // The device most overdue, joined or not.
  uint32_t t = now();
  int32_t best = -1, lateness = -1;
  for (uint16_t i = 0; i < fleetSize; i++) {
    int32_t late = (int32_t)(t - slots[i].dueAt);
    if (late >= 0 && late > lateness) {
      lateness = late;
      best = i;
    }
  }
  if (best < 0) {
    H->clock.sleepMs(20);
    return;
  }

  Slot& s = slots[best];
  if (!readRecord(s.devEui, rec)) {
    view_.error = numbered(best) + " record lost";
    s.dueAt = t + cfg.intervalS * 1000;
    return;
  }
  if (rec.joined) {
    report(s, best);
  } else {
    join(s, best);
  }
  refreshView();
}

const View& view() { return view_; }

}  // namespace fleet
}  // namespace ndw
