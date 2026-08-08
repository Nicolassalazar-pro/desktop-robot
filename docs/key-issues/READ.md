## Readability [READ]
> Last reviewed: 2026-08-07 | Open: 12

### READ-1 — Stale `[DEBUG-DELETE]` markers wrap shipping features
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:1076-1110, 2115-2249, 5202-8226, 10729-10742, 10789-10800, 10828-10833`
- Problem: Two blocks marked `[DEBUG-DELETE] BEGIN … END` are documented in `docs/PROJECT-HISTORY.md` as *shipping* code that will not be deleted — the battery meter (§4.6 / D19) and the entire Override HTTP + WebSocket phone UI (§4.2 / D6–D8). The next reader who takes the marker at face value will delete the only battery indicator and the primary user-facing control surface.
- Why: `[DEBUG-DELETE]` is used elsewhere in the file (e.g. the demo mood-name overlay at 2067–2113) as an accurate "strip before shipping" tag. Reusing the same tag for permanent code trains the reader to ignore it, or worse, to strip it. The battery-meter marker even says "Remove this block + all matching `// [DEBUG-DELETE]` lines before ship." on line 2069 while D19 fixes the battery model as permanent behaviour. The Override marker (5202) frames "override debug WebUI" as scratch code, but the SSID `Hanger OVERRIDE` and password `TheGarageAtl` immediately below are the shipped credentials.
- Fix: keep the code, remove the marker. On the battery block: replace the header at `main.cpp:1074, 2115` with `// Battery meter — see PROJECT-HISTORY §4.6 (D19).` and delete the eight `// [DEBUG-DELETE]` sites at 1076, 1110, 2249, 10735, 10742, 10789, 10791, 10796, 10800, 10828, 10833. On the Override block: replace the banner at 5202 with `// Override phone UI — SSID/WPA per D6, HTTP+WS control plane per D7–D8, telemetry protocol per §4.2.` and drop the `[DEBUG-DELETE]` END inside `overrideLoop()` (10729/10732). Leave the demo-mood-label markers (1070–1074, 2067–2113) alone — that block genuinely is dev-only.

### READ-2 — `printHelp()` lists ~55% of the commands `dispatchCommand` actually handles
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:3200-3238` (vs `dispatchCommand` at 3240-5069 and `README.md:56-105`)
- Problem: The `help` serial command hides ~17 real commands from anyone who does not already know they exist: `wifi`, `swipe`, `state`, `crash`, `lcdpush`, `lcdbus`, `lcdprobe`, `lcdspi`, `touchinfo`, `touchlog`, `touchreset`, `touchdebug`, `lead`, `bench`, `sound`, `sense`, `scale`, `cod`, `lpf`, `straight`, `survey`, `rfsurvey`, `rfscan`. The mood list at line 3204-5 also omits `curious`, which `parseMood` accepts.
- Why: The list is hand-maintained duplication of `dispatchCommand`'s if-chain and has drifted every time a command was added. `README.md:56-105` is now the ground truth, and `help` silently contradicts it — precisely the kind of "comment that lies" the brief calls out. Worse, the omitted commands include the load-bearing diagnostics (`lcdprobe`, `lcdpush`, `lcdbus`, `crash`) — the reader who most needs them is the one debugging a wedged panel with no help text to reach for.
- Fix: cheapest — add the missing commands to `printHelp` and the missing `curious` to the mood list; audit against README on every PR that touches `dispatchCommand`. Better — after READ-5, generate the help text from the dispatch table (`for (auto& e : kCommands) Serial.println(e.usage);`) so the two cannot drift again.

### READ-3 — `sense`/`motion` naming note is attached to the wrong command block
- Severity: HIGH
- Status: OPEN
- File: `src/main.cpp:4279-4281` (comment) vs `src/main.cpp:4310-4348` (block it describes)
- Problem: The comment `// NOTE: named 'sense', not 'motion' — 'motion' is already the eye-engine's gaze-range command and was silently shadowing this one.` sits directly above the `sound` handler at 4281. The `sense` handler is 30 lines further down at 4310, and its output still prints `motion: …` labels (`Serial.printf("motion: tilt %.1f deg …")` at 4317).
- Why: This is the exact class of trap Appendix B calls out — the `motion` shadow was one of the two named bugs (the other being `kServoLeftPin`). The evidence explaining why the block is not called `motion` is now decoupled from the block it justifies, so someone renaming or removing `sound` will lose the explanation entirely, and someone reading `sense` will not understand why its output labels itself `motion:` (the diagnostic label was kept for continuity with existing scripts). Moving `sound` above `sense` was the likely culprit.
- Fix: relocate the comment to line 4310 immediately above `if (strncmp(line, "sense", 5) == 0 …` and expand: `// Named 'sense', not 'motion'; 'motion' was already the eye-engine's gaze-range command and silently shadowed the accelerometer diagnostic for months. Output labels are kept as 'motion:' because external tools grep for them; see Appendix B in PROJECT-HISTORY.`

### READ-4 — Extract the 275-line inlined `OVERRIDE_HTML` raw string to its own translation unit
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:8230-8512`
- Problem: 275 lines of unrelated HTML/CSS/JS sit in the middle of the C++ source, wedged between `jsonFindStr()` (8209) and `isValidMoodId()` (8519). It is the single largest run of "wrong-language code" in `main.cpp` and it separates the WebSocket ingress path (8623+) from the telemetry builder (8527) that both use it.
- Why: The reader searching for the WS handler has to page across 275 lines of unrelated markup to reach it. Editor navigation (grep, "go to symbol", brace-matching) all degrade across a 4 KB raw string. The block is entirely self-contained — it is a single `PROGMEM` `const char[]` in a `R"OD(...)OD"` raw string with no macros, no `#ifdef`, no references into main.cpp — so extraction is mechanical.
- Fix: create `src/override_html.cpp`:
  ```c++
  #include <pgmspace.h>
  extern const char OVERRIDE_HTML[] PROGMEM;
  const char OVERRIDE_HTML[] PROGMEM = R"OD(...paste 8238-8512...)OD";
  ```
  and add `extern const char OVERRIDE_HTML[] PROGMEM;` at the top of main.cpp near the other `extern` decls. Zero behavior change; PROGMEM and raw-string tokens are both TU-safe. Removes 275 lines from main.cpp for essentially zero risk. In-scope per the review brief.

### READ-5 — 1830-line `dispatchCommand` if/return chain should be table-dispatched at the top level
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:3240-5070` (and by extension `printHelp()` at 3200)
- Problem: `dispatchCommand` is 55 top-level `if (strcmp(line, "…") == 0) { … return; }` branches in one function of 1830 lines. Every command handler is already isolated by early return, but the reader still has to skim past all 55 branches to find one, and `printHelp` has to be hand-maintained to match (see READ-2). The brief explicitly lists this as in-scope.
- Why: The top-level chain is a table with the wrong syntax — every branch has the same shape. Making the table explicit turns the file's control flow into data, lets `printHelp` read it directly, and lets a reader find a handler by grepping for a string literal in one location instead of by scanning for an if.
- Fix: shallow table only — one row per top-level command; sub-command parsers (`spin`, `cal`, `bench`, `lead`, `sense`, `scale`, `sound`, `cod`, `lpf`, `straight`, `survey`, `rfsurvey`) keep their internal parsing untouched.
  ```c++
  struct SerialCommand {
    const char* name;
    void (*handler)(char* argument);
    const char* usage;  // one line, used by help
  };
  static const SerialCommand kCommands[] = {
    {"ping",       cmdPing,       "ping"},
    {"status",     cmdStatus,     "status"},
    {"mood",       cmdMood,       "mood <name>  default|bored|…|curious"},
    // …
  };
  void dispatchCommand(char* line) {
    char* argument = splitArgument(line);
    for (const auto& c : kCommands) {
      if (strcmp(line, c.name) == 0) { c.handler(argument); return; }
    }
    Serial.println(F("ERR unknown command"));
  }
  ```
  Migrate handlers a batch at a time (5–10 per commit) so a regression can be bisected to one row. In-scope per the review brief.

### READ-6 — Three parallel mood tables — `kMoodCommands`, `kDemoMoods`, `moodToUpperName` switch
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:315-353` and `src/main.cpp:2071-2091`
- Problem: The 16 moods are enumerated three times: `kMoodCommands` (21 rows including aliases, 315), `kDemoMoods` (13 rows, custom order for the auto-cycle, 339), and `moodToUpperName` (hand-coded switch that uppercases each name, 2071). Adding a mood requires editing all three or the new mood is silently missing from either the demo or the on-screen label.
- Why: `moodToUpperName` is `toupper()` composed over what `moodName` already returns — a pure rewrite, no new data. `kDemoMoods` overlaps `kMoodCommands` on every row except the alias entries and the reordering. History (Appendix B) already contains one "silent shadowing" mood bug; sitting on three ways to spell each mood invites another. `kMoodCommands` also silently depends on canonical-name-first ordering — see READ-9.
- Fix: express `moodToUpperName` as `staticUpperOf(moodName(m))` (a 6-line helper that uppercases into a static thread-local buffer), or replace all three with a single X-macro:
  ```c++
  #define MOOD_TABLE(X)                                                    \
    X(MOOD_DEFAULT,   "default",   /*demo*/1)                              \
    X(MOOD_BORED,     "bored",     /*demo*/1)                              \
    /* … */                                                                \
    X(MOOD_CURIOUS,   "curious",   /*demo*/0)
  ```
  and derive `kMoodCommands`, `kDemoMoods`, and `moodToUpperName` from it. Cheapest local fix: kill `moodToUpperName`.

### READ-7 — Two `processDemo` branches are provably unreachable
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp:5115-5148` (unreachable arms) and `src/main.cpp:391-396` (the constants)
- Problem: `kDemoTotalSteps = kDemoMoodCount` (line 396), and `processDemo` resets `g_demoStep = 0` as soon as `g_demoStep >= kDemoTotalSteps` (5115). Therefore `g_demoStep` never reaches `kDemoMoodCount`, so the `else if (g_demoStep < kDemoMoodCount + kDemoLookCount)` branch at 5131 and the `else { g_eyes.blink(); }` branch at 5141 can never execute. `kDemoLooks[]` and `kDemoLookCount` exist only to feed the dead branch.
- Why: The `[DEBUG-DELETE]` comment at 393–395 documents this as intentional and reversible ("Restore `+ kDemoLookCount + 1U` to re-enable look-direction and blink steps"), but the reader still has to trace three constants, one array, and two if branches to reconcile "the demo cycles moods" with 20 lines of code that isn't running. The dead arms also refer to `kDemoLooks[lookIndex]` and `g_eyes.blink()` — if either is refactored, the compiler will happily keep the still-unreachable code compiling with the wrong types.
- Fix: option A (safest): wrap the whole `else if` + `else` in `#if 0 // Enable look/blink phase of demo — see kDemoTotalSteps` so the code is preserved verbatim but unmistakably off. Option B (cleanest): delete `kDemoLooks`, `kDemoLookCount`, and the dead arms; put "to re-enable look/blink demo phase see commit `<hash>`" in a comment above `processDemo`.

### READ-8 — Inconsistent `strcmp` vs `strncmp` parsing style in the same dispatch chain
- Severity: LOW
- Status: OPEN
- File: `src/main.cpp:4078, 4130, 4281, 4310, 4350, 4514, 4584, 4626, 4673, 4714`
- Problem: Most of `dispatchCommand` uses `if (strcmp(line, "name") == 0)`. Ten branches — `lead`, `bench`, `sound`, `sense`, `scale`, `cod`, `lpf`, `straight`, `survey`, `rfsurvey` — use `if (strncmp(line, "name", N) == 0 && (line[N] == '\0' || line[N] == ' '))`. Since `splitArgument()` at 3241 has already replaced the space after the command with `'\0'`, `line` is always the bare command word by the time these tests run, and the two forms match identically.
- Why: Two idioms for the same test, in the same function, for no discoverable reason. New readers assume the `strncmp` variant is doing something subtle (prefix match? space handling?) and copy it into new handlers, spreading the confusion. It also makes grep for a command name slightly harder (`\"lead\"` vs `\"lead\", 4`).
- Fix: replace each `strncmp(line, "X", N) == 0 && (line[N] == '\0' || line[N] == ' ')` with `strcmp(line, "X") == 0`. Behavior is identical because `splitArgument` has already nulled the delimiter. This is a mechanical grep-and-replace over ten sites and pairs naturally with READ-5.

### READ-9 — `moodName()` silently depends on canonical-name-first row order in `kMoodCommands`
- Severity: LOW
- Status: OPEN
- File: `src/main.cpp:2665-2672` (function) and `src/main.cpp:315-337` (table)
- Problem: `moodName(mood)` returns the *first* row in `kMoodCommands` matching the enum. The table lists aliases `thinking`, `shock`, `terrified`, `asleep`, `heart` immediately after each canonical name — so `moodName(MOOD_SLEEPING)` currently returns `"sleeping"` because that row precedes `"asleep"`. Reordering the table for any reason (e.g. sorting alphabetically) silently changes every string this function emits: serial output, WS telemetry, log lines.
- Why: The "canonical first" convention is enforced only by comment discipline — none. This is a landmine of the same class as the `motion` shadow (Appendix B): an editor with good intentions and no context can break the wire protocol.
- Fix: add a comment at line 315 — `// Row order matters: canonical name FIRST, aliases after. moodName() returns the first match, and its output is on the wire.` OR split into `kMoodNames[]` (one row per enum, canonical only) and `kMoodAliases[]` (name → enum lookup only). `moodName()` reads from the former; `parseMood()` searches both.

### READ-10 — Unexplained `digitalWrite(SYS_EN, HIGH)` bookending `pinMode` in `setup()`
- Severity: LOW
- Status: OPEN
- File: `src/main.cpp:10399-10401`
- Problem: `setup()` writes `SYS_EN` HIGH, sets the pin to OUTPUT, then writes HIGH again — three lines, no comment. Reading top-down, the second write looks like a copy-paste bug. Reading with context, it's likely a belt-and-braces re-assert in case `pinMode()` toggles the level. Every reader has to derive this from scratch, and it sits inside a boot path that is explicitly documented elsewhere as "the first mistake will power the board off."
- Why: The surrounding pinMode-writes for `BOOST_EN`, `kServoRightPin`, `kServoLeftPin` all follow `digitalWrite(pin, LOW); pinMode(pin, OUTPUT);` — the two-write asymmetry for SYS_EN begs for one line explaining it.
- Fix: add a comment — `// Assert SYS_EN HIGH before AND after pinMode: on ESP32-S3, pinMode can float the pad for a few microseconds while the peripheral matrix reconfigures, and SYS_EN dropping LOW even briefly latches the power controller off.` If the double write is *not* deliberate, delete one of them; the rest of the file treats SYS_EN as latching, so a stray glitch would be visible immediately.

### READ-11 — `moodToUpperName` fallback is `"?"` with no diagnostic
- Severity: LOW
- Status: OPEN
- File: `src/main.cpp:2071-2091`
- Problem: If a new `EyeMood` enum member is added and this switch is not updated, the on-screen demo label reads `"?"` and nothing else fires — no serial log, no `static_assert`, no compile warning.
- Why: The switch has no `default: __builtin_unreachable();` or `default: Serial.printf("moodToUpperName: unmapped mood %d", m);`. Silent-fallback + separate-table-of-moods (READ-6) makes new-mood bugs invisible until someone points a camera at the robot.
- Fix: cheapest — replace `return "?";` with `Serial.printf("moodToUpperName: unmapped mood %d\n", static_cast<int>(m)); return "?";`. Best — remove `moodToUpperName` entirely per READ-6.

### READ-12 — File a `main.cpp` staged-split reference finding (owner approval required)
- Severity: MEDIUM
- Status: OPEN
- File: `src/main.cpp` (10,835 lines / 438 KB)
- Problem: `main.cpp` at 10,835 lines is a bounded readability tax on every future change. The brief carves out an applied full split as out of scope until owner approval, and this finding exists to satisfy that carve-out with a concrete plan and an honest risk read.
- Why: The file's *unusual* size (~10,800 lines) is not itself confusing — most of it is genuinely long linear code. What *is* confusing is that some genuinely separable regions (OVERRIDE_HTML, the serial console, the battery meter, the QR/mode-switch screen code) live next to structurally-inseparable regions (WS transport lifecycle, drive session ownership, breadcrumbs, `overrideLoop`). Findings READ-4 and READ-5 already cover the two safest extractions. A full split beyond that is *not* recommended without a full manual regression pass on each phase, because PROJECT-HISTORY R1–R43 lists 43 approaches that already caused subtle bugs — most of them concurrency and lifecycle, and the AsyncTCP↔Arduino handoff in `orOnWsEvent`/`enqueueWsEvent`/`serviceDriveControl` is exactly the region a split would perturb.
- Fix: **do not apply without owner approval.** If approved, do it in independently-revertable phases, each on its own PR, each with an untethered soak run:
  - Phase A: `OVERRIDE_HTML` → `override_html.cpp`. (Covered by READ-4.) **Low risk**.
  - Phase B: `dispatchCommand` → `serial_console.{h,cpp}` with per-command handlers, table dispatched. (Covered by READ-5.) **Low-medium risk** — sub-parsers move as a unit.
  - Phase C: battery meter (2115-2249) + demo mood label (2067-2113) → `battery_meter.{h,cpp}`. **Medium risk** — reads `g_canvas`/`g_gfx` globals; needs an interface header.
  - Phase D: QR / gesture menu / establishing screen (9471-9843) → `lcd_modes.{h,cpp}`. **Medium risk** — touches `LcdMode` state machine which is entangled with `overrideLoop`.
  - Phase E: HTTP routes + WS ingress + `overrideLoop` (5200-10388) → `override_server.{h,cpp}`. **HIGH risk** — this is the region containing 43 rejected approaches; do not attempt without an untethered soak matrix (idle 24 h + wifi churn + WS forced-disconnect + panic-injection) after every intermediate commit.
  - Phases A and B alone would bring `main.cpp` under 8 500 lines with no touch to the concurrency-sensitive regions.

---

**Not flagged — documented decisions, keep despite looking suspicious**:
- `EyeEngine::snapMood(m)` at `main.cpp:1753` — load-bearing (PROJECT-HISTORY §5.2 / D5). Do NOT collapse into `setMood`.
- SPI clock literals (40 MHz / 62.5 MHz ceiling, `main.cpp:52-83`) — measured evidence (§4.1). Do NOT round.
- Coast alpha 2600 deg/s² and the linear-vs-quadratic model comments — measured evidence.
- LEDC channel/timer allocation (Servo=0/0,1/0; Piezo=2/1; Backlight=4/2) — deliberate isolation (D12).
- `writeMicroseconds(0)` clamp workaround via detach+LOW — deliberate (D11, R10).
- `WiFi.persistent(false)`, no `binaryAll()`, `softAPdisconnect(true)`, HTTP routes registered once (§5.1, D16) — solve netstack 12308.
- RTC breadcrumbs + `crash` command + NVS persistence in setup — load-bearing per brief; confirmed in setup(). Keep verbatim.
- `lcdact` heartbeat in `loop()` (10688-10708) — deliberately auto-prints without a serial trigger so a wedged-panel state is visible by listening only. Keep verbatim.
- `lcdprobe` / `lcdpush` / `lcdbus` / `lcdspi` — isolate the two independent components of the same fix (SPI re-init vs full canvas push). Not redundant with each other. Keep.
- `[DEBUG-DELETE]` on the demo mood-name overlay (2067-2113) — genuinely dev-only per its own comment ("mood-review demos"). Keep the marker.
