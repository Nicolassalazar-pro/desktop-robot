## Reliability [RELY]

> Last reviewed: 2026-08-07 | Open: 8
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
- Status: OPEN
- File: `src/main.cpp:8823-8837`
- Problem: When a WS_EVT_DATA fires for a client that is no longer the transport owner, `orOnWsEvent`
  calls `client->close()` directly on the AsyncTCP task. The code comment right above the call site
  (`main.cpp:8831-8833`) explicitly labels this pattern the "prime suspect for the AP restart" and
  calls it "a classic AsyncWebSocket use-after-free."
- Why: This is the same class of hazard PROJECT-HISTORY §recent-bugs #1 fixed for `WS_EVT_CONNECT`
  by moving auth rejection to the HTTP handshake (`WsAuthGuardHandler`, `main.cpp:1595`). But
  in-flight WS_EVT_DATA frames from a client that has just lost owner status (revoked by
  `requestWsTransportReset`, `rejectWsTransportAdmission`, or the phone-reaper at `main.cpp:10294`)
  can still land in `orOnWsEvent` before AsyncTCP has finished tearing the client down. Calling
  `close()` on such a client re-enters its own teardown. Observed failure mode elsewhere in this
  codebase is `rst:0x8 (TG1WDT_SYS_RST) reason = INT_WDT`, no backtrace, because the panic
  handler cannot print over USB CDC and the hardware watchdog resets first.
- Fix: Do not close from AsyncTCP. Instead, enqueue a "revoke non-owner" `WsEvent` (carrying
  `event.transportToken` and `event.client`) and let the loop task run `closeWsEventClient(event, 1008, "Forbidden")`,
  which already does the pointer-identity check via `transportToken` (`main.cpp:5716-5720`).
  The loop-task path is what every other rejection already funnels through (see `main.cpp:9047,
  9054, 9063` in `serviceDriveControl`). This one path is the outlier.

---

### RELY-2 — `client->close()` calls inside `WS_EVT_CONNECT` retain the INT_WDT crash pattern for non-auth rejection paths
- Severity: HIGH
- Status: OPEN
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
- Fix: Same pattern as RELY-1: do not close from `WS_EVT_CONNECT`. Enqueue an "admission
  rejected" event, and let the loop task tear the client down. The transport-owner atomics can
  still be released synchronously in the callback (they already are). Only the `close()` call
  needs to be deferred.

---

### RELY-3 — A running gesture is not aborted when the drive session is force-reset to `SafeOff`; wheels keep spinning after a WS transport failure
- Severity: HIGH
- Status: OPEN
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
- Severity: MEDIUM
- Status: OPEN
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
- Severity: MEDIUM
- Status: OPEN
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
- Status: OPEN
- File: `src/main.cpp:10377`
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
- Status: OPEN
- File: `src/main.cpp:5408-5441`
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
