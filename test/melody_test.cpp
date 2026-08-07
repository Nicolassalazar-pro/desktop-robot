// Host tests for melody playback.
//
// The tests that matter here are the ones that would otherwise be discovered
// as SILENCE on the bench: a note outside the piezo's 200-6000 Hz band produces
// nothing at all, and PiezoTone's 750 ms watchdog cuts any note that is not
// re-commanded. Both are invisible failures, which is exactly why they are
// pinned here.

#include "melody.h"

#include <cassert>
#include <cstdio>

namespace {

using Melody::Note;
using Melody::Player;

void testValidateRejectsUnplayableNotes() {
  // A melody written with low piano notes (NOTE_A1 = 55 Hz) would be silent,
  // not quiet — the hardware simply refuses it. Catch that here.
  const Note tooLow[] = {{55, 100}};
  assert(!Melody::validate(tooLow, 1));

  const Note tooHigh[] = {{9000, 100}};
  assert(!Melody::validate(tooHigh, 1));

  // Zero duration would consume no time and spin the advance loop.
  const Note zeroLen[] = {{1000, 0}};
  assert(!Melody::validate(zeroLen, 1));

  assert(!Melody::validate(nullptr, 3));
  assert(!Melody::validate(tooLow, 0));

  // Rests are legal and are how the melodies get rhythm.
  const Note withRest[] = {{1000, 50}, {0, 50}, {1200, 50}};
  assert(Melody::validate(withRest, 3));
}

void testPlaysNotesInOrderWithCorrectTiming() {
  const Note tune[] = {{1000, 100}, {2000, 100}, {3000, 100}};
  Player p;
  assert(p.start(tune, 3, 0));

  assert(p.service(0) == 1000);
  assert(p.service(50) == 1000);
  assert(p.service(99) == 1000);
  assert(p.service(100) == 2000);    // boundary belongs to the next note
  assert(p.service(150) == 2000);
  assert(p.service(200) == 3000);
  assert(p.service(299) == 3000);
  assert(p.service(300) == 0);       // finished
  assert(!p.active());
}

void testRestsAreSilentButStillConsumeTime() {
  const Note tune[] = {{1000, 100}, {0, 100}, {2000, 100}};
  Player p;
  p.start(tune, 3, 0);
  assert(p.service(50) == 1000);
  assert(p.service(150) == 0);       // rest: silent
  assert(p.active());                // but still playing
  assert(p.service(250) == 2000);
}

void testRepeatedServiceReturnsTheSameFrequency() {
  // This is what defeats PiezoTone's 750 ms watchdog: the caller re-commands
  // the same value every loop, which refreshes the lease. A player that only
  // reported changes would let long notes die mid-way.
  const Note tune[] = {{1000, 2000}};   // 2 s, far beyond the 750 ms watchdog
  Player p;
  p.start(tune, 1, 0);
  for (uint32_t t = 0; t < 2000; t += 10) {
    assert(p.service(t) == 1000);
  }
  assert(p.service(2000) == 0);
}

void testAStalledTickSkipsAheadRatherThanStretching() {
  // If the loop is blocked (an LCD push, a WiFi scan), the melody must stay in
  // time with the movement it accompanies rather than resuming where it left
  // off and drifting later and later.
  const Note tune[] = {{1000, 50}, {2000, 50}, {3000, 50}, {4000, 50}};
  Player p;
  p.start(tune, 4, 0);
  assert(p.service(0) == 1000);
  // 160 ms stall: notes 0-2 have elapsed, so note 3 is current.
  assert(p.service(160) == 4000);
  assert(p.service(200) == 0);
  assert(!p.active());
}

void testStartRejectsAnInvalidTuneAndLeavesThePlayerIdle() {
  const Note bad[] = {{55, 100}};
  Player p;
  assert(!p.start(bad, 1, 0));
  assert(!p.active());
  assert(p.service(10) == 0);
}

void testStopSilencesImmediately() {
  const Note tune[] = {{1000, 5000}};
  Player p;
  p.start(tune, 1, 0);
  assert(p.service(100) == 1000);
  p.stop();
  assert(!p.active());
  assert(p.service(200) == 0);
}

void testIdlePlayerIsSilent() {
  Player p;
  assert(!p.active());
  assert(p.service(0) == 0);
  assert(p.service(100000) == 0);
}

}  // namespace

int main() {
  testValidateRejectsUnplayableNotes();
  testPlaysNotesInOrderWithCorrectTiming();
  testRestsAreSilentButStillConsumeTime();
  testRepeatedServiceReturnsTheSameFrequency();
  testAStalledTickSkipsAheadRatherThanStretching();
  testStartRejectsAnInvalidTuneAndLeavesThePlayerIdle();
  testStopSilencesImmediately();
  testIdlePlayerIsSilent();
  printf("melody tests passed\n");
  return 0;
}
