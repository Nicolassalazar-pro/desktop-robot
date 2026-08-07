#pragma once

#include <stdint.h>

namespace DriveCalibration {

enum class Side : uint8_t {
  Left,
  Right,
};

enum class Field : uint8_t {
  Stop,
  ForwardStart,
  ReverseStart,
  ForwardMax,
  ReverseMax,
};

enum class Source : uint8_t {
  LegacyDefaults = 0,
  User = 1,
};

enum class Flag : uint8_t {
  Confirmed = 1U << 0,
  Locked = 1U << 1,
  Verified = 1U << 2,
};

struct MotorCalibration {
  uint16_t stopUs;
  uint16_t fwdStartUs;
  uint16_t revStartUs;
  uint16_t fwdMaxUs;
  uint16_t revMaxUs;
};

struct Snapshot {
  MotorCalibration left;
  MotorCalibration right;
  Source source;
  uint8_t flags;
  uint32_t generation;
  bool dirty;
  bool loadedFromNvs;
};

struct Result {
  bool success;
  const char* error;

  explicit operator bool() const {
    return success;
  }
};

bool hasFlag(uint8_t flags, Flag flag);
Snapshot legacyDraft();
Result validateMotor(const MotorCalibration& motor);
Result validateSnapshot(const Snapshot& snapshot);

class Store {
 public:
  Store();

  // Opens storage read/write, erases and verifies both schema-2 keys, then
  // loads only schema-3 slots. Missing schema-3 slots are a successful load
  // of the ineligible legacy draft. Corruption or any NVS I/O failure locks.
  Result load();

  // Pulse values are integer microseconds. Every field must be 1000..2000;
  // stop must be 1450..1550. Direction polarity derives from each start
  // pulse relative to stop.
  // A setter validates the complete proposed draft and never partially edits.
  Result set(Side side, Field field, int value);

  // Atomically replaces one motor so polarity and directional values can be
  // recalibrated together without requiring invalid intermediate snapshots.
  Result setMotor(Side side, const MotorCalibration& motor);

  // Confirmation and locking are committed to the public draft only after
  // the inactive NVS slot has been written, read back, and fully verified.
  // After safe cleanup a mutable draft remains retryable; uncertain storage
  // requires another complete load. A prior verified baseline is retained.
  Result saveConfirmed();

  // Explicitly erases both schema-3 and both schema-2 slots, verifies all are
  // absent, then makes a mutable legacy draft storage-ready.
  Result eraseAndReset();

  // Starts a new mutable, dirty legacy draft. A previously verified record is
  // retained internally only as the safe slot/generation baseline for save.
  // This does not recover storage after a failed load or persistence operation.
  void resetDraft();

  Snapshot snapshot() const;
  bool eligible() const;
  bool dirty() const;
  bool loadedFromNvs() const;
  uint32_t generation() const;
  bool hasRetainedBaseline() const;
  uint32_t retainedGeneration() const;

 private:
  enum class Slot : uint8_t {
    None,
    A,
    B,
  };

  Snapshot draft_;
  Snapshot current_;
  Slot currentSlot_;
  bool hasCurrent_;
  bool storageReady_;
  uint16_t touchedMask_;
};

}  // namespace DriveCalibration
