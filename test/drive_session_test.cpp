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
  testManualShutdownAndReset();
  return 0;
}
