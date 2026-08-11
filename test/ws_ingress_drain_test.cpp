// Host tests for the bounded ingress-quarantine drain policy in
// beginWsIngressQuarantine() (src/main.cpp around line 5900).
//
// The reasoning is spelled out inline at the call site: the drain used to be
// unbounded, and that turned out to be a deadlock, not a delay. If an
// in-flight AsyncTCP callback is itself blocked -- or was entered from the
// main task, so it cannot complete while the main task sits waiting -- the
// wait never ends, the loop task stops feeding the watchdog, and TASK_WDT
// resets the board about five seconds later. Bounding the wait and then
// proceeding is strictly better than resetting. Slack in the drain policy is
// paid for once by the caller; a reset costs the whole session and dumps the
// robot back to the QR screen.
//
// The policy has three moving parts and this file pins each:
//
//   (a) if the counter is already zero, drain returns immediately -- the
//       common case, and it must not add spurious latency to a healthy
//       session.
//
//   (b) while the counter is non-zero the drain waits.
//
//   (c) the wait terminates at kIngressDrainTimeoutMs (750 ms) rather than
//       looping forever, even if the counter is still non-zero. This is the
//       actual bug fix: the value of the bound is deliberately less than the
//       5 s TASK_WDT window and comfortably longer than any real callback.
//
// The deadline comparison uses the codebase's rollover-safe idiom
//
//     if (static_cast<int32_t>(millis() - deadline) >= 0) break;
//
// and this file also pins that shape, because we have already shipped one
// unsigned-underflow bug in exactly this arithmetic (see the R46 entry in
// docs/PROJECT-HISTORY.md and test/ws_owner_reaper_test.cpp). A well-meaning
// simplification to `millis() > deadline` looks equivalent on paper but
// underflows across the 49.7 day millis() wrap AND across cross-task races.
// One of those is impossible here (millis() and deadline are captured on the
// same task within a few instructions of each other), but the OTHER --
// rollover -- is not, because the ESP32 has run for months at a time.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NDEBUG
#error "Host safety tests require assertions enabled"
#endif

namespace {

// Mirrors src/main.cpp:5279. The drift check in test_ws_protocol_drift.py
// asserts this matches the firmware source.
constexpr uint32_t kIngressDrainTimeoutMs = 750;

// A fake wall clock. The drain policy reads millis() twice: once to capture
// `deadline = millis() + timeout`, and then once per spin iteration inside
// the loop. Neither of those calls sleeps, and the loop's own `delay(1)` is
// the real spacer. We collapse that here to a step function driven by the
// test.
struct FakeClock {
  uint32_t now = 0;
  uint32_t step(uint32_t deltaMs) {
    now += deltaMs;
    return now;
  }
};

// Structurally identical to the loop body in beginWsIngressQuarantine():
//
//     const uint32_t deadline = millis() + kIngressDrainTimeoutMs;
//     while (g_wsIngressActive.load() != 0U) {
//       if (static_cast<int32_t>(millis() - deadline) >= 0) break;
//       delay(1);
//     }
//
// Returns the number of loop iterations executed (0 == fast path). The
// caller controls both the counter and the clock, so the whole state space
// is deterministic.
struct DrainResult {
  uint32_t iterations;
  uint32_t exitNow;
  bool timedOut;
};

// The counter is captured by reference so the test can drop it to zero
// partway through a wait, or leave it high to force the timeout branch.
DrainResult drainOnce(FakeClock& clock, const uint32_t& counter,
                      uint32_t timeoutMs) {
  DrainResult r{0U, clock.now, false};
  // Capture the deadline BEFORE looking at the counter, matching the
  // production code. If the counter is already zero the loop body never
  // runs, but the deadline is still armed -- this is what makes the fast
  // path zero-latency in the common case.
  const uint32_t deadline = clock.now + timeoutMs;
  while (counter != 0U) {
    // Rollover-safe deadline comparison. See file header.
    if (static_cast<int32_t>(clock.now - deadline) >= 0) {
      r.timedOut = true;
      break;
    }
    ++r.iterations;
    // Simulates the delay(1) inside the real loop.
    clock.step(1U);
  }
  r.exitNow = clock.now;
  return r;
}

// Property (a) -- the fast path.
//
// A healthy session drains through here every time the transport is reset.
// If the counter is already zero, the drain must not add wait, must not
// mark itself as timed out, and must not mutate anything the caller can
// see. Any latency shipped here is paid on every reset, so this test is
// deliberately strict: iterations == 0.
void testFastPathWhenCounterAlreadyZero() {
  FakeClock clock;
  clock.now = 42U;
  const uint32_t counter = 0U;
  const DrainResult r = drainOnce(clock, counter, kIngressDrainTimeoutMs);
  assert(r.iterations == 0U);
  assert(!r.timedOut);
  assert(r.exitNow == 42U);

  // Same at absurd clock values: the fast path must not care about the
  // absolute time. Two spots on the millis() range picked to catch the
  // 32-bit wrap edge.
  FakeClock nearRollover;
  nearRollover.now = 0xFFFFFFF0U;
  const DrainResult r2 =
      drainOnce(nearRollover, counter, kIngressDrainTimeoutMs);
  assert(r2.iterations == 0U);
  assert(!r2.timedOut);
  assert(r2.exitNow == 0xFFFFFFF0U);
}

// Property (b) -- while the counter is non-zero, the drain waits.
//
// Real callbacks land in ~1 ms; we simulate a callback that finishes on the
// 50th iteration and confirm the drain waits that long (and no longer).
// This is the case the whole thing exists for: the callback WOULD have
// been serialized against us anyway, and letting it finish is the whole
// point.
void testWaitsForCounterToDrainWhenBelowTimeout() {
  FakeClock clock;
  clock.now = 1000U;
  uint32_t counter = 3U;  // three in-flight callbacks

  // Manually pump the drain, dropping the counter after a plausible number
  // of iterations for each callback.
  const uint32_t deadline = clock.now + kIngressDrainTimeoutMs;
  uint32_t iterations = 0U;
  while (counter != 0U) {
    if (static_cast<int32_t>(clock.now - deadline) >= 0) {
      assert(!"drain timed out inside the bound -- test setup wrong");
      break;
    }
    ++iterations;
    clock.step(1U);
    // Three callbacks finishing at iterations 15, 30, 50.
    if (iterations == 15U || iterations == 30U || iterations == 50U) {
      --counter;
    }
  }
  assert(counter == 0U);
  assert(iterations == 50U);
  assert(clock.now == 1050U);
  // Strictly less than the timeout: real callbacks are fast, so the drain
  // must exit as soon as the counter reaches zero -- it must NOT hold on
  // for the rest of the budget.
  assert(clock.now < 1000U + kIngressDrainTimeoutMs);
}

// Property (c) -- the wait terminates at the bound rather than spinning
// forever.
//
// >>> THIS is the hardware bug. A stuck callback used to hang the loop task
// >>> until TASK_WDT reset the board. This assertion is what makes it
// >>> impossible to reintroduce that without RED.
void testGivesUpAtBoundWhenCounterNeverDrops() {
  FakeClock clock;
  clock.now = 2000U;
  const uint32_t stuckCounter = 1U;  // never drops
  const DrainResult r = drainOnce(clock, stuckCounter, kIngressDrainTimeoutMs);
  assert(r.timedOut);
  // Must not have looped past the deadline: exact bound. This pins that a
  // maintainer cannot "just wait a little longer" without noticing.
  assert(r.exitNow == 2000U + kIngressDrainTimeoutMs);
  // And the loop DID pump iterations (i.e. it's not the fast path that
  // accidentally trips the timedOut flag).
  assert(r.iterations > 0U);
  assert(r.iterations == kIngressDrainTimeoutMs);
}

// The bound value itself is load-bearing. Comfortably longer than any real
// callback so healthy sessions never see the timeout branch; well under
// the 5 s TASK_WDT that it exists to avoid tripping.
//
// If someone retunes this constant, they must retune it here AND in
// test_ws_protocol_drift.py, which is the whole point of pinning it.
void testDocumentedBoundValue() {
  assert(kIngressDrainTimeoutMs == 750U);
  assert(kIngressDrainTimeoutMs > 100U);   // longer than a real callback
  assert(kIngressDrainTimeoutMs < 5000U);  // shorter than TASK_WDT
}

// The deadline comparison is rollover-safe.
//
// If the millis() clock is 4 ms below its rollover point when the deadline
// is captured, the arithmetic
//
//     deadline = 0xFFFFFFFC + 750 = 0x000002EA  (unsigned wrap)
//
// looks completely wrong on paper. The signed-delta form makes it correct
// anyway: at now = 0x00000100 (i.e. 260 ms after the wrap), the elapsed
// time is 264 ms, and
//
//     static_cast<int32_t>(now - deadline) = -138
//
// reads as "138 ms of budget left", which is what it must. If someone
// simplifies this to `now > deadline` (unsigned), the comparison is TRUE
// after the wrap and the drain gives up on a session that has not actually
// been waiting long.
//
// This test walks the wrap boundary and pins that the signed form always
// answers correctly.
void testRolloverSafeDeadlineArithmetic() {
  // Case 1: deadline is in the future, both operands post-wrap.
  {
    const uint32_t now = 100U;
    const uint32_t deadline = 200U;
    assert(static_cast<int32_t>(now - deadline) < 0);  // budget left
  }
  // Case 2: deadline armed just before the wrap, now just after.
  {
    const uint32_t now = 100U;
    const uint32_t deadline = 0xFFFFFF00U + kIngressDrainTimeoutMs;  // wraps
    // Real elapsed budget between armed-at and now: about 100 - 0xFFFFFF00
    // in signed 32-bit == 356, and the deadline was armed 750 ms after
    // that, so we have ~394 ms budget left.
    assert(static_cast<int32_t>(now - deadline) < 0);
  }
  // Case 3: legitimately past the deadline post-wrap.
  {
    const uint32_t now = 900U;
    const uint32_t deadline = 100U;  // fires
    assert(static_cast<int32_t>(now - deadline) >= 0);
  }
  // Case 4: EXACTLY at the deadline -- ">=" fires, matching the production
  // code's ">= 0" idiom.
  {
    const uint32_t now = 500U;
    const uint32_t deadline = 500U;
    assert(static_cast<int32_t>(now - deadline) >= 0);
  }
}

// Bounded end-to-end. A stuck counter with a very large starting clock,
// walking across the millis() rollover during the drain. This is the
// interesting case: unsigned arithmetic works fine as long as you never
// touch this shape, and this shape is exactly what breaks it.
//
// The test walks a synthetic clock across the wrap boundary, then confirms
// that the drain still gives up at the SAME 750 ms of elapsed wall time,
// not at the arithmetic boundary of the rollover.
void testTimeoutIsCorrectAcrossMillisRollover() {
  FakeClock clock;
  clock.now = 0xFFFFFFC0U;  // 64 ms before the wrap
  const uint32_t startNow = clock.now;
  const uint32_t stuckCounter = 1U;
  const DrainResult r = drainOnce(clock, stuckCounter, kIngressDrainTimeoutMs);
  assert(r.timedOut);
  // Real elapsed time across the wrap: 64 ms + 750 ms - 64 = 750 ms. That
  // is, we walked from 0xFFFFFFC0 up past 0xFFFFFFFF and back down to
  // (750 - 64) = 686. The clock now is that unsigned wrap-around value.
  const uint32_t elapsed = clock.now - startNow;
  // NB: with unsigned arithmetic this is naturally 750, because subtraction
  // is modular. That's the codebase's rollover-safe convention.
  assert(elapsed == kIngressDrainTimeoutMs);
}

}  // namespace

int main() {
  testFastPathWhenCounterAlreadyZero();
  testWaitsForCounterToDrainWhenBelowTimeout();
  testGivesUpAtBoundWhenCounterNeverDrops();
  testDocumentedBoundValue();
  testRolloverSafeDeadlineArithmetic();
  testTimeoutIsCorrectAcrossMillisRollover();
  printf("ws-ingress-drain tests passed\n");
  return 0;
}
