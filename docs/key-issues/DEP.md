## Dependencies [DEP]
> Last reviewed: 2026-08-07 | Open: 8

### DEP-1 — Build is not reproducible: no lockfile, and caret ranges have already floated a minor version
- Severity: HIGH
- Status: OPEN
- File: `platformio.ini:90-107`, `.gitignore:1`, `.pio/libdeps/esp32-s3-devkitc-1/integrity.dat`
- Problem: Nothing in the repo pins the resolved library versions. The next `pio pkg install` on a fresh checkout can pick different minor/patch versions than what is on this machine.
- Why: `.gitignore:1` excludes the entire `.pio/` tree, so both the resolved sources and PlatformIO's `integrity.dat` are non-committed and per-machine. `integrity.dat` (237 B) stores only the request spec strings from `lib_deps` — it is not a hash/version lockfile. Four of the seven `lib_deps` use `^` (minor + patch float) and PlatformIO has no lockfile equivalent to `package-lock.json` / `uv.lock`. Concrete proof the ranges *do* float: `esphome/ESPAsyncWebServer-esphome@^3.3.0` resolved to **3.4.0** today (`.pio/libdeps/.../ESPAsyncWebServer-esphome/library.json` line 15), and `bblanchon/ArduinoJson@^7.1.0` resolved to **7.4.3** (`.../ArduinoJson/library.json` line 10). Both have already moved past the minor version originally selected.
- Fix: Pin the exact versions currently on disk so today's known-good tree is the tree everyone else builds against. Replace each `^X.Y.Z`/`~X.Y.Z` with `= X.Y.Z` (or PIO's `@X.Y.Z`) using the resolved values: `ArduinoJson@7.4.3`, `AsyncTCP-esphome@2.1.4`, `ESPAsyncWebServer-esphome@3.4.0`, `CST816_TouchLib@2.2.0`, `GFX Library for Arduino@1.3.9`. Bump only when a real reason demands it, and re-run a full bench pass after each bump.

### DEP-2 — Caret ranges sit directly on top of the WebSocket/AsyncTCP behaviors this week's fixes depend on
- Severity: HIGH
- Status: OPEN
- File: `platformio.ini:98-99`; consumers at `src/main.cpp:5233-5243`, `src/main.cpp:8629-8775`, `src/main.cpp:9848-9867`, `src/main.cpp:10265-10275`
- Problem: `AsyncTCP-esphome@^2.1.4` and `ESPAsyncWebServer-esphome@^3.3.0` are the exact libraries whose `WS_EVT_CONNECT` / `WS_EVT_DISCONNECT` / `binaryAll()` semantics this project fights around, and both are allowed to float minor + patch versions with no upstream release notes read.
- Why: The code has three load-bearing workarounds for behavior in these two libs — an HTTP-layer auth guard added specifically because closing inside `WS_EVT_CONNECT` re-enters teardown and hangs INT_WDT (`main.cpp:9852-9865`), a `kWsOwnerSilenceTimeoutMs = 8000` timeout to detect the "dead client, no `WS_EVT_DISCONNECT`" case (`main.cpp:5233-5239`, `main.cpp:10265-10275`), and a project-wide ban on `AsyncWebSocket::binaryAll()` (PROJECT-HISTORY.md R7 / D8). These workarounds sit under a moving robot: WS callbacks fire on the AsyncTCP task (`main.cpp:5313`, `main.cpp:8654`), and a change in send-buffer / lifetime semantics could crash while wheels are spinning. `^3.3.0` has already jumped to 3.4.0 with nobody having read a changelog. The esphome fork also does not ship a CHANGELOG or LICENSE file in the package (`.pio/libdeps/.../ESPAsyncWebServer-esphome/`), so "what changed 3.3 → 3.4" is only reconstructable from the fork's GitHub commit log.
- Fix: Same as DEP-1's remediation applied specifically to these two, plus a rule: any bump of these two libraries requires reading the esphome/AsyncTCP and esphome/ESPAsyncWebServer commit log between the old and new tags, diffing `AsyncWebSocket.cpp` / `AsyncClient.cpp`, and re-running the "phone leaves WiFi mid-drive" bench case (D8) plus a 4-client soak. Add that as a comment above the pins in `platformio.ini`.

### DEP-3 — `lib/EyeEngine/library.json` declares an unpinned wildcard for GFX; can silently pull 1.4.x that the platform cannot build
- Severity: MEDIUM
- Status: OPEN
- File: `lib/EyeEngine/library.json:1`
- Problem: The in-tree library `EyeEngine` declares its GFX dependency as `"moononournation/GFX Library for Arduino": "*"` — any version — while the root `platformio.ini` pins `~1.3.7` with a documented reason.
- Why: HARDWARE.md line 768 and `platformio.ini:92` both record that `1.4.x` requires ESP32-Arduino v3.x, which `espressif32@7.0.1` does not ship. The root `~1.3.7` constraint currently wins because PIO's LDF picks the project-level `lib_deps` version, but this is a footgun: any change to `lib_ldf_mode`, or a switch to depend-resolution that respects nested `library.json`, or a future PIO version that changes precedence, would let EyeEngine's `*` resolve 1.4.x and break the build with obscure ESP32-Arduino v3 API errors.
- Fix: Change EyeEngine's `library.json` to match the parent constraint or drop the redundant dep entirely (EyeEngine only includes `Arduino_GFX_Library.h` transitively; the real dep is the root project's). Concrete:
  ```json
  {"name": "EyeEngine", "version": "0.1.0", "frameworks": "arduino", "platforms": "espressif32",
   "dependencies": {"moononournation/GFX Library for Arduino": "~1.3.7"}}
  ```

### DEP-4 — `ricmoo/QRCode@^0.0.1`: abandoned upstream, known-buggy at near-capacity, kept anyway
- Severity: MEDIUM
- Status: OPEN
- File: `platformio.ini:101`; workaround at `src/main.cpp:5326-5336`
- Problem: The QR library is single-release (0.0.1, 2017), unmaintained, and has a documented near-capacity encoding bug that the code works around by staying ≤67% capacity.
- Why: PROJECT-HISTORY.md R9 and E22 (`[057]`, `[058]`) record that v3-M at 93% capacity produced codes phones detected but did not decode; fixed by moving WIFI to v4-M / URL to v3 at ~67% load (`main.cpp:5333-5335`). Upstream `ricmoo/qrcode` has had no releases since 0.0.1. The `^0.0.1` spec, per npm/semver, pins to exactly 0.0.1 for 0.0.x, which happens to be what we want — but "caret on 0.0.x" is a semantics footgun that varies by resolver, and there is no note in `platformio.ini` explaining that the library is deliberately unmaintained. That means a future contributor who "cleans up the deps" is likely to try a "safer" replacement without knowing about the capacity workaround.
- Fix: Change to an exact pin `ricmoo/QRCode@0.0.1` and inline the reason with a warning: `; Unmaintained upstream (only release 2017). Do NOT swap without re-running the QR bench in E22 — this library encodes badly above ~70% capacity and current payload budget assumes it (see main.cpp:5326-5336, PROJECT-HISTORY R9).` Consider adding a periodic (yearly) alternative-scan task to the runbook.

### DEP-5 — LGPL-3.0 on the two esphome forks: fine while private, blocks a distributable build without a compliance plan
- Severity: MEDIUM
- Status: OPEN
- File: `platformio.ini:98-99`; licenses at `.pio/libdeps/.../AsyncTCP-esphome/LICENSE` (LGPL-3.0), `.pio/libdeps/.../ESPAsyncWebServer-esphome/library.json:14` (declared `"license": "LGPL-3.0"`, no LICENSE file actually shipped in the package)
- Problem: Both esphome forks are LGPL-3.0. The firmware statically links them into a single ESP32 image, which is a distribution boundary the LGPL cares about the moment the device ships to anyone outside the project.
- Why: `docs/GIT-NOTES.md` says the repo is private. That is what suppresses the obligation today. If the device is ever handed to another user (Kickstarter, product, gift, sale, employer transfer) the LGPL-3.0 §4/§5 obligations apply and require enabling the recipient to relink with a modified library — nontrivial for a monolithic flashed image without a documented rebuild path or object files. `ESPAsyncWebServer-esphome` also ships no LICENSE text file in the resolved package (verified via `.pio/libdeps/.../ESPAsyncWebServer-esphome/` — only `library.json` and `README.md`), which is itself an LGPL §4 red flag. Note also: `ESP32Servo` and `GFX Library for Arduino` ship *no license file at all* in the PlatformIO tarball; upstream is BSD-like for GFX and LGPL-2.1 for ESP32Servo, but that is not verifiable from the checked-out tree.
- Fix: Two-step. (1) Add a `THIRD_PARTY_LICENSES.md` to the repo listing each `lib_deps` entry with resolved version + license + upstream URL, so the compliance boundary is documented before any distribution question comes up. (2) If distribution is ever planned, either (a) publish the firmware source under the same terms, (b) provide a documented "how to rebuild with a modified library" recipe alongside the object files, or (c) replace the two forks with permissive alternatives (there is no drop-in). Not urgent, but the decision needs to be *made* before shipping, not after.

### DEP-6 — `~1.3.7` on GFX is a coupled pin with no cross-reference to the platform pin that makes it safe
- Severity: LOW
- Status: OPEN
- File: `platformio.ini:24`, `platformio.ini:92`
- Problem: `moononournation/GFX Library for Arduino@~1.3.7` is safe only because `espressif32@7.0.1` (Arduino-ESP32 v2.x) is also pinned. The two pins are coupled by an invariant recorded elsewhere (HARDWARE.md:768: "1.4.x requires ESP32-Arduino v3.x core") but the `platformio.ini` line only says "1.4.x needs ESP32-Arduino v3" without naming the counter-pin.
- Why: Whoever bumps `platform = espressif32@...` in the future will not necessarily read HARDWARE.md; the comment on line 92 explains the GFX pin in isolation but does not mention that moving to a newer `espressif32` (v54 = Arduino-ESP32 v3.x) is the trigger that would unblock GFX 1.4.x. This is a documented invariant, but it lives in two places and only reads one-way. Verified today: resolved GFX version is 1.3.9 (`.pio/libdeps/.../GFX Library for Arduino/library.properties:2`), still on the ~1.3.7 side of the invariant.
- Fix: Add a matching comment on `platform = espressif32@7.0.1`: `; If bumping past 7.x (which brings Arduino-ESP32 v3.x), also revisit the GFX ~1.3.7 pin below and the LEDC channel/timer assumptions in D12.` This makes the coupling visible from either direction.

### DEP-7 — `ESP32Servo@1.2.1` pinned exactly, but the reason is not in the manifest
- Severity: LOW
- Status: OPEN
- File: `platformio.ini:93`; related invariants at `docs/PROJECT-HISTORY.md` D11 / D12 / R10
- Problem: The pin comment (`; MG90D 1000..2000us, 50 Hz, 14-bit`) describes the *configuration*, not why `1.2.1` specifically is required. The real reason (LEDC channel/timer allocation is manually mapped and later library versions changed defaults) is only in PROJECT-HISTORY.md.
- Why: `1.2.1` is 4+ years old and any dependabot-style scanner will flag it. Without an inline rationale, a future contributor will interpret the exact pin as "just old" and bump it. That would break D12 (LEDC ch0/timer0 servo, ch2/timer1 piezo, ch4/timer2 backlight isolation) and R10 (the `Servo::detach()` + drive-pin-LOW workaround for the 1..999µs clamp bug the current version has). D11 explicitly documents `writeMicroseconds(0)` accelerates instead of stopping — a library behavior the code depends on.
- Fix: Extend the pin comment: `madhephaestus/ESP32Servo@1.2.1 ; PINNED — 3.x changed LEDC channel/timer defaults (breaks D12 isolation) and the clamp-at-1000µs behavior R10/D11 relies on. Do NOT bump without re-doing motor safety bench.`

### DEP-8 — Python host tools rely on ~5 undeclared runtime dependencies
- Severity: LOW
- Status: OPEN
- File: `tools/audio_to_piezo.py:108,179,180,188,407`, `tools/render_garage_logo.py:7`, `tools/apply_calibration.py:292-293`, `tools/mascot.py:69`, `tools/melody_player.py:263`; only `tools/requirements-midi-bridge.txt` declares deps and only for the MIDI bridge
- Problem: The tools directory imports `numpy`, `librosa`, `imageio_ffmpeg`, `mido`, `pyserial`, and `PIL/Pillow`, but only three of those are declared anywhere (`tools/requirements-midi-bridge.txt`: `mido==1.3.3`, `python-rtmidi==1.5.8`, `pyserial==3.5`). numpy, librosa, imageio_ffmpeg, and Pillow are not listed in any manifest. No `pyproject.toml`, no `uv.lock`, and no PEP 723 script metadata anywhere in `tools/` (searched `# /// script` across `tools/*.py` and `test/*.py`: zero matches — the task prompt's claim that PEP 723 is in use is not currently true).
- Why: These are host-side calibration and audio-generation tools — not shipped in firmware — so they will not brick a robot, but "run `audio_to_piezo.py` a year from now" is not reproducible today. `librosa` in particular has aggressive minor-version churn and hard numpy pin ranges. The .gitignore line 9 comment says "tools/ has pytest suites and a uv/PEP-723 workflow" — the workflow is not actually in place.
- Fix: Either (a) add a PEP 723 header block to each executable tool script (`# /// script\n# dependencies = ["numpy", "librosa", "imageio-ffmpeg", "Pillow", "mido", "pyserial"]\n# ///`) with concrete lower bounds, which makes `uv run --script` self-contained per the .gitignore comment; or (b) collapse into a single `tools/requirements.txt` with pinned versions and delete the midi-bridge-only file. Either is a 15-minute change and would match the intent that is already documented in `.gitignore`.

