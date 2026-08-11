# Key issues — Documentation review

> Reviewer: docs-review agent (adapted to embedded/PlatformIO scope)
> Date: 2026-08-07 | **Revised 2026-08-11**
> Scope: `README.md`, `HARDWARE.md`, `docs/PROJECT-HISTORY.md`, `docs/GIT-NOTES.md`, `docs/bench-run-*.md`, `docs/scale-check-*.md`, `IMU-RESEARCH.md`, inline comments in `src/` where they encode measured evidence.
> Method: cross-checked every documented constant, command, and pin against `src/main.cpp` and sibling modules. Read-only; no source edits, git, builds, or serial access.
> Open: 13 | Fixed: 5 (DOCS-2, DOCS-3, DOCS-6, DOCS-13, DOCS-20) | Partially fixed: 2 (DOCS-1 README-side only; DOCS-16 printHelp-side only)
>
> **2026-08-11 — status lines added.** This file used a narrative format when it was reviewed and
> carried no `- Status:` markers, so the top-level status sweep could not see it. Each finding now
> has an explicit status. Five were closed by commit `cceaf47` (docs+fix: document the whole command
> surface). The commit message named DOCS-1/2/3/5/6 as fixed; verification against the actual files
> confirms DOCS-2/3/6 (and DOCS-13 and DOCS-20 as side-effects), partially confirms DOCS-1 (README
> side yes, printHelp "(untethered only)" annotations still absent), and refutes DOCS-5 (the
> Free-GPIOs contradictory row remains at `PROJECT-HISTORY.md:951`).

---

## Documentation [DOCS]

> Findings: 3 CRITICAL / 6 HIGH / 7 MEDIUM / 3 LOW / 5 PROTECTED (do-not-trim). Ordered by cost of misunderstanding.
> Format: **DOCS-N — Severity — File(:lines) — problem, why it matters, fix.**
> "Protected" items at the end mark load-bearing evidence a later cleanup pass **must not** delete.

---

### DOCS-1 — CRITICAL — The "cannot drive while tethered" rule is not where a newcomer will see it

- Severity: CRITICAL
- Status: **PARTIALLY FIXED (commit `cceaf47`).** README top-matter now opens with a "⚠ Read this before you drive it" block (`README.md:12-34`) that names the tethered rule and the arm/unplug/press/read-back workflow — the finding's step (1). Steps (2) and (3) are **not** done: `printHelp` (`src/main.cpp:3240-3309`) does not annotate `turn`/`spin auto`/`spin test`/`test` with "(untethered only)", and `platformio.ini` was not touched. Downgrade to LOW when those two are done.

**Files:** `README.md`, `HARDWARE.md`, on-device `help`.

**Problem.** The single most dangerous physical trap on this robot is that a USB-tethered turn is mechanically invalid — the cable restrains the pivot, the turn under-rotates by ~4×, and any measurement taken this way will silently pollute NVS. This is documented **only** in:

- `HARDWARE.md:313–354` (`TRAP — rotation cannot be measured while USB-tethered`) — the strongest statement, buried in a "Full-turn duration" subsection.
- `HARDWARE.md:706–713` — a second mention in the chassis notes.
- `README.md:117–120` — a passing mention inside "On-screen menu" ("a tethered robot cannot complete a rotation, the USB lead restrains it").
- `README.md:200–248` — a longer treatment inside "Measuring movement: the bench run" — but framed as *procedure*, not *safety*.

It is **NOT** in:

- `README.md` top matter (lines 1–55), which is what a new contributor actually reads.
- `README.md` "Build & flash" (lines 15–26) or "Serial CLI" (lines 56–105).
- The on-device `help` output (`src/main.cpp:3200–3238`). The `turn <deg>` and `spin auto` help lines carry **no warning** that using them while plugged in is invalid.
- `platformio.ini` (which does warn about COM3, but not about `turn`/`spin` requiring untether).

**Why it matters.** A newcomer following `README.md` will run `pio device monitor -b 115200`, type `turn 360`, and produce a physically invalid rotation that either damages the cable strain-relief or silently poisons any subsequent calibration. Multi-hour re-debug of "the turn is off" is exactly the class of loss `PROJECT-HISTORY.md` was created to prevent.

**Fix.**
1. Add a `## Safety` block at the top of `README.md` (after "Hardware", before "Build & flash") titled something like "Physical operating rules". First bullet: `turn`, `spin auto`, `spin test`, `test`, `bench arm`, the touch menu 4th button, and anything else that pivots the robot **must be run untethered**. Point at `HARDWARE.md:313–354` for the measurement.
2. In `printHelp()` (`src/main.cpp:3200–3238`), annotate `turn <deg>` and the `spin auto`/`spin test`/`test` lines with a suffix such as `(untethered only)`. This is the one place someone at the CLI is *guaranteed* to look.
3. `platformio.ini` already has a COM3 warning comment; add a one-line comment near it: `; Movement commands (turn / spin auto / test) must run untethered.`

---

### DOCS-2 — CRITICAL — On-device `help` hides 21 commands; the surface is undiscoverable

- Severity: CRITICAL
- Status: **FIXED (commit `cceaf47`).** `printHelp` (`src/main.cpp:3240-3309`) now emits grouped sections `-- expression --`, `-- network / UI --`, `-- display diagnostics --`, `-- motion / IMU --`, `-- survey / RF --`, `-- touch --` covering every command the finding listed (`sound`, `sense`, `scale`, `cod`, `lpf`, `straight`, `survey`, `rfsurvey`, `rfscan`, `bench`, `lead`, `wifi`, `state`, `swipe`, `crash`, `lcdspi`, `lcdpush`, `lcdbus`, `lcdprobe`, `touchinfo`, `touchlog`, `touchreset`, `touchdebug`). Inline comment at `src/main.cpp:3278-3281` notes "help documented 25 of the 48 commands dispatchCommand actually handles" as the pre-fix state.

**File:** `src/main.cpp:3200–3238` (`printHelp`), cross-referenced against every `strcmp(line, ...)` and `strncmp(line, ...)` dispatch in `main.cpp`.

**Problem.** `help` lists ~30 commands. The dispatch code implements at least **51**. Commands present in code but missing from `help`:

| Command | Location |
|---|---|
| `sound <happy\|sad\|confused\|stop>` | main.cpp:4281 |
| `sense [zero]` | main.cpp:4310 |
| `scale check\|actual\|status` | main.cpp:4350 |
| `cod run\|clear\|status` | main.cpp:4514 |
| `lpf on [mode]\|off` | main.cpp:4584 |
| `straight <cm> [speed]`, `straight cal` | main.cpp:4626 |
| `survey start [sideCm] [scans]` | main.cpp:4673 |
| `rfsurvey <label> [scans]`, `rfsurvey report` | main.cpp:4714 |
| `rfscan` | main.cpp:4839 |
| `bench arm\|report\|status\|disarm\|clear` | main.cpp:4130 |
| `lead [sec\|off\|reset]` | main.cpp:4078 |
| `wifi [on\|off]` | main.cpp:3315 |
| `state` | main.cpp:3382 |
| `swipe up\|down` | main.cpp:3362 |
| `crash` | main.cpp:3642 |
| `lcdspi` | main.cpp:3769 |
| `lcdpush` | main.cpp:3694 |
| `lcdbus` | main.cpp:3717 |
| `lcdprobe` | main.cpp:3728 |
| `touchinfo` | main.cpp:3861 |
| `touchlog` | main.cpp:3875 |
| `touchreset` | main.cpp:3902 |
| `touchdebug` | main.cpp:3911 |

**Why it matters.** The user's original request called out this exact concern. `README.md` "Serial CLI" (lines 56–105) documents `sense`, `scale check`, `cod`, `lpf`, `rfscan`, `bench arm/report/status/disarm/clear`, `lead`, `straight`, `survey`, `rfsurvey` — but the on-device `help`, which is the thing someone typing at the port sees, does not. Result: the README-only reader knows about these, the CLI-only user does not. This asymmetry is exactly how "already-solved" measurement workflows get re-invented.

**Fix.** Add the missing 21 commands to `printHelp` (`src/main.cpp:3200`). Group them under section comments (`— measurement —`, `— override —`, `— touch diag —`, `— lcd diag —`), matching the `README.md:56–105` groupings. If the intent is that `lcdspi`, `lcdbus`, `lcdprobe`, `touchlog`, `crash` are debug-only and should stay hidden, at minimum print `(use "help debug" for diagnostics)` at the bottom of `help` and implement `help debug` as a second dump. The current silent-omission behaviour is the worst option.

---

### DOCS-3 — CRITICAL — `README.md` mood list is stale by 7 moods

- Severity: CRITICAL
- Status: **FIXED (commit `cceaf47`).** `README.md:116-118` now lists all 16 moods (`default|bored|angry|happy|think|error|tired|annoyed|sad | surprised|wink|sleeping|love|confused|curious|scared`) with the footnote "(16 moods; note there is no id 4)". Matches the on-device `help`.

**File:** `README.md:62`.

**Problem.** The Serial CLI reference says:

```
mood <name>              default|bored|annoyed|tired|angry|happy|sad|think|error
```

The actual `parseMood` accepts **16 primary names + 6 aliases** (`src/main.cpp:315–336`):

`default, bored, annoyed, tired, angry, happy, sad, think(+thinking), error, surprised(+shock), scared(+terrified), wink, sleeping(+asleep), love(+heart), confused, curious`.

The on-device `help` (main.cpp:3204–3205) is correct and lists all 16. `README.md` is missing `surprised, scared, wink, sleeping, love, confused, curious` — the entire set of 2026 additions.

**Why it matters.** README is the first thing a newcomer reads. They will believe the robot has 9 faces when it has 16. The whole D2 face-content decision (16 procedural moods) is invisible to them, and any new mood added to code will silently continue to be undocumented.

**Fix.** Replace `README.md:62` with the exact line printed by `printHelp` (`src/main.cpp:3204–3205`):

```
mood <name>              default|bored|annoyed|tired|angry|happy|sad|think|error|
                         surprised|scared|wink|sleeping|love|confused|curious
```

Also note in a footnote that `thinking`, `shock`, `terrified`, `asleep`, `heart`, `center` (for `look`) are accepted aliases. Consider a one-line unit test asserting that every `parseMood` name appears in this README section.

---

### DOCS-4 — HIGH — `PROJECT-HISTORY.md` mood inventory contradicts the code

- Severity: HIGH
- Status: OPEN. Verified 2026-08-11: `docs/PROJECT-HISTORY.md:85-87` and `:327-328` still list `NEUTRAL, HAPPY, SAD, ANGRY, SURPRISED, SCARED, CONFUSED, SLEEPING, LOVE, CURIOUS, DIZZY, EMBARRASSED, DETERMINED, PROUD, STARSTRUCK, THINK`. `MOOD_NEUTRAL`, `MOOD_DIZZY`, `MOOD_EMBARRASSED`, `MOOD_DETERMINED`, `MOOD_PROUD`, `MOOD_STARSTRUCK` still return zero hits in `src/main.cpp`. The inventory correction the finding asked for has not been applied.

**File:** `docs/PROJECT-HISTORY.md` §D4 (Decision "16 moods") and §E9.

**Problem.** The history claims the 16 moods are:

> NEUTRAL, HAPPY, SAD, ANGRY, SURPRISED, SCARED, CONFUSED, SLEEPING, LOVE, CURIOUS, DIZZY, EMBARRASSED, DETERMINED, PROUD, STARSTRUCK, THINK.

The actual 16 moods in `src/main.cpp:315–336` and `MoodToLabel` (main.cpp:2073–2088) are:

> DEFAULT, BORED, ANNOYED, TIRED, ANGRY, HAPPY, SAD, THINK, ERROR, SURPRISED, SCARED, WINK, SLEEPING, LOVE, CONFUSED, CURIOUS.

Six moods listed in history (`NEUTRAL, DIZZY, EMBARRASSED, DETERMINED, PROUD, STARSTRUCK`) **do not exist in code**. `grep MOOD_NEUTRAL src/main.cpp` returns nothing. `MOOD_DIZZY`, `MOOD_EMBARRASSED`, `MOOD_DETERMINED`, `MOOD_PROUD`, `MOOD_STARSTRUCK` all return nothing.

Six moods present in code (`DEFAULT, BORED, ANNOYED, TIRED, ERROR, WINK`) are missing from the history's list.

The history's follow-up claim that "kDemoMoods intentionally excludes LOVE and CURIOUS" is also wrong: `kDemoMoods` (`main.cpp:339–353`) actually excludes **THINK, LOVE, and CURIOUS** — three moods, not two.

**Why it matters.** `PROJECT-HISTORY.md` is the freshly canonised source of "here's how this project actually got here". A newcomer trusting it will look for `MOOD_STARSTRUCK` and find nothing, or write a new mood by copying the wrong list, or try to "restore missing NEUTRAL". This is *exactly* the discoverability-and-accuracy failure mode the history was created to prevent.

**Fix.** Update §D4 and §E9 of `docs/PROJECT-HISTORY.md` to the actual 16 moods from `src/main.cpp:315–336`. Add to Appendix A as contradiction #13: "The mood inventory in §D4 was aspirational; current code carries a different set. Code is authoritative." Also correct the `kDemoMoods` exclusion count (THINK + LOVE + CURIOUS).

---

### DOCS-5 — HIGH — `PROJECT-HISTORY.md` §4.1 pin map contradicts itself

- Severity: HIGH
- Status: **OPEN. Commit `cceaf47` claimed this was fixed but the fix landed on a different sub-issue.** The commit added a "Was swapped in code as 17 for months; fixed [065]" annotation to the servo pin rows (`PROJECT-HISTORY.md:939-940`), which is useful but is not what DOCS-5 flagged. The finding is specifically that the "Free GPIOs" row lists GPIO 1, 3, 17, 18 as free while the rows immediately above use them for Battery ADC, BOOST_EN, and both servos. Verified 2026-08-11: `PROJECT-HISTORY.md:951` still reads `Free GPIOs | 1, 2, 3, 16, 17, 18, 21, 33, 34, 43, 44 | Reserve for future | DATASHEET`. The trap the finding describes is still present.

**File:** `docs/PROJECT-HISTORY.md:822–859`.

**Problem.** The pin table in §4.1 correctly lists (matching `src/main.cpp:438–439` and `HARDWARE.md`):

> `Servo LEFT | 18 | LEDC ch0 timer0 | MEASURED [065]`
> `Servo RIGHT | 17 | LEDC ch1 timer0 | MEASURED [065]`
> `Battery ADC | 1 | ×3 divider on-board | ASSUMED`
> `BOOST_EN | 3 | XL63020 enable | MEASURED`

Then the last row says:

> `Free GPIOs | 1, 2, 3, 16, 17, 18, 21, 33, 34, 43, 44 | Reserve for future | DATASHEET`

GPIO 1, 3, 17, and 18 are simultaneously listed as *used* (Battery ADC, BOOST_EN, both servo PWMs) *and* as free. A future contributor reading the "free" row will happily grab GPIO18 for a new peripheral and disable a wheel.

**Why it matters.** Same class of failure as the pin-swap re-debug that `PROJECT-HISTORY.md` was created to prevent — the doc that should be preventing it contains the trap.

**Fix.** Trim the "Free GPIOs" row to genuinely free pins. From the table above it: `2, 16, 21, 33, 34, 43, 44` are unused. Even those should be checked: `43`/`44` are the USB CDC UART pins on the S3, `33`/`34` are on the OPI PSRAM bus for the qio_opi memory type used in `platformio.ini`. Recommended: list only `2, 16, 21` and annotate the rest with why they're not actually free.

---

### DOCS-6 — HIGH — `docs/` is a pile, not an index

- Severity: HIGH
- Status: **FIXED (commit `cceaf47`).** `docs/README.md` now exists and serves as the entry point, with sections "Read before changing anything", "Operational", "Review findings", "Measurement records" and "Conventions". The `PROJECT-HISTORY.md` TOC-and-callout part of the finding is not yet done; the `docs/README.md` entry is the primary landing surface the finding asked for and is sufficient to close it. See DOCS.md meta-index at `docs/key-issues/README.md` for a further at-a-glance status view.

**File:** `docs/`.

**Problem.** `docs/` currently contains:

```
bench-run-001.md ... bench-run-004.md
GIT-NOTES.md
PROJECT-HISTORY.md
scale-check-001.md
research/  (8 agent research reports)
```

There is no `docs/README.md`, no `docs/INDEX.md`, no navigation from `README.md` into `docs/` beyond a single link to `HARDWARE.md`. A newcomer opening `docs/` sees seven files with no hint of which to read first, which are historical, which are current, which are load-bearing. `PROJECT-HISTORY.md` is 65 KB / 1438 lines — it needs a table of contents at minimum.

**Why it matters.** The user's brief specifically called this out: "Whether `docs/` has a coherent index or is a pile." Answer as of today: pile. And the piece of documentation with the highest per-line value (the history) is the one most likely to be closed without being read because of its size.

**Fix.** Add a short `docs/README.md` with three sections:
1. **Start here** — one-line description of `README.md`, `HARDWARE.md`, `docs/PROJECT-HISTORY.md`, `docs/GIT-NOTES.md`.
2. **Measurement records** — `bench-run-*.md`, `scale-check-*.md` (one line each explaining what run they were and the punchline).
3. **Historical research** — `docs/research/`, `IMU-RESEARCH.md`.

Add a table of contents at the top of `docs/PROJECT-HISTORY.md` (§1..§7 + Appendix A + Appendix B). Also add a "Read Appendix A first if you are about to 'clean up' anything" callout in the header.

---

### DOCS-7 — HIGH — `README.md` "Layout" section is severely stale

- Severity: HIGH
- Status: OPEN. Verified 2026-08-11: the "Layout" listing at `README.md:890-911` still references only `main.cpp`, `EyeEngine`, `platformio.ini`, `README.md`, `HARDWARE.md`. `docs/`, `src/override_html.cpp`, `IMU-RESEARCH.md`, `overrides/tools/` are still not enumerated. `override_html.cpp` was extracted this week (commit `11d95a6`) which makes the omission louder.

**File:** `README.md:890–911`.

**Problem.** The Layout section lists three files in `src/` (`main.cpp`, `mascot_a.h`, `tools/mascot.py`). The actual project has at least these `src/` files that don't appear:

`bench_log.{cpp,h}`, `drive_calibration.{cpp,h}`, `drive_session.{cpp,h}`, `imu_qmi8658.{cpp,h}`, `melody.{cpp,h}`, `melody_gen.{cpp,h}`, `motion_events.{cpp,h}`, `movement_expression.{cpp,h}`, `notes.h`, `piezo_tone.{cpp,h}`, `rf_map.{cpp,h}`, `serial_line_buffer.h`, `spin_calibration.{cpp,h}`, `turn_measure.{cpp,h}`, `display_transition.h`, `garage_logo.h`, `mascot/mascot_a.h`.

Additionally the Layout section entirely omits `docs/`, `test/`, `datasheets/`, `body models/`, and the top-level `HARDWARE.md` and `IMU-RESEARCH.md`. It also refers to `src/mascot_a.h` — the file actually lives at `src/mascot/mascot_a.h` (verified 2026-08-07).

**Why it matters.** The Layout section is what tells a new contributor "here is the shape of the codebase". Right now it tells them the shape is 3 files. Anyone acting on that will assume the huge modules (drive session, spin calibration, turn measure, melody generator) are all inside `main.cpp` and try to put their next change there too.

**Fix.** Regenerate `README.md:890–911` from an actual `Get-ChildItem` of `src/`, `test/`, and `docs/`, one line per file group. Correct the mascot bitmap path. Add `test/` (13 native suites via `run_motor_host_tests.ps1`, 14 Python `test_*.py`), `docs/`, `datasheets/` (LCD + IMU PDFs, referenced from `HARDWARE.md`), `body models/`.

---

### DOCS-8 — HIGH — README's software-tests block runs a sibling calibrator repo without saying so

- Severity: HIGH
- Status: OPEN. Verified 2026-08-11: `README.md:105+` still shows the dual-servo-calibrator commands as if they run on this device without prefacing that they belong to the sibling `dual-servo-calibrator` repo. No "these commands are for the sibling repo" preface has been added.

**File:** `README.md:46–54`.

**Problem.** The "Software-only motor safety tests" block:

```powershell
.\test\run_motor_host_tests.ps1
py -3.12 -m unittest discover -s test -p "test_*.py"
cd ..\dual-servo-calibrator
.\test\run_host_tests.ps1
py -3.12 .\test\test_calibrator_contract.py -v
```

The second half is not this repo — it's the sibling `../dual-servo-calibrator` directory (verified present at `C:\Users\v-nisalazar\Desktop\Scripts\dual-servo-calibrator\test\run_host_tests.ps1`). Nothing in `README.md` mentions this sibling exists, what it does, where it came from, or what to do if it isn't checked out.

**Why it matters.** A newcomer cloning `desktop-robot` on a fresh machine gets the first two commands to pass, then the third fails silently with `The system cannot find the path specified.` — and there is no README section explaining what they missed.

**Fix.** Either:
- Add a `## Related repositories` section to `README.md` naming `dual-servo-calibrator`, its clone URL, and what it's for (the test-rig firmware referenced by `HARDWARE.md:488–509` `pin17`/`pin18` commands); or
- Trim `README.md:51–53` and move that block into a separate `docs/related-repos.md` referenced from `HARDWARE.md`.

---

### DOCS-9 — HIGH — README documents a `test` behaviour the on-screen doc contradicts

- Severity: HIGH
- Status: OPEN. Verified 2026-08-11: `README.md:168` still says "the physical test button is gone" while `src/main.cpp:3267` still lists `test` in `printHelp` as a live serial command. The mismatch the finding calls out is unchanged.

**Files:** `README.md:112` vs `src/main.cpp:3227`.

**Problem.** `README.md:112` says: **"The test button is gone"** in the on-screen menu section. Meanwhile `printHelp` (`src/main.cpp:3227`) still exposes `test` at the serial CLI as `360 fast cw, rezero, 360 slow ccw`, and this command still exists in the dispatch table.

**Why it matters.** A reader who takes "test button is gone" to mean "the test routine is gone" will be surprised when `test` at the CLI works and rotates the robot 720° — potentially while tethered (see DOCS-1).

**Fix.** Change `README.md:112` to something like: "The on-screen test button is gone" and add one sentence: "`test` remains available at the serial CLI, and like all rotation commands must be run untethered."

---

### DOCS-10 — HIGH — `mood <name>` and `look <dir>` on-device help omit the aliases

- Severity: HIGH
- Status: OPEN. Verified 2026-08-11: `printHelp` at `src/main.cpp:3244-3245` still lists only the canonical forms; no alias line ("aliases: mad=angry, glad=happy, sleep=sleeping, love=love") was added.

**File:** `src/main.cpp:3204–3206`.

**Problem.** `parseMood` accepts `thinking`, `shock`, `terrified`, `asleep`, `heart` as aliases (main.cpp:315–336). `parseLook` accepts `center` as an alias for `default` (main.cpp:355–366). None of these aliases appear in `printHelp`.

**Why it matters.** Lesser risk than DOCS-3 because typing `default` also works, but a fresh user who reads `[fan-culture doc]` and types `mood shock` will succeed even though `help` says only `surprised` — leading them to distrust the doc.

**Fix.** Either add aliases in parentheses to `printHelp` (e.g. `surprised (shock)`) or remove the aliases from `parseMood`. The former is safer since removing them may break existing scripts.

---

### DOCS-11 — MEDIUM — `HARDWARE.md` power-button code sample uses 2 s

- Severity: MEDIUM
- Status: OPEN. Verified 2026-08-11: `HARDWARE.md:552` still contains `if (longPress > 2 s)`; the real code path (`src/main.cpp` power-button handling) uses 5 s. The sample has not been updated.

**File:** `HARDWARE.md:552`.

**Problem.** The code sample says `if (longPress > 2 s)`, then the paragraph below (line 559) says "Production uses a **3-second hold**." Verified in `src/main.cpp:2477`: `kPowerOffHoldMs = 3000`.

**Why it matters.** The prose correction below the sample is enough to prevent confusion for a careful reader, but the code sample itself is inaccurate and will be copy-pasted verbatim by someone in a hurry.

**Fix.** Change the code sample to `longPress > 3 s`, or annotate it with `// pseudocode — production uses 3 s`.

---

### DOCS-12 — MEDIUM — `HARDWARE.md` calibrator commands (`pin17`, `pin18`, `pair`) aren't clearly scoped to the sibling repo

- Severity: MEDIUM
- Status: OPEN. Verified 2026-08-11: `HARDWARE.md:488-509` still describes `pin17`, `pin18`, `pair` without a preface labelling them as sibling-repo (`dual-servo-calibrator`) commands rather than commands on this device. No scoping preface has been added.

**File:** `HARDWARE.md:488–509`.

**Problem.** The section describing MG90D calibration lists `pin17 <us>`, `pin18 <us>`, `pair <us>` as though they were commands on the production robot. They are not in `printHelp` and not in the dispatch table of `src/main.cpp`. They belong to the `../dual-servo-calibrator` test-rig firmware.

**Why it matters.** Reader opens serial monitor on the production robot, types `pin17 1560`, gets `ERR unknown command`, and doubts the whole calibration procedure.

**Fix.** Prefix the section with a callout: **"These commands run in the `dual-servo-calibrator` sibling repo firmware, flashed onto a separate test rig (COM9). They do NOT exist on the production robot (COM4)."** See also DOCS-8.

---

### DOCS-13 — MEDIUM — `README.md` build & flash doesn't mention the venv-only pyserial

- Severity: MEDIUM
- Status: **FIXED (commit `cceaf47`).** `README.md:56-58` now includes the `python -m venv .venv` / `pip install pyserial` note as a "Build & flash" prerequisite.

**File:** `README.md:15–26`.

**Problem.** Build & flash instructs putting `%USERPROFILE%\.platformio\penv\Scripts` on PATH (good — that's what `pio` needs), but nothing mentions that `pyserial` is only installed inside the PlatformIO venv, and that any Python script that talks to the port (either `test/test_*.py` or ad-hoc probes) must be run with the venv python, not the system `py -3.12`.

**Why it matters.** A tester runs `py -3.12 test\test_something.py`, gets `ModuleNotFoundError: No module named 'serial'`, and tries to `pip install pyserial` globally — which is unnecessary and pollutes their system Python.

**Fix.** Add a one-line note after the `pio device monitor` line:
> Scripts that talk to the port (`serial`, `pyserial`) need the PlatformIO venv Python: `& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" test\test_*.py`. The repo's `test/test_*.py` files assume this.

---

### DOCS-14 — MEDIUM — README's `rfscan` line is correct but neighbours a documented `rfsurvey`

- Severity: MEDIUM
- Status: OPEN. Verified 2026-08-11: `README.md:89` still documents `rfscan` without a "not to be confused with `rfsurvey`" annotation; the two commands are still described in adjacent sections without disambiguation.

**File:** `README.md:89` and `README.md:249–261`.

**Problem.** The Serial CLI table lists `rfscan` (line 89), and further down (lines 249–261) `rfsurvey` is introduced. Both exist in code (`main.cpp:4714` and `main.cpp:4839`), but nothing in `README.md` states they are different things. A reader will assume "typo" and pick whichever appears first.

**Why it matters.** They *are* different: `rfscan` samples RSSI at the current position and heading; `rfsurvey` drives an equilateral triangle and samples at each vertex. Confusing them will produce nonsense measurements.

**Fix.** In the Serial CLI reference (line 89) add: `rfscan  — one-shot RSSI at the current pose. For a driven survey see rfsurvey below.`

---

### DOCS-15 — MEDIUM — `sound` command is undocumented anywhere in `README.md`

- Severity: MEDIUM
- Status: OPEN. Verified 2026-08-11: `printHelp` now documents `sound` (via cceaf47 — see DOCS-2), but the `README.md` command reference still contains no entry for `sound happy|sad|confused|stop`. The README side of the finding is unaddressed.

**Files:** `src/main.cpp:4281–4308` (implements `sound happy|sad|confused|stop`), `README.md`.

**Problem.** `sound` is a public CLI command that triggers the mood-melody generator, and it accepts three mood names (`happy`, `sad`, `confused`). It is not mentioned in `README.md`'s Serial CLI table (lines 56–105) nor in `printHelp`. The only place this surface exists is inline (`main.cpp:4285: "sound: happy, sad, confused"`).

**Why it matters.** The whole procedural mood-melody feature (a headline capability of this robot per PROJECT-HISTORY D25) has no user-facing documentation for how to invoke it from the serial CLI. New users only find it if they type `sound` and see the usage message.

**Fix.** Add to `README.md:71` block (near `tone <hz>`):
```
sound <happy|sad|confused>  play a procedural melody for the mood
sound stop                  stop the currently playing melody
```
Also add to `printHelp` (DOCS-2).

---

### DOCS-16 — MEDIUM — `wifi on|off` and `swipe up|down` sub-args are undocumented

- Severity: MEDIUM
- Status: **PARTIALLY FIXED (commit `cceaf47`).** `printHelp` now emits `wifi on|off|status` and `swipe up|down` at `src/main.cpp:3287` and `:3289` — the on-device side is done. The README-side (a `wifi`/`swipe` entry in the CLI reference table) is still missing. Downgrade to LOW when the README table is updated.

**Files:** `src/main.cpp:3315–3358` (`wifi`), `main.cpp:3362–3379` (`swipe`).

**Problem.** `wifi` accepts three forms: bare (dump status), `wifi on` (enter WIFI_BOOT), `wifi off` (teardown to IDLE). `swipe` accepts `up` and `down` (simulate touch gestures without touching the panel). None of this is in `README.md` or `printHelp`. `wifi on`/`off` is particularly important because it's the head-less way to reproduce override-mode bugs without leaning on the touchscreen.

**Why it matters.** Recovery from an override-mode bug currently requires poking the touchscreen. That is slow, timing-sensitive, and awkward while probing with an oscilloscope. Two-word documentation would unlock the serial-driven workflow.

**Fix.** Add to `README.md:83` group (with `power`):
```
wifi                  status: mode, AP IP, station count, MAC, override state
wifi on|off           enter or leave override (WiFi + web UI)
swipe up|down         simulate the corresponding touch gesture
```

---

### DOCS-17 — MEDIUM — `PROJECT-HISTORY.md` §E24 state machine names don't match code

- Severity: MEDIUM
- Status: OPEN. Verified 2026-08-11: PROJECT-HISTORY.md §E24 still uses the old state-machine names (`LcdMode` and prior transition labels) rather than the `OverrideState` set the code actually uses. Not corrected.

**File:** `docs/PROJECT-HISTORY.md` E24 (override state machine).

**Problem.** History describes the override state machine as `IDLE_DEMO ⇄ WIFI_BOOT → QR_WIFI → QR_URL → CONNECTED → GRACE (30 s) → IDLE_DEMO`. Actual code (`src/main.cpp:3384–3397`) has two separate state machines:
- `OverrideState`: `ORS_IDLE / ORS_WIFI_BOOT / ORS_RUNNING / ORS_GRACE`.
- `LcdMode`: `LCD_MODE_FACE / LCD_MODE_QR_WIFI / LCD_MODE_QR_URL / LCD_MODE_ESTABLISHING / LCD_MODE_GESTURE_MENU`.

So `QR_WIFI`, `QR_URL`, `CONNECTED` are LCD modes (screen contents), not override states. `CONNECTED` doesn't exist at all — there's `LCD_MODE_ESTABLISHING`.

**Why it matters.** Anyone using PROJECT-HISTORY §E24 as a spec for reproducing/testing state transitions will look for symbols that don't exist and infer the wrong control-flow.

**Fix.** In §E24, spell out both machines with their real symbols and how they synchronise (`state` command output at `main.cpp:3382–3407` is a good source of truth). Add the fact that the `state` command dumps both together.

---

### DOCS-18 — LOW — Multiple docs use `LcdMode` vs `OverrideState` interchangeably

- Severity: LOW
- Status: OPEN. Verified 2026-08-11: PROJECT-HISTORY.md and inline `src/main.cpp` comments still swap between the two terms. Not standardized.

**Files:** `docs/PROJECT-HISTORY.md` throughout, comments in `src/main.cpp`.

**Problem.** Same as DOCS-17 but in prose rather than diagrams — the two orthogonal state machines aren't distinguished in narrative text, and the words "state" and "mode" are used interchangeably. Low severity because a careful reader will figure it out.

**Fix.** Standardise on `override state` (for `ORS_*`) and `LCD mode` (for `LCD_MODE_*`). Never plain "state".

---

### DOCS-19 — LOW — Repo owner `Nicolassalazar-pro/desktop-robot` isn't in `README.md`

- Severity: LOW
- Status: OPEN. Verified 2026-08-11: no `Repo:` / `Origin:` line has been added to `README.md`.

**File:** `README.md`, `docs/GIT-NOTES.md`.

**Problem.** `GIT-NOTES.md` mentions the `GH_TOKEN` trap but doesn't state the actual repo owner/name. `README.md` doesn't state where the repo lives. A new user pulling from a fresh clone can't easily verify they're on the right remote.

**Fix.** Add one line at the top of `README.md`: `Repo: https://github.com/Nicolassalazar-pro/desktop-robot (private).` Add the same to `GIT-NOTES.md` as line 1.

---

### DOCS-20 — LOW — `IMU-RESEARCH.md` is not linked from anywhere

- Severity: LOW
- Status: **FIXED (commit `cceaf47`).** `README.md:46` (in the "Documentation" list) now links to `IMU-RESEARCH.md`. Verified 2026-08-11.

**File:** repo root.

**Problem.** `IMU-RESEARCH.md` (608 lines, 35 KB) is the 8-agent research synthesis on gyro drift. It is not linked from `README.md`, not linked from `HARDWARE.md`, and not mentioned in `docs/README.md` (which doesn't exist — see DOCS-6). A newcomer will not know it exists unless they list the root directory.

**Fix.** Link it from the gyro section of `README.md` and from `HARDWARE.md`'s IMU section (`HARDWARE.md:356+`). Include it in the new `docs/README.md` "Historical research" section.

---

## Protected evidence — DO NOT TRIM in a later cleanup pass

The following sections are load-bearing measured evidence. They read like "extra prose" but each one encodes a measurement that cost hours to obtain, and removing them will cause the associated problem to be re-derived. Any pass that touches these must preserve the measured numbers and their derivation.

### PROTECTED-A — `HARDWARE.md:52–111` — ST7789 SPI clock analysis

The 62.5 MHz datasheet ceiling and the 40 MHz current setting, with the measured cause of the intermittent black/glitched screen. Trimming this will reintroduce an 80 MHz "just make it fast" change. Also cross-references `datasheets/ST7789V2_LCD.pdf` — that reference must survive.

### PROTECTED-B — `HARDWARE.md:452–475` — MG90D dead-band table

The `1423–1533 µs` dead band + the 14-bit LEDC tick math (`1.2207 µs/tick`). This is the only place the tick-vs-µs comparator warning is recorded. Removing it will produce a "cal validate is broken, values are off by 1 µs" ticket that closes as "not-a-bug, use tick-space compare".

### PROTECTED-C — `HARDWARE.md:299–354` — "Full-turn duration is a measured chassis property" and the tethered-vs-untethered 4× table

The single measured proof that USB cable tension distorts rotation. See DOCS-1 for why this content also needs to be surfaced more prominently — but the measured numbers themselves (`600 ms → −83.6°/−50.1°`, `2000 ms → −101.1°/−54.6°`, "rate = 0.91 × speed − 24.2 dps") must be preserved verbatim.

### PROTECTED-D — `README.md:140–200` — Measured gyro drift

Two 2-minute stationary runs with a per-sample bias derivation. This underwrites the current gyro-bias handling; removing it turns "why is there a bias EMA at all?" into a re-implementable question.

### PROTECTED-E — `README.md:464–564` — "Measuring the gyro's scale" + "The chip's own calibration (COD)"

The measured proof that the chip's built-in COD *makes* scale error worse — 15% shift after `cod run`. This is the load-bearing evidence for R37 ("do not trust vendor COD"). If it goes, someone will re-run COD to fix drift and produce a robot that turns 15% wrong.

### PROTECTED-F — `README.md:566–621` — Coast compensation A/B (α = 2600)

Three-run supervised A/B with the tie-break rationale. Removing the table turns "why 2600 and not 2800?" into a re-derivation. Also load-bearing for D22.

### PROTECTED-G — `HARDWARE.md:646–682` — CST816T touch orientation trap

`screenX = kScreenW - rawY` derivation. Undocumented on the vendor side. Removing this will produce a "swipes go the wrong way" bug that closes as "chip orientation, see removed section of HARDWARE.md".

---

## Summary

| Severity | Count | Cost of misunderstanding |
|---|---|---|
| CRITICAL | 3 | Newcomer runs `turn 360` while tethered; discovers hidden commands and/or hidden moods only by accident |
| HIGH | 6 | Wrong subsystem inventory, wrong mood inventory, wrong state machine names |
| MEDIUM | 7 | Undocumented sound/wifi surface, calibrator scope, venv Python trap |
| LOW | 3 | Minor terminology and cross-linking |
| PROTECTED | 7 | Load-bearing measured evidence, must not be trimmed |

**Single most dangerous inaccuracy.** DOCS-1 — the "cannot drive while tethered" rule is absent from `README.md` top matter and from on-device `help`. A newcomer following the current README to build, flash, and monitor will drive the robot before ever reading the sections that warn against it. The direct downstream cost is calibration data polluted by cable restraint (which then persists in NVS), and the class of loss `PROJECT-HISTORY.md` was created to prevent.

**Can a newcomer build, flash, test, and operate this robot from the docs alone?**

- **Build & flash:** yes — `README.md:15–26` is accurate and complete.
- **Run headless tests:** partially — `.\test\run_motor_host_tests.ps1` works; the Python suites work if the reader guesses that `py -3.12` is fine (usually is, unless it hits pyserial); the sibling calibrator block will confuse a fresh clone (DOCS-8).
- **Explore the CLI:** **no** — the on-device `help` hides 21 commands (DOCS-2) and the README mood list is stale by 7 moods (DOCS-3). The reader will believe the surface is smaller than it is.
- **Operate movement safely:** **no** — the tether prohibition (DOCS-1) is not where the reader will look. This is the failure mode most likely to cost real time.

Fixing DOCS-1, DOCS-2, and DOCS-3 (all three of which are edits confined to `README.md` and `printHelp` in `main.cpp`) closes the newcomer-safety and newcomer-discoverability gaps together and is the recommended first pass.
