// How NDW's simulated meters behave: the daily rhythm real households have,
// the faults real meters show, and the payloads MeterFax's NDW Simulator
// codec reads.
//
//   water  port 10  uint32 BE decilitres (running total), uint8 battery %
//   power  port 11  uint32 BE watt-hours (running total), uint16 BE watts
//   gas    port 12  uint32 BE decilitres (running total), uint8 battery %
//
// Shared by the fleets that differ only in how a reading leaves the board —
// NDW LoRaWAN meter fleet over LoRa, NDW BLE meter fleet over Bluetooth — so
// a water meter reads the same whichever carries it. Pure: no radio, no
// storage, no clock of its own, so it builds and is tested on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace ndw {
namespace meters {

enum Kind : uint8_t { WATER = 0, POWER = 1, GAS = 2 };

// What a meter is doing, for a stretch of its reports. One kind of fault:
// consumption far above the household's normal — the pattern MeterFax's
// alerts look for — for a few hours at a time, about the fleet's anomaly
// share of each device's reports in the long run.
enum Anomaly : uint8_t {
  NORMAL = 0,
  EXCESS,  // several times the usual draw, burn or demand (not HIGH: Arduino's pin level)
  ANOMALY_KINDS,
};
extern const char* const ANOMALY_NAMES[ANOMALY_KINDS];

// An episode lasts this many reports: an hour to six, at a 15-minute interval.
static const uint8_t EPISODE_MIN = 4, EPISODE_MAX = 24;

// How far above normal an episode runs.
static const float EXCESS_MIN = 4.0f, EXCESS_MAX = 10.0f;

// One meter's state, as its fleet keeps it.
struct Meter {
  uint8_t kind;
  float scale;   // how big a user this household is, around 1
  uint32_t reg;  // decilitres or watt-hours: only ever rises
  uint16_t demandW;
  uint8_t battery;
  uint8_t anomaly;
  uint16_t anomalyLeft;
};

// The wall clock, as the fleet has it.
struct Clock {
  uint32_t epoch;        // Unix seconds, 0 when the fleet has no clock
  int32_t tzOffsetMin;   // minutes east of UTC, so the curves follow the local day
  uint32_t uptimeS;      // for a fleet with no clock at all
};

// The random source: 32 uniform bits. The hardware RNG on a board; a seeded
// one in a test.
typedef uint32_t (*Random)();
void setRandom(Random source);

// A uniform number in [lo, hi).
float rnd(float lo, float hi);

// A meter as a new device starts: a household's size, a battery part-used,
// and a register a few years in — real meters do not start at zero.
void seed(Meter& m, Kind kind);

// Starts, continues or ends a high-consumption episode, for a fleet whose
// meters are anomalous for about `anomalyPct` percent of their reports.
void stepAnomaly(Meter& m, uint8_t anomalyPct);

// Advances the register over the hours since the last reading.
void consume(Meter& m, float hours, const Clock& clock);

// The reading as it goes on the air: its payload into `out` (6 bytes is
// enough), its port, and its length.
size_t encode(const Meter& m, uint8_t* out, uint8_t& port);

}  // namespace meters
}  // namespace ndw
