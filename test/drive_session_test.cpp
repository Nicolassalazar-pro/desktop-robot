#include "../src/drive_session.h"

#include <assert.h>
#include <stdint.h>

#ifdef NDEBUG
#error "Host safety tests require assertions enabled"
#endif

namespace {

using Action = DriveSession::Action;
using Reason = DriveSession::Reason;
using State = DriveSession::State;

void expect(const DriveSession::Decision& decision, Action action, State state,
            Reason reason) {
  assert(decision.action == action);
  assert(decision.state == state);
  assert(decision.reason == reason);
}

void arm(DriveSession& session, uint32_t client, uint8_t firstSequence,
         uint32_t firstTime) {
  for (uint8_t i = 0; i < DriveSession::kNeutralFramesRequired; ++i) {
    const DriveSession::Decision decision =
        session.frame(client, static_cast<uint8_t>(firstSequence + i), 0, 0,
                      firstTime + static_cast<uint32_t>(i) * 30U, true);
    expect(decision, Action::PowerDown,
           i + 1 == DriveSession::kNeutralFramesRequired ? State::ArmedOff
                                                         : State::WaitNeutral,
           i + 1 == DriveSession::kNeutralFramesRequired
               ? Reason::NeutralReady
               : Reason::NeutralProgress);
  }
}

void testArmingAndDriveTransitions() {
  DriveSession session;
  expect(session.connect(7, 100), Action::PowerDown, State::WaitNeutral,
         Reason::Connected);
  assert(session.hasOwner());
  assert(session.owner() == 7);
  assert(session.generation() == 1);
  assert(session.clientOwner() == 7);
  assert(session.sessionGeneration() == 1);

  expect(session.frame(7, 1, 20, 0, 110, true), Action::PowerDown,
         State::WaitNeutral, Reason::NeutralRequired);
  arm(session, 7, 2, 120);

  DriveSession::Decision decision =
      session.frame(7, 7, -100, 100, 250, true);
  expect(decision, Action::Drive, State::Active, Reason::DriveAccepted);
  assert(decision.l == -100);
  assert(decision.r == 100);

  expect(session.frame(7, 8, 0, 0, 280, true), Action::Neutral,
         State::NeutralHold, Reason::NeutralHoldEntered);
  expect(session.frame(7, 9, 0, 0, 310, true), Action::Neutral,
         State::NeutralHold, Reason::NeutralHeld);
  expect(session.frame(7, 10, 1, -1, 340, true), Action::Drive, State::Active,
         Reason::DriveAccepted);
}

void testNeutralGapAndSequenceFreshness() {
  DriveSession session;
  session.connect(1, 0);
  expect(session.frame(1, 250, 0, 0, 0, true), Action::PowerDown,
         State::WaitNeutral, Reason::NeutralProgress);
  expect(session.frame(1, 251, 0, 0, 90, true), Action::PowerDown,
         State::WaitNeutral, Reason::NeutralProgress);
  expect(session.frame(1, 252, 0, 0, 181, true), Action::PowerDown,
         State::WaitNeutral, Reason::NeutralGapRestart);
  expect(session.frame(1, 252, 0, 0, 182, true), Action::PowerDown,
         State::WaitNeutral, Reason::StaleSequence);
  expect(session.frame(1, 251, 0, 0, 183, true), Action::PowerDown,
         State::WaitNeutral, Reason::StaleSequence);

  // Forward modulo wrap is fresh: 252 -> 0 has delta 4.
  expect(session.frame(1, 0, 0, 0, 184, true), Action::PowerDown,
         State::WaitNeutral, Reason::NeutralProgress);
  // Half-range and larger deltas are out of order.
  expect(session.frame(1, 128, 0, 0, 95, true), Action::PowerDown,
         State::WaitNeutral, Reason::StaleSequence);
}

void testFailSafeInputsRequireRearming() {
  DriveSession session;
  session.connect(10, 0);
  arm(session, 10, 1, 0);
  expect(session.frame(11, 6, 1, 1, 130, true), Action::PowerDown,
         State::WaitNeutral, Reason::NonOwner);
  expect(session.frame(10, 6, 101, 0, 140, true), Action::PowerDown,
         State::WaitNeutral, Reason::MalformedCommand);
  expect(session.frame(10, 6, 0, 0, 150, false), Action::PowerDown,
         State::WaitNeutral, Reason::CalibrationIneligible);
  expect(session.frame(10, 6, 1, 0, 160, true), Action::PowerDown,
         State::WaitNeutral, Reason::NeutralRequired);
}

void testOwnershipAndGeneration() {
  DriveSession session;
  session.connect(4, 10);
  expect(session.connect(5, 20), Action::PowerDown, State::WaitNeutral,
         Reason::OwnerBusy);
  assert(session.clientOwner() == 4);
  assert(session.sessionGeneration() == 1);

  expect(session.connect(4, 30), Action::PowerDown, State::WaitNeutral,
         Reason::Connected);
  assert(session.sessionGeneration() == 2);
  expect(session.disconnect(5), Action::PowerDown, State::WaitNeutral,
         Reason::NonOwner);
  assert(session.hasOwner());
  expect(session.disconnect(4), Action::PowerDown, State::SafeOff,
         Reason::Disconnected);
  assert(!session.hasOwner());
}

void testTimeoutAndTimeWrap() {
  DriveSession session;
  const uint32_t start = UINT32_MAX - 100U;
  session.connect(3, start);
  arm(session, 3, 1, start);
  session.frame(3, 6, 30, 30, start + 150U, true);

  expect(session.tick(start + 449U), Action::None, State::Active,
         Reason::None);
  expect(session.tick(start + 450U), Action::PowerDown, State::WaitNeutral,
         Reason::ControlTimeout);
  expect(session.tick(start + 1000U), Action::None, State::WaitNeutral,
         Reason::None);
}

void testSequenceDeltaAndCommandBoundaries() {
  DriveSession session;
  session.connect(6, 0);
  expect(session.frame(6, 0, DriveSession::kCommandMin,
                       DriveSession::kCommandMax, 1, true),
         Action::PowerDown, State::WaitNeutral, Reason::NeutralRequired);
  expect(session.frame(6, 127, 0, 0, 2, true), Action::PowerDown,
         State::WaitNeutral, Reason::NeutralProgress);
  expect(session.frame(6, 255, 0, 0, 3, true), Action::PowerDown,
         State::WaitNeutral, Reason::StaleSequence);
  expect(session.frame(6, 128, 0, 0, 4, true), Action::PowerDown,
         State::WaitNeutral, Reason::NeutralProgress);
  expect(session.frame(6, 129, DriveSession::kCommandMin - 1, 0, 5, true),
         Action::PowerDown, State::WaitNeutral, Reason::MalformedCommand);
  expect(session.frame(6, 129, 0, DriveSession::kCommandMax + 1, 6, true),
         Action::PowerDown, State::WaitNeutral, Reason::MalformedCommand);
}

// The finding, verified against src/drive_session.cpp:41-57 as of this
// commit: calling disconnect() with a non-owner token slams the true owner
// back to WaitNeutral. The relevant code:
//
//   Decision DriveSession::disconnect(uint32_t client) {
//     ...
//     if (!hasOwner_ || owner_ != client) {
//       if (hasOwner_ && state_ != State::Manual) {
//         state_ = State::WaitNeutral;
//         resetNeutral();
//       }
//       return decide(Action::PowerDown, 0, 0, Reason::NonOwner, ...);
//     }
//     ...
//   }
//
// So YES, a spurious disconnect(strangerId) call while a legitimate owner
// is driving will kick the operator out of Active and force them to
// re-centre both sticks and count out five neutral frames before the
// robot moves again. Ownership itself is NOT taken away (hasOwner_ stays
// true, owner_ stays as the true owner), so the previous suite's
// assertion of `session.hasOwner()` after `disconnect(5)` correctly holds
// -- but the previous suite happened to call disconnect from WaitNeutral,
// where the slam is a no-op. Nothing pins the interesting Active case.
//
// The main.cpp WsEventType::Disconnect handler currently guards
// `driveSessionOwnerMatchesEvent(event)` before calling into
// DriveSession, so the transport layer prevents this from firing today
// (main.cpp:9024-9026). This test pins the DriveSession side of the
// contract, so a future maintainer who removes the transport guard
// thinking "the session will DTRT" gets a clear signal that no, it will
// not, and the transport-layer guard is load-bearing.
//
// Physically: WaitNeutral means the operator's sticks have to travel
// back to zero, hold zero for five frames (~150 ms at 30 Hz), and only
// then does drive re-arm. A hostile client that could reach the
// disconnect path could grief a legitimate driver.
//
// A note on intent: I'm treating the CURRENT behaviour as intentional
// (the same failSafe/clear-neutral pattern is applied to every other
// non-owner touch of the session, and it fails safe -- the wheels stop),
// and pinning it as-is. If the team decides the disconnect path should
// instead be a pure NonOwner NO-OP (leave state_ alone), this test must
// be updated in the same change -- which is exactly the kind of
// deliberate decision the pin exists to force.
void testNonOwnerDisconnectSlamsActiveOwnerBackToWaitNeutral() {
  DriveSession session;
  session.connect(10, 0);
  arm(session, 10, 1, 0);
  // Take the owner all the way into Active with a real drive frame.
  const DriveSession::Decision drive = session.frame(10, 6, 50, 50, 200, true);
  expect(drive, Action::Drive, State::Active, Reason::DriveAccepted);
  assert(session.state() == State::Active);
  const uint32_t ownerBefore = session.owner();
  const uint32_t generationBefore = session.generation();

  // A stranger's disconnect arrives. The session logic slams the state
  // back to WaitNeutral and clears the neutral-frame counter.
  const DriveSession::Decision spurious = session.disconnect(11);
  expect(spurious, Action::PowerDown, State::WaitNeutral, Reason::NonOwner);

  // Ownership itself survives: the true owner is still owner. Otherwise
  // a stranger could steal the session by just calling disconnect().
  assert(session.hasOwner());
  assert(session.owner() == ownerBefore);
  assert(session.generation() == generationBefore);

  // The neutral-frame counter has been reset, so the true owner must
  // send kNeutralFramesRequired zeros again before they get another
  // Drive decision. This is the physical cost to the operator.
  //
  // Prove it by streaming neutrals and confirming we walk the arming
  // ramp from scratch, not just get an immediate ArmedOff.
  for (uint8_t i = 0; i < DriveSession::kNeutralFramesRequired - 1U; ++i) {
    const DriveSession::Decision d =
        session.frame(10, static_cast<uint8_t>(7 + i), 0, 0,
                      210U + static_cast<uint32_t>(i) * 30U, true);
    expect(d, Action::PowerDown, State::WaitNeutral, Reason::NeutralProgress);
  }
  const DriveSession::Decision armed = session.frame(
      10, static_cast<uint8_t>(7 + DriveSession::kNeutralFramesRequired - 1U),
      0, 0, 210U + (DriveSession::kNeutralFramesRequired - 1U) * 30U, true);
  expect(armed, Action::PowerDown, State::ArmedOff, Reason::NeutralReady);
}

// The Manual carve-out in disconnect(): a spurious non-owner disconnect
// while the session is in Manual must NOT drop it out of Manual. The
// source code explicitly guards this (drive_session.cpp:48
// `if (hasOwner && state_ != State::Manual)`), and the guard is why the
// LCD Manual mode can safely co-exist with a phantom disconnect from a
// stale AsyncTCP thread.
//
// This case is not actually reachable in production because Manual
// clears the owner first (enterManual() calls clearOwner()), so hasOwner
// is false and the outer `!hasOwner` branch fires without entering the
// inner clause. Still worth pinning because the code as written IS
// defensive against a hypothetical future path that keeps ownership
// through Manual.
void testNonOwnerDisconnectInManualDoesNotEscapeManual() {
  DriveSession session;
  session.enterManual();
  assert(session.state() == State::Manual);
  assert(!session.hasOwner());

  const DriveSession::Decision d = session.disconnect(42);
  // hasOwner_ is false, so the branch returns Reason::NonOwner without
  // touching state. Manual survives. (Note the reason is still NonOwner,
  // not ManualMode, because the outer `!hasOwner_` shortcut fires first.)
  expect(d, Action::PowerDown, State::Manual, Reason::NonOwner);
  assert(session.state() == State::Manual);
}

void testManualShutdownAndReset() {
  DriveSession session;
  session.connect(8, 0);
  expect(session.enterManual(), Action::PowerDown, State::Manual,
         Reason::ManualEntered);
  assert(!session.hasOwner());
  expect(session.connect(8, 1), Action::PowerDown, State::Manual,
         Reason::ManualMode);
  expect(session.exitManual(), Action::PowerDown, State::SafeOff,
         Reason::ManualExited);
  expect(session.shutdown(), Action::PowerDown, State::Shutdown,
         Reason::ShutdownEntered);
  expect(session.connect(8, 2), Action::PowerDown, State::Shutdown,
         Reason::ShutdownLocked);
  expect(session.reset(), Action::PowerDown, State::SafeOff, Reason::Reset);
  assert(session.sessionGeneration() == 0);
}

}  // namespace

int main() {
  testArmingAndDriveTransitions();
  testNeutralGapAndSequenceFreshness();
  testFailSafeInputsRequireRearming();
  testOwnershipAndGeneration();
  testTimeoutAndTimeWrap();
  testSequenceDeltaAndCommandBoundaries();
  testNonOwnerDisconnectSlamsActiveOwnerBackToWaitNeutral();
  testNonOwnerDisconnectInManualDoesNotEscapeManual();
  testManualShutdownAndReset();
  return 0;
}
