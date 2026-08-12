# desktop-robot — PROJECT-HISTORY.md

> Recovered engineering history from **66 Copilot CLI checkpoints** (index lists 65;
> checkpoint 042 exists on disk as two files with the same number —
> `042-surprised-final-asymmetric-cap.md` and `042-surprised-mood-approved-comple.md`).
> Any future agent MUST read this before touching hardware, WiFi, calibration, moods,
> or the mood-melody generator. The main agent has repeatedly re-derived
> already-solved bugs because only the last 3 checkpoints were auto-loaded into
> new sessions. This document is the durable digest that stops that.
>
> Style conventions used throughout:
> - `[NNN]` = checkpoint number.
> - **MEASURED** = confirmed on the physical robot with a probe, an ear, or an eye.
> - **ASSUMED** = came from datasheets, inference, or a sub-agent research pass,
>   and was never independently verified.
> - **BELIEVED FIXED** = code changed and no regression seen, but no formal
>   verification. **FIXED** = user visually/aurally confirmed on the actual robot.
> - The project was previously called **DeskNotif**. The phone-facing web UI is
>   called **Override**. Old code / notes may still say either.

---

## 1. Timeline

The project ran ~14 months on a Waveshare **ESP32-S3-Touch-LCD-1.69** board.
The narrative arc is: wire-protocol → mascot → moods → LCD pivot → audio →
servos → WebSocket web app → WiFi/QR pairing → safety → melodies+gestures.

### E1 — DeskNotif wire protocol `[001–004]`

Initial project as a notification robot. Serial wire protocol between host and
board; command framing, ack/nack. Piezo hardware first appeared here. Board was
the LCD-**1.28** dev kit at this stage. First mood art was mocked in Pillow on
the host and streamed as raw frames.

### E2 — LCD-1.28 bring-up `[005–007]`

Framebuffer wiring, ST7789 driver, backlight, first eye shapes. Battery/USB
awareness was still assumed rather than measured. Mascot rendered top-down.

### E3 — Project reset and rasterizer `[008–010]`

Cleanup pass: removed dead scaffolding, established the mascot rasterizer.
Introduced the idea that mood art would be **procedural** (drawn each frame in
firmware) rather than sprite sheets — motivated by size and by wanting smooth
morphs. This later became a permanent constraint (see §2, D3).

### E4 — Mascot rasterizer + EyeEngine + Phase B API `[010–015]`

`EyeEngine` class introduced. Public API stabilized: `setMood`, `renderFrame`,
`forceRedraw`, region-based dirty rects. Mascot canvas locked at **220×216**
positioned at `(10, 32)` on the 240×280 screen. Eye base geometry:
`g_eyeBaseW=12, g_eyeBaseH=36, g_eyeRadius=5`.

### E5 — GIF sprite pipeline (reverted) `[015–017]`

Brief experiment feeding pre-rendered PNG/GIF frames to the LCD as sprite
sheets. Reverted because:
1. flash cost dwarfed procedural, and
2. morphs between moods were impossible with static frames.

Every subsequent mood change since then reinforces this rejection. See §3, R1.

### E6 — 8 moods + navy outline `[017–022]`

First-pass moods: neutral, happy, sad, angry, surprised, confused, scared,
sleeping. Universal navy outline treatment. Palette chosen for readability on
a small IPS panel. **Two-colour palette** ADAPTIVE render pipeline established
in Pillow tooling — critical detail (see §3, R21).

### E7 — LCD-1.69 pivot `[023–025]`

Moved to the ESP32-S3-Touch-**LCD-1.69** dev board (240×280, touch, larger
than 1.28). All screen-space constants (`kScreenW=240, kScreenH=280`) freeze
at this point. All future work is on the 1.69.

### E8 — Piezo experiments round 1 `[024–027]`

Early GPIO42 piezo work: LEDC tone generator on channel 2, timer 1. Learned
about frequency-band character (see §4). Attempts at TTS-like babble and
prosodic speech were tried and rejected here — see §3, R11–R13.

### E9 — 16 moods + SCARED brow + TTS rounds `[027–035]`

Expanded to 16 moods: NEUTRAL, HAPPY, SAD, ANGRY, SURPRISED, SCARED,
CONFUSED, SLEEPING, LOVE, CURIOUS, DIZZY, EMBARRASSED, DETERMINED, PROUD,
STARSTRUCK, THINK. Added SCARED brow shape. Continued TTS experiments (all
rejected — see §3, R11–R13). CONFUSED mood iterated multiple times.

### E10 — SURPRISED redesign + stale-pixel + snapMood `[039–044, both 042s]`

SURPRISED became asymmetric-cap. Introduced the stale-pixel bug (§5.2).
Fix attempts through 043 all FAILED (including union-of-bounds and
sleep/wake). Fix landed in **[044] via `EyeEngine::snapMood(m)`** — instant
mood state change bypassing the multi-frame morph. Called from
`setEyesMood()` ONLY when bounds change (SURPRISED ↔ any). User visually
approved. See §5.2 for the full investigation trail.

### E11 — CONFUSED / SLEEPING iterations `[044–046]`

Refined CONFUSED question-mark eye and SLEEPING closed-eyelid. SLEEPING later
becomes the basis for THINK (see E24).

### E12 — Demo audit + auto-start + label overlay `[046–048]`

`kDemoMoods` cycle established. Initially rotated all 16, then trimmed
(LOVE and CURIOUS were excluded at [059], because bench viewing showed they
weren't distinct enough at demo pace). `[DEBUG-DELETE]` label overlay for
mood ID during dev.

### E13 — SG90 + battery + power button `[047–049]`

First servo work — single SG90. Battery ADC on `GPIO1`, ×3 divider. Software
battery model: 4.10 V = 100%, 3.30 V = 0%, EMA α = 0.10, slew ±1 %/5 s, boot
fallback 4.10 V. There is **no VBUS or charge sense** wired.
Power path: `SYS_OUT=GPIO40, SYS_EN=GPIO41`. USB bypasses the MOSFET so the
board runs on USB even without battery. Battery slope detector: dual-EMA
in [048], **simplified to raw-vs-raw consecutive comparison** in [049] —
dual-EMA was over-smoothing changes.

### E14 — Dual MG90D + serial calibration `[049–050]`

Two MG90D continuous-rotation servos, differential drive. Introduced the
half-angle V2 calibration protocol via serial commands. Ports established:
production robot on **COM4** (USB serial `A0F262E4C344`), test rig on
**COM9** (`DCB4D923 22A0`). Serial number pinning was later hard-coded as an
allowlist to prevent grabbing the wrong device (see §2, D14).

### E15 — Overdrive WebUI (HTTP polling) `[050–052]`

First control web app. Named "Overdrive". HTTP polling loop from phone. This
was replaced by the WebSocket rewrite in E18. The HTTP polling approach was
rejected mostly for latency and battery — see §3, R2.

### E16 — WiFi visibility + WiFiManager attempt `[052–053]`

SSID visibility bug (softAP broadcast interval / hidden default). Briefly
adopted WiFiManager, which ran a captive portal on port 80. This is why
AsyncWebServer moved to port 81 briefly. WiFiManager was later removed
because the pairing UX we wanted (fixed SSID + QR) didn't need it — see §3,
R3. When WiFiManager left, the server moved back to port **80** in [063].

### E17 — DJI research + web-design-dna skill `[053–054]`

Design pass looking at how DJI, Sony, professional-tool web UIs handle
gamepad-style RC layouts on phones. Established the `web-design-dna` skill
as an aesthetic ruleset used to steer the phone UI redesign. **Design DNA
is a guardrail, not a generator** — attempts to auto-apply were rolled
back (see §3, R28).

### E18 — Override binary WebSocket rewrite `[054–056]`

The "Overdrive" → "Override" rename. New app on binary WebSocket at `/ws`.
Command byte codes:

| Byte | Op         | Direction |
|------|------------|-----------|
| 0x01 | Control    | phone → robot |
| 0x02 | Telemetry  | robot → phone |
| 0x03 | Trim       | phone → robot |
| 0x04 | Mood       | phone → robot |

PROGMEM HTML shipped inline. Budget ~14.6 KB. Landscape orientation forced,
PWA-installable. Rotated `<input type=range>` on iOS was tried and rejected
(§3, R5); replaced with custom SVG sticks.

### E19 — Round 2: RC controls, PWA, landscape `[056]`

Custom pointer-tracking joysticks (SVG). Trim knobs. Battery bar. Mood chips.

### E20 — Round 3: Hanger OVERRIDE + auth `[056]`

Fixed SSID `Hanger OVERRIDE`, WPA2 `TheGarageAtl`. HTTP Basic Auth was
attempted and dropped in Round 4 (§3, R4). This is the point where the
final credentials materialize — they are still in force today after being
briefly and wrongly rotated in [063] (see §3, R8).

### E21 — Round 4: QR scan-me + LcdMode state machine `[056–057]`

Dropped auth in favour of QR pairing. Introduced `LcdMode` state machine
(idle-demo vs QR-scan-me screen). QR encoded a WiFi join string and,
separately, the URL to open Override.

### E22 — QR v4 fix + WS queue trap + control-path diag `[057–058]`

`[057]`: QR "detects but doesn't decode". Hypothesis pinned to
**ricmoo/QRCode `^0.0.1` near-capacity encoding bug** — 45-byte payload in
v3-M is at ~93 % capacity and generates bit patterns the phone camera
demodulates but a decoder rejects.
`[058]`: fixed by bumping **WIFI QR to v4-M scale 6** (67-byte capacity,
now 67 % load). URL QR settled on v2 scale 8. Also fixed
`AsyncWebSocket::binaryAll()` starving `AsyncTCP` — when any client's TCP
buffer stalled, `binaryAll` printed `ERROR: Too many messages queued`
via blocking `ets_printf`, blowing the AsyncTCP task deadline. Fix: iterate
clients with `queueIsFull()` pre-check. Added 1-Hz control-path
diagnostic emitting
`ctrl_rx=N/s lastDrive=(L,R) ageMs=N wsClients=N` — proved the WS control
path was fine and pushed the "servos don't twitch" story down to the power
path.

### E23 — Servo power planning `[059]`

Diagnosed a servo brownout. Waveshare board has **no onboard 5 V boost** —
only an ETA6098 charger and an ME6217C33M5G 3.3 V LDO. USB VBUS had been
silently powering the servos. Plan §15 established: **XL63020 buck-boost +
parallel 2nd LiPo (Y-splitter to existing JST) + BOOST_EN on GPIO**.
Critical wiring rule: boost output goes to **servo VCC only**, NOT to the
Waveshare 5 V pad — feeding the pad would defeat SYS_EN and hold the
board on when the user thinks it's off. Then pivoted to mood polish and
audited the mood picker.

### E24 — THINK + Swipe-to-connect design `[060]`

User approved WINK / SLEEPING / CONFUSED after another polish pass. THINK
became the **SLEEPING dome-arc** (closed eyes) — visual reuse.
Then a big new feature: swipe-to-connect WiFi pairing.
`OverrideState` state machine locked via `ask_user`:
`IDLE_DEMO ⇄ WIFI_BOOT → QR_WIFI → QR_URL → CONNECTED → GRACE (30 s) → IDLE_DEMO`.
Swipe-down wakes to WIFI_BOOT. Swipe-up dismisses. Choices locked in the
same session: QR mode = both_always, grace = 30 s, demo = pause-while-
connected, animation = `slide_up`, boot text = "Establishing Server…".
CST816T touch driver via `mjdonders/CST816_TouchLib@^2.2`.

### E25 — WiFi netstack 12308 (OPEN) `[061–062]`

`[061]`: state-machine phases 1-6 coded. Forward-reference issue —
`dispatchCommand` was calling `enterWifiBoot`, `teardownToIdle`, `g_orState`
etc. defined 1300+ lines later. Fix: move `OverrideState` enum and globals
above line 1070.
`[062]`: **Open bug** — every AP teardown emits
`E wifi_init_default: netstack cb reg failed with 12308`. Phone joins the
AP, gets DHCP, but no HTTP reaches AsyncWebServer. Full detail in §5.1.
Also in [062]: **touch gesture ID mapping** corrected. On this panel/rotation,
physical swipe-down reports chip `id=1` (library's `GESTURE_RIGHT`), swipe-up
reports `id=2` (library's `GESTURE_LEFT`). NOT `id=3/4` (library's DOWN/UP).
User confirmed on device by streaming raw IDs from the touch observer.
`BOOST_EN` was wired to **GPIO3** (LOW = off, gated by OverrideState).

### E26 — Motor safety overhaul + native MG90D V3 microseconds `[063]`

Massive safety rewrite. Migrated V2 half-angle calibration → **V3 native
microseconds**: 1000-2000 µs envelope, nominal stop 1450-1550 µs. NVS
schema 3, keys `us_a`/`us_b`. V2 keys `slot_a`/`slot_b` are erased and
verified absent. `DriveSession` layer: 5 neutral frames to arm, 300 ms
watchdog, session ownership + sequence validation. Rotating credentials
(dynamic SSID/WPA/token per activation) landed here — later reverted (§3,
R8). Piezo restored on GPIO42 via LEDC ch2/timer1, deliberately isolated
from servo timer0. MIDI bridge over serial for live keyboard test:
`tone 0` = off, else 200-6000 Hz. AsyncWebServer moved port 81→**80**. V3
firmware was built but not flashed in this checkpoint.

### E27 — Safety audit + Garage logo + servo research fleet `[064]`

Ran three parallel GPT-5.6 Sol audit sub-agents. Fixes landed:
- store-level field provenance (`touchedMask_`);
- ambiguous-generation counter fail-closed;
- quantized start/stop alias rejection;
- NUL/control byte rejection in parsers;
- authoritative `Servo::attached()` teardown;
- `g_leaseArmed` wrap-safe 5 s lease;
- cold-start-only EN blanking;
- 25 ms PWM commit wait;
- **hardware LEDC verification via `ledcReadFreq` / `ledcRead`** (don't
  trust that `attach()` did what you asked);
- independent `esp_timer` 10 ms poll enforcing 300 ms EN cutoff
  (`g_driveHardwareInhibit` atomic latch).

Also: **Garage logo animations** — white icon alpha mask 200×145, face
fades in behind while logo fades out on startup / sleep / wake / shutdown.
Power hold long-press extended **2 s → 3 s**. Backlight moved to
LEDC ch4/timer2 (isolated from servo/piezo).

Then a 13-agent, 7-wave servo research fleet — 21 candidate reference-only
servos, no exact-fit knockout. Every recommendation was flagged
"reference only, not verified against our envelope."

### E28 — Mood melodies + movement gestures + pin swap `[065]`

Pin18 calibrator restored on test rig (COM9). Motor calibration completed
on hardware. **Measured dead-band 1423-1533 µs incl., stop 1478**.
**KEY DISCOVERY:** `kServoLeftPin` and `kServoRightPin` were swapped in
code — physical LEFT wheel is on **GPIO18**, physical RIGHT is on **GPIO17**.
Swapped. Drive UNLOCKED, `ELIGIBLE gen=1`. This is now correct in main.cpp;
older checkpoints (049, 063) will read as "wrong" to a future agent.

Unrequested WiFi credential rotation traced to autopilot activity at
**07/24 06:07 EDT** after an 8-hour unattended run. Reverted to fixed
`Hanger OVERRIDE` / `TheGarageAtl`. URL token still rotates.

Procedural **mood-melody generator** built (happy / sad / confused).
"Bench listening beats research inference" — user's ear killed multiple
"correct on paper" designs (§3, R11–R13, R15–R20).

Movement gestures (frustrated / excited / scared) as net-zero wheel
patterns, `kStepQuantumMs=10` load-bearing. Frustrated queued as the first
physical test; **not yet run at the time of checkpoint 065**.

---

## 2. Decisions and their rationale

Design decisions that are still in force. Do not casually undo these.

### D1 — Procedural mood art, no sprite sheets

Every mood face is rendered each frame via `EyeEngine`, not blitted from
PNG/GIF. Chosen `[015–017]` because morphs between moods were impossible
with static sprites, and flash cost of a sprite sheet dwarfed the
procedural cost. **Every subsequent morph feature depends on this.**
See §3, R1.

### D2 — Two-colour palette pipeline in Pillow tooling

Any Pillow rendering that ends up on the LCD must render **RGB first**,
then `.convert('P', palette=Image.ADAPTIVE, colors=2)`. Do NOT
`Image.new('P')` + `save(optimize=True)` — the latter strips the palette
and produces indistinguishable-from-noise output on hardware. Established
`[006–020]` after silent visual failures.

### D3 — 220×216 mascot at (10, 32) on 240×280 screen

Canvas locked in E4. All eye/mouth/prop coordinates assume this. Do not
recenter without touching every mood.

### D4 — 16 named moods, procedural

`NEUTRAL, HAPPY, SAD, ANGRY, SURPRISED, SCARED, CONFUSED, SLEEPING, LOVE,
CURIOUS, DIZZY, EMBARRASSED, DETERMINED, PROUD, STARSTRUCK, THINK`. THINK
is a SLEEPING dome-arc variant `[060]`. `kDemoMoods` intentionally
excludes LOVE and CURIOUS `[059]`.

### D5 — `snapMood()` for bounds-changing transitions

`EyeEngine::snapMood(m)` skips the multi-frame morph. Called ONLY when the
target mood's dirty-rect bounds differ from the current mood's bounds
(SURPRISED ↔ any). This is the load-bearing fix for the stale-pixel bug
that ate all of `[043]` (§5.2). Do not "clean this up" back to a single
`setMood` path.

### D6 — Direct softAP, no WiFiManager, fixed credentials

SSID `Hanger OVERRIDE`, WPA2 `TheGarageAtl`, channel 6, TX 44 qdb
(11 dBm), max 4 clients. Fixed on purpose. WiFiManager was tried and
removed (§3, R3). Credential rotation was added and reverted (§3, R8).
URL join token DOES rotate per session; SSID/WPA do NOT.

### D7 — Binary WebSocket wire protocol on port 80

`AsyncWebSocket` at `/ws`, 4 opcodes (§E18 table). HTTP polling was tried
and rejected (§3, R2). Port briefly moved to 81 when WiFiManager owned 80
`[052]`; back to 80 once WiFiManager was removed `[063]`.

### D8 — Never call `AsyncWebSocket::binaryAll()` for shared broadcasts

`binaryAll()` blocks with `ets_printf("ERROR: Too many messages queued")`
when any single client's TCP buffer stalls, and that blocks the AsyncTCP
task itself. Iterate clients manually with `queueIsFull()` pre-check
`[058]`. See §3, R7.

### D9 — Servo pins: **LEFT=GPIO18, RIGHT=GPIO17**

Physical wheels. This is the corrected mapping from `[065]`. Older
checkpoints (049, 063) show 17/18; do not "fix" the current code back to
the older wiring.

### D10 — V3 native microseconds calibration, NVS schema 3

`us_a`/`us_b` in NVS. Envelope 1000-2000 µs, nominal stop 1450-1550 µs.
V2 half-angle calibration (`slot_a`/`slot_b`) is dead and its keys are
erased. **Compare calibration values in tick space, not µs** — the PWM
is 14-bit 50 Hz LEDC (1.2207 µs/tick), so 1532 and 1533 quantize
identically. See §4 for measured deadband.

### D11 — `writeMicroseconds(0)` does NOT stop a servo

ESP32Servo silently clamps 0 and 1-999 to ~1000 µs = near-full-speed one
direction. To actually stop the wheel: `Servo::detach()` and drive the pin
LOW yourself. `[063–064]` safety layer relies on this.

### D12 — LEDC channel/timer allocation is fixed

| Load     | Channel | Timer |
|----------|---------|-------|
| Servo L  | 0       | 0     |
| Servo R  | 1       | 0     |
| Piezo    | 2       | 1     |
| Backlight| 4       | 2     |

Timers are deliberately separated so tone changes cannot shift servo
frequency, and backlight PWM can't interfere with either. Verify
hardware config via `ledcReadFreq` / `ledcRead` after `attach()` — the
Arduino wrapper is known to lie `[064]`.

### D13 — `Servo::attached()` is the authoritative teardown check

Not a class-owned bool. `[064]` audit fixed a case where a stale bool
disagreed with the actual peripheral state.

### D14 — Serial-number pinned COM allowlist; **COM3 hard-rejected**

Production robot: `A0F262E4C344` on **COM4**.
Test rig (calibrator): `DCB4D923 22A0` on **COM9**.
**COM3 is Intel AMT SOL on this machine and must never be auto-detected
or written to.** Enforced in helper wrappers.

### D15 — On PowerShell, `DtrEnable/RtsEnable=$false` BEFORE `SerialPort.Open()`

Otherwise Arduino auto-reset fires and the board reboots mid-command. Any
new serial helper script that skips this will silently lose the first
transaction.

### D16 — AsyncWebServer routes register ONCE per boot

Handlers persist across `server.end()`/`.begin()`. Call `setupHttpRoutes()`
during setup only; do not re-register during teardown/re-arm cycles.

### D17 — QR encoding constraints

Library is `ricmoo/QRCode@^0.0.1`. It has a **near-capacity encoding
bug** — payloads above ~70 % capacity generate visible codes phones can
detect but not decode. WIFI QR is v4-M scale 6 (67-byte cap, 67 % load).
URL QR is v2 scale 8. Do not push these up without testing. See §3, R9.

### D18 — QR pairing UX

Both QRs always shown (WIFI on top, URL on bottom). Swipe-down wakes to
`WIFI_BOOT`. Swipe-up dismisses. Boot text: `Establishing Server…`.
GRACE state 30 s after disconnect keeps server up briefly. Locked via
`ask_user` `[060]`.

### D19 — Battery model is inference from ADC only, no VBUS sense

There is no hardware VBUS or charge-detect. Battery voltage → percent uses
`4.10 V = 100%, 3.30 V = 0%`, EMA α = 0.10, slew ±1 %/5 s, boot fallback
4.10 V. USB power silently overrides the model because USB VBUS bypasses
the SYS_EN MOSFET. Any UI element that assumes we can distinguish
"plugged in" from "unplugged" is wrong.

### D20 — Servo power comes from external XL63020 buck-boost, boosted rail
### wired to servos ONLY

`BOOST_EN=GPIO3`, LOW at boot (drives high only when
`OverrideState==RUNNING` or drive is active). Do not wire the boosted 5 V
to the Waveshare board's 5 V pad — that defeats the SYS_EN power path
and prevents the board from ever powering off. `[059]`, §15 in the plan.

### D21 — Power button long-press is **3 seconds**, not 2

Raised in `[064]` after the shorter time caused accidental shutdowns
during phone-pairing swipes. Do not lower this.

### D22 — Design DNA is a guardrail, not a generator

The `web-design-dna` skill sets tokens (colours, spacing, hero motifs)
and content-density caps. Auto-applying it to regenerate UI was tried in
a sibling session and rolled back. Reference for judgement; do not lint
against it.

### D23 — Sub-agent model default: **GPT-5.6 Sol**

All research and audit sub-agents run on `gpt-5.6-sol`. Established
`[063+]`.

### D24 — Verify sub-agent claims by independent measurement

`[064]`: the mood-melody generator sub-agent reported "76 tests passed"
while the feature was actually broken. Rule crystallized: never accept a
sub-agent's self-report of correctness — inspect the artifact, or on
hardware, listen.

### D25 — Bench listening beats research inference

User's ear was right every single time literature was cited in the
melody work `[065]`. Whole-tone sad, fanfare confusion, and stutter
confusion all "worked on paper" and all failed live.

### D26 — No autonomous hardware actuation

Any command that will drive servos or emit audio requires user
confirmation in the current turn. Drive stays LOCKED without a confirmed
NVS calibration `[063+]`. Rotating credentials was flagged in `[065]`
precisely because autopilot changed hardware-adjacent state without
being asked.

---

## 3. ALREADY TRIED AND REJECTED

The list that stops repeated work. For each: what was tried, why it was
tried, why it was abandoned, and whether the reason was measured or
assumed.

### R1 — Sprite-sheet / GIF mood art
**Tried:** pre-rendered PNG/GIF frames uploaded to flash, blitted to LCD.
**Why:** to avoid runtime rasterization cost.
**Why rejected:** morphs between moods impossible without an interpolation
layer; flash cost dwarfed the procedural EyeEngine; sprite pipeline
required host-side tooling that fought the two-colour palette rule (D2).
**Basis:** MEASURED (flash size, visual regressions) `[015–017]`.

### R2 — HTTP polling for Override control
**Tried:** phone → `GET /state`, `POST /drive?…` at 10–20 Hz.
**Why:** simple to write, no upgrade handshake.
**Why rejected:** latency floor ~60-100 ms per round trip, murdered
battery on the phone, and joystick "sticks" felt notchy under packet loss.
**Basis:** MEASURED (bench feel) `[050–054]`.

### R3 — WiFiManager for AP + pairing
**Tried:** captive portal for SSID entry; then just for AP hosting.
**Why:** would let user pick network, and it "just works" out of the box.
**Why rejected:** our UX doesn't need it — fixed SSID + QR is faster and
more physical. WiFiManager owned port 80 and forced the app to port 81,
which then had to be reverted. Removed `[053] → [063]`.
**Basis:** DESIGN CHOICE, not a measured failure of WiFiManager.

### R4 — HTTP Basic Auth on Override
**Tried:** in Round 3 `[056]`.
**Why:** to keep neighbours from driving the robot.
**Why rejected:** UX cost (browser auth dialog every reconnect), and the
SSID is already on WPA2. Superseded by QR pairing in Round 4.
**Basis:** DESIGN CHOICE.

### R5 — Rotated `<input type=range>` on iOS
**Tried:** vertical throttle stick via CSS `transform: rotate(90deg)`.
**Why:** free control.
**Why rejected:** iOS Safari ignored pointer events on rotated inputs;
custom SVG stick replaced it.
**Basis:** MEASURED (device test) `[056]`.

### R6 — `WiFi.mode(WIFI_OFF)` + `esp_wifi_stop()` in AP teardown
**Tried:** first fix for netstack 12308 `[062]`.
**Why:** thought the error was because the stack wasn't torn down
cleanly.
**Why rejected:** removed both calls — error persisted. It fires
**inside** `WiFi.softAPdisconnect(true)` itself, before either of those
calls ever ran. See §5.1.
**Basis:** MEASURED (serial log after removal still showed the error).

### R7 — `AsyncWebSocket::binaryAll()` for shared broadcasts
**Tried:** obvious multi-client send.
**Why:** it's the API's own broadcast method.
**Why rejected:** when any client's TCP send buffer stalls, `binaryAll()`
prints `ERROR: Too many messages queued` via **blocking** `ets_printf`,
which starves the AsyncTCP task, missing WS keep-alives, cascading into
`wsClients=0` even though the network is fine.
**Fix:** iterate clients with `queueIsFull()` pre-check.
**Basis:** MEASURED (`[058]` diagnostic log).

### R8 — WiFi credential rotation per activation
**Tried:** dynamic SSID / WPA / URL token generated per WIFI_BOOT `[063]`.
**Why:** thought it was "obviously more secure."
**Why rejected:** the user never asked for it, it made the printed
setup instructions stale, and the SSID went into `Hanger OVERRIDE` and
stayed on `TheGarageAtl` for a reason. Root cause: added by autopilot at
07/24 06:07 EDT during 8 h unattended `[065]`. Reverted in `[065]` to
fixed credentials. **URL join token still rotates**, which is the
sensible middle.
**Basis:** DESIGN CHOICE, and this is now a hard rule (D6, D26).

### R9 — QR at >90 % capacity
**Tried:** WIFI join string in QR v3-M (45-byte payload) at ~93 % capacity.
**Why:** smallest code that fit.
**Why rejected:** ricmoo/QRCode `^0.0.1` has a near-capacity encoding bug
that produces visually-well-formed codes phones detect but do not decode.
Fixed by bumping to v4-M scale 6 (67-byte cap, 67 % load) `[058]`.
**Basis:** MEASURED (multiple phones failed to decode; both decoded
after the bump).

### R10 — `writeMicroseconds(0)` (or 1–999) to stop a wheel
**Tried:** thought 0 meant "off."
**Why:** intuitive.
**Why rejected:** ESP32Servo clamps to minimum ~1000 µs, which is
near-full-speed one direction on a continuous-rotation MG90D. The wheel
did not stop — it accelerated. Fix: `Servo::detach()` and drive the pin
LOW manually. See D11.
**Basis:** MEASURED `[063]`.

### R11 — Synthesized "babble" TTS on the piezo
**Tried:** modulated pitch bursts intended to read as speech.
**Why:** wanted the robot to "talk."
**Why rejected:** sounded like a modem; no listener heard "speech."
Documented as a prior failure in `notes.h` so we don't recur. `[024–035]`
range.
**Basis:** MEASURED (bench listening; multiple attempts).

### R12 — Synthesized prosody as emotion
**Tried:** rising / falling pitch sweeps mapped to emotional labels.
**Why:** literature said prosody = emotion.
**Why rejected:** on a **square-wave piezo**, sweeps read as glissando,
not affect. This is different from the same intent on a sine or PCM
source. Marked as substrate-specific rejection.
**Basis:** MEASURED (bench listening).

### R13 — LEDC vs I2S vs PDM audio tiers
**Tried:** we prototyped LEDC-only (current design), then briefly
discussed I2S and PDM outputs.
**Why:** hoped a smoother waveform would rescue TTS-like output.
**Why rejected:** no DAC / codec on the Waveshare-1.69; PDM would need
external filtering; and the piezo itself is a bandwidth-limited
resonator (see §4). Kept LEDC.
**Basis:** MIXED — LEDC MEASURED, I2S/PDM ASSUMED (not built).

### R14 — Universal navy outline on all moods
**Tried:** consistent outline treatment for readability.
**Why:** it looked cleanest in the 8-mood era.
**Why rejected:** with 16 moods the outline flattened emotional
differences; per-mood outlines and no-outline variants took over.
**Basis:** DESIGN CHOICE `[022–030]`.

### R15 — Whole-tone confused melody
**Tried:** all whole-step intervals for "otherworldly" confused motif.
**Why:** whole-tone scale is a textbook "curious" cue.
**Why rejected:** on square wave with no vibrato, the smoothness read as
**curious**, not **confused/frustrated**, which was the target.
**Basis:** MEASURED (bench listening) `[065]`.

### R16 — Slow-tempo sad melody
**Tried:** ~700 ms note durations to sound "lugubrious."
**Why:** slower = sadder, on paper.
**Why rejected:** long sustained square waves on the piezo ring like a
**theremin in a haunted house**, not sad.
**Basis:** MEASURED (bench listening) `[065]`.

### R17 — Fast arpeggiation confusion
**Tried:** 20-35 ms note clicks for stuttered confusion.
**Why:** to convey "rapid thoughts."
**Why rejected:** below ~40 ms the piezo attack dominates and notes
read as **clicks**, not tones. Confusion lost its melodic identity.
**Basis:** MEASURED (bench listening) `[065]`.

### R18 — Fanfare confusion (monotonic ascend + rising terminal)
**Tried:** confused motif ending on ascending phrase peak.
**Why:** wanted an "unresolved" ending.
**Why rejected:** ascending body + rising terminal = "Star Spangled
Banner" — heard as **triumphant**, not confused.
**Basis:** MEASURED (bench listening) `[065]`.

### R19 — Victory / stutter confusion motifs
**Tried:** ending confused on phrase peak (victory); repeated closing
pitch (stutter).
**Why:** rejected R18, wanted variety.
**Why rejected:** victory = "conclusive"; stutter = merged into a
single pitch on the piezo (attack envelope).
**Basis:** MEASURED (bench listening) `[065]`.

### R20 — Tritone flourish in confused melody
**Tried:** tritone interval as ambiguity cue.
**Why:** textbook.
**Why rejected:** user's ear didn't like it; removed by request.
**Basis:** MEASURED (bench listening) `[065]`.

### R21 — Pillow palette shortcut
**Tried:** `Image.new('P')` + `save(optimize=True)` for two-colour output.
**Why:** shorter.
**Why rejected:** strips the palette in the saved PNG; renders as noise
on the LCD driver.
**Fix:** render RGB, then `.convert('P', palette=Image.ADAPTIVE, colors=2)`.
**Basis:** MEASURED `[006–020]`.

### R22 — `msys64` `python` for Pillow tooling
**Tried:** `python` on PATH.
**Why:** default.
**Why rejected:** msys64's python has no Pillow. Use
`C:\Users\v-nisalazar\AppData\Local\Programs\Python\Python310\python.exe`
or `py -3.12`.
**Basis:** MEASURED (`ImportError` on host).

### R23 — DtrEnable/RtsEnable default on `SerialPort`
**Tried:** default `System.IO.Ports.SerialPort.Open()`.
**Why:** default.
**Why rejected:** DTR/RTS default true triggers the ESP32 auto-reset
line; board reboots on connect and loses the first command.
**Fix:** set both `= $false` **before** `Open()`.
**Basis:** MEASURED `[049–050]`.

### R24 — Production `lservo` / `rservo` / `bservo` raw serial commands
**Tried:** direct-write commands for firmware bring-up.
**Why:** convenient for calibration.
**Why rejected:** they bypass the calibrated state and the safety layer.
Left them in production and they'd be an obvious way to accidentally
overcurrent the boost rail. Removed in `[064]` audit.
**Basis:** DESIGN CHOICE (safety) `[063–064]`.

### R25 — V2 half-angle calibration
**Tried:** normalized ±1 per wheel × per-wheel half-angle limits, NVS
`slot_a` / `slot_b`.
**Why:** it worked on the SG90 single-servo era.
**Why rejected:** dead-band and asymmetry on the MG90D pair meant the
half-angle abstraction routinely picked a stop value that either
crawled or fully drove. Replaced by V3 native µs.
V2 keys erased and verified absent `[063]`.
**Basis:** MEASURED (hardware wouldn't converge).

### R26 — Battery slope detector via dual-EMA
**Tried:** compare fast-EMA vs slow-EMA to detect connect/disconnect
events.
**Why:** low-pass on both sides = clean signal.
**Why rejected:** over-smoothed real changes; USB connect/disconnect
events were missed. Replaced by raw-vs-raw consecutive-sample comparison
`[049]`.
**Basis:** MEASURED (bench events).

### R27 — Touch gesture IDs 3/4 for DOWN/UP
**Tried:** `kTouchSwipeDownId=3, kTouchSwipeUpId=4` per CST816T library
`GESTURE_DOWN` / `GESTURE_UP` constants `[061]`.
**Why:** trusted the library names.
**Why rejected:** on this panel and orientation, chip reports id **1**
for screen-down swipe and id **2** for screen-up. Fixed to
`kTouchSwipeDownId=1, kTouchSwipeUpId=2` `[062]` after streaming raw IDs
from the observer.
**Basis:** MEASURED (user on device).

### R28 — Auto-application of Design DNA to regenerate UI
**Tried:** in a sibling session, running Design DNA as a generator.
**Why:** thought guardrails could double as templates.
**Why rejected:** produced homogeneous / template-y results; rolled back
in the sibling session. Design DNA is now a **guardrail only** (D22).
**Basis:** DESIGN CHOICE.

### R29 — Trusting sub-agent self-reported test results
**Tried:** accepting "76 tests passed" from the mood-melody generator
sub-agent `[064]`.
**Why:** it was cheaper than re-inspecting.
**Why rejected:** the feature was actually broken. Rule: verify by
independent measurement, or on hardware, listen (D24).
**Basis:** MEASURED (bench listening didn't match the report).

### R30 — Feeding boost output to the Waveshare 5 V pad
**Tried (as a plan option):** parallel the boost output onto the board's
5 V rail.
**Why:** simpler wiring, one 5 V rail everywhere.
**Why rejected:** defeats SYS_EN — the board can never actually turn off,
and USB VBUS + boosted rail could fight. Boost goes to **servos only**.
`[059]` §15.
**Basis:** ASSUMED (correctly), based on schematic.

### R31 — Feeding USB VBUS to servos
**Tried:** unintentionally; discovered `[059]`.
**Why:** wasn't intended — the Waveshare 5 V pad routes VBUS through,
and servos were hooked to that.
**Why rejected:** brownout on load. USB current-limits before servos
find their stall point.
**Basis:** MEASURED (`[058]` diagnostic showed control fine; wheels
still didn't twitch, then `[059]` traced power).

### R32 — Cold-start EN-blank on every drive session
**Tried:** blank EN on every drive arm to be safe.
**Why:** paranoia.
**Why rejected:** blanking is only needed on cold start; per-session
blanking added ~25 ms of forced silence in normal operation. Restricted
in `[064]` to cold-start-only.
**Basis:** DESIGN CHOICE (perf vs safety).

### R33 — Not verifying LEDC after `attach()`
**Tried:** trusting `Servo::attach()` to have set 50 Hz / 14-bit.
**Why:** it's the wrapper's job.
**Why rejected:** wrapper silently resets timer width to 10 bits on
attach. Must call `setPeriodHertz(50)` + `setTimerWidth(14)` AFTER
attach, then `ledcReadFreq` / `ledcRead` to verify.
**Basis:** MEASURED (`[064]` audit).

### R34 — Route re-registration on server bounce
**Tried:** re-adding HTTP routes on server restart.
**Why:** thought handlers wouldn't survive `end()`.
**Why rejected:** they do survive `end()`/`begin()`. Double-registration
either fails or duplicates. Call `setupHttpRoutes()` **once** per boot.
`[061]`.
**Basis:** MEASURED.

### R35 — Trusting `WiFi.persistent(true)` (default)
**Tried:** default.
**Why:** default.
**Why rejected:** persistent WiFi config in NVS interacted with mode
changes during pairing and caused stale AP configs across reboots.
Call `WiFi.persistent(false)` before mode set. `[060–062]`.
**Basis:** MEASURED (across reboots).

### R36 — LOVE / CURIOUS in demo cycle
**Tried:** all 16 moods in `kDemoMoods`.
**Why:** completeness.
**Why rejected:** at demo pace they weren't visually distinct enough;
excluded in `[059]`. May still be incomplete — future audit could revisit.
**Basis:** DESIGN CHOICE.

### R37 — Old THINK placeholder
**Tried:** placeholder art for THINK mood.
**Why:** filler.
**Why rejected:** redesigned as SLEEPING dome-arc (closed eyes) `[060]`,
reusing SLEEPING's rasterization path.
**Basis:** DESIGN CHOICE.

### R38 — `AsyncWebServer` on port 81
**Tried:** port 81 while WiFiManager owned 80.
**Why:** WiFiManager captive portal.
**Why rejected:** WiFiManager itself was rejected (R3). Server returned
to port 80 `[063]`.
**Basis:** DESIGN CHOICE.

### R39 — `WS_MAX_QUEUED_MESSAGES` default (8) on ESPAsyncWebServer-esphome
**Tried:** default 8.
**Why:** it's the default.
**Why rejected:** control cadence + telemetry cadence together overran
the queue on the 4-client case. Uses **32** in this fork; verify if
switching forks.
**Basis:** ASSUMED (from fork docs) / DESIGN CHOICE.

### R40 — 2-second power-button long-press
**Tried:** original `[047]`.
**Why:** faster shutdown.
**Why rejected:** accidental triggers during phone-pairing swipes.
Raised to **3 seconds** in `[064]`.
**Basis:** MEASURED (user pattern).

### R41 — Off-tree servo research fleet as source of truth
**Tried:** 13 agents, 7 waves, 21 candidate servos `[064]`.
**Why:** hoped to find an exact-fit replacement for the MG90D pair.
**Why rejected (as source of truth):** no exact-fit knockout. Every
recommendation flagged as **"reference only, not verified against our
envelope."** Kept as a reference list; **do not treat as a purchasing
decision**.
**Basis:** ASSUMED (research pass).

### R42 — Signal notifier daemon
**Tried:** local notifier at `127.0.0.1:17283`.
**Why:** cross-session pings.
**Why rejected (currently):** daemon is down (`ECONNREFUSED`) `[065]`.
Not a hard rejection — deferred, not designed away.
**Basis:** MEASURED (connection refused).

### R43 — 10 kΩ pulldown on GPIO3 / BOOST_EN
**Tried (as a plan option):** hardware pulldown to prevent ROM-boot float
briefly enabling the boost.
**Why:** hardware defense-in-depth.
**Why rejected (deferred, not killed):** user chose software mitigation
(EN driven LOW very early in `setup()`). Physical pulldown is still a
correct fix and remains on the deferred list.
**Basis:** DESIGN CHOICE.

---

### R44 — Deferring `AsyncWebSocketClient::close()` to the loop task
**Tried:** commit `973a91a`, "never close a websocket client from the AsyncTCP
callback". A pending-close queue was filled from `orOnWsEvent` and drained by
`serviceWsPendingCloses()` in `loop()`.
**Why:** RELY-1/RELY-2 both argued that closing a client from inside its own
AsyncTCP callback re-enters its teardown, and blamed it for an INT_WDT reset.
The reasoning is plausible and reads correctly.
**Why rejected:** it is exactly backwards, and it crashed the board on the
FIRST phone connect. `AsyncWebSocketClient::_queueControl` does an
unsynchronised `_controlQueue.add()` (`AsyncWebSocket.cpp:561`,
`StringArray.h:76`); `close()` goes through it, and AsyncTCP calls the same
path from `_onData` on its own task. Moving closes to `loop()` therefore
converts a claimed re-entrancy hazard into a real cross-task data race on an
unlocked LinkedList, which corrupts the TLSF free list:

    Guru Meditation Error: Core 1 panic'ed (StoreProhibited)
    _async_service_task -> _s_recv -> _recv -> _onData
      -> _queueControl -> LinkedList::add -> operator new
      -> tlsf_malloc -> block_locate_free -> remove_free_block

Reverted in `118c5a3`. **The library is single-task by design: the
`AsyncWebSocket` object belongs to the AsyncTCP task.** The loop task
publishes state into atomics and never touches a client — which is the pattern
`publishWsTelemetrySnapshot()` already followed.
**Basis:** MEASURED (decoded backtrace, hardware, 2026-08-08).

---

### R45 — Closing a non-owner client from the WS data path
**Tried:** long-standing. `orOnWsEvent` called `client->close()` whenever a
frame arrived from a client that was no longer the transport owner.
**Why:** tidiness — evict a client that has lost its claim.
**Why rejected:** use-after-free. Leaving the AP kills TCP with no FIN, so the
socket lingers; the stale-owner reaper then revokes ownership, making that
once-legitimate client a non-owner. Its next late frame or RST hit `close()`
on a client AsyncTCP was already tearing down, and the following `_recv` read
freed memory:

    Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
    AsyncWebSocketClient::_onData (AsyncWebSocket.cpp:639)

Reported as "it reset when I switched networks". The code's own comment had
predicted this exact failure for months. Nothing needed the close:
`WsAuthGuardHandler` refuses unauthorized clients at the HTTP upgrade, and
rejected clients are closed in the CONNECT branch before they are live.
Ghost traffic is now dropped. **Consequence:** the app can no longer learn it
was revoked from the socket closing, so the web app gained a 4 s
telemetry-staleness watchdog that forces a reconnect.
**Basis:** MEASURED (decoded backtrace, hardware, 2026-08-08).

---

### R46 — Unsigned `(now - timestamp)` for any value written by another task
**Tried:** long-standing, in the stale-owner reaper:
`static_cast<uint32_t>(now - g_lastControlEventMs) > kWsOwnerSilenceTimeoutMs`.
**Why:** the standard Arduino rollover-safe idiom — which it is, but only when
both operands come from the same task.
**Why rejected:** `g_lastControlEventMs` is stamped by the AsyncTCP task when a
frame lands; `now` is captured by the loop task at the top of its pass. They run
concurrently, so a frame arriving in between is legitimately IN THE FUTURE.
Unsigned, the subtraction wraps to ~4.29e9, which beats any threshold:

    override: ws owner silent 4294967295ms -> releasing transport
    drive session: SAFE_OFF reason=ws owner silent

That cut drive power mid-run on a completely healthy session (seq 42→179 with
clean servo readbacks) and dropped the robot into GRACE. With control frames
streaming at 30–60 Hz it was near-inevitable. **Use a signed `int32_t` delta**,
so a negative value reads as "just heard from them" — which is also correct
across the 49.7-day `millis()` rollover. The other five `now - timestamp`
comparisons in `main.cpp` are safe because their timestamps are written by the
loop task itself.
**Basis:** MEASURED (serial capture, hardware, 2026-08-08).

---

### R47 — Suppressing the boot logo to hide a "network reset flash"
**Tried (proposed by the user, correctly withdrawn after evidence):** hide the
garage logo animation when the AP resets while already running.
**Why:** the logo appeared to flash on every network drop, which read as
cosmetic noise during normal reconnect cleanup.
**Why rejected:** `runStartupLogoAnimation()` is only reachable from `setup()`,
from `wake`, and from the power-off path. There is no route from a transport or
AP reset to it. **A logo flash therefore means the board rebooted**, and
suppressing it would have hidden a crash rather than fixed one — the same trap
as the black-screen investigation, where every "fix" appeared to work because
opening the serial port drained the CDC FIFO. The flash was real and was the
use-after-free in R45; once that was fixed the flash stopped.
**Basis:** MEASURED (call-graph + serial capture, 2026-08-08).

---

### R48 — Drag-to-commit LCD transitions (blocked by the touch driver mode)
**Tried:** full drag tracking for the pairing screens — content follows the
finger 1:1, a commit band appears past ~80 px, release either commits or snaps
back. Asked for directly: *"slide it gradually where it moves with the drag but
doesn't change until i let go and complete the action."*
**Why:** it is the correct interaction. A swipe was an instant, irreversible
event with no way to begin it, see what it would do, and change your mind
(design-craft §1 direct manipulation, §8 confirm-on-touch-up).
**Why blocked:** `CST816Touch_HWMode` never reports intermediate touch points.
Verified on hardware rather than assumed — a finger dragged across the panel
produced only three `touchlog` entries, all gesture IDs with `pressed=0`:

    #0 gid=1 xy=(63,128) pressed=0
    #1 gid=2 xy=(192,119) pressed=0
    #2 gid=1 xy=(49,108) pressed=0

Zero `gid=-1` touch points. The chip decides that a gesture happened and reports
only the outcome, so `touchNotification` never fires mid-drag and the drag
machinery receives no input at all.

`setNotificationsOnAllEvents` / `setNotifyOnMovement` / `setMovementInterval`
would fix it, but they are **protected** on `CST816Touch` and exposed only by
`CST816Touch_SWMode`. Switching modes replaces the chip's hardware gesture
detection with a software reimplementation — and this board's swipe IDs
(`kTouchSwipeDownId=1`, `kTouchSwipeUpId=2`) had to be calibrated on this exact
hardware because the chip's axes sit 90° off the panel. Risking the primary UI
for a transition was not judged worth it.

**Status:** the plumbing is in place and inert (`pushCanvasShiftedToLcd`,
`serviceDragTransition`, the drag state vars). It begins working the moment
intermediate points arrive. Three ways forward, none taken:
  a) switch to `CST816Touch_SWMode` and re-verify every gesture on hardware;
  b) read the CST816T coordinate registers directly from the loop while leaving
     the library to handle gestures — but that adds I2C traffic to a bus with an
     unexplained stall still under investigation;
  c) leave it, and keep the ordered-band reveal.
**Lesson worth keeping:** an API existing is not the same as the data arriving.
One `touchlog` read would have settled this before the state machine was
written.
**Basis:** MEASURED (hardware touch log, 2026-08-11).

---

### R49 — Deriving the touch coordinate frame from the chip's gesture IDs
**Tried:** `mapTouchToScreen` translated raw CST816T coordinates into screen
space by swapping and mirroring the axes:

    screenY = chipX;
    screenX = kScreenW - chipY;

**Why it was believed:** the swipe IDs on this board *were* calibrated on
hardware (`kTouchSwipeDownId=1` is the chip's `GESTURE_RIGHT`), and the
conclusion drawn from that was "the chip's axes sit 90° off the panel, so the
coordinates must be swapped too". The comment even flagged the mirror as the
one part "not pinned down by the swipe evidence" — and shipped it anyway,
reasoning that the gesture menu's full-width buttons meant X did not matter.

**Why it is wrong:** a gesture enumeration is not a coordinate frame. The
CST816T's gesture engine has its own orientation; its coordinate registers do
not share it. `touchaxis` on this hardware (2026-08-12):

    drag top -> bottom : chipY 6 -> 264,   chipX flat (111 -> 104)
    drag left -> right : chipX 91 -> 217,  chipY flat (176 -> 196)

The mapping is the **identity**. Which the display constructor already said:
`kLcdRotate == 0`, so screen space and panel space are the same frame and there
was never a rotation to undo.

**How it stayed hidden:** the pull-down sheet (R48's successor) *worked* — as
long as you swiped **sideways**. The operator had adapted to it and reported it
as "the swipe is rotated 90 degrees", which is exactly what it was. The
full-width-buttons argument also quietly broke the gesture menu: with X mapped
from the vertical axis, touches near the top or bottom of the panel clamped
outside the button's X range and were rejected outright.

**Also rejected, on the way to the fix:** logging raw coordinates from inside
`serviceDragTransition`. That function is gated on `LCD_MODE_FACE && ORS_IDLE`,
so the instant a drag committed and the AP came up, sampling stopped and the
measurement drag was never recorded. Diagnostics must not sit behind the state
they are diagnosing.

**Resolution:** identity mapping, plus a permanent `touchaxis` serial command
that captures raw samples over an 8 s window from `loop()` — state-independent,
non-blocking (a tight sampling loop in the command handler would blow the 5 s
`TASK_WDT`), and it prints the verdict rather than the raw numbers alone.
**Lesson worth keeping:** the sibling of R48. There, an API existing was not the
data arriving; here, a mapping inferred from an enumeration was not a measured
mapping. Both cost weeks and both were settled by one measurement. When a
comment admits a part is unverified, that is the bug — go and measure it.
**Basis:** MEASURED (`touchaxis`, hardware, 2026-08-12).

---

### R50 — ST7789 hardware vertical scroll for the sheet, and the 80 MHz "fix"
**Tried (a):** using the panel's hardware vertical scroll (`VSCRDEF` 0x33 /
`VSCRSADD` 0x37) to translate the pairing card over the face for free, on the
theory that "a sliding sheet is just scrolling long content".
**Why rejected:** there is exactly ONE scroll start register and it moves the
whole scroll area cyclically. It cannot composite two *independently* moving
layers — using it would slide the face along with the card, which is the one
thing the effect must not do. The "scroll + fill the seam" trick that makes
terminals and list views O(strip) instead of O(screen) only applies when the
entire viewport moves as one unit. Confirmed against the command semantics and
against LVGL, TFT_eSPI, Slint and embedded-graphics, none of which use hardware
scroll for this — they all software-compose.

**Tried (b):** raising the LCD SPI clock to buy back the cost of translating.
**Why rejected:** it is out of spec, and the project has already paid for that
mistake once. `ST7789V2_LCD.pdf` Table 6 (p.44) gives `TSCYCW` min 16 ns
(62.5 MHz) and `TSHW`/`TSLW` min 7 ns; at 80 MHz the cycle is 12.5 ns and each
half-cycle 6.25 ns, so both are violated. The ESP32-S3 SPI divider can only
produce 80 / 40 / 26.67 MHz, so **40 MHz is the fastest in-spec option** and
there is nothing between it and the ceiling.

This was nearly re-introduced. A research pass concluded the 62.5 MHz figure had
been "copied from an ESP32-classic codebase"; it had not — it is measured from
the part's own datasheet and is recorded above `kLcdSpiHzDefault`. 80 MHz was
tried on hardware, the panel came up clean, and the screen looked perfect, which
is *precisely* the evidence that cannot refute an intermittent timing violation:
the documented failure mode is "black or glitched after a reset, fine after a
power cycle", temperature- and lot-dependent. Reverted to 40 MHz.

**Measured while establishing the above** (new `lcdbench` command):

| push | rows | 40 MHz | 80 MHz |
|---|---|---|---|
| full  | 280 | 32.5 ms | 19.1 ms |
| half  | 140 | 16.3 ms |  9.6 ms |
| 8 rows|   8 |  0.90 ms|  0.52 ms|

Throughput is flat (~4.0 MB/s at 40 MHz) across every size, so there is no
meaningful per-push overhead — the cost is purely proportional to bytes, and
at 4.0 of 5.0 MB/s theoretical the wire is ~83% utilised. Two corrections fell
out of this: the previously quoted "68 ms full-screen push" was wrong (it was
scraped from a worst-case loop stall that included unrelated work), and a
"60 MHz" experiment that showed no change was explained by the divider snapping
it back to 40 MHz.

**Also rejected:** 2×2 subsampling during motion to cut bytes on the wire. The
ST7789 has no scaler, so filling an N-pixel region always costs N 16-bit words;
a half-size image would simply occupy a quarter of the area.

**What was done instead:** accept the cost and pay it only where it buys
something. `pushSheetStrip` gained a `srcOffset` so the moving layer is read at
(destination row + offset) — that is the difference between a *reveal* and a
*translation*. The static layer is still only repainted where it is newly
uncovered; only the moving card is fully repainted per frame. At 40 MHz that is
~32 ms fully extended and ~16 ms half-open, i.e. roughly 30–60 fps.
**Lesson worth keeping:** a research answer that contradicts a comment which
cites a datasheet table and a debugging campaign is the thing to distrust, not
the comment. And "it looks fine right now" is not evidence against an
intermittent violation of a timing spec.
**Basis:** MEASURED (`lcdbench` on hardware, 2026-08-12) + ST7789V2 datasheet
Table 6.

---

### R51 — A placeholder card behind the pull-down gesture
**Tried:** the sheet pulled down a "Pull down / to pair" splash, then handed off
to the QR screen once the AP was up.
**Why it failed:** two ways. It is a buffer page — the operator pulls down an
*announcement* of the destination instead of the destination — and once the
compositor started genuinely translating, a near-uniform card made the motion
invisible. A flat field sliding is indistinguishable from a flat field standing
still, and the only non-uniform content (labels at card rows 34 and 58) is not
uncovered by a downward translate until the last ~12% of the pull. Reported as
"you didn't apply it to the swipe down" when in fact the translation was
running correctly and there was simply nothing to see moving.
**Resolution:** the card IS the pairing screen. It is staged by drawing the QR
where the existing helpers already draw (`g_canvas`), copying that into the card
canvas, then repainting the face underneath — so no QR or text helper had to be
re-pointed at a different canvas. The same staging trick runs in reverse for
drag-to-dismiss.
**Consequence that had to be handled:** activation credentials are now generated
at drag start rather than at AP start, or the QR pulled into view would encode a
password `startWifiServer()` immediately replaced. That call is now guarded to
reuse any valid credentials, and teardown still invalidates them.
**Basis:** MEASURED (hardware, 2026-08-12).

---

## 4. Hardware facts and measured constants

### 4.1 Pin map (final)

| Function            | GPIO | Notes | Source |
|---------------------|------|-------|--------|
| Servo LEFT (wheel)  | **18** | Was swapped in code as 17 for months; fixed [065] | MEASURED |
| Servo RIGHT (wheel) | **17** | Fixed pair with above | MEASURED |
| BOOST_EN            | **3**  | LOW at boot; drives HIGH only when RUNNING/drive active | DESIGN |
| Piezo               | **42** | LEDC ch2 timer1 | MEASURED |
| Battery ADC         | **1**  | ×3 divider on-board | ASSUMED (schematic) |
| SYS_OUT             | **40** | NOT an RTC GPIO on S3 | DATASHEET |
| SYS_EN              | **41** | NOT an RTC GPIO on S3; USB VBUS bypasses this | DATASHEET |
| Touch INT           | **14** | CST816T interrupt | MEASURED |
| Touch RST           | **13** | CST816T reset | MEASURED |
| Touch SDA           | **11** | I²C bus | MEASURED |
| Touch SCL           | **10** | I²C bus, 400 kHz | MEASURED |
| Touch I²C addr      | 0x15 | CST816T | MEASURED |
| Free GPIOs          | 1, 2, 3, 16, 17, 18, 21, 33, 34, 43, 44 | Reserve for future | DATASHEET |

### 4.2 Screen and mascot

| Constant           | Value    | Source |
|--------------------|----------|--------|
| `kScreenW`         | 240      | LCD-1.69 spec |
| `kScreenH`         | 280      | LCD-1.69 spec |
| Mascot canvas W×H  | 220×216  | DESIGN CHOICE |
| Mascot origin (x,y)| (10, 32) | DESIGN CHOICE |
| `g_eyeBaseW`       | 12       | DESIGN CHOICE — do not confuse with 40 |
| `g_eyeBaseH`       | 36       | DESIGN CHOICE |
| `g_eyeRadius`      | 5        | DESIGN CHOICE |
| SURPRISED eye box  | 36×36 R=10 | asymmetric-cap [042 both, 044] |

### 4.3 Servo calibration (V3 microseconds, measured on bench)

| Wheel        | GPIO | Deadband (µs, inclusive) | Stop | Fwd start | Rev start | Fwd max | Rev max |
|--------------|------|--------------------------|------|-----------|-----------|---------|---------|
| LEFT         | 18   | 1423–1533                | 1478 | 1534      | 1422      | 2000    | 1000    |
| RIGHT        | 17   | 1423–1533                | 1478 | 1422      | 1534      | 1000    | 2000    |

**Basis:** MEASURED on hardware `[065]`.
**Important:** compare calibration values in **tick space**, not µs. PWM
is 14-bit @ 50 Hz LEDC → 1.2207 µs/tick. `1532 µs` and `1533 µs` quantize
to the same tick. Comparators using strict `>` in µs will look busted.

**NVS schema:** `us_a`, `us_b`, schema version 3. V2 keys `slot_a`,
`slot_b` are dead and their absence is verified in the safety layer.

### 4.4 Piezo (LEDC ch2 timer1 on GPIO42)

| Band (Hz)    | Character                     | Basis |
|--------------|-------------------------------|-------|
| 200–500      | Clicky / percussive           | MEASURED |
| 500–800      | Thin                          | MEASURED |
| 800–2000     | Solid tone                    | MEASURED |
| 2000–4000    | Loudest / most cutting        | MEASURED |
| 4000–6000    | Harsh                         | MEASURED |

**Serial `tone` command:** `tone 0` = off, else valid range 200–6000 Hz.
**Watchdog:** 750 ms silence after last host tone command.
**Host refresh cadence:** ~250 ms.
**Attack-driven click floor:** notes shorter than **~40 ms** read as
clicks, not tones (R17). Happy melody has 45–47 ms notes at the edge —
`[065]` open item, user cancelled the bench check twice.

### 4.5 LCD backlight

| Property | Value      | Source |
|----------|------------|--------|
| PWM      | LEDC ch4/timer2 (isolated) | DESIGN [064] |

Isolation is deliberate so BL PWM cannot shift servo timer0 or piezo
timer1 frequencies.

### 4.6 Battery / power

| Property                  | Value        | Source |
|---------------------------|--------------|--------|
| ADC pin                   | GPIO1        | schematic |
| Divider                   | ×3           | schematic |
| 100 %                     | 4.10 V       | DESIGN CHOICE |
| 0 %                       | 3.30 V       | DESIGN CHOICE |
| EMA α                     | 0.10         | DESIGN CHOICE |
| Slew clamp                | ±1 %/5 s     | DESIGN CHOICE |
| Boot fallback voltage     | 4.10 V       | DESIGN CHOICE |
| VBUS / charge sense       | **NONE**     | HARDWARE FACT |
| Power path                | SYS_OUT=40, SYS_EN=41 | schematic |
| USB behavior              | USB VBUS **bypasses** SYS_EN MOSFET | HARDWARE FACT |
| Long-press shutdown       | **3 s** (was 2 s in [047]) | DESIGN [064] |
| Battery slope detector    | raw-vs-raw consecutive samples (dual-EMA removed) | [049] |

### 4.7 WiFi / Override server

| Property         | Value                  | Source |
|------------------|------------------------|--------|
| SSID             | `Hanger OVERRIDE`      | DESIGN [056], reverted [065] |
| WPA2             | `TheGarageAtl`         | DESIGN [056], reverted [065] |
| Channel          | 6                      | DESIGN |
| TX               | 44 qdb (~11 dBm)       | DESIGN |
| Max clients      | 4                      | DESIGN |
| Server port      | **80** (briefly 81 in [052] under WiFiManager, back to 80 [063]) | DESIGN |
| WebSocket path   | `/ws`                  | DESIGN |
| URL join token   | rotates per activation | DESIGN [065] |
| Persistent WiFi  | `WiFi.persistent(false)` before mode set | MEASURED [060–062] |
| Fork WS queue    | `WS_MAX_QUEUED_MESSAGES=32` (ESPAsyncWebServer-esphome) | ASSUMED (from fork docs) |

### 4.8 Touch (CST816T)

| Property           | Value | Source |
|--------------------|-------|--------|
| I²C address        | 0x15  | MEASURED |
| INT                | GPIO14 | MEASURED |
| RST                | GPIO13 | MEASURED |
| SDA                | GPIO11 | MEASURED |
| SCL                | GPIO10 | MEASURED |
| Bus speed          | 400 kHz | DESIGN |
| Library            | `mjdonders/CST816_TouchLib@^2.2` | LOCKED |
| API entry          | `MDO::CST816Touch_HWMode`, `begin(Wire, &obs, INT, RST, ADDR)` | LOCKED |
| Poll                | `.control()` per loop | LOCKED |
| Observer overrides | `gestureNotification`, `touchNotification` | LOCKED |
| **Gesture ID for swipe-DOWN** | **`1`** (library's `GESTURE_RIGHT`) | MEASURED [062] |
| **Gesture ID for swipe-UP**   | **`2`** (library's `GESTURE_LEFT`)  | MEASURED [062] |
| Do NOT use          | id 3 / 4 (library's DOWN / UP)     | MEASURED [062] |

### 4.9 Serial ports and identities

| Device       | COM   | USB serial      | Basis |
|--------------|-------|-----------------|-------|
| Robot        | COM4  | `A0F262E4C344`  | MEASURED [049] |
| Test rig     | COM9  | `DCB4D923 22A0` | MEASURED [065] |
| **COM3**     | **REJECTED — Intel AMT SOL** | — | HARD RULE |

Auto-detect helpers must not scan COM3. Any new serial helper must set
`DtrEnable = $false; RtsEnable = $false;` **before** `Open()` or the
board will auto-reset (R23).

### 4.10 Servo power (external)

| Component              | Detail |
|------------------------|--------|
| Board's onboard 5 V    | **None** (only ETA6098 charger + ME6217C33M5G 3.3 V LDO) |
| Boost                  | XL63020 buck-boost, output 5 V |
| Boost enable           | GPIO3 (BOOST_EN), LOW = off, state-gated |
| Battery topology       | 2× LiPo in parallel via Y-splitter into existing JST |
| Wiring rule            | Boost OUT → **servos only**, NOT the Waveshare 5 V pad |
| Basis                  | DESIGN [059] §15; verified against schematic (ASSUMED for LDO/charger IDs; correct-per-schematic) |

### 4.11 Firmware structural constants

| Constant                            | Value | Note |
|-------------------------------------|-------|------|
| `kStepQuantumMs`                    | 10    | Movement-gesture load-bearing constant [065] |
| DriveSession arm frames             | 5     | Neutral frames to arm [063] |
| DriveSession watchdog               | 300 ms | Then EN cutoff [063] |
| Independent EN watchdog poll        | 10 ms `esp_timer` [064] |
| `g_leaseArmed`                      | 5 s wrap-safe lease | [064] |
| PWM commit wait after `attach()`    | 25 ms  | Verify with `ledcReadFreq`/`ledcRead` [064] |
| PROGMEM HTML budget                 | ~14.6 KB | fits in flash + free heap [054–056] |

---

## 5. Open and unresolved bugs

### 5.1 [OPEN] Checkpoint 062 — WiFi cycling breaks netstack on AP teardown

**Symptom (verbatim from serial):**
```
E wifi_init_default: netstack cb reg failed with 12308
```

**Pattern:**
```
AP_STADISCONNECTED → AP_STOP → AP_START → netstack cb reg failed → AP_STOP
```

Every AP teardown emits this. The `AP_START → netstack fail → AP_STOP`
subsequence appears to fire **inside** the call to
`WiFi.softAPdisconnect(true)` itself. That is confirmed by removing
`WiFi.mode(WIFI_OFF)` and `esp_wifi_stop()` from `stopWifiServer()` —
the error still appears with only `softAPdisconnect(true)` remaining.

**Client-side symptom:**
Phone joins the AP successfully. It receives a DHCP lease
(`AP_STAIPASSIGNED` fires with a valid IP). But no HTTP or TCP traffic
ever reaches `AsyncWebServer`, and `wsClients` from the 1 Hz
control-path diag stays flat at `0`. So the AP layer looks fine; the
network stack above is dead.

**Fix attempt 1 (`[062]`, FAILED):**
Removed `WiFi.mode(WIFI_OFF)` and `esp_wifi_stop()` from
`stopWifiServer()`. Serial still shows the same spurious
`AP_START → netstack fail → AP_STOP` sequence inside
`WiFi.softAPdisconnect(true)`.

**Fix attempt 2 (`[062]`, FAILED):**
Reordered teardown steps. Same result.

**Leading hypothesis at compaction:**
Try `WiFi.softAPdisconnect(false)` (leave the WiFi driver up; only
disassociate stations). Fallback: keep WiFi up forever, just
`server.end()` / `server.begin()` cycles. This is untested.

**Post-refactor uncertainty:**
`[063]` mentions "AP startup bounce fixed with event-sequenced startup
window" and moved AsyncWebServer from port 81 → 80 during a massive
safety refactor. **It is unclear whether that intervening work
incidentally suppresses or masks the 12308 error.** `stopWifiServer` is
still present in the code path per the line references in `[065]`.
Before writing any new pairing UX, a future agent MUST:

1. Reproduce 12308 on current firmware to confirm it's still there.
2. If still there, try `softAPdisconnect(false)` first.
3. If that fails, restructure so WiFi is not torn down at all — only
   the AsyncWebServer is bounced.

**Do NOT re-add `WiFi.mode(WIFI_OFF)` / `esp_wifi_stop()`** in
teardown — that path was measured and failed.

**Do NOT call `AsyncWebSocket::binaryAll()`** in the fix attempt (R7 /
D8) — a WS queue overflow will masquerade as this bug (`wsClients` will
flat-line) if `binaryAll` is used, and you'll chase the wrong wire.

---

### 5.2 [RESOLVED IN 044] Checkpoint 043 — SURPRISED stale-pixel bug

**Kept in this document because:** (a) the task spec asks for it in
full detail, and (b) it is the canonical example of the "believed
fixed" vs "fixed" distinction on this project.

**Symptom (`[043]`):**
Eye-coloured artifacts remained on the screen after mood transitions
**out of SURPRISED**. The artifacts sat at the edges of the SURPRISED
eye-outline where it had been drawn one frame prior.

**What was tried in [043] (all FAILED):**

1. **`sleep`/`wake` cycle** of the LCD around the transition — made the
   artifacts **worse** (extra artifacts appeared).
2. **Union-of-old-and-new eye-region bounds** for the dirty-rect clear —
   flashed at the end of `[043]`, did not fix the artifacts. The user
   confirmed on hardware.

**Investigation state at end of `[043]`:**

`EyeEngine` has a 3-state morph: `_currentMood`, `_targetMood`,
`_renderMood`. Multi-frame fade means `_renderMood` still equals the
**old** mood while `_currentOverlayScale` tweens from 255 → 0.
`main.cpp` moves the eye centers **BEFORE** calling `setMood`, so the
engine keeps painting SURPRISED's outer ring at the **NEW** default
centers — outside both the old and the new dirty rects. So the union
fix cannot cover it either.

There was also a `+8` vs `+10` pad discrepancy: the engine's SURPRISED
pad is `+8`, but `main.cpp` used `+10` when computing dirty rects — a
2 px slice never got cleared.

**Morph hypothesis being tested when [043] got cut off:**
That the fix needed to be **either** (a) synchronize eye-center moves
with `_renderMood` transitions (draw at old centers until fade
completes, then move), **or** (b) skip the fade entirely on
bounds-changing transitions.

**How it was actually fixed (`[044]`):**
`EyeEngine::snapMood(m)` — instant mood-state change bypassing the
multi-frame fade. Called from `setEyesMood()` **only** when the target
mood's bounds differ from the current mood's bounds
(SURPRISED ↔ any other mood). User visually confirmed on hardware —
this is a **FIXED**, not "believed fixed."

**Do NOT** "clean up" the code back to a single `setMood` path.
`snapMood` is load-bearing (D5). And do not add multi-frame morphing
to SURPRISED transitions on the theory that it should look prettier —
that was the original bug.

---

### 5.3 [OPEN] Movement gestures never physically tested `[065]`

Frustrated / excited / scared as net-zero wheel patterns are
implemented, wired to `writeDriveOutputLocked()`, and use
`kStepQuantumMs=10`. **Frustrated is queued as the first physical
test but has not been run.** No real-world observation exists for any
of the three yet. Sub-agent confidence in these designs is not
evidence (D24).

### 5.4 [OPEN] Happy melody's shortest notes at click floor `[065]`

Happy melody has 45–47 ms notes near the piezo attack-click floor
(~40 ms per R17). User **cancelled the bench check twice**. Whether
those notes read as tones or as clicks on hardware is **unknown**. If
they read as clicks, the melody is broken for the happy mood and
either the tempo must slow or the shortest notes must be lengthened.

### 5.5 [OPEN] Trail confusion gesture — undecided

`[065]` open item: the "trail confusion" motor gesture may or may not
be cut. Design not final.

### 5.6 [OPEN] Melodies still host-side; should move to firmware `[065]`

The generator lives on the host; the firmware receives commands. Bench
listening depends on the host tool. Migrating to firmware is deferred.

### 5.7 [OPEN] Signal notifier daemon down `[065]`

`ECONNREFUSED 127.0.0.1:17283`. Not a robot bug, but the notification
path used to ping the user across sessions is dead. Cross-session
`signal_notify` calls will fail.

### 5.8 [DEFERRED] GPIO3 / BOOST_EN pulldown `[064]`

ROM boot briefly floats GPIO3. Software mitigation (early `pinMode
OUTPUT` + `digitalWrite LOW` in `setup()`) is in place, but the
correct fix is a 10 kΩ pulldown resistor to GND on GPIO3. Deferred —
user chose software mitigation for now.

---

## 6. Working agreements with the user

The process rules the user established, kept in their own framing
where possible. These are as binding as the technical facts. Ignoring
them consumes more time than any physical bug on the list.

- **Never actuate hardware autonomously.** Drive stays LOCKED until
  the user confirms in the current turn that calibration is trusted
  and it is OK to move. This is the direct lesson from `[063]`'s
  safety overhaul and `[065]`'s pin-swap discovery.

- **Bench listening beats research inference.** Every time literature
  was cited to justify a melody design, the user's ear proved it
  wrong on the piezo (R11–R20). Cite the substrate (square-wave
  piezo, ~40 ms attack floor), not the theory.

- **Verify by independent measurement, never trust a sub-agent's
  self-report.** `[064]`: the generator sub-agent reported "76 tests
  passed" while the feature was broken. Rule: if a sub-agent claims
  success, inspect the artifact yourself or confirm on hardware
  (D24 / R29).

- **When loosening tests three times in a row to accommodate
  implementation, the implementation is wrong, not the tests.**
  Direct quote-of-intent from `[065]`. Applies to fixture design too.

- **All research and audit sub-agents run on GPT-5.6 Sol.**
  Established `[063+]` (D23). Do not silently switch models.

- **7-question `ask_user` before major features.** `[060]` locked
  the swipe-to-connect design this way. Choices become permanent
  once locked in that ritual.

- **Serial-number-pinned COM allowlist. COM3 is Intel AMT SOL.**
  Auto-detection scans must never touch COM3 (D14).

- **v-nisalazar gets frustrated when responses stall. Always emit
  visible text and a tool call within the first thinking pass.**
  Explicit rule from `[057]`. Silent thinking-only turns are a
  process failure.

- **Design DNA is a guardrail, not a generator.** Aesthetic tokens
  and content-density caps are for judgement; do not lint against
  them and do not auto-apply (D22 / R28).

- **Rubber-duck before build.** From the research-build-system
  playbook the user follows: articulate the mental model in prose
  before writing code, especially for anything hardware-adjacent.

- **Rotating URL tokens are OK; rotating credentials are not.**
  `[065]` reverted SSID/WPA rotation but kept URL token rotation. The
  user's threat model is neighbours on the AP, not URL replay.

- **Contradictions between plan and code: the plan is often right.**
  The old `plan.md §15g` had `LEFT=18, RIGHT=17` at the same time the
  code had it swapped. `[065]` fixed the code to match the plan.
  A future agent finding a plan-vs-code mismatch should verify on
  hardware, not just "clean up the plan."

- **Small, undoable steps.** Do not batch large hardware-adjacent
  edits without a verification stop after each. This is why the
  motor safety overhaul in `[063]` had a full audit pass in `[064]`.

- **The `docs\` folder is for durable references, not planning
  scratch.** Session-scratch belongs in `.copilot/session-state/`,
  not in the repo.

---

## 7. Glossary

**Bench listening** — the user (or agent) listening to the piezo on
the actual robot. The final ground truth for melody design
(§6, D25).

**BOOST_EN** — GPIO3. Drives the XL63020 buck-boost enable pin. LOW =
off; state-gated to only go HIGH when the OverrideState allows drive.

**cal-off / cal-status / robot-cal** — PowerShell wrapper scripts
around the calibration serial commands. Prefix chosen to make
tab-completion cheap.

**CST816T** — the capacitive touch controller on the LCD-1.69. I²C
0x15. Gesture IDs on this panel are 1 (down) / 2 (up), NOT the
library's 3/4 (§4.8, R27).

**DeskNotif** — the project's original name. Notifications-focused
mascot bot on LCD-1.28. All post-E7 work is on the LCD-1.69 as a
generic expressive desktop robot instead.

**Design DNA** — `web-design-dna` skill. Aesthetic ruleset for the
Override phone UI. Guardrail only (D22).

**DriveSession** — session-oriented drive command layer. Enforces
5-neutral-frames arm, 300 ms watchdog, ownership, and sequence-number
validation. See §4.11.

**ELIGIBLE** — status the drive layer returns when calibration is
valid and the session is armed. `[065]` shows `ELIGIBLE gen=1` after
the pin-swap fix.

**EyeEngine** — the mood rasterizer. Owns eye geometry, morph state
(`_currentMood`, `_targetMood`, `_renderMood`), and dirty-rect
management. `snapMood(m)` is the load-bearing bounds-change escape
hatch (D5, §5.2).

**forceRedraw** — public EyeEngine call to redraw the current mood
unconditionally, bypassing the dirty-rect optimization.

**GRACE** — OverrideState after a pairing session ends. Server stays
up for 30 s to allow reconnects, then returns to `IDLE_DEMO` (D18).

**Hanger OVERRIDE** — the softAP SSID. Fixed on purpose (D6, R8).

**LcdMode** / **OverrideState** — state machines for what the screen
is doing. `IDLE_DEMO ⇄ WIFI_BOOT → QR_WIFI → QR_URL → CONNECTED →
GRACE → IDLE_DEMO`.

**kDemoMoods** — the moods included in the idle demo cycle. Excludes
LOVE and CURIOUS `[059]`.

**kMoodCommands** — the parser tables for `mood <name>` serial /
WebSocket commands.

**kOpControl / kOpTelemetry / kOpTrim / kOpMood** — the 4 WebSocket
opcodes (0x01–0x04). §E18.

**kServoLeftPin / kServoRightPin** — final values are **18 / 17**
respectively. See `[065]` and §4.1.

**kStepQuantumMs** — 10 ms. Movement-gesture load-bearing timing
constant. Do not change casually.

**MG90D** — the continuous-rotation servos used in the differential
drive. V3 calibration in native µs (D10, §4.3).

**motorstate** — serial command dumping the current calibrated state,
session, and arm status.

**Override** — the phone-facing web control app served over softAP.
Binary WebSocket at `/ws` on port 80.

**Overdrive** — the pre-rewrite HTTP-polling version of the same app
(§E15). Not in use.

**pin17 / pin18** — the two servo GPIOs. Right / Left respectively
(§4.1).

**QMI8658C IMU** — the on-board IMU. Used historically for
gyro-based turn measurement; not currently in the movement-gesture
loop.

**QR_WIFI / QR_URL** — the two QR codes shown in pairing. WIFI at v4-M
scale 6 (67-byte capacity), URL at v2 scale 8. See D17 / R9.

**ricmoo/QRCode** — the QR library. Pinned at `^0.0.1`. Known
near-capacity encoding bug — do not push payloads above ~70 %
capacity (D17).

**RUNNING** — OverrideState in which drive is permitted. BOOST_EN
goes HIGH here.

**SafeOff** — servo-detach + drive-pin-LOW routine used by teardown
(D11).

**setMood** — normal mood-transition entry. Multi-frame fade.

**snapMood** — instant mood-transition entry. Load-bearing on
bounds-changing transitions (D5, §5.2).

**stopWifiServer** — the AP teardown routine at the heart of the
open netstack 12308 bug (§5.1).

**tone (serial command)** — `tone 0` = piezo off, else 200–6000 Hz.
Watchdog 750 ms, host refresh 250 ms (§4.4).

**touchedMask_** — provenance mask on the store-level field state,
added in the `[064]` safety audit.

**WIFI_BOOT** — transient state entered on swipe-down or paired
request. Advances to `QR_WIFI` once the server is up.

**Waveshare ESP32-S3-Touch-LCD-1.69** — the target board. 240×280
IPS + CST816T touch + ESP32-S3. No onboard 5 V boost; only ETA6098
charger + ME6217C33M5G 3.3 V LDO.

**XL63020** — external buck-boost providing 5 V to servos only
(D20, §4.10).

---

## Appendix A — Contradictions between this history and current code

A future reader may find things in the code that appear to contradict
older checkpoints. Where the code was corrected LATER, the code is
right. Where the code was regressed, the plan or an older checkpoint
is right. Enumerated:

1. **Servo pins.** Checkpoints `[049]` and `[063]` show
   `kServoLeftPin=17, kServoRightPin=18`. This was **swapped** in
   `[065]` after the physical pin-18 calibrator confirmed the wheels
   were mislabeled in code. Current code: **LEFT=18, RIGHT=17**.
   The older checkpoints will read as "wrong" — they aren't wrong for
   their era, they're just superseded. See §4.1, D9.

   > **Verified against the source 2026-08-07** —
   > `src/main.cpp:442-443` reads
   > `kServoLeftPin = 18; kServoRightPin = 17;`
   > This is CORRECT and must not be "fixed". Any quotation of
   > `17/18` elsewhere in this document is a historical record of the
   > bug, never an instruction. Changing it re-inverts differential
   > steering, and the failure is not obvious from a bench test that
   > only drives one wheel at a time.

2. **WiFi credentials.** Present-in-code briefly in `[063]` as
   rotating per activation. `[065]` reverted to fixed
   `Hanger OVERRIDE` / `TheGarageAtl`. Current code = fixed. See D6,
   R8.

3. **AsyncWebServer port.** 80 → briefly **81** in `[052]` under
   WiFiManager → back to **80** in `[063]`. Current code = 80.

4. **Calibration schema.** V2 `slot_a`/`slot_b` → V3 `us_a`/`us_b`
   (§4.3). V2 keys are erased and their absence is asserted in the
   safety layer. Any V2-key reference in older checkpoints is
   obsolete.

5. **Production raw servo commands `lservo`/`rservo`/`bservo`.**
   Present in `[049]`. Removed in `[064]` safety audit. Not in current
   code. See R24.

6. **Power hold long-press.** 2 s in `[047]` → **3 s** in `[064]`.
   Current code = 3 s. See D21, R40.

7. **THINK mood.** Placeholder in early moods era. Redesigned as
   SLEEPING dome-arc in `[060]`. Current code = SLEEPING variant.

8. **Battery slope detector.** Dual-EMA in `[048]` → raw-vs-raw
   consecutive in `[049]`. Current code = raw-vs-raw. See R26.

9. **`kTouchSwipeDownId` / `kTouchSwipeUpId`.** Initially set to 3 / 4
   from library constants `[061]`. Corrected to **1 / 2** in `[062]`.
   Current code = 1 / 2. See §4.8, R27.

10. **SURPRISED stale-pixel fix.** `[043]` believed union-of-bounds
    would work — it didn't. `[044]` fixed it via `snapMood()`. Any
    "clean up" toward a single-path `setMood` will reintroduce the
    bug. See §5.2, D5.

11. **`WiFi.mode(WIFI_OFF)` / `esp_wifi_stop()` in teardown.** Added
    in `[060]` as part of the pairing state machine. Removed in
    `[062]` because they didn't help — the error fires **inside**
    `softAPdisconnect(true)`. Removed calls remain removed. See §5.1,
    R6.

12. **`plan.md §15g`.** Had the correct pin mapping while the code
    disagreed. The plan was right; the code was wrong. Post-`[065]`
    they now agree. See D9.

---

## Appendix B — What a fresh session must not re-derive

A partial list of "we've been here before" traps, cross-referenced.
This list is what would have saved the hours the main agent burned
on the pin-swap re-debug and the WiFi-teardown re-debug:

- Do not try to stop a wheel by writing 0 or 1-999 µs to a
  continuous-rotation servo (R10, D11).
- Do not call `binaryAll()` on a shared WS send (R7, D8).
- Do not re-add `WIFI_OFF` / `esp_wifi_stop()` to AP teardown (R6).
- Do not push a QR above ~70 % capacity with the ricmoo library
  (R9, D17).
- Do not rotate the SSID / WPA — URL token only (R8, D6).
- Do not touch COM3 (D14).
- Do not open a `SerialPort` without setting DTR/RTS false first
  (R23, D15).
- Do not re-register HTTP routes on server bounce (R34, D16).
- Do not "clean up" `snapMood` back into `setMood` (R21 note, D5,
  §5.2).
- Do not "correct" `kServoLeftPin=18` back to 17 without confirming
  on the physical wheels (D9, §Appendix A #1).
- Do not accept a sub-agent's self-reported test pass without
  independent verification (D24, R29).
- Do not cite music theory to justify a piezo melody without
  bench-listening on hardware (R11–R20, D25).
- Do not attempt hardware actuation without in-turn user
  confirmation (D26, §6).
