#pragma once

// ============================================================================
// bench_log.h — persisted results from an untethered bench run.
//
// WHY THIS EXISTS AT ALL.
// The robot cannot be meaningfully tested while tethered: the USB cable
// restrains rotation, and sitting on its stand takes the wheels out of the
// friction path and roughly doubles the board tilt. Both silently corrupt every
// dynamic measurement. So the measurements that matter have to happen with the
// robot untethered, on the desk, on its own wheels.
//
// But reconnecting the serial port RESETS the board (DTR/RTS), which destroys
// anything held in RAM. A test whose results live in RAM therefore cannot be
// read: the act of reading it is what erases it. Results must be persisted at
// the moment they are taken, or they do not exist.
//
// Hence an append-only log in NVS. Each entry is one measurement, tagged with
// what it was and under what condition, so the whole run can be dumped after
// reconnecting and analysed offline.
//
// This is a diagnostic record, not a safety-critical one: a lost or corrupt log
// costs a re-run and nothing else. So it gets a single checksummed record and
// honest failure, in the same spirit as SpinCalibration, rather than A/B slots.
//
// Kept free of Arduino headers so the packing, validation and statistics can be
// unit-tested on the host.
// ============================================================================

#include <stdint.h>

namespace BenchLog {

// What a single entry measured. Stored as a uint8_t so the record layout is
// stable across compiler versions.
enum class Metric : uint8_t {
  None = 0,
  GravityX = 1,        // g, resting, per axis - the pose the run happened in
  GravityY = 2,
  GravityZ = 3,
  ProjectionGain = 4,  // 1/fraction: how much single-axis integration loses
  DriftDegrees = 5,    // heading after a stationary hold; param = seconds
  BiasDps = 6,         // gyro bias at that moment
  NoiseSpreadDps = 7,  // peak-to-peak over a ZUPT window
  SteadyRateDps = 8,   // measured turn rate; param = commanded speed
  CoastDegrees = 9,    // degrees swept AFTER the motors cut; param = speed
  CoastSeconds = 10,   // implied lead constant = coast/rate
  TurnErrorDegrees = 11,  // signed final error of a commanded turn
  NetSequenceDegrees = 12,  // signed sum of a zero-sum sequence
  VibrationSpreadDps = 13,  // gyro spread with the servos running
};

// One measurement. Deliberately small and fixed-width: the whole point is that
// a few dozen of these survive a reset.
struct Entry {
  uint8_t metric;    // Metric
  uint8_t condition; // free-form tag, e.g. lead on/off. 0 = unconditioned
  int16_t param;     // what was varied: speed, seconds, angle
  float value;
};

// 3 metrics x 24 turns = 72, plus the baseline block. 128 leaves headroom for
// a longer script; the first run truncated at 96 and lost the last two turns of
// the A/B block, which is the most expensive kind of data to lose because it
// costs another supervised session to retake.
constexpr uint16_t kMaxEntries = 128;

// Condition tags, so an A/B comparison is readable after the fact.
constexpr uint8_t kCondNone = 0;
constexpr uint8_t kCondLeadOn = 1;
constexpr uint8_t kCondLeadOff = 2;

const char* metricName(Metric metric);
const char* conditionName(uint8_t condition);

// Arming is persisted separately from the results, and deliberately so.
//
// The workflow this exists to support: the run is armed over serial while
// tethered, the robot is then UNPLUGGED, and the user triggers the run with the
// on-screen button. Nobody can drive the motors on the agent's behalf, and a
// tethered movement test is invalid anyway. So the armed flag has to survive
// both the unplug and any power-cycle in between, which rules out RAM.
//
// It is cleared as soon as a run starts, so a bench run can never fire twice by
// accident - the second press does the ordinary thing.
bool setArmed(bool armed);
bool isArmed();

class Store {
 public:
  Store();

  // Loads a previously persisted run. A missing or rejected record is not an
  // error; it simply leaves the log empty.
  bool load();

  // Drops everything held in RAM. Does not touch NVS until save().
  void reset();

  // Appends one measurement. Returns false when full rather than overwriting:
  // a truncated run that says so is more useful than one that silently loses
  // its earliest and most carefully-taken readings.
  bool add(Metric metric, int16_t param, float value,
           uint8_t condition = kCondNone);

  // Persists the whole log and verifies it by reading it back.
  bool save();

  // Removes the stored record.
  bool erase();

  uint16_t count() const;
  bool full() const;
  const Entry& at(uint16_t index) const;

  // True once more entries were offered than there was room for.
  bool overflowed() const;

  // Mean and peak-to-peak of every entry matching a metric (and optionally a
  // condition), which is what turns a list of repeats into a result. Returns
  // false when nothing matches, rather than reporting a mean of zero.
  bool summarise(Metric metric, uint8_t condition, float* meanOut,
                 float* spreadOut, uint16_t* countOut) const;

  // As summarise, but also matching `param` — which is how the per-speed split
  // is done. Pooling speeds averages a 16 deg coast together with a 4 deg one
  // and reports a number that describes neither.
  bool summariseParam(Metric metric, uint8_t condition, int16_t param,
                      float* meanOut, float* spreadOut,
                      uint16_t* countOut) const;

 private:
  Entry entries_[kMaxEntries];
  uint16_t count_;
  bool overflowed_;

  bool summariseParamInternal(Metric metric, uint8_t condition, int16_t param,
                              bool matchParam, float* meanOut, float* spreadOut,
                              uint16_t* countOut) const;
};

}  // namespace BenchLog
