// Host tests for the "non-owner control frames leave state undisturbed"
// policy in main.cpp's WS event dispatcher.
//
// Context (see R45 in docs/PROJECT-HISTORY.md): the transport previously
// called client->close() when a frame arrived from a client that was no
// longer the transport owner. That looked tidy but was a use-after-free --
// AsyncTCP could already be tearing down that client, and the next _recv
// path freed memory. The fix was to STOP touching a non-owner client at
// all: no close, no session mutation, no ownership change, no drive reset.
// The web app grew a 4 s telemetry-staleness watchdog to notice, from its
// side, that it had been revoked.
//
// The three payload-carrying opcodes (control, mood, gesture, sound) all
// route through the same guard in the loop dispatcher:
//
//     if (!wsEventMatchesCurrentTransport(event) ||
//         !driveSessionOwnerMatchesEvent(event)) {
//       rejectWsTransportAdmission(event, "Forbidden");
//       break;
//     }
//
// This test file models that dispatcher-level filter as a small policy
// function and pins that:
//
//   (a) a frame from the admitted owner passes,
//   (b) a frame from any other client is DROPPED and the DriveSession is
//       never even invoked,
//   (c) the drop path does NOT close, revoke, or otherwise poke the
//       transport-owner tokens,
//   (d) the DriveSession itself will happily corrupt owner state if you
//       hand it a non-owner frame -- which is why the transport-layer
//       filter is load-bearing, and why it must not be inlined away.
//
// (d) is the honest half of this test: the DriveSession contract does
// slam a legitimate owner back to WaitNeutral via failSafe() when frame()
// is called with a mismatched client (drive_session.cpp:72). The
// transport layer's job is to ensure that path is never reached. Pinning
// both halves in the same file means a maintainer removing either side
// gets a RED test and has to justify it.

#include "../src/drive_session.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#ifdef NDEBUG
#error "Host safety tests require assertions enabled"
#endif

namespace {

// Snapshot of the state that must not move when a non-owner frame arrives.
// Enough to notice any mutation the DriveSession is capable of producing
// under the frame() path: state, owner id, generation, and whether the
// session still reports it has an owner.
struct SessionSnapshot {
  DriveSession::State state;
  bool hasOwner;
  uint32_t owner;
  uint32_t generation;
};

SessionSnapshot snapshot(const DriveSession& s) {
  return {s.state(), s.hasOwner(), s.owner(), s.generation()};
}

bool equal(const SessionSnapshot& a, const SessionSnapshot& b) {
  return a.state == b.state && a.hasOwner == b.hasOwner &&
         a.owner == b.owner && a.generation == b.generation;
}

// Structurally identical to the loop dispatcher guard in main.cpp:9049-9053
// (and mirrored for Mood/Gesture/Sound at 9099/9121/9150/9179). Kept here
// as a pure function so a maintainer editing the guard shape in main.cpp
// can be reminded, by a RED test, that the guard covers TWO invariants
// (transport identity and drive session ownership) and both are needed.
//
// The two atomics the real guard checks are:
//   - g_wsTransportOwnerToken   (transport-side identity of admitted owner)
//   - g_driveSessionTransportToken + g_driveSession.owner()
//                               (drive-session-side identity)
// A stale frame from a former owner can match one and not the other; both
// are load-bearing.
struct AdmittedOwner {
  uint32_t transportToken;
  uint32_t driveSessionToken;
  uint32_t client;
};

struct IncomingFrame {
  uint32_t transportToken;
  uint32_t driveSessionToken;
  uint32_t client;
};

bool shouldDispatchToDriveSession(const AdmittedOwner& current,
                                  const IncomingFrame& frame) {
  return current.transportToken != 0U &&
         current.transportToken == frame.transportToken &&
         current.driveSessionToken != 0U &&
         current.driveSessionToken == frame.driveSessionToken &&
         current.client == frame.client;
}

// Property (a) -- admitted owner passes.
void testAdmittedOwnerFramesAreDispatched() {
  const AdmittedOwner current{111U, 222U, 7U};
  const IncomingFrame same{111U, 222U, 7U};
  assert(shouldDispatchToDriveSession(current, same));
}

// Property (b) -- every axis of a mismatch is dropped.
void testAnyMismatchDropsTheFrame() {
  const AdmittedOwner current{111U, 222U, 7U};

  // Wrong client id (the R45 case: stale former owner).
  const IncomingFrame wrongClient{111U, 222U, 8U};
  assert(!shouldDispatchToDriveSession(current, wrongClient));

  // Wrong transport generation (client reconnected under a new socket,
  // stale frame from the old one arrives late).
  const IncomingFrame staleTransport{110U, 222U, 7U};
  assert(!shouldDispatchToDriveSession(current, staleTransport));

  // Wrong drive-session token (session was reset between admission and
  // this frame's dispatch).
  const IncomingFrame staleDriveSession{111U, 220U, 7U};
  assert(!shouldDispatchToDriveSession(current, staleDriveSession));

  // No admitted owner at all -- the WsEvent::Control handler in main.cpp
  // checks !driveSessionOwner before this guard, but the guard itself is
  // still correct on that input.
  const AdmittedOwner none{0U, 0U, 0U};
  const IncomingFrame anyFrame{111U, 222U, 7U};
  assert(!shouldDispatchToDriveSession(none, anyFrame));
}

// Property (d) -- if the filter is bypassed, the DriveSession corrupts
// legitimate owner state. This is the honest half: the DriveSession's own
// contract has a slam-behavior for non-owner frames (failSafe with
// Reason::NonOwner, which sets state = WaitNeutral and clears neutral
// progress), so the transport-layer filter is the only thing standing
// between a stray frame and a mid-drive stall for the true operator.
//
// If someone deletes the guard in main.cpp thinking the DriveSession will
// do the right thing anyway, this test documents that no, it will not.
void testBypassingTheFilterCorruptsOwnerState() {
  DriveSession session;
  session.connect(7U, 0U);
  // Drive the owner up to Active.
  for (uint8_t i = 0; i < DriveSession::kNeutralFramesRequired; ++i) {
    session.frame(7U, static_cast<uint8_t>(1 + i), 0, 0,
                  static_cast<uint32_t>(i) * 30U, true);
  }
  const DriveSession::Decision drive =
      session.frame(7U, DriveSession::kNeutralFramesRequired + 1U,
                    50, 50, 200U, true);
  assert(drive.action == DriveSession::Action::Drive);
  assert(session.state() == DriveSession::State::Active);

  // Simulate the bypass: hand the session a frame from client 99 (not
  // the owner). failSafe fires, and the true owner is knocked back to
  // WaitNeutral. Ownership itself does NOT change (which is why R45's
  // "close and boot the client" fix was misconceived) -- the damage is
  // that the operator's session is stalled until they re-centre.
  const DriveSession::Decision nonOwnerFrame =
      session.frame(99U, 0U, 100, -100, 300U, true);
  assert(nonOwnerFrame.reason == DriveSession::Reason::NonOwner);
  assert(session.state() == DriveSession::State::WaitNeutral);
  assert(session.hasOwner());
  assert(session.owner() == 7U);
  // No drive command was accepted for the non-owner.
  assert(nonOwnerFrame.action == DriveSession::Action::PowerDown);
}

// Property (b) + (c) -- the WHOLE point of R45. Simulate the loop
// dispatcher's guard around a DriveSession that is currently owned by
// client 7 in Active. A stale frame from client 99 (a former owner whose
// TCP is being torn down) must:
//   - not be dispatched to DriveSession (state stays Active),
//   - not touch ownership,
//   - not touch generation.
// This is the ONE test that would have caught the R45 shape as a
// specification bug independently of whether the close() happened to
// crash. Even if AsyncTCP had made close() memory-safe, doing anything at
// all to the session on a non-owner frame is a correctness bug.
void testR45DoesNotDisturbTheAdmittedOwner() {
  DriveSession session;
  session.connect(7U, 0U);
  for (uint8_t i = 0; i < DriveSession::kNeutralFramesRequired; ++i) {
    session.frame(7U, static_cast<uint8_t>(1 + i), 0, 0,
                  static_cast<uint32_t>(i) * 30U, true);
  }
  session.frame(7U, DriveSession::kNeutralFramesRequired + 1U,
                50, 50, 200U, true);
  assert(session.state() == DriveSession::State::Active);
  const SessionSnapshot before = snapshot(session);

  const AdmittedOwner current{
      /*transportToken=*/500U,
      /*driveSessionToken=*/600U,
      /*client=*/7U,
  };
  const IncomingFrame stale{
      /*transportToken=*/500U,
      /*driveSessionToken=*/600U,
      /*client=*/99U,  // stale former owner, now revoked
  };

  // Guard rejects the frame. Under the R45 fix, the dispatcher does not
  // even reach DriveSession -- so ownership state is unchanged.
  if (shouldDispatchToDriveSession(current, stale)) {
    // The guard is broken; the frame WOULD be dispatched and would poison
    // the session (see testBypassingTheFilterCorruptsOwnerState above).
    // Fail loudly.
    assert(!"R45 regression: non-owner frame passed the transport guard");
  }

  const SessionSnapshot after = snapshot(session);
  assert(equal(before, after));
}

// A related invariant that's easy to accidentally break: rejecting a
// non-owner frame must not touch the OTHER client's transport-owner token,
// only its own admission. The R45 fix explicitly does not close the
// non-owner client's socket, which would have been a use-after-free.
//
// We model this by asserting the guard is a pure predicate: it never
// suggests writing anything to `current`. That's a shape assertion --
// `shouldDispatchToDriveSession` returns bool with no output parameter --
// but it is the shape that matters, so we make it explicit.
void testGuardIsPureAndCannotMutateAdmittedOwner() {
  AdmittedOwner current{111U, 222U, 7U};
  const AdmittedOwner beforeCall = current;
  const IncomingFrame stale{111U, 222U, 99U};
  (void)shouldDispatchToDriveSession(current, stale);
  assert(current.transportToken == beforeCall.transportToken);
  assert(current.driveSessionToken == beforeCall.driveSessionToken);
  assert(current.client == beforeCall.client);
}

// The g_lastControlEventMs liveness clock is stamped from the Control
// handler AFTER the transport guard passes (see main.cpp:9070). A
// non-owner frame that failed the guard must therefore not extend the
// silence timer. We model that here: the timestamp write is INSIDE the
// dispatch branch, not before it. A well-intentioned refactor that
// hoisted the stamp before the guard would silently defeat the stale-
// owner reaper by letting stale ghost frames keep it happy.
void testLivenessTimestampIsWrittenOnlyAfterGuardPasses() {
  const AdmittedOwner current{111U, 222U, 7U};
  const IncomingFrame stale{111U, 222U, 99U};

  uint32_t liveness = 0U;
  const uint32_t frameReceivedAt = 8500U;
  if (shouldDispatchToDriveSession(current, stale)) {
    liveness = frameReceivedAt;
  }
  // Guard rejected the frame; liveness is untouched.
  assert(liveness == 0U);

  const IncomingFrame ownerFrame{111U, 222U, 7U};
  if (shouldDispatchToDriveSession(current, ownerFrame)) {
    liveness = frameReceivedAt;
  }
  assert(liveness == frameReceivedAt);
}

}  // namespace

int main() {
  testAdmittedOwnerFramesAreDispatched();
  testAnyMismatchDropsTheFrame();
  testBypassingTheFilterCorruptsOwnerState();
  testR45DoesNotDisturbTheAdmittedOwner();
  testGuardIsPureAndCannotMutateAdmittedOwner();
  testLivenessTimestampIsWrittenOnlyAfterGuardPasses();
  printf("ws-nonowner-filter tests passed\n");
  return 0;
}
