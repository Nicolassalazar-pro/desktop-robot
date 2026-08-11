## Reliability [RELY]

> Last reviewed: 2026-08-07 | **Revised against hardware evidence: 2026-08-08, 2026-08-11**
> Open: 1 (RELY-6) | Fixed: 6 (RELY-1, RELY-3, RELY-5, RELY-7, RELY-8, and the reaper half of RELY-4) | Partially fixed: 1 (RELY-4 teardown paths — quarantined by design) | Won't fix: 1 (RELY-2)
>
> **2026-08-11 — the loop-task/AsyncTCP defect class is now CLOSED.** RELY-1, RELY-4 and RELY-5 were
> three faces of one bug: the loop task calling into `AsyncWebSocket`. There are no such calls left
> in the firmware. The rule below is now enforced everywhere, not aspirational. RELY-3, RELY-7 and
> RELY-8 were closed the same week — a movement-abort atomic, a `WIFI_OFF`-halt-path documentation
> comment, and a constant-time-compare hardening respectively.
>
> **2026-08-08 — read this before acting on any finding below.**
> A hardware debugging session confirmed some findings, refuted one, and showed that two of the
> *suggested fixes* were actively harmful. Severity was not a reliable guide: the two MEDIUM findings
> (RELY-4, RELY-5) turned out to describe the single most damaging defect on the AP path — heap
> corruption from calling into `AsyncWebSocket` off the AsyncTCP task — while the HIGH-severity
> RELY-2 is not supported by any captured crash.
>
> **The rule the evidence supports, which supersedes the individual fix suggestions below:**
> the `AsyncWebSocket` object belongs to the AsyncTCP task. The loop task publishes state into
> atomics and never touches a client. `AsyncWebSocketClient::_queueControl` does an unsynchronised
> `_controlQueue.add()` (`AsyncWebSocket.cpp:561`), so any `close()`, `closeAll()` or
> `cleanupClients()` from `loop()` races AsyncTCP's `_onData` and corrupts the heap.
> "Defer the close to the loop task" is therefore the wrong instinct, and shipping it as `973a91a`
> crashed the board on the first phone connect.
>
> Scope: `src/main.cpp` (WS/AsyncTCP callbacks, drive session, servo write, override state machine),
> `src/drive_session.cpp`, `src/movement_expression.h`, `src/piezo_tone.cpp`. Read-only review.
>
> Rejected approaches per `docs/PROJECT-HISTORY.md` §3 (R1–R43) were honoured: no finding proposes
> `binaryAll()` (R7/D8), `WiFi.mode(WIFI_OFF)`/`esp_wifi_stop()` in teardown (R6), `writeMicroseconds(0)`
> to stop a CR servo (R10/D11), re-registered routes on bounce (R34), unbounded ingress drain (recent
> bug #2), or SSID/WPA rotation (R8/D6). The bounded 750 ms ingress drain (`kIngressDrainTimeoutMs`,
> `main.cpp:5807`) and the `serialCanWrite()` diag guards are documented deliberate trade-offs and
> are not flagged.

---

### RELY-1 — Non-owner `WS_EVT_DATA` still calls `client->close()` from inside the AsyncTCP callback ("prime suspect for AP restart" per the code's own comment)
- Severity: HIGH
- Status: **FIXED (2026-08-08, commit 118c5a3) — but NOT by the fix proposed below, which was wrong and crashed the board.**
- File: `src/main.cpp:8823-8837`
- Problem: When a WS_EVT_DATA fires for a client that is no longer the transport owner, `orOnWsEvent`
  calls `client->close()` directly on the AsyncTCP task. The code comment right above the call site
  (`main.cpp:8831-8833`) explicitly labels this pattern the "prime suspect for the AP restart" and
  calls it "a classic AsyncWebSocket use-after-free."
- **Confirmed on hardware.** The comment was right. Reproduced by leaving the AP mid-session, which
  kills TCP with no FIN; the stale-owner reaper then revokes ownership, and the next late frame or
  RST for that socket took this branch:

      Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
      AsyncWebSocketClient::_onData (AsyncWebSocket.cpp:639)
        <- _recv <- _s_recv <- _async_service_task

  Reported by the operator as "it reset when I switched networks".
- **Actual fix: drop the frame, do not close at all.** Nothing needed the close. Unauthorized clients
  never reach here (`WsAuthGuardHandler` refuses them at the HTTP upgrade), and rejected clients are
  closed in the CONNECT branch before they are live. What reaches this branch is only a ghost, and the
  right treatment of ghost traffic is to ignore it and let AsyncTCP reap the socket on its own timeout.
- **The fix proposed below was implemented as commit `973a91a` and had to be reverted.** Deferring the
  close to the loop task does not remove the hazard, it converts it into a worse one: a cross-task data
  race on `_controlQueue` (see RELY-4), which corrupted the heap and panicked on the *first* phone
  connect. Recorded here because the reasoning looks correct and is not.
- Consequence for the operator: the app can no longer learn it has been revoked from the socket
  closing, so a telemetry-staleness watchdog was added to the web app (4 s without a telemetry frame
  forces a reconnect). Without it the app shows "live" forever while nothing responds.
- ~~Fix: Do not close from AsyncTCP. Instead, enqueue a "revoke non-owner" `WsEvent` (carrying
  `event.transportToken` and `event.client`) and let the loop task run `closeWsEventClient(event, 1008, "Forbidden")`~~
  — **REJECTED, see above. `closeWsEventClient` is itself unsafe; see RELY-5.**

---

### RELY-2 — `client->close()` calls inside `WS_EVT_CONNECT` retain the INT_WDT crash pattern for non-auth rejection paths
- Severity: HIGH
- File: `src/main.cpp:8679, 8690, 8734, 8756, 8764`
- Problem: The `WsAuthGuardHandler` at `main.cpp:1595` was added specifically because "closing a
  websocket client from inside its own `WS_EVT_CONNECT` callback" crashed the board with
  `INT_WDT`/`TG1WDT_SYS_RST` (PROJECT-HISTORY §recent-bugs #1, and code comment at `main.cpp:9852-9861`).
  That fix eliminates the AUTH path — an unauthed upgrade is 403'd before `AsyncWebSocketClient`
  is ever constructed. But `orOnWsEvent`'s `WS_EVT_CONNECT` branch still calls `client->close()`
  five other times, from inside the same callback:
    - `main.cpp:8679` — belt-and-suspenders auth re-check (racy vs. `invalidateActivationCredentials`).
    - `main.cpp:8690` — quarantined ingress reject.
    - `main.cpp:8734` — owner-busy / activation-faulted / faulted-transport-closing rejection.
    - `main.cpp:8756` — enqueue-failed rollback.
    - `main.cpp:8764` — owner-token/generation TOCTOU rollback.
- Why: The auth guard only prevents ONE reason to reject an upgrade; every other reason still
  produces a real client, then closes it from inside its own connect callback — the exact hazardous
  pattern the fix was designed to eliminate. `main.cpp:8734` fires whenever a second client tries
  to connect while the first still owns the transport, which is a normal user scenario (phone
  reconnects before the reaper has run).
- **2026-08-08 — RE-ASSESSED, and this finding is not supported by evidence. Downgraded, and the
  proposed fix was implemented, tested, and REVERTED.**
  - The suggested change shipped as commit `973a91a` (defer all closes to the loop task). It did not
    fix a crash; it CAUSED one, on the very first phone connect, by converting a claimed re-entrancy
    hazard into a real cross-task data race on `_controlQueue` (see RELY-4).
  - The INT_WDT crash this finding extrapolates from was on the *unauthenticated* connect path, which
    `WsAuthGuardHandler` already eliminated at the HTTP handshake. There is no captured crash for the
    remaining connect-time closes.
  - `WS_EVT_CONNECT` runs on the AsyncTCP task, which is the task that owns the client. Closing a
    just-rejected client there is the library's intended usage: the client is not yet live, and no
    inbound frames are queued against it. That is materially different from RELY-1, where the close
    landed on a client with in-flight data and a dying TCP connection.
- Status: **CLOSED — WON'T FIX.** The closes at `8679/8690/8734/8756/8764` are correct where they are.
- ~~Fix: Same pattern as RELY-1: do not close from `WS_EVT_CONNECT`. Enqueue an "admission
  rejected" event, and let the loop task tear the client down.~~ — **REJECTED. Tried as `973a91a`,
  crashed the board, reverted in `118c5a3`. Do not re-propose; see PROJECT-HISTORY §3.**

---

### RELY-3 — A running gesture is not aborted when the drive session is force-reset to `SafeOff`; wheels keep spinning after a WS transport failure
- Severity: HIGH
- Status: **FIXED (2026-08-11, commit `13511b2`).** A `g_movementAbortPending` atomic is set inside
  `resetDriveControl` (`src/main.cpp:5979`), cleared once handled (`src/main.cpp:6449`), and checked
  at the top of `serviceMovementGesture` (`src/main.cpp:8256`) — exactly the "signal orthogonal to
  `session.state()`" the finding proposed. WS queue-overflow and hardware-trip both flow through the
  same `resetDriveControl`, so all four fault paths named in the finding now cancel a running
  gesture within one loop tick rather than letting it run to the 8 s pattern cap.
- File: `src/main.cpp:8084-8110`; contrast with `main.cpp:9314, 9326, 9332`
- Problem: `serviceMovementGesture` aborts only when the session is in `Active`, `Manual`, or
  `Shutdown` (`main.cpp:8099-8102`). But `resetDriveControl(...)` — invoked on
  `MalformedControl` (`main.cpp:9314`), `ProtocolFailure` (`main.cpp:9326`), `TransportFailure`
  (`main.cpp:9332`), and queue-overflow emergency (`handleWsQueueEmergency`, `main.cpp:8982`) —
  resets the session to `SafeOff` (`DriveSession::reset`, `drive_session.cpp:215-226`). `SafeOff`
  is NOT in the abort set, so a `spin` or `excited` gesture in flight when the WS transport
  fails will keep driving the motors until it finishes on its own.
- Why: The reset code calls `powerDownDriveMotors` and clears the rail, but on the very next
  loop tick `serviceMovementGesture` runs `writeDriveOutputLocked` which cold-starts the rail
  again (100 ms + 25 ms delays inside `g_driveMutex`, `main.cpp:6016, 6034`) and drives the
  gesture step. `writeDriveOutputLocked` also refreshes the 300 ms hardware lease each write,
  so the independent EN cutoff never fires while the gesture is running. Net effect: the safety
  envelope for a WS-triggered gesture is the 8 s pattern cap (`kMaxPatternMs`,
  `movement_expression.h:50`), not "operator lost → stop."
- Manifestation: user triggers `excited`, gesture starts (2 × turn + bounce middle), phone leaves
  WiFi range (no `WS_EVT_DISCONNECT`), WS queue overflows or protocol-faults, `resetDriveControl`
  runs, but the gesture proceeds for up to 8 s including a full-speed spin. The comment at
  `main.cpp:8087-8093` documents that the abort condition was DELIBERATELY narrowed from
  `state() != SafeOff` (which used to cancel every gesture on its first tick because a live
  neutral WS session sits at `ArmedOff`, not `SafeOff`) — but the fix over-corrected. Fault
  resets that force `SafeOff` also fail to cancel.
- Fix: Add an owner-lost / session-reset signal that is orthogonal to `session.state()`. E.g. bump
  a `g_movementAbortToken` inside `resetDriveControl` and in `handleWsQueueEmergency` /
  `handleDriveHardwareTrip`; check it at the top of `serviceMovementGesture` alongside the
  existing `operatorIsDriving` guard. The web app has no way to cancel a gesture mid-flight
  today; that same signal wired to a WS `stop-gesture` opcode is the correct user-visible
  primitive.

---

### RELY-4 — `g_orWs.closeAll()` and `g_orWs.cleanupClients()` are invoked from the loop task while AsyncTCP is concurrently servicing the same object
- Severity: **HIGH (raised from MEDIUM — this was the heap corruptor)**
- Status: **FIXED in the reaper (2026-08-08, commit 118c5a3). Still present in the two teardown
  paths (`stopWifiServer`, `handleApLifecycleFault`), which are one-shot and quarantined.**
- **CONFIRMED ON HARDWARE — no longer "would require correlating a crash".** The review called this
  MEDIUM and hard to attribute. It was in fact the most damaging defect on the AP path, and the
  correlation the finding asked for was captured directly:

      Guru Meditation Error: Core 1 panic'ed (StoreProhibited)
      _async_service_task -> _s_recv -> _recv -> _onData
        -> _queueControl -> LinkedList::add -> operator new
        -> malloc -> tlsf_malloc -> block_locate_free -> remove_free_block

  The fault is inside the allocator's own free-list unlink, i.e. the heap metadata was already
  corrupt and `operator new` was merely the first allocation to touch the poisoned block.
  `close()`/`closeAll()` reach `AsyncWebSocketClient::_queueControl`, which does an unsynchronised
  `_controlQueue.add()` (`AsyncWebSocket.cpp:561`) — the identical call AsyncTCP makes from `_onData`.
- Why the per-tick `cleanupClients()` was the worst part: it ran on EVERY loop pass in `ORS_RUNNING`,
  so the race window was permanently open, not just during a reaper firing.
- **Fix applied: the reaper no longer calls into AsyncWebSocket at all.** `requestWsTransportReset()`
  is pure atomics and already does the whole job — it makes `wsTransportOwnerConnected()` false, which
  is the entire meaning of "release the transport". Both library calls were deleted. A ghost socket
  left open is harmless: it is no longer the owner, and its frames are ignored.
- Note on the suggested fix below (wrap the reaper in ingress quarantine): that would NOT have been
  sufficient. The quarantine drains inbound callbacks, but `_queueControl` is also reachable from
  AsyncTCP's own TCP-level processing; the only safe rule is that the loop task never touches the
  object. See RELY-5 for the remaining instance.
- File: `src/main.cpp:10024, 10124, 10286, 10294`
- Problem: `AsyncWebSocket` is not documented as thread-safe. `orOnWsEvent` runs on the AsyncTCP
  task and may be mid-callback (holding a `WsIngressGuard`, iterating the client list, or
  writing to `client->binary(...)`) at the moment the loop task calls `g_orWs.closeAll(1001,
  ...)` (phone-reaper, `main.cpp:10294`; `stopWifiServer`, `main.cpp:10024`;
  `handleApLifecycleFault`, `main.cpp:10124`) or the per-tick `g_orWs.cleanupClients()`
  (`main.cpp:10286`).
- Why: These calls walk and mutate `AsyncWebSocket`'s internal client list. AsyncTCP simultaneously
  walks and mutates the same list. There is no external mutex around the object. The bounded
  ingress-quarantine (`kIngressDrainTimeoutMs = 750`, `main.cpp:5807`) drains one direction of
  in-flight callbacks before `closeAll` is called in `stopWifiServer`/`handleApLifecycleFault`
  (via `resetDriveControl` → `beginWsIngressQuarantine`), which is exactly the right hardening.
  BUT the phone-reaper at `main.cpp:10286-10298` and the per-tick `cleanupClients()` at
  `main.cpp:10286` do NOT quarantine first — they run while AsyncTCP callbacks may be in flight.
- Manifestation: unexplained AP restarts / kernel panics under load (which is exactly the
  symptom the code at `main.cpp:8831-8833` is currently investigating). Confirmation would
  require correlating a crash with an active `closeAll` or `cleanupClients` window, which
  needs a scope on the breadcrumb stream (`kCrumbWsCleanup` and `kCrumbWsCloseAll` are already
  emitted at `main.cpp:10287, 10293`).
- Fix: Quarantine before `closeAll`. Wrap the phone-reaper at `main.cpp:10286-10298` with
  `beginWsIngressQuarantine()`/`endWsIngressQuarantine()` the same way `stopWifiServer` does.
  `cleanupClients()` is documented in ESPAsyncWebServer as callable from the loop task, but it
  still walks the same list — verify AsyncWebSocket source (or vendor a fork) to confirm. If
  in doubt, gate `cleanupClients()` behind ingress quarantine too; a 750 ms stall once per
  reaper firing is cheaper than an AP restart.

---

### RELY-5 — `closeWsEventClient` looks up a client on the loop task via `g_orWs.client(id)` while AsyncTCP may be adding/removing that client
- Severity: **HIGH (raised from MEDIUM)**
- Status: **FIXED (2026-08-11, commit `fe30172`). Renamed to `revokeWsEventClient`, which only logs.**
- **Resolution.** The close was removed from all 7 loop-task call sites. Two independent reasons make
  that safe, both confirmed by reading the code rather than assumed:
  1. **Authority is already revoked at every call site.** `rejectWsTransportAdmission` clears the
     owner token before calling; the other six fire only when the client is already stale, faulted or
     a non-owner. A revoked client cannot drive, cannot be re-admitted, and its frames are dropped on
     the non-owner path.
  2. **The socket is still closed — from the correct task.** Both telemetry send paths in
     `orOnWsEvent` re-check ownership and close on mismatch (`main.cpp:8982, 9192`), and those run on
     AsyncTCP driven by the client's own next frame. A live phone is dropped within one control tick,
     so recovery does not wait on the 4 s client-side staleness watchdog — that is only the backstop.
- The replacement log line is rate-limited to 1 Hz and gated on `serialCanWrite()`, because a client
  wedged in a revoked state can generate them at control-frame rate and an unbounded `Serial.printf`
  on the loop task is itself RELY-6.
- **There are now no calls into `AsyncWebSocket` from `loop()` anywhere in the firmware.** That closes
  the entire defect class behind this week's crashes (RELY-1, RELY-4, RELY-5).
- Verified: 13/13 host suites; panel `RDDPM=0x9C`; 15 min AP up with a station associating and
  disassociating twice, 0 crashes. **Still unverified: the revocation path itself**, which needs a
  phone to load the web app and then leave the network.
- **2026-08-08 update.** RELY-4 was fixed and the crash it caused was captured in full:
  `close()` reaches `AsyncWebSocketClient::_queueControl`, which does an unsynchronised
  `_controlQueue.add()` (`AsyncWebSocket.cpp:561`, `StringArray.h:76`) — the same call AsyncTCP makes
  from `_onData` on its own task. Two tasks allocating into one unlocked LinkedList wrecks the TLSF
  free list, and the next `malloc` dies in the allocator's own bookkeeping. `closeWsEventClient`
  performs *exactly* this call from the loop task, from 7 sites.
- **The codebase already has the correct pattern, and this function is the outlier.**
  `publishWsTelemetrySnapshot()` deliberately does not send: the loop task publishes into a
  seqlock-protected atomic snapshot and the AsyncTCP task performs the write
  (`copyWsTelemetrySnapshot`). Loop publishes state; AsyncTCP does all client I/O.
  `terminateWsTransportForProtocolFault` likewise closes from the AsyncTCP task, which is correct.
- Why it has not yet been changed: the safety-critical half already happens without touching the
  client — ownership is revoked and `g_wsTransportFaultToken` is latched, so a faulted client cannot
  drive and cannot re-admit. The close is only how the *phone* learns to reconnect. Removing it
  naively reproduces "connected but won't respond" (see RELY-1), so the web-app telemetry-staleness
  watchdog had to land first. It now has, which unblocks this.
- Suggested fix: delete the close from the loop path and let revocation + the client-side 4 s
  staleness watchdog drive recovery. Do NOT "defer" it to another task — that was tried for RELY-1
  as commit `973a91a` and made things strictly worse.
- File: `src/main.cpp:5714-5722`; call sites `main.cpp:5744, 9047, 9054, 9063`
- Problem: `closeWsEventClient` calls `AsyncWebSocket::client(id)` from the loop task, walks the
  internal client list, and then calls `close()`. The pointer-identity check against
  `event.transportToken` (`main.cpp:5718`) is a good defence against ID reuse, but it does not
  protect against a torn read of the list while AsyncTCP is inserting a new client on
  WS_EVT_CONNECT or freeing one on disconnect.
- Why: Every AsyncWebServer/AsyncTCP object is single-owner by design. There is no cross-task
  lock. In practice this is only visible under sustained connect/disconnect churn, and would
  present as an occasional crash inside `AsyncWebSocket::client` or `close` — very hard to
  attribute.
- Fix: Route close through AsyncWebSocket's own API if it offers a thread-safe variant, or hold
  ingress quarantine while walking. A weaker mitigation: capture the client pointer at enqueue
  time and pass it through the event unchanged (the token already does this — the pointer
  lookup step is redundant). But calling any `AsyncWebSocketClient::*` method from the loop
  task while AsyncTCP holds the client is still unsafe by design; the loop-task close should
  be replaced with "post a request to close and let AsyncTCP see it on its next callback."
  Concretely: mark the transport as fault-pending under `g_wsTransportFaultToken` (the machinery
  at `main.cpp:5539-5573` already exists), and let the AsyncTCP-side callback observe the flag
  and close its own client from its own callback — which is the AsyncWebSocket sanctioned pattern.

---

### RELY-6 — Unguarded `Serial.printf` calls inside `orOnWsEvent` and `onWiFiEvent` truncate to a single-byte drop under FIFO pressure
- Severity: LOW
- Status: OPEN
- File: `src/main.cpp:8676, 8727, 9391, 9400, 9405, 9411, 9416, 9426, 9435, 9441` (AsyncTCP + WiFi event tasks)
- Problem: These print sites run on the AsyncTCP task (`orOnWsEvent`) and the WiFi event task
  (`onWiFiEvent`), and they do NOT check `serialCanWrite()` (`main.cpp:1338`) first. With
  `Serial.setTxTimeoutMs(0)` set at `main.cpp:10410`, individual byte writes drop rather than
  block, so this does NOT stall the tasks (unlike the USB CDC blocking bug fixed on the loop
  task per PROJECT-HISTORY §recent-bugs #4).
- Why: The failure mode is truncation, not stall. A `Serial.printf(...)` gets partly emitted
  and stops mid-record. The diagnostic stream — which is the ONLY witness for INT_WDT crashes,
  since `panic_handler` cannot print over USB CDC — becomes ambiguous when a partial line from
  the AsyncTCP task interleaves with a partial line from the loop task. Given the ongoing
  investigation into AP-restart crashes (RELY-1, RELY-2, RELY-4), the value of a clean log
  stream during a fault is high.
- Fix: Wrap AsyncTCP/WiFi-event `Serial.printf` sites with `if (serialCanWrite()) ...` the same
  way the loop-task diag lines already are (`main.cpp:10310, 10695`). Losing a whole record is
  clearer than a garbled half of one.

---

### RELY-7 — `haltAfterDisplayInitFailure` still calls `WiFi.mode(WIFI_OFF)` — the exact pattern R6 rejects
- Severity: LOW
- Status: **FIXED (commit `cceaf47`).** The `WiFi.mode(WIFI_OFF)` at `src/main.cpp:10364` is now
  preceded by a comment block (`src/main.cpp:10358-10363`) that names R6, states the halt-path
  invariant ("Safe here, and ONLY here — the board never brings an AP up again from this function"),
  and asks a future refactor not to unify this call with the teardown path. The finding's proposed
  fix was to replace the call with `WiFi.softAPdisconnect(false)`; the fix that shipped instead
  keeps the call but documents it as the halt-only variant, on the reasoning that a `[[noreturn]]`
  halt is not the teardown case R6 was written about. Documented deliberate divergence.
- File: `src/main.cpp:10357-10385`
- Problem: PROJECT-HISTORY §R6 and the two extensive comments at `main.cpp:9909-9932` and
  `main.cpp:10018-10023` explicitly reject `WiFi.mode(WIFI_OFF)` and `esp_wifi_stop()` in teardown
  because they corrupt the netstack (`E wifi_init_default: netstack cb reg failed with 12308`),
  leaving the AP able to hand out DHCP leases while routing nothing to AsyncWebServer.
  `haltAfterDisplayInitFailure` (`main.cpp:10357-10385`) calls `WiFi.mode(WIFI_OFF)`.
- Why: The function is `[[noreturn]]` and ends in `for(;;) delay(1000);`, so the netstack
  corruption never has a chance to matter — the system halts before anyone would try to talk to
  it. So this is technically safe TODAY. It is a landmine for tomorrow: a future refactor that
  removes `[[noreturn]]` (e.g. "recover from display failure and re-init") reintroduces the
  netif-chain corruption immediately, with no local hint of what broke.
- Fix: Match the pattern in `requestExpectedSoftApStop`: `WiFi.softAPdisconnect(/*wifioff=*/false)`
  (`main.cpp:9931`). Even in a halt path, doing the same shutdown everywhere else does removes
  ambiguity for the next reader.

---

### RELY-8 — Constant-time access-token compare short-circuits on wrong length, leaking length via response timing
- Severity: LOW
- Status: **FIXED (commit `cceaf47`).** `requestHasValidAccessToken` (`src/main.cpp:5480-5524`)
  now always runs the full 16-iteration XOR compare against a fixed-length local copy of the
  expected token, and folds the length mismatch into the accumulated difference at return rather
  than short-circuiting. `secureZero` is called on both the probe and the expected copy before
  return. Matches the finding's proposed fix exactly.
- File: `src/main.cpp:5480-5524`
- Problem: `requestHasValidAccessToken` runs a constant-time XOR compare, but ONLY when the
  supplied token length matches `kAccessTokenHexLength` (`main.cpp:5433-5438`). A wrong-length
  token returns immediately with the loop unexecuted.
- Why: This is a security concern more than a reliability one, but it matters here because the
  reliability model of the whole activation gate is "only the owner phone can drive." An attacker
  observing HTTP response times can distinguish "wrong length" from "wrong content" and iterate
  toward the correct length in fewer probes. Not a wheel-moving hazard, low priority against
  the drive-control findings above.
- Fix: Always run the 16-iteration compare regardless of the supplied length; compare
  `supplied.length() == kAccessTokenHexLength` at the end with the accumulated difference.
