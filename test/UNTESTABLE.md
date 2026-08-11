# What this test suite cannot cover, and what to do instead

This directory has 16 native suites and 214 python tests. All of them are
green on a workstation with no ESP32 present. The suite does NOT prove
that the firmware works; it proves that a small, deliberate set of
policies has not silently drifted since it was written.

Six defects shipped this week. Every one lived in the WebSocket
transport/ownership region. Every one was caught by the operator or by a
hardware capture — none by a test. Waves 1 and 2 added coverage for the
policies that ARE reachable from a host build (reaper delta arithmetic,
non-owner drop, drain bound, opcode/frame-shape drift, DriveSession
non-owner disconnect). This document is the honest half: **what the host
build cannot see, and what to instrument on the device instead.**

If a change touches one of these regions and this document does not, the
change is incomplete. Read this file before proposing "we should test
that".

---

## 1. The WS transport-ownership state machine (still inside `main.cpp`)

**What it is.** Three atomics (`g_wsTransportOwner`,
`g_wsTransportOwnerToken`, `g_wsTransportGeneration`) plus one
`portMUX_TYPE` spinlock guard the "who is admitted right now" invariant.
The critical section decides which of {admit, ignore, reject} happens for
every incoming AsyncTCP callback.

**Why it is not extractable today.** Wave 2 tried and correctly declined
the extraction:

- `faultAndRevokeWsTransport` reads *unrelated* state inside the critical
  section (the drive session's owner, the LCD activity heartbeat, the
  battery-critical flag). Moving the lock without moving those reads
  would break the invariant they encode: the release path fires only if
  the caller can prove none of those signals is in the way.
- The lock type is FreeRTOS-only (`portMUX_TYPE` / `portENTER_CRITICAL`),
  with no portable `std::mutex` fallback and no header seam.
- The atomics live in an anonymous namespace with file-static linkage.
  Any host target that wants to call them has to re-declare them, which
  means the host target is testing a copy, not the code.

**What extraction would need, if we ever do it.**

1. Split the lock behind a header interface with two concrete impls
   (FreeRTOS `portMUX_TYPE` for device, `std::mutex` for host).
2. Move all reads inside the critical section into an explicit
   `Snapshot` struct passed in by the caller. The caller reads the state
   it needs before taking the lock. No cross-cutting reads inside the
   crit.
3. Give the three atomics a class home (`WsOwnership`) with public
   accessors. Anonymous-namespace globals do not survive linkage into a
   test binary.
4. Bring `faultAndRevokeWsTransport` with them — it is not a separate
   concern, it IS the release path.

Until then, the reaper-delta arithmetic (`ws_owner_reaper_test.cpp`) and
the non-owner-frame filter (`ws_nonowner_filter_test.cpp`) cover the two
policies of this machine that can be extracted as pure functions. The
lock ordering and the transaction shape are not covered.

**Runtime signal that DOES exist.** Every state transition of the
transport writes an `override:` breadcrumb line to serial (e.g.
`override: ws owner silent 8123ms -> releasing transport`,
`override: admitted client=<id>`). Grepping a `pio device monitor` log
for `override:` is the current inspection surface. If a change alters
the ownership handshake, that log MUST reflect the new sequence — a
change that goes silent is a regression by definition.

**What should be added.** A single-line periodic breadcrumb
(`diag: ws owner=<hex> silent=<ms> gen=<n>`) at 1 Hz would make the
ownership state observable without waiting for a transition. The
existing `diag:` heartbeat has the vocabulary for this.

---

## 2. AsyncTCP callback timing

**What it is.** AsyncTCP delivers WebSocket callbacks on its own task,
concurrently with the loop task. The transport enters a quarantine
before releasing the current admission, spins on
`g_wsIngressActive.load() != 0`, then proceeds after
`kIngressDrainTimeoutMs = 750 ms` whether or not the counter reached
zero. This is what `ws_ingress_drain_test.cpp` pins as a policy — but
the ONLY thing the host build sees is the policy shape. It cannot
prove that AsyncTCP actually increments/decrements the counter in the
callback path, and it cannot prove that a real callback ever exceeds
the bound.

**What the host build cannot see.**

- Whether the AsyncTCP callback actually calls the increment (they
  currently do, via a scoped guard, but a refactor could bypass it and
  the drain would silently think the transport is idle).
- Whether the decrement path fires on the error branches of the
  callback, not just the success branch.
- The real distribution of callback latencies. On the ESP32-S3 with
  the current WS traffic, the 99th-percentile is <5 ms; the 750 ms
  bound exists for pathological cases, not the common one. The host
  build cannot tell whether the common case is 5 ms or 500 ms.

**Runtime signal that DOES exist.** The NVS `crash` log (see the
existing crash handler in `main.cpp`) records TASK_WDT resets with the
task name. A drain that hangs past 5 s reboots the loop task with a
recognisable crash trailer. That trailer is the only proof-of-fire
we get today for the whole class of bug the bound was added to
prevent.

**What should be added.** A 1-line stat print when the drain bound
actually fires — currently it just proceeds silently. Making the
bound-fire path noisy would let a real deployment surface a "the
timeout is happening in practice" signal, which is currently
invisible.

---

## 3. Anything requiring a real phone

**What it is.** The full WS lifecycle — admit, drive, revoke, staleness,
GRACE recovery — is a conversation between the firmware and the browser
in `src/override_html.cpp`. The browser has its own state: a `seq`
counter, a `stopLatched` flag, an XR IMU handler, a `<canvas>` telemetry
gauge, a swipe-down QR restore path.

**Cannot be host-tested:**

- **The revocation path.** The firmware sends a close frame and the JS
  handler is expected to teardown and re-request the QR. Whether the JS
  actually does that, and whether the timing matches the firmware's
  quarantine window, is browser + real-radio territory.
- **The 4 s owner-staleness watchdog.** `kWsOwnerSilenceTimeoutMs` and
  the reaper are host-testable; the reaper's *interaction with a phone
  whose radio is dropping frames* is not. A phone in a pocket loses 30 s
  of packets and then reappears; the shape of that recovery has been
  seen in operator captures but has never been reproduced in the host
  suite.
- **GRACE swipe-down QR restore.** The user's escape hatch when
  everything is broken. Requires a touchscreen and a phone camera.

**Runtime signal that DOES exist.** RTC breadcrumbs from
`docs/PROJECT-HISTORY.md`: on reboot the firmware prints the last few
transition tokens it wrote to `RTC.slow_mem` before the crash, so a
watchdog reset in the middle of an admission is inspectable. The
`crash` NVS entry captures the last known-good state alongside the
crash reason. Between the two, an operator with serial capture can
reconstruct "which state did we die in".

**What should be added.** A structured "session started / session
ended" line the phone side echoes back to the firmware over WS so that
the firmware log records BOTH ends of every session. Right now the
firmware log only records its own view; when a phone hangs up
uncleanly, we cannot tell from the log whether it was network, browser,
or user action.

---

## 4. Servo, motion, and drive behaviour on real hardware

**What it is.** `movement_expression`, `drive_calibration`,
`turn_measure`, `spin_calibration` all extract pure numeric policies
from the drive path and pin them. What they do NOT do — and can NEVER
do from a host build — is prove that the robot physically moves the
way the tests describe.

**Cannot be host-tested at all:**

- **A tethered turn is physically invalid.** The USB cable holds the
  robot back; the encoder counts are wrong; the yaw estimate is
  wrong. Any calibration performed tethered is garbage. This means
  the drive-calibration path has to be exercised untethered, and
  untethered means you cannot see serial output live.
- **Reconnecting serial resets the board.** The USB-Serial bridge on
  this board pulses DTR/RTS on open, which the ESP32-S3 boot ROM
  latches as a reset. Every `pio device monitor` invocation is a
  reboot. This makes "walk the robot, then plug in and read logs"
  destructive — the state that produced the interesting log is gone
  by the time you can read it.
- **Servo trim, mood-driven idle motion, gesture playback.** All
  timing-sensitive against real inertia. The suite pins the policy
  numbers (`test/movement_expression_test.cpp`) but the numbers only
  encode what SHOULD be output; the mapping from PWM to actual servo
  angle involves the real load, the real supply voltage, and the
  real gear backlash, none of which the host build has.

**Runtime signal that DOES exist.**

- The panel driver reads back `RDDPM` at boot, which pins whether the
  display initialized. If the panel is dark, `RDDPM` said so.
- `lcdact:` heartbeats every N loop passes: proves the loop task is
  alive.
- The bench-log NVS entries record per-speed distance samples across
  power cycles; `test/bench_log_test.cpp` pins the persistence policy.
  A real drive session leaves a trail that survives the crash.

**What should be added.** A per-drive-session summary line dumped to
NVS at the end of each session (peak yaw error, mean throttle,
number of frames). Would let a post-hoc analysis of untethered
sessions answer the question "did that feel wrong?" from data
rather than memory.

---

## 5. What is host-testable today, cross-referenced

| Concern                                | Where covered                                    |
|:---------------------------------------|:--------------------------------------------------|
| Opcode / constant drift                | `test/test_ws_protocol_drift.py`                  |
| Telemetry frame size (30 B) drift      | `test/test_ws_protocol_drift.py` (Wave 2)         |
| Telemetry byte-offset drift            | `test/test_ws_protocol_drift.py` (Wave 2)         |
| Control/Mood/Gesture/Sound frame size  | `test/test_ws_protocol_drift.py` (Wave 2)         |
| Reaper delta arithmetic (R46)          | `test/ws_owner_reaper_test.cpp`                   |
| Ingress-drain bound (R47)              | `test/ws_ingress_drain_test.cpp` (Wave 2)         |
| Non-owner-frame filter (R45)           | `test/ws_nonowner_filter_test.cpp` (Wave 2)       |
| DriveSession non-owner disconnect      | `test/drive_session_test.cpp` (Wave 2)            |
| DriveSession state machine (main)      | `test/drive_session_test.cpp`                     |
| Serial line buffer                     | `test/serial_line_buffer_test.cpp`                |
| Bench log persistence                  | `test/bench_log_test.cpp`                         |
| Motion event classification            | `test/motion_events_test.cpp`                     |
| Melody bounds & piezo watchdog         | `test/melody_test.cpp`, `test/melody_gen_test.cpp`|
| Gesture-menu geometry & touch rotate   | `test/gesture_menu_test.cpp`                      |
| Display fade math                      | `test/display_transition_test.cpp`                |
| Turn / spin calibration policy         | `test/turn_measure_test.cpp`, `test/spin_calibration_test.cpp` |
| RF map                                 | `test/rf_map_test.cpp`                            |
| Movement expression                    | `test/movement_expression_test.cpp`               |
| Drive calibration policy               | `test/drive_calibration_test.cpp`                 |

Everything else — actual radio, actual servos, actual panel, actual
touch, actual battery — needs the device. That is not a hole; that is
just what the tests are.

---

## A rule for future PRs

Before adding a test for anything in the WS transport, drive path, or
display, check this file. If the region you want to test is on this
list, the answer is one of:

1. Add a runtime breadcrumb / heartbeat / `diag:` line the operator can
   grep for.
2. Add a policy pin like the ones in Waves 1–2, and be honest in the
   PR that it does not prove the runtime behaviour.
3. If neither works, say so in the PR. A test that quietly proves less
   than it appears to is worse than no test at all — that is the whole
   reason six defects shipped this week.
