// Host tests for the stale-owner reaper policy in main.cpp.
//
// The reaper releases the WebSocket transport when the admitted owner has
// been silent too long. Two production bugs, both caught by operators on the
// hardware rather than by a test:
//
//   Bug A -- the liveness clock was never armed at admission.
//     g_lastControlEventMs started at 0 and was only written when a control
//     FRAME arrived. A client admitted at millis() == 90000 was instantly
//     judged to have been silent for 90000 ms, and the reaper killed the
//     session on the very next loop pass, before the phone had even sent a
//     frame. Symptom: connecting a phone reset the board.
//
//   Bug B -- the silence comparison underflowed across a cross-task race.
//     The check was `static_cast<uint32_t>(now - g_lastControlEventMs)`.
//     `g_lastControlEventMs` is stamped by the AsyncTCP task; `now` is
//     captured by the loop task. They run concurrently, so a frame landing
//     in between is legitimately IN THE FUTURE relative to `now`. Unsigned,
//     the subtraction wraps to ~4.29e9, which beats any threshold and the
//     reaper immediately kills a healthy session. Captured on hardware:
//
//       override: ws owner silent 4294967295ms -> releasing transport
//       drive session: SAFE_OFF reason=ws owner silent
//
//     The fix is a signed int32_t delta: a negative value now correctly
//     reads as "just heard from them", and the same signed form remains
//     correct across the 49.7-day millis() rollover.
//
// The point of these tests is that a future maintainer who "simplifies" the
// delta back to unsigned gets a RED test. Which specifically, see the
// comment on testFutureTimestampFromCrossTaskRaceIsNotReaped.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NDEBUG
#error "Host safety tests require assertions enabled"
#endif

namespace {

// Mirrors src/main.cpp:5311. The drift check in test_ws_protocol_drift.py
// asserts this matches the firmware source.
constexpr uint32_t kWsOwnerSilenceTimeoutMs = 8000;

// Extracted policy under test. Structurally identical to the check in
// src/main.cpp around line 10263:
//
//   const int32_t ownerSilentMs =
//       static_cast<int32_t>(now - g_lastControlEventMs);
//   if (wsTransportOwnerConnected() &&
//       ownerSilentMs > static_cast<int32_t>(kWsOwnerSilenceTimeoutMs)) {
//     ...
//   }
//
// If you ever simplify this back to
//
//   return static_cast<uint32_t>(now - lastEventMs) > timeoutMs;
//
// testFutureTimestampFromCrossTaskRaceIsNotReaped WILL FAIL. That is the
// whole point.
bool shouldReapOwner(uint32_t now, uint32_t lastEventMs, uint32_t timeoutMs) {
  const int32_t ownerSilentMs =
      static_cast<int32_t>(now - lastEventMs);
  return ownerSilentMs > static_cast<int32_t>(timeoutMs);
}

// Bug A regression -- freshly admitted owners must not be reaped.
//
// The admission path in src/main.cpp now stamps g_lastControlEventMs = now
// when the client is accepted (around line 8624). This test pins that: a
// client whose lastEventMs equals the current time cannot be reaped, no
// matter how large the absolute clock value.
void testFreshlyAdmittedOwnerIsNotReaped() {
  // Small clock value.
  assert(!shouldReapOwner(0, 0, kWsOwnerSilenceTimeoutMs));

  // The exact hardware scenario that reset the board: admission at
  // millis() == 90000 with an unarmed lastEventMs == 0 would have looked
  // silent for 90000 ms. Once the admission path stamps lastEventMs = now,
  // ownerSilentMs is 0 and the reaper stays quiet.
  assert(!shouldReapOwner(90000U, 90000U, kWsOwnerSilenceTimeoutMs));

  // A very late admission, past the millis() rollover midpoint. Still safe.
  assert(!shouldReapOwner(0x80000000U, 0x80000000U, kWsOwnerSilenceTimeoutMs));
}

// Bug B regression -- the whole reason the delta is signed.
//
// g_lastControlEventMs is written from the AsyncTCP task; `now` is read by
// the loop task. If a frame lands in the microsecond between the two, the
// stamp is legitimately AHEAD of `now`. The reaper must interpret that as
// "just heard from them", not as ~4.29e9 ms of silence.
//
// >>> If someone rewrites shouldReapOwner using unsigned arithmetic, THIS
// >>> exact assertion goes red. That is the assertion that would have
// >>> caught the "ws owner silent 4294967295ms" hardware bug in seconds.
void testFutureTimestampFromCrossTaskRaceIsNotReaped() {
  for (uint32_t aheadMs = 1; aheadMs <= 5; ++aheadMs) {
    const uint32_t now = 50000U;
    const uint32_t lastEventMs = now + aheadMs;
    assert(!shouldReapOwner(now, lastEventMs, kWsOwnerSilenceTimeoutMs));
  }

  // A slightly larger race window -- still not silent.
  assert(!shouldReapOwner(50000U, 50000U + 50U, kWsOwnerSilenceTimeoutMs));

  // And near the rollover boundary too. `now = 5`, `lastEventMs = 10`: the
  // frame landed 5 ms after `now`, still a race, not silence.
  assert(!shouldReapOwner(5U, 10U, kWsOwnerSilenceTimeoutMs));
}

// Boundary of the silence window itself. Timeout - 1 must not fire;
// timeout + 1 must fire. This pins the ">" (strict) comparison so a
// well-intentioned edit to ">=" doesn't shave a millisecond off every real
// session unnoticed.
void testTimeoutWindowBoundaries() {
  const uint32_t now = 1000000U;

  // timeout - 1 ms silent -> not reaped
  assert(!shouldReapOwner(now, now - (kWsOwnerSilenceTimeoutMs - 1U),
                          kWsOwnerSilenceTimeoutMs));

  // exactly at the timeout -> not reaped (strict >).
  assert(!shouldReapOwner(now, now - kWsOwnerSilenceTimeoutMs,
                          kWsOwnerSilenceTimeoutMs));

  // timeout + 1 ms silent -> reaped
  assert(shouldReapOwner(now, now - (kWsOwnerSilenceTimeoutMs + 1U),
                         kWsOwnerSilenceTimeoutMs));

  // Way over -> reaped
  assert(shouldReapOwner(now, now - 60000U, kWsOwnerSilenceTimeoutMs));
}

// millis() wraps every 49.7 days. The signed delta form is correct across
// that boundary too. lastEventMs = 0xFFFFFF00 (just before the wrap),
// now = 100 (just after). Real elapsed time is 356 ms, well under the
// timeout. The unsigned diff (100 - 0xFFFFFF00) is 356 when interpreted as
// int32_t, and it is 356 when interpreted as uint32_t here too because
// modular arithmetic. That is fine -- rollover isn't the case that broke,
// the cross-task future-stamp is. But we pin both anyway so a future
// refactor that "fixes" rollover by clamping doesn't reintroduce the
// future-stamp bug.
void testMillisRolloverIsHandledCorrectly() {
  // 356 ms elapsed across the wrap -- not reaped.
  assert(!shouldReapOwner(100U, 0xFFFFFF00U, kWsOwnerSilenceTimeoutMs));

  // Way past the wrap: 8500 ms of real silence straddling the wrap.
  //   lastEventMs = 0xFFFFF000
  //   now         = lastEventMs + 8500  (wraps to 0x0000000004... etc.)
  const uint32_t lastEventMs = 0xFFFFF000U;
  const uint32_t nowAfterWrap = lastEventMs + 8500U;
  assert(shouldReapOwner(nowAfterWrap, lastEventMs, kWsOwnerSilenceTimeoutMs));

  // Exact wrap boundary, no elapsed time -- not reaped.
  assert(!shouldReapOwner(0x00000000U, 0xFFFFFFFFU, kWsOwnerSilenceTimeoutMs));
}

// Bug A "never-armed" case. With the fix in main.cpp the admission path
// stamps lastEventMs = now, so this input shape (lastEventMs == 0 while
// now is large) should not normally occur. Pin the raw arithmetic anyway
// so that if someone regresses the admission path AND the reaper policy
// simultaneously, we still know: the reaper with an unarmed clock will
// treat the client as silent for the full absolute millis() value.
//
// The value of pinning this is documenting the invariant. If the admission
// path stops arming the clock, the reaper still fires on this input --
// which is exactly what killed the phone connection. That is not a policy
// bug in shouldReapOwner; the fix belongs at admission. This assertion
// documents that contract in one place, so a maintainer investigating a
// regression can find the coupling.
void testNeverArmedClockWithLargeNowIsReaped() {
  const uint32_t now = 90000U;      // ~90 s after boot
  const uint32_t lastEventMs = 0U;  // never armed
  // Absolute silence of 90 s >> 8 s timeout, so yes, reaped. The FIX is at
  // admission (stamp lastEventMs = now), not here.
  assert(shouldReapOwner(now, lastEventMs, kWsOwnerSilenceTimeoutMs));
}

// Sanity: the reaper is monotonic in "silence". If silent for N ms triggers
// a reap, silent for N+1 also triggers; if N does not, N-1 does not either.
// This catches an implementation that used absolute values, division, or
// anything else non-monotonic.
void testMonotonicInSilence() {
  const uint32_t now = 500000U;
  bool prev = false;
  for (uint32_t silence = 0; silence <= 20000U; silence += 250U) {
    const bool reaped =
        shouldReapOwner(now, now - silence, kWsOwnerSilenceTimeoutMs);
    if (prev) assert(reaped);  // once true, stays true as silence grows
    prev = reaped;
  }
  // Sanity: at some point in the sweep we did cross the threshold.
  assert(prev);
}

}  // namespace

int main() {
  testFreshlyAdmittedOwnerIsNotReaped();
  testFutureTimestampFromCrossTaskRaceIsNotReaped();
  testTimeoutWindowBoundaries();
  testMillisRolloverIsHandledCorrectly();
  testNeverArmedClockWithLargeNowIsReaped();
  testMonotonicInSilence();
  printf("ws-owner-reaper tests passed\n");
  return 0;
}
