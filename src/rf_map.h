#pragma once

// ============================================================================
// rf_map.h — averaged RSSI per access point, per surveyed position.
//
// WHY THIS EXISTS.
// The open question is whether translation produces enough RSSI signal to be
// usable: moving the robot genuinely changes its range to every AP (unlike
// rotating in place, which changes nothing), but the predicted change is small
// against the measured noise floor.
//
//   translation signal, n=2.5, AP at 5 m:  0.63 dB per 0.3 m,  1.98 dB per 1 m
//   measured noise at a fixed point:       1-2 dB typical, up to 5 dB
//
// So a single scan cannot see it and averaging is mandatory. This holds the
// per-position averages and — critically — the SPREAD, because the comparison
// that decides the question is "is the difference between positions larger than
// the scatter within a position?" A mean without its spread cannot answer that.
//
// It also records how many scans each average came from, since the noise floor
// only falls as 1/sqrt(N) and a 4-scan average is not comparable to a 36-scan
// one.
//
// LIMIT WORTH KNOWING: averaging suppresses TEMPORAL fading. It does nothing
// about multipath, which is spatially fixed — a standing-wave pattern that
// offsets a given point by several dB no matter how long you sample it. That
// bias is indistinguishable from a range change, and it is the reason indoor
// RSSI ranging plateaus at metre-scale error. This store cannot separate them;
// it can only show whether the position differences are repeatable, which is
// the necessary (not sufficient) condition.
//
// Kept free of Arduino headers so the matching, averaging and validation can be
// unit-tested on the host.
// ============================================================================

#include <stdint.h>

namespace RfMap {

constexpr uint8_t kMaxAps = 12;
constexpr uint8_t kMaxPositions = 8;
constexpr uint8_t kBssidLen = 6;
constexpr uint8_t kLabelLen = 12;

// RSSI is stored in tenths of a dBm so averages keep useful resolution: the
// whole experiment turns on sub-dB differences, and rounding to integer dBm
// would discard the quantity being measured.
struct Reading {
  int16_t meanTenthDbm;
  int16_t spreadTenthDbm;
  uint16_t samples;   // 0 means this AP was never seen at this position
};

class Store {
 public:
  Store();

  bool load();
  void reset();
  bool save();
  bool erase();

  // Opens (or reuses) a survey position. Returns its index, or -1 if full.
  int8_t beginPosition(const char* label);

  // Adds one observation. `bssid` is 6 raw bytes. Unknown BSSIDs are appended
  // until the table is full, after which they are ignored rather than evicting
  // an AP that other positions already depend on — a partial row is analysable,
  // a row whose columns silently changed meaning is not.
  bool addSample(int8_t position, const uint8_t* bssid, int16_t rssiDbm);

  uint8_t apCount() const;
  uint8_t positionCount() const;
  const uint8_t* bssidAt(uint8_t apIndex) const;
  const char* labelAt(uint8_t position) const;
  const Reading& readingAt(uint8_t position, uint8_t apIndex) const;

  // The comparison the whole experiment exists to make: for one AP, the
  // difference in mean between two positions, and the larger of the two
  // spreads. If the difference does not exceed the spread, the move did not
  // produce a detectable signal.
  //
  // Returns false unless BOTH positions actually saw that AP, rather than
  // treating an unseen AP as a reading of zero.
  bool compare(uint8_t apIndex, uint8_t positionA, uint8_t positionB,
               float* deltaDbOut, float* worstSpreadDbOut,
               uint16_t* minSamplesOut) const;

 private:
  int8_t findAp(const uint8_t* bssid) const;

  uint8_t bssids_[kMaxAps][kBssidLen];
  char labels_[kMaxPositions][kLabelLen];
  Reading readings_[kMaxPositions][kMaxAps];
  // Running min/max per cell, kept separately so spread is exact rather than
  // estimated from a variance accumulator.
  int16_t minTenth_[kMaxPositions][kMaxAps];
  int16_t maxTenth_[kMaxPositions][kMaxAps];
  int32_t sumTenth_[kMaxPositions][kMaxAps];
  uint8_t apCount_;
  uint8_t positionCount_;
};

}  // namespace RfMap
