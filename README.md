# desktop-robot

A second body for the DeskNotif mascot eye-engine, running on a
**Waveshare ESP32-S3-Touch-LCD-1.69** (rectangular 240×280 touch IPS,
onboard buzzer, RTC, IMU, LiPo charger, USB-C).

Same moods, same eye renderer, same serial CLI as DeskNotif — different
board.

## Hardware

See [`HARDWARE.md`](HARDWARE.md) for the full pin map, power-architecture
notes, and the critical "always drive SYS_EN HIGH in setup()" gotcha.

## Build & flash

```powershell
cd C:\Users\v-nisalazar\Desktop\Scripts\desktop-robot
$env:PATH = "$env:USERPROFILE\.platformio\penv\Scripts;$env:PATH"
pio run -t upload
pio device monitor -b 115200
```

First flash of a brand-new board: hold **BOOT**, tap **RESET**, release
**BOOT**, then `pio run -t upload`. After that, esptool auto-resets via
native USB.

## Garage startup, sleep, and shutdown

The original Microsoft Garage logo is embedded as a 240×280 grayscale asset.
On every boot—including USB insertion—the backlight fades up on the white
icon, holds briefly, then the face brightens in **behind** it while the logo
fades away as a foreground layer.

- `sleep`: powers the drive safely down, rejects WebSocket drive admission,
  darkens the face behind the foreground Garage logo as the logo fades in,
  then fades the backlight off.
- `wake`: fades the logo back into the face, then restores drive admission.
- Power button held for **3 seconds**: immediately safes motors, piezo, and
  Wi-Fi; cross-fades to the logo; fades the backlight off; then drops
  `SYS_EN`.
- On battery, dropping `SYS_EN` cuts physical power.
- On USB, the board remains electrically alive in soft-off. Release the
  button, then press it once to restart and replay the startup animation.

Software-only motor safety tests:

```powershell
.\test\run_motor_host_tests.ps1
py -3.12 -m unittest discover -s test -p "test_*.py"
cd ..\dual-servo-calibrator
.\test\run_host_tests.ps1
py -3.12 .\test\test_calibrator_contract.py -v
```

## Serial CLI (over native USB CDC)

```
help                     show this list
ping                     -> pong
status                   JSON snapshot
mood <name>              default|bored|annoyed|tired|angry|happy|sad|think|error
look <dir>               default|center|n|ne|e|se|s|sw|w|nw
blink
wander <on|off>
dims <w> <h> <r>
motion <x> <y>
blinkrate <sec> <jitter>
color <RRGGBB|named>
headgain <xDiv> <yDiv>
tone <hz>                exactly one argument: 0 (stop) or 200..6000
sleep | wake
demo                     cycle every mood + look
motorstate               JSON drive/calibration diagnostics in microseconds
cal show                 show the staged MG90D calibration
cal set <side> <field> <1000..2000>
                         side: left|right
                         field: stop|fwdstart|revstart|fwdmax|revmax
cal validate
cal save confirm         requires all ten fields set explicitly
cal reset confirm        start a runtime-only draft; retained NVS reloads on reboot
cal erase confirm        erase current and legacy calibration records
power                    battery latch (SYS_EN) + backlight + button state
sense [zero]             bump / shake / pick-up detection state and counters
scale check [revs]       arm the gyro scale measurement (needs a floor mark)
scale actual <deg>       enter the observed offset; computes and stores the fix
cod run|clear|status     the chip's own scale calibration (see the warning)
lpf on [mode]|off        gyro low-pass, for anti-aliasing not averaging
rfscan                   RSSI per AP at the current heading (robot must be stopped)
bench arm                arm the untethered measurement run (survives unplug)
bench report             dump the results of the last run
bench status|disarm|clear
heading [zero]           relative heading, gyro bias and drift state
turn <deg> [cw|ccw]      closed-loop turn: spins until the gyro says <deg>
lead                     show the coast lead constant and predicted overshoot
lead <sec>|off|reset     pin, disable, or restore the coast compensation
spin calib               probe, model, predict, verify, save (on-device)
spin auto [ms] [cw|ccw]  measure the turn with the gyro and stage the result
spin test [ms] [cw|ccw]  run one measurement turn (defaults to current value)
spin set <200..2000>     stage the measured full turn
spin show                staged vs saved vs compiled default
spin save confirm
spin erase confirm       return to the compiled default
servooff                 safe drive powerdown only
```

## On-screen menu

Swipe **up** from the idle face for the movement menu; swipe up again to leave.
(Swipe **down** is the WiFi/QR flow.)

Three buttons, one per expression. **The test button is gone** — instrumentation
does not belong on the face of a robot whose job is to emote, and a permanent
"360 test" next to three expressions invites a mis-tap.

A fourth button appears **only while a bench run or scale check is armed**, and
disappears once used. Those measurements have to be startable without a cable —
a tethered robot cannot complete a rotation, the USB lead restrains it — and
arming already persists across the unplug, so the button simply follows the
armed state.

**Touch no longer beeps.** That existed to verify the panel without a
synchronised serial capture, which mattered while the touch mapping was being
debugged and does not now. On a robot that expresses itself with sound, a beep
on every tap competes with what the piezo is actually for.

## Relative heading and closed-loop turns

The gyro can track how far the robot has rotated from wherever it was last
zeroed. There is no compass on this board, so this is **relative** heading —
good for "turn 90° from here", useless for "face north".

```
heading                  0.42 deg (axis X, bias 0.184 dps, zupt 7 applied, ...)
heading zero             re-zero at the current orientation
turn 360                 spin clockwise until the gyro reports a full turn
turn 90 ccw              quarter turn the other way
```

### Measured drift

Not a claim — measured on this robot with `heading`, stationary, on the correct
yaw axis. Two independent two-minute runs:

```
run 1:  -0.26  -0.09  +0.03  -0.08   0.00  -0.19   (bias drifted 0.28 -> 0.50 dps)
run 2:  +0.33  +0.09  -0.22  -0.07  +0.03  -0.08   (bias drifted 0.14 -> 0.06 dps)
        t+20s  t+40s  t+60s  t+80s  t+100s t+120s
```

Two things to read here. The heading stays inside **±0.33° across both runs**
and oscillates around zero rather than climbing — it is bounded, not merely
slow. And it did that *while the gyro's own bias moved* by 0.22 dps in one run
and 0.08 dps the other way in the second. Tracking a moving bias without letting
it reach the heading is precisely what the zero-velocity update is for, and
these runs are it working.

Over a 1.3 s turn the contribution is unmeasurable.

**Trap — the bias must be estimated on every axis the projection reads.**
Because heading is projected onto gravity it is a weighted sum of all three
gyro axes, so all three biases reach it. Correcting only the dominant axis
leaves the other two leaking in at their gravity weight. On this board, tilted
0.27 g in Z, that is about a quarter of the Z bias — measured as **3.39° of
drift over 65 s**, monotonic and accumulating, versus the bounded run above
once all three axes were estimated. The single-axis version looked healthy
because it never read Y or Z at all; adding the projection is what made their
bias matter. `heading` prints applied *and* rejected update counts so this
stays visible.

**Trap:** the yaw axis must be established at boot from the gravity vector.
A stationary robot looks perfectly healthy on the *wrong* axis, because every
axis reads about zero when nothing is moving — the error only appears once it
rotates. `heading` prints the axis it is using for exactly this reason.

### Why the drift is bounded

Integrating a rate always drifts. Two things keep it small enough to ignore
here:

- **The integration windows are short.** A full turn takes about 1.3 s, so even
  a sloppy 0.5 °/s residual bias contributes under a degree of error. Drift
  only matters if you integrate for minutes, and nothing here does.
- **Zero-velocity updates.** Whenever the drive rail is off and the measured
  rate is inside ±8 °/s, the robot is known to be stationary, so the gyro's
  own reading *is* the bias. It gets re-learned every 2 s of stillness — and
  this robot is stationary far more than it moves. `heading` reports how many
  updates have been applied and the last value learned.

  The window is also gated on **spread**, not just magnitude. A robot whose bias
  has drifted reads a large but *steady* rate, and it needs a new estimate; a
  robot being nudged reads a *varying* rate and must be rejected. A single
  magnitude threshold cannot tell those apart, and tightening one would lock out
  the update exactly when drift had made it necessary. `heading` reports
  rejected windows alongside applied ones.

A sequence like *360° right → pause → 360° left* is therefore self-correcting:
each turn is too short to drift meaningfully, and the pause between them
re-zeros the bias before the second one starts.

### Measuring movement: the bench run

**Movement cannot be measured while tethered, and the numbers you get if you try
are wrong in ways that do not announce themselves.**

- The USB cable measurably restrains rotation.
- On its stand the wheels leave the friction path and the board tilt roughly
  doubles (~8° on the desk, ~16.5° on the stand), which corrupts both the coast
  figure and the gravity projection.
- Reconnecting the serial port **resets the board** (DTR/RTS), so results held
  in RAM are destroyed by the act of going to read them.

Together those mean every real measurement costs supervised, untethered human
time — so the run is built to spend that time once and extract everything,
rather than asking for a separate session per question.

```
bench arm            (tethered)
  -> unplug USB; the robot stays up on battery
  -> put it on the desk, on its wheels, with clear space around it
  -> press the TEST button; leave it alone for ~2 minutes
  -> plug back in
bench report         (tethered)
```

One run measures, from the same turns:

| Phase | What it yields |
|---|---|
| 30 s stationary hold | Desk pose (gravity, projection gain), drift, bias, noise spread |
| 3 pairs of 180° at speed 100, lead off | Coast and rate at full speed; baseline turn error |
| 3 pairs at speed 75, lead off | Coast vs rate — is coast linear or quadratic in speed? |
| 3 pairs at speed 55, lead off | Third point for the fit; untethered rate model |
| 3 pairs at speed 100, lead on | The A/B that says whether coast compensation actually works |

Turns are issued as clockwise/counter-clockwise **pairs**, so the robot returns
to where it started after each repeat and stays put on the desk.

Results are written to NVS as they are taken, so an aborted run still yields
whatever it got to, and `bench report` computes the lead-on/lead-off comparison
and the measured coast constant on-device rather than leaving it to arithmetic
by hand.

The armed flag lives in NVS too, so it survives the unplug and any power-cycle,
and it is **consumed on use** — the next TEST press does the ordinary short
sequence again.


### Driving straight, and the autonomous RSSI survey

**Rotating in place produces exactly zero RSSI information** — the antenna's
range to every access point is unchanged, so there is nothing to measure.
**Translation does change range**, so it is the only motion that can reveal
anything. Hence a survey that drives.

```
straight <cm> [speed]        drive roughly straight, holding heading on the gyro
straight cal <actual cm>     tell it how far it REALLY went, after measuring
survey start [sideCm] [scans]  drive an equilateral triangle, sampling at each vertex
rfsurvey <label> [scans]     sample RSSI at one position by hand
rfsurvey report              the matrix, plus signal-vs-noise per position pair
```

⚠️ **The survey needs clear floor. It will drive off a desk.**

#### What the robot can and cannot know

The gyro holds heading, so the robot drives roughly straight and turns
accurately between legs. Distance is measured two independent ways (below) —
neither of them precise, but their disagreement detects wheel slip.

The default of 20 cm/s is derived rather than guessed: spinning at command 100
measures ~286 °/s = 4.99 rad/s, and for a differential drive pivoting about its
centre `v = ω·b/2`, so an 8 cm wheelbase gives 20 cm/s. **It still must be
calibrated with a tape measure**, because slip while pivoting is not the same
as slip while rolling.

Heading hold also corrects *heading*, not *lateral position*. A robot running
perfectly parallel to its intended course but displaced sideways looks identical
to one exactly on course.

#### Distance: two independent estimates, deliberately not averaged

The robot measures each leg **twice**, and reports both:

```
straight: commanded 100 cm | imu 94.2 cm (raw 111.8, residual 0.012 m/s, 2431 samples over 5.02s)
straight: imu/commanded = 0.94 -> estimates agree
```

**Time × calibrated speed** is simple but physically cannot see wheel slip —
spinning wheels take exactly as long as rolling ones.

**Double-integrated acceleration** can see slip, and normally would be useless:
position error is `½·a·t²`, so this part's ±100 mg offset spec would put **12 m**
of error on a 1 m leg, and a 1° tilt error — indistinguishable from real
acceleration — adds 2.1 m.

What rescues it is that **the robot stops at both ends of every leg**. `v_end = 0`
is known, so any leftover velocity is pure accumulated error; removing it also
removes the average acceleration error that produced it. What survives is bias
*instability* and tilt *drift* rather than bias and tilt:

| Residual | Error over a 5 s leg |
|---|---|
| 0.1 mg bias instability | 1.2 cm |
| 0.05° tilt drift | 10.7 cm |
| sensor noise | 0.9 cm |

So ~10–30 cm on a 1 m leg. **No better than time × speed** — and that is not why
it is here. It is here because it is *independent*: when the two disagree, the
wheels turned without the robot moving. **Their disagreement is the slip
measurement**, which is why they are printed side by side and never blended.

`residual` is the honest confidence signal. A large leftover velocity means the
leg's inertial estimate should not be trusted, and it is reported rather than
silently absorbed by the correction.


#### Why a triangle, and what the fourth sample is for

Equilateral, so each exterior turn is 120° and three legs return the robot to
its start. That makes the closing position free information: sampling there and
comparing with vertex 0 measures how far dead reckoning drifted over the whole
circuit — the only check available without encoders.

#### What the survey is actually testing

One question: **does moving change RSSI by more than the scatter at a fixed
point?**

| Baseline | predicted signal (AP @5m, n=2.5) | measured fixed-point noise |
|---|---|---|
| 0.3 m | 0.63 dB | **1–2 dB** |
| 1.0 m | 1.98 dB | 1–2 dB |
| 2.0 m | 3.65 dB | 1–2 dB |

At 0.3 m the signal is below the noise, which is why the default side is 1 m and
longer is better. `rfsurvey report` states the verdict per AP per position pair:

```
   v0->v1  delta +1.85 dB vs spread 1.10  -> SIGNAL
   v0->v2  delta +0.40 dB vs spread 1.10  -> below noise
```

Averaging suppresses *temporal* noise as 1/√N. It does **not** touch multipath,
which is spatially fixed — a standing-wave offset of several dB at a given point
that is indistinguishable from a range change. That is the floor this technique
cannot get under, and it is why indoor RSSI ranging plateaus at metre-scale
error.

**Honest expectation:** even at 1 m baselines with heavy averaging, bearing to an
AP works out around ±7°, against sub-degree from the gyro. This is not a
competitor for heading. It is the only candidate for *absolute position*, which
the IMU cannot supply at any accuracy.


### Bump, shake and pick-up detection

All three come from the same accelerometer. What separates them is **signature,
not magnitude** — and getting that distinction right is the whole design.

```
sense              live tilt / magnitude / linear-g, plus event counts
sense zero         re-take the reference at the current pose
```

| Event | Signature | Threshold |
|---|---|---|
| **Bump** | one sharp spike, ~50 ms, **while driving** | 0.25 g |
| **Shake** | sustained **alternation**, 3–8 Hz | 6 sign changes/s |
| **Lifted** | gravity changes and **stays** changed | 15° or 0.15 g, held 300 ms |
| **Placed** | quiet again after a lift | 600 ms settled |

Where the numbers come from — measured or derived, not guessed:

- Hitting a wall at 20 cm/s and stopping in 50 ms is **0.41 g**. Servo vibration
  measures under 0.05 g. So 0.25 g separates them with ~4× margin. Confirmed at
  rest: linear reads **0.002 g**, over 100× below threshold.
- Hand shaking runs 3–8 Hz = 8–16 sign changes per second; a bump gives one or
  two. Detection keys on **alternation rather than amplitude**, which matters
  because a real shake reaches 2–4 g and the accelerometer is configured for
  ±2 g — so it saturates. A clipped waveform still alternates at the same rate,
  and there is a host test that proves detection survives clipping.
- **Duration is the primary discriminator between a bump and a pick-up**: ~50 ms
  versus 300–1000 ms.
- Tilt is measured against **the robot's own resting vector** (~(0.98, 0.08,
  −0.26)), not an assumed flat orientation, or a healthy robot would look
  permanently tipped over.

Each event does something, not just print:

- **Bump during a straight run** → stop. The commanded motion no longer
  describes reality, and driving into an obstacle stalls the servos, which the
  vendor warns damages them.
- **Lifted while moving** → stop, rather than spin the wheels in mid-air.
- **Placed** → re-establish the yaw axis and re-zero heading. After a carry the
  gravity reference is stale and the robot may be pointing anywhere; integrating
  from the old reference would be integrating from a fiction.

14 host tests cover the discrimination, including the negatives that matter:
a stationary knock is not a bump, servo vibration is not a bump, a single impact
is not a shake, a brief tilt is not a lift, and a still robot reports nothing at
all.

**Naming note:** the command is `sense`, not `motion` — `motion` was already the
eye-engine's gaze-range command and silently shadowed it.


### Averaging, and why it does *not* fix drift

The instinct that "take more samples and average" reduces error is right — but only
for one of the three error types, and **not** for the one that matters most here.

**Averaging before integration buys exactly nothing.** Verified numerically:

```
average  1 sample  then integrate -> angle error 48.4 millideg
average  4 samples then integrate -> angle error 48.1 millideg
average 16 samples then integrate -> angle error 52.3 millideg
average 64 samples then integrate -> angle error 48.3 millideg
theory: sigma*sqrt(N)*dt = 50.0 millideg, independent of block size
```

The reason is that **integration is already an average**. Heading is `Σ ω·Δt`,
which sums every sample; pre-averaging groups of them changes nothing about the
sum. Angle error from white noise is `σ·√(T·Δt)` either way — this is *angle
random walk*, and it is a property of the sensor and the elapsed time, not of
how the samples were grouped on the way in.

Where averaging genuinely helps is **estimating a constant**:

| Samples | Bias estimate error |
|---|---|
| 1 | 0.50 dps |
| 32 | 0.088 dps |
| 256 | 0.031 dps |

That is what the zero-velocity update does, and why it uses ≥32 samples.

But averaging only helps until **bias instability** dominates — white noise falls
as 1/√T while bias instability is flat, so their sum has a minimum. Past that
point, longer averaging makes the estimate *worse*, because the thing being
estimated has moved. That minimum is what an Allan-variance plot finds, and it
is the real noise floor of any gyro.

So the three places averaging is used here are the three places it works:

| Use | Samples | Why it works |
|---|---|---|
| ZUPT bias estimate | ≥32 | Estimating a constant |
| Coast-lead rate | 4 | Instantaneous decision, not integrated |
| RSSI per survey position | 12+ | Estimating a constant |

And the place it is deliberately *not* used — before integrating heading —
is the place where it provably cannot help.

The one filtering technique that is **not** averaging, and does help, is
anti-aliasing (`lpf on`): servo vibration above Nyquist folds down into the
signal band as a false low-frequency rate, and once aliased no amount of
averaging can remove it.

### Measuring the gyro's scale — the one error needing outside help

Bias is visible at standstill. Coast is visible after the motors cut. **Scale is
visible neither way** — a 3% scale error produces 3% of zero at zero rate, which
is zero. It takes exactly one external observation: a mark on the floor.

```
scale check 3          arm a 3-revolution check
  -> mark the floor at the robot's front
  -> unplug USB, press TEST, it spins and stops
  -> measure how far PAST the mark it stopped
scale actual <deg>     negative if it stopped short
scale status | scale reset
```

**Why multiple revolutions:** the scale error accumulates every turn while your
ability to read the mark stays the same.

| Revolutions | Error if scale is 3% off | Coast residual |
|---|---|---|
| 1 | 11° | 0.5° |
| **3** | **32°** | 0.5° |
| 5 | 54° | 0.5° |

Three turns makes even a small error obvious, and leaves the 0.5° coast residual
negligible by comparison.

The arithmetic is just a ratio — the gyro claims `revs × 360`, the floor says
`revs × 360 + offset`, and the correction is one divided by the other. It is
applied in the single place raw counts become degrees, so everything downstream
inherits it and nothing can bypass it.

**It also fixes up what it invalidates.** Coast `α` is measured *through* the
gyro (`α = ω²/(2·coast)`, and both terms scale together), so applying a scale
correction rescales `α` by the same factor automatically rather than silently
invalidating three bench runs of tuning.

**Preferred over the chip's own COD**, because the provenance is known: this is a
number measured against a physical reference, not an opaque gain whose encoding
had to be guessed. See `docs/bench-run-004.md` for what happened when COD was
trusted blind.

**Result of the first check** (`docs/scale-check-001.md`): 3 revolutions, offset
from the mark **1–2°** ⇒ scale error **< 0.28%, i.e. < 1.0° per 360°** — about
11× inside the datasheet tolerance. **No correction was applied**, and that is
the right outcome: the implied error (0.185%) is smaller than the resolution of
the measurement that produced it (0.278%), so storing it would encode reading
error as a sensor property. The check reports a *bound* instead, which is the
stronger claim. The firmware distinguishes "never checked" from "checked and
found good" rather than showing both as a correction of 1.0.

This also settles COD: it wanted to rescale by 15.2%, which over 3 revolutions
would have shown as **164° off the mark**. Observed: 1–2°.

**The robot also walks slightly while it spins** — 2.2 cm over 3 revolutions,
~0.75 cm per turn. Notably this is *not* a rotation-centre offset, and not even
a steady wheel mismatch: both of those still return to the start after whole
revolutions. It is either slip or the non-steady ramp/coast phases, and this
measurement cannot separate them. The gyro cannot see it either way — it
measures rotation, and this is translation. 0.7% of a survey leg, so recorded
rather than acted on.


### The chip's own calibration (COD) — measured, and correctly rejected

Scale factor is the conversion from raw counts to degrees/second. The datasheet
tolerance is **±3%**, up to **±10.8° per 360°**.

The QMI8658C can calibrate its own gyro sensitivity:

```
cod run        ~2 s, must be perfectly still; reports vibration as a failure
cod status
cod clear
```

The measured gains are **volatile on the device** — lost on every power cycle —
so they are persisted to NVS and re-applied automatically at boot.

⚠️ **COD is OFF, and the floor says it should stay off.** Run 004 measured what
it actually did: it changed the gyro scale by **15%**, 5× the ±3% datasheet
tolerance. A subsequent 3-revolution check against a floor mark then showed the
**factory scale was already correct to within ~0.3%** — so COD was not fixing a
real error, it was introducing one.

Proof it changed the scale, from comparing runs 003 and 004 where nothing
physical changed:

| Speed | rate before | rate after | ratio | α before | α after | ratio |
|---|---|---|---|---|---|---|
| 100 | 283.2 | 240.2 | **0.848** | 2304 | 1971 | **0.855** |

A scale change by `k` moves rate and α by the *same* factor — and it did. The
post-COD A/B read a flawless **+0.02°**, but **that error is measured in gyro
units, which are exactly what changed.** An instrument cannot detect an error in
its own scale. That number was the most convincing wrong result of the project.

⚠️ **Running COD also invalidates previously measured constants.** If scale
changes by `k`, then `α = ω²/(2·coast)` scales as `k·α`, and the calibrated full
turn changes too — both are measured *through* the gyro. Re-run a bench run
after any `cod run`.


### Coast compensation

Stopping the motors does not stop the robot. **Measured on the desk,
untethered, it coasts 16.5° past the point where power was cut** at full speed —
roughly twice the 7–9° the compensation was originally designed against, and by
far the largest error on the robot. The accuracy limit is mechanical, not
inertial.

The coast is **not proportional to rate**. Measured across two untethered runs:

| Speed | Rate | Coast | `coast/rate` | `alpha = ω²/2c` |
|---|---|---|---|---|
| 100 | 283 dps | 17.40° | 0.0614 s | 2304 |
| 75 | 195 dps | 8.76° | 0.0449 s | 2169 |
| 55 | 136 dps | 5.36° | 0.0394 s | 1727 |

That `coast/rate` column varies 1.7×, so no single lead *time* can fit it.
Coulomb friction — `coast = ω²/(2·α)` — does, and reproduces across runs to
within 1.3% at speeds 75 and 55.

**α = 2600 deg/s²** is what the controller uses. It is set from the closed-loop
A/B rather than from the table above: running live at α = 2100 measured a
−4.49° error, i.e. it over-led and *undershot*. Extrapolating that says ~2750
would zero it, while the table says ~2300; 2600 sits between, and both estimates
put the residual under 2°.

The tie-break is an asymmetry worth remembering: **higher α means less predicted
coast, so a later stop, so a small overshoot** — which is recoverable.
Under-shooting is not, because correcting it needs a nudge smaller than the
servo deadband can produce. **When in doubt, lead less.**

The controller stops early by the predicted coast:

```
lead                     alpha 2600 deg/s2 -> 15.4 deg predicted coast at 283 dps
lead off                 stop on the measured angle, as before
```

Two properties matter:

- It is driven by the **measured gyro rate**, not the commanded speed. A
  restrained, slipping or low-battery chassis turns slower, so it coasts less,
  so it leads less — automatically.
- It **learns**. Every turn measures its own coast, so α is refined from
  `ω²/(2·coast)` after each one. The raw coast is still printed, so the
  compensation stays visible rather than silently folded in.

Measured effect at full speed, across three supervised runs:

| Run | α | lead-off | lead-on |
|---|---|---|---|
| 001 | 0.030 s (linear) | +16.51° | +6.83° (under-corrected) |
| 002 | 2100 | +17.57° | −4.49° (over-corrected) |
| 003 | **2600** | +16.67° | **−0.49°** |

**97% reduction, half a degree of residual.** See `docs/bench-run-00{1,2,3}.md`.

### Why closed-loop beats the timed turn

`spin calib` measures a duration. A duration is only correct for the conditions
it was measured in — battery level, surface grip, motor temperature and wheel
slip all change it. A closed-loop turn stops when the gyro says the angle has
been swept, so it stays correct as those change. It also beats wheel odometry
outright here, because the gyro measures actual body rotation and is not fooled
by the wheel slip this chassis has.

The saved full-turn value is still used — as a starting estimate and to derive
the timeout, rather than as the thing accuracy depends on.

### The safety tradeoff, stated plainly

Gesture patterns are net-zero **by construction**: their durations cancel, and
the host tests prove it arithmetically. A closed-loop turn is net-zero **by
measurement**, which is more accurate in reality but only as trustworthy as the
sensor. So every closed-loop turn carries a hard timeout (1.6× the predicted
duration, clamped to the spin range) and drives through the ordinary spin
pattern, inheriting the drive mutex, the 300 ms hardware lease, the emergency
latch and the clean powerdown. A wedged or disconnected gyro can stall the turn
but can never keep the motors running.

`turn` reports what actually happened either way:

```
turn: reached after 360.4 deg (target 360, 1204 samples)
turn: TIMED OUT after 86.2 deg (target 360, 1580 samples)
```

## Full-turn (spin) calibration


`excited` opens and closes with a full 360. That turn length was originally
written as `kHalfTurnMs * 2` — the frustrated bench value, doubled. It was
never measured, and it under-rotates: a full turn is not two half turns,
because both pay the same standing-start acceleration.

Measure it directly. **Do this unplugged** — a USB-tethered robot cannot
complete a pivot at all (see HARDWARE.md); the cable restrains it, the turn
plateaus around 50–100° and repeats vary by 2×.

**On-device:** swipe up from the face and tap the amber **`360 test`** button.
It runs a full turn clockwise, pauses to re-learn the gyro bias from a
standstill, then a full turn back — the most demanding thing the robot is asked
to do. The footer shows each result and the leftover error:

```
turn 1: 360 cw ...
cw +360, rezeroing ...
turn 2: 360 ccw ...
+360 -359  err +1
```

Both turns are **closed-loop** — each stops when the gyro says 360° has been
swept, not when a timer expires. Splitting it into two separate turns with a
pause between is what keeps it accurate: each integration window is only ~1.3 s,
and the pause re-zeros the bias so the second turn does not inherit the first
one's error.

`STALLED at <n> deg` means a turn hit its timeout without reaching 360° — the
chassis could not complete the rotation. Tethered, that is expected: the USB
cable physically restrains it.

The same sequence is on serial as `test`.

**Measuring the turn duration** (`spin calib`) now lives on serial only, since
the button was repurposed. That matters less than it used to: closed-loop turns
do not depend on the saved duration for accuracy, only to derive a sensible
timeout. The measured 1260 ms is already saved.

**Over serial** (only useful if the robot can pivot freely while connected):

```
spin auto            spin, measure with the gyro, stage the corrected value
spin auto            run again to confirm it converged
spin save confirm
```

`spin auto` reports what it actually did, so nothing is taken on trust:

```
spin auto: rest g=(1.01,0.12,-0.09) -> yaw axis X; bias=(0.07,3.63,0.55) dps over 200 samples
spin: 2000 ms cw
spin auto: rotation X=-106.8 Y=5.8 Z=10.7 deg (yaw=X)
spin auto: turned -106.8 deg in 2000 ms (2130 samples, mean 1.1 ms, worst gap 27.4 ms)
ERR a full turn needs ~6744 ms, outside the 200..2000 ms range; nothing staged.
```

All three axes are reported, not just the chosen one: a wrong axis and a slow
chassis both look like "a small number", and comparing the axes is what tells
them apart.

A correction pinned at a range limit is reported as a **failure**, never as
convergence. Clamping twice returns the same value, which reads exactly like a
fixed point — this genuinely misfired once, reporting "converged at 2000 ms"
for a chassis that never completed a turn.

### Why a gyro and not a compass

The QMI8658C is a **6-axis** IMU — accelerometer and gyroscope, no
magnetometer. That is not a limitation here: a compass would be the *worse*
instrument. Two DC motors, a LiPo under load and steel gearing sit inches away,
and all three corrupt a magnetic heading. A gyro measures angular *rate*
directly, and over the second or two a spin lasts, integration drift is
negligible.

Three things that would each produce a confident wrong answer, and what is done
about them:

- **Clipping.** A spin can exceed 600 °/s. A gyro range set too low saturates
  and silently *under*-reads, which looks like a valid measurement. The range is
  ±2048 dps and any near-full-scale sample marks the run and rejects it.
- **Yaw axis.** The board is mounted upright, so yaw is *not* simply Z. The
  vertical axis is found from gravity at rest rather than hardcoded.
- **Zero-rate bias.** A gyro at rest does not read zero. 2 °/s of uncorrected
  offset over a 1.2 s spin is 2.4° of pure error, so the bias is sampled while
  stationary and subtracted.

The measurement also continues through the coast after the last commanded step,
because where the robot comes to rest is what a person judges as "did it turn
all the way".

If the IMU fails to initialise, `spin auto` refuses and says so; the manual
`spin test` loop below still works.

### Manual fallback

```
spin test 600        run one turn, watch how far it actually goes
spin test 900        adjust and repeat until it is a true 360
spin set 900
spin save confirm
```

`spin test` with no argument reuses the value currently in effect, so the loop
is test → observe → `spin set` → test. The staged value takes effect
immediately for `excited`, including the on-screen button; `spin save confirm`
is what survives a reboot.

Calibrate a **single continuous turn from a standstill**. That is exactly what
the *opening* turn does — the rail is off and the wheels start at rest. The
*closing* turn is entered from the last bob (`{-85,-85}`), so the right wheel
reverses fully while the left barely changes; it accelerates differently and
rotates slightly less for the same duration. Net-zero is unaffected, because
that is a property of matched *durations*, not of actual rotation.

Do not derive the value from a multi-turn run either: consecutive turns
amortise one acceleration between them, so ms-per-turn measured that way is
shorter than a single turn needs.

The measurement spin is deliberately **not** net-zero — it rotates one way so
it can be observed. It still pivots in place, and it runs through the same
sequencer and canonical drive output as a gesture, so it inherits the drive
mutex, the 300 ms hardware lease, the emergency latch and the clean powerdown.
Gestures cannot opt out of net-zero by accident: `Balance::NetZero` is the
default and only a deliberate measurement passes `Balance::Unbalanced`.

Storage is a single checksummed NVS record in its own `spincal` namespace,
**not** the A/B verified machinery used for servo pulse limits. That is
deliberate: a wrong pulse limit drives the motors outside their safe band,
whereas a wrong turn length only makes a gesture rotate the wrong amount. Any
record failing its magic, version, checksum or range check is treated as absent
and the compiled default is used, so a corrupt record can never become a
plausible wrong number.

## MG90D drive calibration

Both continuous-rotation MG90D motors run at **50 Hz** with native
**1000–2000 µs** commands and ESP32-S3 **14-bit** timer resolution
(approximately **1.22 µs per tick**). Calibration uses `Stop`,
`ForwardStart`, `ReverseStart`, `ForwardMax`, and `ReverseMax` for each
motor. Stop must be 1450–1550 µs, starts must straddle stop, and maxima must
extend outward on the matching side. Values are integer-microsecond
**requests**. At 50 Hz/14-bit, hardware quantizes them to approximately
1.2207 µs ticks, so adjacent requests can alias. Firmware rejects a start
that quantizes to the same tick as stop.

Calibration storage is schema 3. Earlier schema-2 records are erased,
never translated, and are intentionally ineligible; all ten fields must be
explicitly recalibrated before drive output can become eligible. A `0 µs`
diagnostic value means PWM is off/detached and is never written as a pulse.
The diagnostic `nominalReadbackPulseUs` value is reconstructed from cached
timer ticks; it is not an oscilloscope measurement.

Routine valid drive updates keep GPIO3/EN HIGH and update the paired PWM
channels without power-cycling the boost. A cold start or channel rebuild
holds EN LOW, configures and verifies both channels, writes both pulses,
waits at least one complete 50 Hz period, then enables the rail.

An independent `esp_timer` lease drops GPIO3 LOW after 300 ms even if the
Arduino loop stalls, and latches re-enable until the main task performs a
safe session reset. The five-second loop-task watchdog is secondary recovery;
the physical EN pulldown documented in `HARDWARE.md` is still required for
reset/ROM/bootloader coverage.

`cal reset confirm` preserves the last verified NVS record as an atomic
fallback, so that record returns after reboot. For motor replacement, use
`cal erase confirm`, verify that no retained record remains, then calibrate
and save all ten fields.

The temporary calibrator accepts only `left <1000-2000>`, 
`right <1000-2000>`, `status`, and `off`. Each valid pulse is a renewable
five-second lease; malformed input, overflow, or lease expiry powers the
servo rail down. Because GPIO3 controls one shared rail, physically unplug
the unselected motor during calibration.

An active tone is a watchdog lease, not an indefinite command. The MIDI
bridge refreshes it every **250 ms**; firmware stops it after **750 ms**
without a valid refresh. `tone 0` stops synchronously and leaves GPIO42 LOW.

The `tools/mascot.py` helper from DeskNotif works here unchanged — just
point it at the new COM port.

## Live MIDI keyboard

Run setup from an **unelevated** PowerShell window and approve any WinGet
agreement or UAC installer prompts:

```powershell
.\tools\setup_midi_keyboard.ps1
```

The script installs/validates loopMIDI, VMPK, and the Python dependencies,
then opens the two GUI applications. Complete the manual routing exactly:

1. In loopMIDI, create a port named **`Robot MIDI`**.
2. In VMPK, open **Edit > MIDI Connections** and select **`Robot MIDI`** as
   MIDI OUT.
3. Start the `robot-midi` bridge manually:

   ```powershell
   .\tools\start_midi_bridge.ps1
   ```

There is **no autostart, service, or scheduled task**. Start loopMIDI, VMPK,
and `robot-midi` manually when needed.

The bridge is monophonic with true last-press priority. Without sustain, the
newest press sounds and releasing it restores the newest press still active.
Repeated note-ons are tracked separately; note-offs release the oldest
matching press. Sustain pedal CC64 latches released notes until pedal-up,
while CC120/CC123 panic immediately. Every MIDI note is octave-folded by
pitch class into **A6 through G♯7 (1760–3322 Hz)**, the piezo's useful
musical band. The final note is kept for at least 45 ms to avoid a click-only
pulse.

The bridge sends `tone 0` at startup. On Ctrl+C, MIDI loss, or other cleanup,
it panics (clears notes and sustain), sends `tone 0`, and closes MIDI/serial
resources. If the host dies before cleanup, the 750 ms firmware watchdog
stops the piezo.

### MIDI troubleshooting

- Run `.\tools\start_midi_bridge.ps1 --list-devices` to see MIDI inputs and
  USB serial ports.
- If no MIDI input appears, keep loopMIDI running, verify the port is named
  exactly `Robot MIDI`, and reselect it as VMPK MIDI OUT.
- Production robot USB identity may be generic or ambiguous, COM numbers can
  change after reconnects, and multiple ESP devices require explicit
  selection. Use `--serial-port COMx` or `--serial-number ...`; do not assume
  the previous COM number is still the robot.
- If a manually issued active tone stops after 750 ms, that is the watchdog
  working; use the bridge for the required 250 ms refreshes.
- If sound is weak, remember that 200–500 Hz is accepted but barely audible;
  the passive piezo is strongest around 2–4 kHz.

## Layout

```
desktop-robot/
├── platformio.ini                  # ESP32-S3R8, USB-CDC, ST7789 pinout
├── partitions_desktop_robot.csv    # 16 MB layout with LittleFS
├── HARDWARE.md                     # full board reference
├── README.md
├── lib/
│   └── EyeEngine/                  # ← ported as-is from DeskNotif
├── src/
│   ├── main.cpp                    # board init + serial CLI
│   └── mascot/
│       └── mascot_a.h              # 220×216 RGB565 mascot bitmap
└── tools/
    └── mascot.py                   # host-side CLI for serial control
```

The mascot bitmap is the same 220×216 art used on DeskNotif, centered on
the taller 240×280 panel (32-px black bars top and bottom). Eye coordinates
adjust automatically through the existing offset math — no art changes
needed to get the first flash working.
