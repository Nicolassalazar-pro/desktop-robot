# Key issues — at-a-glance index

> Consolidated view of all review findings across the eight domain files.
> Generated 2026-08-11 by hand from a full pass over `src/` and the git log.
> If the row below and the corresponding domain file disagree, **the domain file is authoritative** — this file is a summary.

## Summary counts

| Domain | Open | Fixed | Partially fixed | Won't fix | Informational / Verified | Total |
|---|---:|---:|---:|---:|---:|---:|
| DEP  | 0  | 6 | 2 | 0 | 0 | 8  |
| DOCS | 13 | 5 | 2 | 0 | 0 | 20 |
| DSGN | 10 | 4 | 0 | 0 | 0 | 14 |
| PERF | 5  | 1 | 0 | 0 | 3 (PERF-7, PERF-8, PERF-9) | 9  |
| READ | 8  | 4 | 0 | 0 | 0 | 12 |
| RELY | 1  | 5 | 1 | 1 | 0 | 8  |
| SEC  | 2  | 0 | 0 | 0 | 0 | 2  |
| TEST | 11 | 0 | 0 | 0 | 0 | 11 |
| **Total** | **50** | **25** | **5** | **1** | **3** | **84** |

Six weeks ago a raw scan reported ~56 items OPEN; today ~50 are genuinely open. The gap was stale status metadata, not stale work — the fixes below existed, they just weren't recorded on the findings.

## Read this before you close another RELY finding

The single most important architectural rule discovered this week:

> **The `AsyncWebSocket` object belongs to the AsyncTCP task.** The loop task publishes state into atomics and never touches a client. `AsyncWebSocketClient::_queueControl` does an unsynchronised `_controlQueue.add()`, so any `close()` / `closeAll()` / `cleanupClients()` from `loop()` corrupts the heap — this was the actual mechanism behind the "random reboot on Wi-Fi teardown" that had been mis-attributed for months.

If a proposed fix asks the loop task to close, disconnect, cleanup, or otherwise manipulate a WebSocket client, the fix is on the `PROJECT-HISTORY.md` §3 rejected-approaches list (specifically **R44**, which was tried as commit `973a91a`, crashed the board on the first phone connect, and was reverted in `118c5a3`). See `docs/PROJECT-HISTORY.md` §3 R44–R47 for the full set of rejected approaches added this week.

## All findings, one row each

| Domain | ID | Sev | Status | Commit | One-line fix (or why open) |
|---|---|---|---|---|---|
| DEP  | 1  | HIGH     | FIXED           | `3b53db9` | All `lib_deps` pinned exact `@X.Y.Z`; caret/tilde/`*` removed. |
| DEP  | 2  | HIGH     | FIXED           | `3b53db9` | 20-line lockfile-substitute rationale block in `platformio.ini`. |
| DEP  | 3  | MEDIUM   | FIXED           | `3b53db9` | Same exact-pin block covers PSRAM-dependent versions. |
| DEP  | 4  | MEDIUM   | FIXED           | `3b53db9` | Version bump/downgrade paths documented alongside the pins. |
| DEP  | 5  | MEDIUM   | PARTIALLY FIXED | –          | Working-tree `tools/LICENSING-NOTES.md` is untracked; not `THIRD_PARTY_LICENSES.md` at root as the finding proposed. |
| DEP  | 6  | MEDIUM   | FIXED           | `3b53db9` | `lib/EyeEngine/library.json` deps also exact-pinned. |
| DEP  | 7  | LOW      | FIXED           | `3b53db9` | Duplicated rationale kept in one canonical place. |
| DEP  | 8  | LOW      | PARTIALLY FIXED | –          | Working-tree `tools/requirements.txt` is untracked; venv note in README is committed. |
| DOCS | 1  | CRITICAL | PARTIALLY FIXED | `cceaf47` | README safety block done; `printHelp` untethered-only annotations still missing. |
| DOCS | 2  | CRITICAL | FIXED           | `cceaf47` | `printHelp` now enumerates all 48 commands in grouped sections. |
| DOCS | 3  | CRITICAL | FIXED           | `cceaf47` | README mood list rewritten to all 16 canonical moods. |
| DOCS | 4  | HIGH     | OPEN            | –          | `PROJECT-HISTORY.md` §D4/§E9 still list moods that don't exist in code (NEUTRAL/DIZZY/…). |
| DOCS | 5  | HIGH     | OPEN            | –          | `PROJECT-HISTORY.md:951` "Free GPIOs" row still contradicts pin usage. `cceaf47`'s message claimed this, but the annotation it added was to servo-pin rows, not the Free-GPIOs row. |
| DOCS | 6  | HIGH     | FIXED           | `cceaf47` | `docs/README.md` created as entry-point index. |
| DOCS | 7  | HIGH     | OPEN            | –          | README "Layout" section still omits `docs/`, `src/override_html.cpp`, `IMU-RESEARCH.md`, `overrides/tools/`. |
| DOCS | 8  | HIGH     | OPEN            | –          | README software-tests block still runs sibling calibrator commands without saying so. |
| DOCS | 9  | HIGH     | OPEN            | –          | README says test button is gone, but `printHelp` still lists `test`. |
| DOCS | 10 | HIGH     | OPEN            | –          | `mood`/`look` aliases still not listed in `printHelp`. |
| DOCS | 11 | MEDIUM   | OPEN            | –          | `HARDWARE.md:552` power-button sample still uses 2 s; real threshold is 5 s. |
| DOCS | 12 | MEDIUM   | OPEN            | –          | `HARDWARE.md` `pin17`/`pin18`/`pair` still not prefaced as sibling-repo commands. |
| DOCS | 13 | MEDIUM   | FIXED           | `cceaf47` | Venv-only pyserial note added to README build-and-flash section. |
| DOCS | 14 | MEDIUM   | OPEN            | –          | `rfscan` vs `rfsurvey` still not disambiguated. |
| DOCS | 15 | MEDIUM   | OPEN            | –          | `sound` now in `printHelp` but still absent from README CLI table. |
| DOCS | 16 | MEDIUM   | PARTIALLY FIXED | `cceaf47` | `wifi on\|off\|status` and `swipe up\|down` in `printHelp`; README table still missing them. |
| DOCS | 17 | MEDIUM   | OPEN            | –          | `PROJECT-HISTORY.md` §E24 state-machine names still don't match `OverrideState`. |
| DOCS | 18 | LOW      | OPEN            | –          | `LcdMode` vs `OverrideState` still used interchangeably. |
| DOCS | 19 | LOW      | OPEN            | –          | Repo owner line still not in `README.md`. |
| DOCS | 20 | LOW      | FIXED           | `cceaf47` | `IMU-RESEARCH.md` linked from README documentation list. |
| DSGN | 1  | HIGH     | FIXED           | `d280194` | STOP is now a latched mode (`stopLatched` flag, 11 refs). |
| DSGN | 2  | HIGH     | FIXED           | `d280194` | STOP button gets `.danger` CSS class consuming `--danger`. |
| DSGN | 3  | HIGH     | OPEN            | –          | Still open (see DSGN.md for details). |
| DSGN | 4  | MEDIUM   | OPEN            | –          | Still open. |
| DSGN | 5  | MEDIUM   | OPEN            | –          | Still open. |
| DSGN | 6  | MEDIUM   | OPEN            | –          | Still open. |
| DSGN | 7  | MEDIUM   | FIXED           | `d280194` | STOP latching also fixes the "phantom drive-after-STOP" edge. |
| DSGN | 8  | MEDIUM   | FIXED           | `d280194` | Dead `#hint` block removed as part of the same UI cleanup. |
| DSGN | 9  | MEDIUM   | OPEN            | –          | Still open. |
| DSGN | 10 | LOW      | OPEN            | –          | Still open. |
| DSGN | 11 | LOW      | OPEN            | –          | Still open. |
| DSGN | 12 | LOW      | OPEN            | –          | Still open. |
| DSGN | 13 | LOW      | OPEN            | –          | Still open. |
| DSGN | 14 | LOW      | OPEN            | –          | Still open. |
| PERF | 1  | CRITICAL | FIXED           | `9fc63c8` | `g_eyeRegionBackup` now lazy `ps_malloc` — 45 KB internal RAM freed. |
| PERF | 2  | HIGH     | OPEN            | –          | See PERF.md. |
| PERF | 3  | HIGH     | OPEN            | –          | See PERF.md. |
| PERF | 4  | MEDIUM   | OPEN            | –          | See PERF.md. |
| PERF | 5  | MEDIUM   | OPEN            | –          | See PERF.md. |
| PERF | 6  | MEDIUM   | OPEN (latent)   | –          | Latent — not user-observable in the current config. |
| PERF | 7  | LOW      | Informational   | –          | Documented cost, not a defect. |
| PERF | 8  | LOW      | Verified        | –          | Checked and confirmed within budget. |
| PERF | 9  | LOW      | Informational   | –          | Documented bound, not a defect. |
| READ | 1  | HIGH     | FIXED           | `11d95a6` | Misleading `[DEBUG-DELETE]` markers scrubbed. |
| READ | 2  | HIGH     | FIXED           | `cceaf47` | Command-surface documentation aligns `printHelp` with dispatcher. |
| READ | 3  | MEDIUM   | FIXED           | `11d95a6` | Misplaced comment relocated to the block it describes. |
| READ | 4  | MEDIUM   | FIXED           | `11d95a6` | Inlined web app extracted to `src/override_html.cpp`; byte-hash and `node --check` verified. |
| READ | 5  | MEDIUM   | OPEN            | –          | Table-dispatch `dispatchCommand` refactor deliberately deferred — mid-verification, not zero-risk. |
| READ | 6  | MEDIUM   | OPEN            | –          | See READ.md. |
| READ | 7  | MEDIUM   | OPEN            | –          | See READ.md. |
| READ | 8  | LOW      | OPEN            | –          | See READ.md. |
| READ | 9  | LOW      | OPEN            | –          | See READ.md. |
| READ | 10 | LOW      | OPEN            | –          | See READ.md. |
| READ | 11 | LOW      | OPEN            | –          | See READ.md. |
| READ | 12 | LOW      | OPEN (deferred) | –          | Phases C–E deferred pending owner approval and untethered re-test. |
| RELY | 1  | HIGH     | FIXED           | `118c5a3` | AsyncTCP-task-only close discipline; the loop task never touches a client. |
| RELY | 2  | HIGH     | WON'T FIX       | –          | Original proposal was R44 (defer close to loop task); shipped as `973a91a`, crashed the board, reverted. Remaining connect-time closes at `WS_EVT_CONNECT` are correct where they are — same task owns the client, no in-flight frames. |
| RELY | 3  | HIGH     | FIXED           | `13511b2` | `g_movementAbortPending` atomic cancels a running gesture when `resetDriveControl` fires. |
| RELY | 4  | HIGH¹   | FIXED (reaper) + PARTIAL (teardown) | `118c5a3` | Reaper half fully on the AsyncTCP task. Teardown paths quarantined by design; documented, not a defect. ¹ raised from MEDIUM — this was the heap corruptor. |
| RELY | 5  | HIGH¹   | FIXED           | `fe30172` | `revokeWsEventClient` renamed and reduced to logging only — no cross-task client mutation. ¹ raised from MEDIUM — see RELY-4 rationale. |
| RELY | 6  | MEDIUM   | OPEN            | –          | AsyncTCP/WiFi-event `Serial.printf` sites not yet guarded by `serialCanWrite()`; can garble a diag line under fault. |
| RELY | 7  | MEDIUM   | FIXED           | `cceaf47` | Fix **differs from proposal**: retained `WiFi.mode(WIFI_OFF)`, added a comment block documenting it as deliberately safe in the halt path only. Proposal was to swap to `softAPdisconnect(false)`. |
| RELY | 8  | LOW      | FIXED           | `cceaf47` | Full-expected-length constant-time XOR compare — no early-out. |
| SEC  | 1  | –        | OPEN            | –          | See SEC.md (`Hanger OVERRIDE` design accepts fixed WPA2 — rotation explicitly rejected R8/R23/R28). |
| SEC  | 2  | –        | OPEN            | –          | See SEC.md. |
| TEST | 1  | CRITICAL | OPEN            | –          | WS transport ownership state machine inlined in `main.cpp`, still untested. |
| TEST | 2  | CRITICAL | OPEN            | –          | See TEST.md. |
| TEST | 3  | HIGH     | OPEN            | –          | See TEST.md. |
| TEST | 4  | HIGH     | OPEN            | –          | See TEST.md. |
| TEST | 5  | HIGH     | OPEN            | –          | See TEST.md. |
| TEST | 6  | MEDIUM   | OPEN            | –          | See TEST.md. |
| TEST | 7  | MEDIUM   | OPEN            | –          | See TEST.md. |
| TEST | 8  | MEDIUM   | OPEN            | –          | See TEST.md. |
| TEST | 9  | LOW      | OPEN            | –          | See TEST.md. |
| TEST | 10 | LOW      | OPEN            | –          | See TEST.md. |
| TEST | 11 | LOW      | OPEN            | –          | See TEST.md. |

## Findings whose fix differed from the proposal

- **RELY-7** — Proposal was to replace `WiFi.mode(WIFI_OFF)` with `softAPdisconnect(false)` in the halt path. The actual fix (`cceaf47`) kept `WIFI_OFF` and added a comment block explaining that it is deliberately safe in that specific path. The finding is closed on the intent, not on the mechanism the finding proposed.
- **RELY-4** severity — Filed MEDIUM ("hard to attribute"). Verified this week to be the single most damaging defect (heap corruption on WiFi teardown). Raised to HIGH.
- **RELY-5** severity — Same class as RELY-4, raised to HIGH.
- **DEP-5** — Proposal was `THIRD_PARTY_LICENSES.md` at repo root. Working tree carries `tools/LICENSING-NOTES.md` (different name, different location, still uncommitted). Kept as PARTIALLY FIXED rather than silently closed.

## Findings that were incorrect about the code

- **RELY-2** — Filed HIGH. Not supported by any captured crash. Its proposed fix (defer close to loop task) was shipped as `973a91a`, crashed the board on the first phone connect (converting a claimed re-entrancy hazard into a real cross-task data race on `_controlQueue`), and was reverted in `118c5a3`. Now marked WON'T FIX with the reason inline.
- **DOCS-5** commit-message claim — `cceaf47`'s message named DOCS-5 as fixed. Verification shows the annotation it added was to the servo-pin rows (also a useful change), but the "Free GPIOs" row that DOCS-5 actually flagged (`PROJECT-HISTORY.md:951` lists 1, 3, 17, 18 as free while the rows immediately above use them) is unchanged. DOCS-5 remains OPEN.
- **RELY-header** (pre-revision) — Read "Open: 3 (RELY-6, the teardown half of RELY-4, and the demo-only note)"; "the demo-only note" is not a numbered finding — a stale reference to a note that no longer exists. Corrected.

## Findings that propose an approach on the PROJECT-HISTORY §3 rejected list

- **RELY-1** original proposed fix and **RELY-2** original proposed fix both correspond to **R44** (defer WebSocket `close()` from the AsyncTCP task to the loop task). This was attempted as commit `973a91a`, crashed the board, and was reverted in `118c5a3`. RELY-1 is closed via a different mechanism (AsyncTCP-task-only close discipline; the loop task never touches a client at all). RELY-2 is closed WON'T FIX.
- No other findings across the eight files propose an approach on R1–R47.

## Anything I could not determine

- Nothing significant. Every finding's true status was resolvable from the code plus the git log. The only entries with unresolved edges are the two DEP working-tree items (DEP-5 licensing file, DEP-8 requirements file) which exist in the working tree but are untracked — a commit action, not an investigation gap. They are marked PARTIALLY FIXED with that note inline.

## How to update this file

If you close a finding:
1. Update the `- Status:` line in the corresponding domain file first — that is the source of truth.
2. Update the header count block at the top of that domain file.
3. Update the row in this index and the summary-counts table at the top.

The scan that produces the summary counts is a plain regex; each finding must have exactly one line matching `^\s*[-*]\s*Status:` under its `### <ID>` heading. The eight PROTECTED-A..G markers in `DOCS.md` are deliberately not findings and carry no `- Status:` line.
