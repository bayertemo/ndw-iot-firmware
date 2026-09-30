#include "ndw_meters.h"

#include <math.h>
#include <stdlib.h>


namespace ndw {
namespace meters {

const char* const ANOMALY_NAMES[ANOMALY_KINDS] = {"normal", "high"};

namespace {

// Until the hardware layer hands over its own (the hardware RNG on a board).
uint32_t hostRandom() { return (uint32_t)rand() << 16 ^ (uint32_t)rand(); }
Random random = hostRandom;

const float TWO_PI_F = 6.28318530718f;

// Relative water use through the day: near nothing overnight, the morning's
// showers, a lull, the evening's cooking, washing and baths.
const float WATER_HOURS[24] = {
    0.005, 0.003, 0.002, 0.002, 0.003, 0.012, 0.055, 0.095, 0.080, 0.050, 0.040, 0.040,
    0.045, 0.040, 0.035, 0.035, 0.040, 0.055, 0.075, 0.080, 0.070, 0.055, 0.035, 0.018,
};

// Electricity above the base load: low overnight, a morning bump, the long
// evening peak of cooking, lighting and screens.
const float POWER_HOURS[24] = {
    0.25, 0.20, 0.18, 0.18, 0.20, 0.30, 0.55, 0.75, 0.65, 0.50, 0.45, 0.45,
    0.50, 0.45, 0.45, 0.50, 0.60, 0.85, 1.00, 1.00, 0.95, 0.80, 0.55, 0.35,
};

// About 350 L a day for a household, before its own scale.
const float WATER_LITRES_PER_DAY = 350.0f;

// Gas through the day: the heating's morning run from before six, a midday
// low with the thermostat set back, the evening's heating and cooking, and a
// night setback that still burns a little. Relative, not a share.
const float GAS_HOURS[24] = {
    0.25, 0.20, 0.20, 0.20, 0.30, 0.70, 1.00, 1.00, 0.80, 0.50, 0.35, 0.35,
    0.40, 0.35, 0.35, 0.40, 0.55, 0.85, 1.00, 0.95, 0.85, 0.70, 0.50, 0.35,
};

// About 2.5 m³ a day across a year, before the season and the household:
// heating makes the winter several times the summer, when only hot water and
// cooking burn any.
const float GAS_LITRES_PER_DAY = 2500.0f;

float sum24(const float* hours) {
  float s = 0;
  for (int i = 0; i < 24; i++) s += hours[i];
  return s;
}

// Seconds into the local day, and the day of the week (0 is Sunday). Without
// any clock the fleet runs as though it started at noon on a Wednesday.
void localTime(const Clock& c, float* hour, int* weekday) {
  int64_t local = c.epoch ? (int64_t)c.epoch + (int64_t)c.tzOffsetMin * 60 : 3 * 86400 + 12 * 3600 + c.uptimeS;
  int64_t day = local >= 0 ? local / 86400 : (local - 86399) / 86400;
  *hour = (float)(local - day * 86400) / 3600.0f;
  // 1 January 1970 was a Thursday.
  *weekday = (int)(((day + 4) % 7 + 7) % 7);
}

// The day of the local year, 0 to 365, for the season gas heating follows.
// Close enough without leap-year bookkeeping: a day's error moves a season by
// a day. Without any clock, early April — mid-season, neither extreme.
float dayOfYear(const Clock& c) {
  if (!c.epoch) return 95;
  int64_t local = (int64_t)c.epoch + (int64_t)c.tzOffsetMin * 60;
  return fmodf((float)(local / 86400), 365.2425f);
}

// The factor an episode puts on consumption, drawn afresh each report.
float episode(const Meter& m) { return m.anomaly == EXCESS ? rnd(EXCESS_MIN, EXCESS_MAX) : 1.0f; }

}  // namespace

void setRandom(Random source) { random = source; }

float rnd(float lo, float hi) { return lo + (hi - lo) * (random() / 4294967296.0f); }

void seed(Meter& m, Kind kind) {
  m.kind = kind;
  m.scale = rnd(0.5f, 1.6f);
  m.battery = (uint8_t)rnd(70, 101);
  m.reg = kind == WATER ? (uint32_t)rnd(2e5f, 2e6f) : kind == GAS ? (uint32_t)rnd(2e5f, 2e7f) : (uint32_t)rnd(5e6f, 4e7f);
  m.demandW = 0;
  m.anomaly = NORMAL;
  m.anomalyLeft = 0;
}

// Episodes start with the probability that makes a device anomalous for
// about anomalyPct of its reports in the long run: a share r with episodes of
// mean length L starts one at p = r / (L·(1 − r)) per normal report.
void stepAnomaly(Meter& m, uint8_t anomalyPct) {
  // A device recorded mid-fault by probe 0.3.0, whose faults were other
  // kinds, resumes normal: its old kind means nothing here.
  if (m.anomaly >= ANOMALY_KINDS) m.anomaly = NORMAL;
  if (m.anomaly != NORMAL) {
    if (m.anomalyLeft > 0) m.anomalyLeft--;
    if (m.anomalyLeft == 0) m.anomaly = NORMAL;
    return;
  }
  float r = (anomalyPct < 90 ? anomalyPct : 90) / 100.0f;
  if (r <= 0) return;
  float meanLen = (EPISODE_MIN + EPISODE_MAX) / 2.0f;
  if (rnd(0, 1) >= r / (meanLen * (1 - r))) return;
  m.anomaly = EXCESS;
  m.anomalyLeft = EPISODE_MIN + random() % (EPISODE_MAX - EPISODE_MIN + 1);
}

void consume(Meter& m, float hours, const Clock& clock) {
  float hour;
  int weekday;
  localTime(clock, &hour, &weekday);
  int h = (int)hour % 24;
  bool weekend = weekday == 0 || weekday == 6;

  if (m.kind == WATER) {
    // Water is drawn, not trickled: a shower, a flush, a kettle filled. The
    // chance of any draw in an interval follows the hour, and a draw carries
    // what the hour would average — so the busy hours are steady and the
    // night is mostly empty intervals, as a real register reads.
    float share = WATER_HOURS[h] / sum24(WATER_HOURS);
    float expected = WATER_LITRES_PER_DAY * m.scale * (weekend ? 1.15f : 1.0f) * share * hours;
    float draws = share * 24 * 1.5f * hours;
    float p = 1 - expf(-draws);
    // An episode draws in every interval, not only the busy ones: a hose
    // left running, a toilet that does not stop.
    float litres = m.anomaly == EXCESS ? expected * episode(m)
                   : rnd(0, 1) < p     ? expected / p * rnd(0.5f, 1.5f)
                                       : 0;
    m.reg += (uint32_t)(litres * 10.0f + 0.5f);
    // A meter's cell loses a percent every week or so.
    if (rnd(0, 1) < hours / 168.0f && m.battery > 5) m.battery--;
  } else if (m.kind == GAS) {
    // Burnt, not drawn: a furnace cycling through the hour rather than draws
    // of a few litres, so every interval in a heating hour moves the register
    // and the spread is the cycling. The season is most of it — mid-January
    // near its peak, mid-July near the floor, northern hemisphere.
    float season = 1.0f + 0.8f * cosf(TWO_PI_F * (dayOfYear(clock) - 15) / 365.25f);
    float share = GAS_HOURS[h] / sum24(GAS_HOURS);
    float litres = GAS_LITRES_PER_DAY * m.scale * season * (weekend ? 1.1f : 1.0f) * share * hours *
                   rnd(0.3f, 1.7f) * episode(m);
    m.reg += (uint32_t)(litres * 10.0f + 0.5f);
    if (rnd(0, 1) < hours / 168.0f && m.battery > 5) m.battery--;
  } else {
    // A base load that never stops — the fridge, the router — the household's
    // own use through the day on top, and now and then a kettle or an oven.
    float watts = 150.0f * m.scale + 1400.0f * m.scale * (weekend ? 1.1f : 1.0f) * POWER_HOURS[h] * rnd(0.6f, 1.4f);
    if (rnd(0, 1) < 0.08f) watts += rnd(1500.0f, 3000.0f);
    watts *= episode(m);
    m.demandW = (uint16_t)(watts < 65535.0f ? watts : 65535.0f);
    m.reg += (uint32_t)(watts * hours + 0.5f);
  }
}

size_t encode(const Meter& m, uint8_t* out, uint8_t& port) {
  out[0] = m.reg >> 24;
  out[1] = m.reg >> 16;
  out[2] = m.reg >> 8;
  out[3] = m.reg;
  if (m.kind == POWER) {
    out[4] = m.demandW >> 8;
    out[5] = m.demandW;
    port = 11;
    return 6;
  }
  out[4] = m.battery;
  port = m.kind == GAS ? 12 : 10;
  return 5;
}

}  // namespace meters
}  // namespace ndw
