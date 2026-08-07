#pragma once

// ============================================================================
// spin_calibration.h — the measured duration of one full turn.
//
// WHY THIS IS NOT drive_calibration's storage.
// DriveCalibration guards servo pulse limits, where a wrong value drives the
// motors outside their safe band. It earns its A/B slots, generation counters
// and read-back verification. This value is an expression tuning constant: a
// wrong or missing one makes a gesture turn the wrong amount. Losing it falls
// back to the compiled default and nothing unsafe happens. So it gets a single
// checksummed record and honest failure, not the full machinery — and saying
// so here is cheaper than someone later assuming the two are equivalent.
//
// Any record that fails its magic, version, checksum or range check is treated
// as absent rather than repaired, so a corrupt record can never turn into a
// plausible-looking wrong number.
//
// Kept free of Arduino headers so it can be unit-tested on the host against
// the fake NVS.
// ============================================================================

#include <stdint.h>

namespace SpinCalibration {

class Store {
 public:
  Store();

  // Reads the stored record. Returns true only if a valid record was loaded.
  // A missing or rejected record is not an error: the store falls back to the
  // compiled default and reports that nothing was loaded.
  bool load();

  // Stages a candidate. Rejects anything out of range or off the tick grid,
  // leaving the previous staged value intact.
  bool stage(uint16_t fullTurnMs);

  // Persists the staged value and verifies it by reading it back.
  bool save();

  // Removes the stored record and returns to the compiled default.
  bool erase();

  // The value in effect right now — staged if staged, else stored, else the
  // compiled default. Always a valid full turn.
  uint16_t fullTurnMs() const;

  // The persisted value, or 0 if there is no valid stored record.
  uint16_t savedMs() const;

  bool dirty() const;
  bool loadedFromNvs() const;

 private:
  uint16_t effective_;
  uint16_t saved_;
  bool dirty_;
  bool loadedFromNvs_;
};

}  // namespace SpinCalibration
