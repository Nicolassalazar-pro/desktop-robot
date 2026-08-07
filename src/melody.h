#pragma once

// ============================================================================
// melody.h — note-sequence playback for expressions.
//
// WHY A PLAYER RATHER THAN JUST CALLING PiezoTone.
// PiezoTone plays ONE frequency until told otherwise, and auto-stops after a
// 750 ms watchdog if nothing re-commands it. That is the right shape for a
// safety-bounded output and the wrong shape for expression: a robot that can
// only beep has one sound, and one sound cannot carry frustration, delight and
// alarm.
//
// This turns a table of notes into something the loop can drive, with two
// deliberate design choices:
//
//   1. It is PURE LOGIC. service() returns the frequency that should be
//      sounding right now; it never touches hardware. That keeps every timing
//      decision unit-testable on the host, and it means the watchdog is handled
//      for free — the caller re-commands the same frequency every tick, which
//      is exactly what refreshes the lease.
//
//   2. Notes are bounded to what the hardware can actually produce.
//      PiezoTone rejects anything outside 200-6000 Hz, so a melody written with
//      NOTE_A1 (55 Hz) would silently produce nothing. Melodies are validated
//      against that range rather than discovering it as silence on the bench.
// ============================================================================

#include <stdint.h>

namespace Melody {

// The playable band, mirroring PiezoTone's limits. A note outside this is a
// bug in the melody, not something to clamp: clamping would transpose a note
// into a different pitch and quietly change the tune.
constexpr uint16_t kMinHz = 200;
constexpr uint16_t kMaxHz = 6000;

// hz == 0 is a rest, which is how a melody gets rhythm and separation between
// repeated pitches.
struct Note {
  uint16_t hz;
  uint16_t ms;
};

// Returns false if any note is outside the playable band or has zero duration.
// Rests (hz == 0) are always allowed.
bool validate(const Note* notes, uint8_t count);

class Player {
 public:
  Player();

  // Starts a sequence. `notes` must outlive playback — the player holds the
  // pointer rather than copying, since every melody here is a compile-time
  // table.
  bool start(const Note* notes, uint8_t count, uint32_t nowMs);
  void stop();

  // The frequency that should be sounding right now, 0 for silence. Call every
  // loop: re-issuing the same value is what keeps PiezoTone's 750 ms watchdog
  // from cutting a long note short.
  uint16_t service(uint32_t nowMs);

  bool active() const;
  uint8_t noteIndex() const;
  const char* name() const;
  void setName(const char* name);

 private:
  const Note* notes_;
  uint8_t count_;
  uint8_t index_;
  uint32_t noteStartedMs_;
  bool active_;
  const char* name_;
};

// ---------------------------------------------------------------------------
// Fixed expression voice tables (kFrustrated, kExcited, kScared, kChirp,
// kQuery, kAlarm) used to live here. They have been REPLACED by the seeded
// mood-melody GENERATOR — see melody_gen.h. The generator produces phrases
// at runtime for three moods (happy/sad/confused), which is what the phone
// UI now drives. See melody_gen.h for the storage-lifetime rule that
// Player::start() imposes on generated note buffers.
// ---------------------------------------------------------------------------

}  // namespace Melody
