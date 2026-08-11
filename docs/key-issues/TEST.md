## Testing [TEST]
> Last reviewed: 2026-08-07 | **Revised 2026-08-11** | Open: 11
>
> **2026-08-11 count fix:** the header previously read "Open: 10" but the file has always contained
> TEST-1 through TEST-11 (11 findings). No test was added since the review; the count was simply
> wrong. TEST.md is otherwise unchanged — none of the closed reliability/design/perf fixes this
> week added a host test that would flip any TEST-N to FIXED.

### TEST-1 — The WS transport ownership state machine is inlined in main.cpp and untested; three of the four recent bugs live in it
- Severity: CRITICAL
- Status: OPEN
- File: `src/main.cpp:5233-5243`, `src/main.cpp:8620-8940`, `src/main.cpp:10268-10298`; `test/` has no counterpart
- Problem: The ownership admission, disconnect, non-owner-frame, faulted-transport, activation-faulted, enqueue-fail, and silent-owner-reap paths are all glued together inside `orOnWsEvent()` (~320 lines) and `resetDriveControl()` and share ~10 atomics (`g_wsTransportOwnerToken`, `g_wsTransportOwnerGeneration`, `g_wsTransportFaultToken`, `g_wsTransportOpenCount`, ...). Three of the four bugs shipped this week (WS_EVT_CONNECT self-close crash, silent-owner deadlock, non-owner close-during-teardown) are pure state-machine faults in that code, and none of them is covered by the 13 host suites — the same "kept free of `<Arduino.h>` so it can be host-compiled" discipline that saved `DriveSession` / `TurnMeasure` was never applied here.
- Why: This is the single most consequential coverage gap in the project. The bug shape ("callback on AsyncTCP task calls `close()` on its own transport, re-entering teardown") is inherently a lifecycle-graph bug and inherently host-testable — the AsyncTCP dependency is only `client->close()` and `client->id()`, both trivially injectable. The `DriveSession` header (`src/drive_session.h:1-16`) already documents the pattern: "no `<Arduino.h>` here so this state machine can be host-compiled." Nothing in the WS transport requires Arduino headers; the atomics and the token/generation counters are pure logic. Leaving it inline in `main.cpp` means (a) every one of these bugs was found by user-visible symptoms (black screen, "Owner busy", crash), never by a test, and (b) each fix landed with no regression net, so the next AsyncTCP semantic drift (DEP-2) is one bug and one board reset away.
- Fix: Extract a `src/ws_transport.{h,cpp}` with a `WsTransport` class that owns the token/generation/fault atomics behind explicit methods — `AdmitResult tryAdmit(uintptr_t token, uint32_t clientId, uint32_t now)`, `DisconnectResult disconnect(uintptr_t token, uint32_t clientId, uint32_t now)`, `bool isOwner(uintptr_t, uint32_t) const`, `bool shouldReap(uint32_t now, uint32_t lastControlMs) const`, `void faultAndRevoke()`. Move the atomic reads/writes there; leave only the `client->close()` and `Serial.printf` calls in `orOnWsEvent`. Add `test/ws_transport_test.cpp` with the following minimum scenarios (each one exists because a real bug hit it):
    1. `admitTwice_firstWins_secondGetsOwnerBusy` (existing baseline).
    2. `admit_thenEnqueueFails_ownerReleasedAtomically` (bug 1 fix path, `main.cpp:8743-8757`) — if the caller reports "enqueue failed", `tryAdmit` must roll the token back to 0 so the *next* connect can succeed. Assert the state is fully reversed.
    3. `nonOwnerFrame_ownerStateUndisturbed` (bug 4 fix path, `main.cpp:8830-8837`) — the "phone2 sends a frame while phone1 owns" path must return `NotOwner` without touching phone1's generation, `openCount`, or last-telemetry timestamp.
    4. `admit_thenActivationFault_rejectsAndCloses` (`main.cpp:8708-8737`) — after `faultAndRevoke()`, a new admit returns `Rejected(kActivationTerminalFault)` and does not steal ownership.
    5. `shouldReap_boundary` (bug 3) — for `hasOwner=true`, `shouldReap` is false at `now - lastControlMs == kWsOwnerSilenceTimeoutMs - 1` and true at `== kWsOwnerSilenceTimeoutMs`.
    6. `shouldReap_wrapAround` — same as (5) with `lastControlMs = UINT32_MAX - 100`, `now = 200`; the timeout math must survive `millis()` wrap.
    7. `shouldReap_noOwner_isFalse` — the reaper must never fire on an empty transport.
  Wire into `run_motor_host_tests.ps1` after `drive-session`. **This is the single test that would most reduce risk** and is what the review's central question ("what would have caught the four bugs?") asks for.

### TEST-2 — `beginWsIngressQuarantine` bounded-drain policy is pure logic but has no test
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:5797-5819`; nothing in `test/`
- Problem: The unbounded spin that used to deadlock the loop task (bug 2) is now bounded by `kIngressDrainTimeoutMs = 750` (`main.cpp:5241-5243`). The bounding correctness — that the loop returns within the deadline regardless of whether the in-flight callback count ever reaches zero, and that a `millis()` wraparound during the wait doesn't loop forever — is not tested anywhere. Given that this is the exact defence against a TASK_WDT reset, an unnoticed regression to `delay(1); goto`-style logic would silently re-arm the original bug.
- Why: The drain policy is exactly the shape of code the project already tests as pure logic: `TurnController` uses `now()` injection to test rollover convergence in `test/turn_measure_test.cpp` (see `testTimeoutAndTimeWrap` in `test/drive_session_test.cpp` for the pattern). This case is even simpler — the only inputs are `now`, `deadline`, and an "in-flight count fetcher" callable.
- Fix: Extract the loop body into a pure `WsIngressDrainPolicy::drain(nowFn, activeCountFn, deadlineMs) -> {Drained, TimedOut, activeAtExit}`. Add `test/ws_ingress_drain_test.cpp` with:
    1. `activeGoesToZeroBeforeDeadline_returnsDrained`.
    2. `activeStaysNonZero_returnsTimedOutAt750ms` — deterministic count `= 1`, `now` advanced by 10 ms/iteration; assert the loop exits within `[750, 760]` ms of virtual time (not wall time).
    3. `nowWrapsDuringWait_stillTerminates` — `now` starts at `UINT32_MAX - 100`, deadline is 200 ms later, `activeCount=1` forever; loop must exit and mark `TimedOut`.
    4. `deadlineIsExact` — one iteration where `now == deadline` must terminate rather than sleep another 1 ms (currently the code checks `>= 0` so this is already true; pin it).
  Add to `run_motor_host_tests.ps1` alongside `drive-session`.

### TEST-3 — Python contract test `test_telemetry_opcode_frame_and_big_endian_decode_match` is currently RED and nothing runs it
- Severity: HIGH
- Status: OPEN
- File: `test/test_motor_us_contract.py:156-175`, `src/main.cpp:8390-8402`, `test/run_motor_host_tests.ps1` (no pytest invocation), `.github/workflows/` (does not exist)
- Problem: `py -3 -m pytest test/` today reports `1 failed, 204 passed`. The failing assertion is `assertIn("CAL_KEYS.forEach((k,i)=>$(k).value=v.getUint16(10+i*2))", MAIN)` — the JS in `main.cpp` no longer decodes the ten trailing `uint16` calibration fields the C++ side (`orBuildTelemetry`, `main.cpp:8395-8401` region) still writes into every 5 Hz telemetry frame. Nobody knew: the C++ suites are green, `run_motor_host_tests.ps1` never touches pytest, no CI exists (`.github/workflows/` is absent), so a red contract test has been living in the tree unnoticed.
- Why: This is the highest-leverage kind of miss — a test that DID catch the drift, but was routed around because no umbrella runner gates on it. The failure itself is diagnostic: either the JS-side calibration surface was deleted on purpose (in which case the C++ still wastes 20 bytes of every telemetry frame writing values nothing reads, which by itself is worth a fix) or the JS regressed and nobody noticed. Concrete confirmation: `grep -n CAL_KEYS src/main.cpp` returns nothing; `grep -n "getUint16" src/main.cpp` returns only the four early fields, not the ten calibration fields the C++ still emits.
- Fix: Two independent actions.
    1. Add a top-level `test/run_all_host_tests.ps1` that first calls `run_motor_host_tests.ps1` then `py -3 -m pytest test/ -q` and non-zero-exits on either failure. This is a five-line script and immediately makes the current red state visible; without it, every future Python contract test can silently rot the same way.
    2. Resolve `test_telemetry_opcode_frame_and_big_endian_decode_match` deliberately — either update the regex to match the new JS surface (and delete the ten unused trailing `uint16` fields from `orBuildTelemetry` to save 20 bytes/frame × 5 Hz = 100 B/s of TX budget) or restore the JS decode. **Do not weaken the test to make it pass.**

### TEST-4 — Silent-owner reaper policy (bug 3) is untested; only the timeout constant would need to change to re-open the bug
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:5233-5239` (`kWsOwnerSilenceTimeoutMs`), `src/main.cpp:10288-10298` (the reaper site); no test file
- Problem: The bug where a phone that leaves WiFi mid-drive strands the transport forever is fixed by the reaper at `main.cpp:10288-10298`: if `now - g_lastControlEventMs > kWsOwnerSilenceTimeoutMs` and an owner is present, call `closeAll(1001, "Stale")` and reset the drive. This is a pure timer predicate — but a single stray edit that flips `>` to `>=` on the wrong side, or that resets `g_lastControlEventMs` in a codepath that shouldn't (e.g., inside the LCD idle branch that runs every loop iteration), silently re-enables the bug. The property "silence for 8 seconds ⇒ reap" is not asserted anywhere.
- Why: The exact same shape as `testTimeoutAndTimeWrap` in `test/drive_session_test.cpp:121-134`, which pins the 450 ms control timeout and its `millis()` wrap behavior. It works, it's cheap, and it caught bugs before this project ever shipped a physical robot. The silence reaper deserves the same treatment; without it, this bug can recur without a single test flipping red.
- Fix: Covered structurally by the TEST-1 extract (`WsTransport::shouldReap(now, lastControlMs)` case 5-7). If TEST-1 is deferred, at minimum add a Python contract test in `test/test_ws_reaper.py` that pins `kWsOwnerSilenceTimeoutMs` to `8000` in `main.cpp`, pins the comparison operator (`>`, not `>=`), pins that `g_lastControlEventMs` is not written from the LCD idle path, and pins that `resetDriveControl(...)` is what runs when the timeout fires. This is the same regex/grep-over-main.cpp pattern that `test/test_motor_us_contract.py` already uses.

### TEST-5 — 558 `Serial.print*` sites vs. 7 `serialCanWrite()` guards; bug 4 was a discipline bug and nothing enforces the discipline
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:1338-1341` (the predicate), 30+ periodic-loop `Serial.printf` sites; no test file
- Problem: Bug 4 (USB CDC blocks loop task → TASK_WDT reset when the host is not draining) is fixed by `inline bool serialCanWrite(size_t bytes = 200)` at `main.cpp:1338-1341` and by gating every *periodic* chatter site behind it. But `grep -c "Serial\.print" src/main.cpp` reports 558 write sites and `grep -c "serialCanWrite" src/main.cpp` reports 7. The comment (`main.cpp:1295-1337`) is explicit: "routine, periodic chatter is emitted only when a host is actually attached." That discipline is honored today at the seven sites we can see, but there is nothing to catch the eighth one someone adds inside a `if (now >= s_diagNextMs) { s_diagNextMs = now + 1000; ... }` block six months from now — and that is exactly the bug shape that hard-hangs the loop task.
- Why: The comment at `main.cpp:1338` even says "200 bytes covers the longest periodic line (the 1 Hz `diag:` record)". That's a load-bearing implementation detail — the diag line's actual expanded length is not bounded anywhere, so a new field pushing it past 200 bytes silently re-opens the bug by making `serialCanWrite()` reject the message *even when the FIFO does have room*, and then a subsequent unguarded caller will still be free to write the too-long line and hang. This is a fragility that testing can eliminate.
- Fix: Add `test/test_serial_gating.py` (Python, same shape as `test_motor_us_contract.py`) that does two things:
    1. Scans `main.cpp` for every `Serial.printf(` / `Serial.print(` / `Serial.println(` inside a periodic-loop scope (heuristic: the enclosing function body contains `NextMs`, `s_last`, `>= s_diagNextMs`, or `if (now >= .*Ms)` before the write). Assert each such site is preceded by `if (serialCanWrite()` on the same or previous line (or is inside a `serialCanWrite`-guarded block). An allowlist of known-safe non-periodic call sites (boot lines, error handlers, command replies) is maintained inline in the test with justifications, exactly like `test_motor_us_contract.py` does for its regexes.
    2. Extracts every `Serial.printf` format string that IS guarded by `serialCanWrite()`, expands worst-case field widths (`%lu` → 10, `%s` → known constant lengths, `%02X` → 2, etc.) and asserts none exceed 200 bytes. This locks the "200 bytes covers the longest periodic line" comment as a machine-checked invariant.

### TEST-6 — `DriveSession::disconnect(non-owner)` silently slams the true owner back to `WaitNeutral`; no test asserts this and no test would notice if it changed
- Severity: HIGH
- Status: OPEN
- File: `src/drive_session.cpp:47-52`; `test/drive_session_test.cpp:102-119`
- Problem: `DriveSession::disconnect(client)` from a non-owner while the true owner is armed does not just return `NonOwner` — it also executes `state_ = State::WaitNeutral; resetNeutral();` (`drive_session.cpp:47-52`). That means a spurious `WS_EVT_DISCONNECT` from a rejected second client silently disarms the current owner, forcing a full re-arm sequence. The tests never assert this: `testOwnershipAndGeneration` at `drive_session_test.cpp:102-119` calls `session.disconnect(5)` on a session owned by client `4` and checks only the `Decision` return; it does not read back the owner's `state_`. If someone "fixes" what looks like an unused branch, real hardware would see phantom disarms with no failing test.
- Why: This is exactly the kind of subtle mutation the review is asked to surface — the state machine tests are behavior-adequate for the paths they exercise, but they treat a non-owner action as a leaf (returns `NonOwner`) rather than as a full state transition. Whether the current behavior is intentional (defensive: "any transport churn ⇒ re-arm" is a safety property) or accidental (a stray reset that leaked into a non-owner branch) is not obvious from the code alone, and a test would force us to pin the answer.
- Fix: Extend `testOwnershipAndGeneration` and `testFailSafeInputsRequireRearming`:
  ```cpp
  DriveSession session;
  session.connect(10, 0);
  arm(session, 10, 1, 0);                                   // owner in ArmedOff
  auto priorGeneration = session.generation();
  // Non-owner disconnect while owner is armed:
  expect(session.disconnect(11), Action::PowerDown, State::WaitNeutral, Reason::NonOwner);
  assert(session.hasOwner());                               // owner NOT lost
  assert(session.owner() == 10);
  assert(session.generation() == priorGeneration);          // generation NOT bumped
  // Owner's next frame must go through the FULL arm sequence again:
  expect(session.frame(10, 20, 0, 0, 500, true), Action::PowerDown,
         State::WaitNeutral, Reason::NeutralProgress);
  ```
  If the intent is "any transport churn re-arms", write it as a case in the header docstring and pin it here; if the intent is "non-owner is a no-op", change the impl and let this test document what changed.

### TEST-7 — Load-bearing timeout constants (`kWsOwnerSilenceTimeoutMs`, `kIngressDrainTimeoutMs`, `kControlTimeoutMs`, WS opcodes) live only in `main.cpp` with no drift check
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:5239` (`kWsOwnerSilenceTimeoutMs = 8000`), `main.cpp:5243` (`kIngressDrainTimeoutMs = 750`), `main.cpp:5250-5263` (six WS opcodes), `src/drive_session.h:kControlTimeoutMs` (mirrored); `test/run_motor_host_tests.ps1:120-136` has the drift-check pattern but only for gesture-menu constants
- Problem: The runner already carries a proven "constant drift" regex block (`run_motor_host_tests.ps1:120-136`) that fails the build if `kGestureBtnH`, `kGestureBtnTop` etc. drift from the values `gesture_menu_test.cpp` mirrors. The same pattern is not applied to the timers that gate WS-transport safety (`kWsOwnerSilenceTimeoutMs`, `kIngressDrainTimeoutMs`), the drive control timeout, or the six WS opcodes that must match the JS side byte-for-byte (`main.cpp:8363` is the JS constants declaration).
- Why: The four bugs this week were partly bugs of *values*, not just *code*: bug 2 has an implicit "timeout ≪ 5 s watchdog" invariant, bug 3 has an implicit "8 s > radio-stall window but < user attention span" invariant, bugs in the WS opcodes would silently produce `MalformedControl` disconnects on the phone side. Silent value drift is exactly what the gesture-menu pattern was invented to catch, and this is a proven, one-hour, high-signal extension.
- Fix: Extend `run_motor_host_tests.ps1` after the existing gesture-menu block:
  ```powershell
  $expectedWs = @{
      'kWsOwnerSilenceTimeoutMs' = '8000'
      'kIngressDrainTimeoutMs'   = '750'
      'kOpControl'               = '0x01'
      'kOpTelemetry'             = '0x12'
      'kOpTrim'                  = '0x03'
      'kOpMood'                  = '0x04'
      'kOpGesture'               = '0x05'
      'kOpSound'                 = '0x06'
  }
  foreach ($name in $expectedWs.Keys) {
      $pattern = "constexpr\s+\w+\s+$name\s+=\s+$($expectedWs[$name])\s*;"
      if ($mainSrc -notmatch $pattern) { throw "ws constant drift: $name" }
  }
  # Cross-check: JS in main.cpp uses the same numeric values for OP_*.
  if ($mainSrc -notmatch 'OP_CONTROL=0x01,OP_TELEMETRY=0x12,OP_TRIM=0x03,OP_MOOD=0x04,OP_GESTURE=0x05,OP_SOUND=0x06') {
      throw 'ws opcode drift: embedded JS does not match C++ opcodes'
  }
  ```
  This is directly copyable and closes half the risk of a silent constant edit.

### TEST-8 — Embedded web app inside `main.cpp` PROGMEM has no test for byte-code / opcode / handshake matching the C++ side
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:8363-8410` (embedded JS), `src/main.cpp:5249-5263` (C++ opcodes); no dedicated test
- Problem: The web app is inlined as a giant PROGMEM string that (a) parses telemetry using `getUint16(3)` / `getUint8(5)` etc., (b) emits control frames via `setUint8(0, OP_CONTROL)`, (c) declares reconnect/backoff constants, (d) does its own token substitution during HTTP serve. The only test that touches this at all is the failing `test_telemetry_opcode_frame_and_big_endian_decode_match` (TEST-3). Everything else — malformed frame handling, `bufferedAmount>1024` back-pressure, `RECONNECT_MAX_MS`, the reason-string switch in `onclose` (`'Owner busy'`, `'Stale'`, `'Unavailable'`, `'Forbidden'`) — is unlocked.
- Why: The reason strings are directly wired to the C++ `client->close(code, reason)` calls (`main.cpp:8637`, `8734`, `8756`, `10024`, `10124`, `10294`). If a C++ edit changes `"Unavailable"` to `"UnavailableNow"`, the JS `onclose` will fall through to `'RETRYING'` and the user will lose the "AUTH REQUIRED"/"OWNER BUSY"/"UNAVAILABLE"/"STALE" UX with no test flipping red — that is a real regression the reviewers will not catch by hand.
- Fix: Add `test/test_web_ui_contract.py` in the same shape as `test_motor_us_contract.py`. Regex-lock:
    1. `kOpControl=0x01` etc. against the JS `OP_CONTROL=0x01,...` declaration.
    2. Every `client->close(code, "reason")` argument pair in the WS event handler must appear in the JS `onclose` reason-string switch (or be a documented "silently retry" case).
    3. `kTelemetryFrameSize == 30` matches the JS `v.byteLength !== 30` gate.
    4. The reconnect-backoff constants (`RECONNECT_BASE_MS`, `RECONNECT_MAX_MS`) exist and have plausible bounds (base ≥ 100 ms, max ≤ 10 s) — someone once set exponential backoff to 4 s base and made every reconnect feel dead.
  This is a coverage class the project already trusts (motor_us_contract) and it will catch the "reason string drifted" class of bug immediately.

### TEST-9 — Non-owner data frame path has no assertion that owner state is undisturbed, matching TEST-6 pattern
- Severity: MEDIUM
- Status: OPEN
- File: `test/drive_session_test.cpp:88-100`, `src/main.cpp:8830-8837`, and covered structurally by TEST-1
- Problem: `testFailSafeInputsRequireRearming` at `drive_session_test.cpp:92-93` calls `session.frame(11, 6, 1, 1, 130, true)` (non-owner client `11` while owner is `10`) and asserts only the `NonOwner` decision. It never checks that the owner's arming progress (`neutralFrames_`, `lastControlTime_`) or `hasOwner_` was left alone. Same class of gap as TEST-6, different call site.
- Why: The C++ WS path (`main.cpp:8830-8837`) explicitly comments "A non-owner frame. This is the path a rejected second client takes, and it is the prime suspect for the AP restart: close() on a client that is already tearing down is a classic AsyncWebSocket use-after-free." That means non-owner frames are *known* to be a lifecycle-hazardous path; the tests should force the property that they never mutate owner-visible state.
- Fix: Extend the existing test:
  ```cpp
  arm(session, 10, 1, 0);              // owner armed
  const auto stateBefore = session.state();
  const auto genBefore = session.generation();
  expect(session.frame(11, 6, 1, 1, 130, true), Action::PowerDown,
         State::WaitNeutral, Reason::NonOwner);
  assert(session.hasOwner());
  assert(session.owner() == 10);
  assert(session.generation() == genBefore);
  // Owner's next frame must succeed as if the non-owner frame never happened:
  auto d = session.frame(10, 2, 0, 0, 140, true);
  assert(d.action == Action::PowerDown);
  // ...whatever the "still armed" invariant is; pin it explicitly.
  ```
  This is the smallest possible edit that locks in the "non-owner is a no-op on owner state" property, and it plugs directly into TEST-1 when the WS transport is extracted.

### TEST-10 — Behavior-adequacy of the 13 existing suites, plus one nit
- Severity: LOW
- Status: OPEN
- File: entire `test/` tree
- Problem / Why: The review's question "are the 13 existing suites testing behaviour or restating the implementation?" — the honest answer is that most of them test behavior well:
    - `test/turn_measure_test.cpp` (1120 lines) is exemplary — bias, integrator convergence, `micros()` rollover, saturation, iteration convergence, end-to-end pattern build. **This is the model the WS transport tests in TEST-1 should follow.**
    - `test/melody_gen_test.cpp` is invariant-based on purpose (band/mask/terminal/leaps/gate/gap), documented explicitly (`melody_gen_test.cpp:1-9`) as NOT bit-exact against Python because PCG32 vs MT19937 is a deliberate divergence (`melody_gen.h:20-24`). Do not "fix" this by demanding bit-equality; the invariants are the contract.
    - `test/movement_expression_test.cpp` asserts net-zero displacement and step ≤ `kMaxStepMs < 300ms` (the drive lease bound). These are the two properties that matter; the test correctly refuses to overspecify.
    - `test/drive_calibration_test.cpp` covers all four failure modes noted in `drive_calibration.h` (interrupted commit, divergent equal generation, half-range ambiguity, corrupt-slot fallback), including a legacy-key erasure test.
    - `test/motion_events_test.cpp` asserts what actually matters — "still robot never claims bump", "stationary knock is not a bump", "bump vs pickup are told apart" — not the specific `_lifted_` field.
  
  The two suites that lean the most towards restating the implementation are `test/drive_session_test.cpp` (TEST-6 / TEST-9 above — leaf-return-only assertions) and `test/gesture_menu_test.cpp`, which mirrors constants that live in `main.cpp` and would pass identically if half the button geometry were deleted — that's fine because the runner's drift regex (`run_motor_host_tests.ps1:120-136`) is what actually pins the constants.
- Fix: No structural change to the 13 suites is warranted. Do the surgical extensions in TEST-6 and TEST-9. Keep the invariant-based style of `melody_gen_test.cpp` and `turn_measure_test.cpp` as the house pattern for future host tests.

### TEST-11 — What is genuinely NOT host-testable and should be made observable at runtime instead
- Severity: MEDIUM
- Status: OPEN
- File: bench-side observability across `main.cpp`, `src/bench_log.cpp`
- Problem: The hardware constraint ("tethered turn is physically invalid", PROJECT-HISTORY §6) means several things that could be tested on some other platform simply cannot be here:
    1. Real-world `AsyncWebSocket` teardown timing under actual TCP loss (phones leaving Wi-Fi) — the test doubles in TEST-1 can pin the state machine, but the "does AsyncTCP actually deliver a `WS_EVT_DISCONNECT` in these cases" question is empirically-answered only.
    2. Servo microsecond → wheel-degree calibration under load, thermal drift, battery sag — the `test_motor_us_contract.py` pins the encoding, but the ground-truth mapping lives in `BenchLog::Store` and is measured with the physical robot.
    3. USB CDC FIFO fill/drain race with `TASK_WDT` — the actual FIFO depth is board-firmware-defined and only reproducible on the real chip.
    4. Interactions between AsyncTCP task, LCD SPI push, ledc PWM, and the loop watchdog — timing is emergent.
- Why: Rather than pretend these are host-testable, the discipline the project already uses for `BenchLog` (persist bench measurements NOW because reconnecting Serial resets the board — `test/bench_log_test.cpp:1-7`) should be extended: make the untestable *observable* at runtime and persist observations across resets. Concretely, the "45 seconds of ws-owner silence" fact that led to bug 3 was only discovered because someone happened to be watching the diag line at the right moment; a small ring-buffered "recent WS lifecycle events" record persisted through NVS would have made this diagnosable in seconds.
- Fix:
    1. Add a `WsLifecycleLog::note(EventKind, uint32_t detail, uint32_t nowMs)` ring buffer (size ~32 entries, NVS-backed like `BenchLog`) that records: admits, rejects (with reason enum), disconnects (with token+client), reap-events, ingress-drain-timeouts, and enqueue-fails. Expose via serial `wslog` command. Testable on host: it's pure logic + NVS.
    2. Add a persistent `TaskWdtCounters` blob that increments on each reset kind (WDT, panic, brown-out) — the crash-reason block at `main.cpp:3678-3687` is already printing them; persist a count. Testable on host with `FakeNvs`.
    3. Extend the 1 Hz `diag:` line (`main.cpp:10311`, guarded by `serialCanWrite`) with the current WS transport age (`now - g_lastControlEventMs`) so that reap timing is visible before it fires. Under 200 bytes budget (TEST-5).
  These are runtime observability additions; they exist because the host test suite genuinely cannot cover them and the project's existing NVS-persistence-of-diagnostic-data pattern already proves the recipe works.

---

### Review summary — for the parent prompt

**Which of the four recent bugs were host-testable?**
- **Bug 1 (WS_EVT_CONNECT self-close crash):** YES. Pure lifecycle state machine; TEST-1 case (2) would have caught it before the board first crashed.
- **Bug 2 (unbounded ingress spin-wait):** YES. Pure timeout policy; TEST-2 case (2) would have caught it.
- **Bug 3 (phone leaves Wi-Fi with no FIN):** YES. Pure timer predicate; TEST-1 case (5)/(6) or TEST-4 would have caught it.
- **Bug 4 (USB CDC write blocks loop task):** PARTIALLY. The `serialCanWrite()` predicate itself is trivially testable (TEST-5 second part). The *discipline* (every periodic chatter call site guarded) is testable by grep-based lint (TEST-5 first part). The absolute FIFO fill/drain race with `TASK_WDT` on real hardware is not host-testable and needs runtime observability instead (TEST-11).

**Findings by severity:** 1 CRITICAL (TEST-1), 5 HIGH (TEST-2..6), 3 MEDIUM (TEST-7, 8, 11), 1 MEDIUM (TEST-9), 1 LOW (TEST-10).

**Single test that would most reduce risk:** TEST-1 — extract `src/ws_transport.{h,cpp}` from `main.cpp` and add `test/ws_transport_test.cpp` with the seven scenarios listed. It would have caught three of the four recent bugs and closes the largest un-tested surface in the project (~320 lines of atomic-manipulating lifecycle code with zero coverage), applying the same "keep it free of `<Arduino.h>` so it can be host-compiled" discipline that already made `DriveSession` and `TurnMeasure` shippable.

**Hardware boundary:** the tests proposed here explicitly do not attempt to actuate the servos or use `<Arduino.h>` — the "no autonomous actuation" and "tethered turn is physically invalid" rules from PROJECT-HISTORY §6 are respected. TEST-11 addresses the residual "cannot be host-tested" cases by moving them into runtime observability rather than pretending they're testable.
