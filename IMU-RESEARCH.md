# IMU research: how accurately can this robot know which way it is pointing?

Synthesis of an eight-agent research pass into inertial heading estimation,
drift compensation, and whether any external reference could bound heading
indefinitely.

Full agent reports are in the session workspace as `research-*.md`:
`fusion`, `filters`, `drift`, `domains`, `wheeled`, `qmi8658`, `wifi-rssi`,
`rf-bearing`.

Evidence is labelled throughout. `[MEASURED]` means observed on this robot.
`[SOURCED]` means a citation. `[DERIVED]` means arithmetic from those.
`[INFERRED]` means reasoned judgement. Nothing here has been verified on the
hardware unless it says `[MEASURED]`.

---

## 1. Executive summary

**The headline is a reframing, not a technique.** The robot's heading *sensing*
is already excellent and effectively finished. What limits it is *mechanics*.

Heading holds **±0.33° over 120 seconds** while stationary, reproduced across
two independent runs `[MEASURED]`, and it does so while the gyro's own bias
moves by up to 0.22 dps. The robot overshoots by **7–9° every time it stops**
`[MEASURED]`. The second number is roughly **25× larger** than the first
`[DERIVED]`. Every one of the eight agents, working independently, ranked
closing that overshoot as the single highest-value change — above every filter,
every register, and the entire radio programme.

> **Caveat on the drift figure, added after the fact.** The ±0.33° runs were
> taken with the robot on its *stand*; the coast and turn figures were taken
> with its weight on its wheels on the *desk*. The comparison above is still
> directionally right — overshoot dominates drift by more than an order of
> magnitude either way — but the drift number is a best case and needs
> re-measuring on the desk. See §2 and §5.1.

So the honest summary of the night is: **the estimation problem is solved, and
we were about to spend effort making a solved thing 1.6× more solved while a
much larger error sat untouched in the actuator.** That is now addressed (§6).

**A bug was found by measuring, not by reading.** Verifying the above on the
hardware — rather than asserting it — caught a live regression that the
gravity projection had introduced: heading was drifting **3.39° over 65 s**,
accumulating, roughly 13× worse than before the projection existed. Cause and
fix in §6.3. It would not have been found by any amount of further reading, and
it is the strongest argument in this document for measuring before claiming.

Four further conclusions, each of which closes a line of enquiry:

- **A 6-axis IMU cannot bound yaw drift. This is structural, not a tuning
  problem.** Gravity is invariant under rotation about the gravity axis, so
  `∂f_body/∂ψ ≡ 0` — the accelerometer's sensitivity to heading is *identically
  zero* `[DERIVED]`. No filter recovers information that is not present.
- **Mahony, Madgwick and attitude EKFs buy exactly zero for yaw here.**
  Unanimous across agents. Their correction term is structurally orthogonal to
  the quantity we want `[DERIVED]`. This is the clearest "do not build it"
  finding of the night.
- **WiFi cannot supply a heading reference.** Not marginal — dead, for a
  geometric reason that no amount of signal processing fixes (§4). There is a
  published negative result on very nearly this exact experiment `[SOURCED]`.
- **The largest remaining *sensing* error is gyro scale factor**, spec'd at
  ±3%, i.e. up to **±10.8° per 360°** `[SOURCED + DERIVED]`. It is invisible to
  the robot — measuring it requires an external angle reference. Importantly it
  is **exactly zero for zero-sum gestures**, which is most of what this robot
  does (§5.2).

### The honest ceiling

| Question | Answer |
|---|---|
| How well can it *know* its heading? | **±0.33° over 120 s stationary**, two independent runs `[MEASURED]`, through a moving bias; sub-degree over a single turn. Already achieved. |
| How accurately can it *execute* a commanded turn? | **~±1° is realistic** with lead termination + a terminal speed ramp + scale calibration `[SOURCED — wheeled]`. Was ~8°. |
| **±0.1° per turn?** | **No.** Below one sample-interval of rotation at full speed (0.57° at 500 Hz), below the run-to-run *variance* of the coast, and below what a deadband-limited hobby servo can trim `[DERIVED]`. |
| Can heading drift be bounded indefinitely with no external reference? | **Not by sensing.** Only by a *structural prior* — snapping to the 45° lattice the gesture vocabulary already uses (§5.3) `[DERIVED]`. |

**These two must never be reported as one number.** "How well do I know my
heading" (±0.33° / 2 min) and "how accurately can I execute a turn" (was ~8°,
now expected ~1–2°) differ by more than an order of magnitude, and conflating
them would be badly misleading.

---

## 2. Measured baseline

Everything below was observed on this robot. Future claims should be compared
against these, not against the literature.

> **Read the pose column.** Measurements taken with the robot's weight on its
> wheels on the desk ("desk") describe operation. Measurements taken with it
> sitting on its stand ("stand") do not — the stand holds it at roughly twice
> the tilt and takes the wheels out of the friction path. Mixing the two is a
> mistake I made and had to correct; see §5.1.

| Quantity | Value | Pose | Notes |
|---|---|---|---|
| Gyro bias | 0.28 → 0.50 dps over 2 min; **−0.545 dps after 9 h** | stand | Tracks board temperature; re-estimated every 2 s of stillness |
| Stationary heading hold | **±0.33° over 120 s, 2 runs** | **stand** | ~64 ZUPTs each, through the bias drift above. **Needs re-measuring on the desk** — the stand decouples it from desk vibration |
| Long-term heading | **−8.71° over 9.1 h** (16133 ZUPTs) | stand | Bounded over minutes; a slow random walk over hours. Both are true and must not be conflated |
| Residual bias after ZUPT | bounded, non-accumulating | stand | Verified by the heading returning to zero repeatedly |
| Full 360° turn | 1260 ms; measured 265 dps at speed 100 | **desk, untethered** | Rate varies 235-287 within a block - unexplained |
| **Coast after motors cut** | **17.4°** @283 dps, 8.8° @195, 5.4° @136 | **desk, untethered, 2 runs** | **The dominant error, ~2x the 7-9 deg first believed.** Quadratic, NOT linear: alpha = 2600 deg/s^2 |
| Servo rate model | `rate = 0.91·speed − 24.2` dps | desk, tethered | Deadband at command ≈27 |
| Resting gravity vector | (1.0125, 0.0833, −0.1007) g | **desk, measured** | ~8° tilt. Off-axis sits *exactly* at the ±100 mg offset tolerance |
| Resting gravity vector | (0.97, 0.08, −0.27) g | stand | ~16.5° tilt. **Do not use this for operating claims** |
| Single-axis integration deficit | **3.0° per 360°** (gain 1.0083) | **desk, measured** | The operating figure, confirmed by bench run 001. (14.7° on the stand) |
| IMU sample rate | 500 Hz | — | 0.57° of rotation per sample at full speed |

Two caveats worth carrying. The tethered rate model and the untethered 285 °/s
are not obviously consistent, and the USB cable measurably restrains rotation —
the rate model should be re-measured untethered `[SOURCED — wheeled, rec #2]`.
And the drift figures above were all taken on the stand, so they are a *best
case*: on the desk the robot is coupled to whatever the desk is doing.

---

## 3. The corrected error budget

This table is the single most useful output of the night.

| Error source | Magnitude | Observable on-board? | Status |
|---|---|---|---|
| **Coast overshoot** | **7–9° per stop** | Yes — measured and printed per turn | **Was uncompensated. Dominant by ~300×. A *control* problem.** Addressed in §6. |
| Gyro scale factor (uncalibrated) | ±3% spec ⇒ up to **±10.8°/360°** | **No** — needs an external angle reference | Unobservable on-board; **exactly 0 for zero-sum gestures** |
| Axis projection (if single-axis) | **~3.5°/360°** in the desk pose | Partly | Fixed by gravity projection; gain is re-derived per pose (§5.1) |
| Uncorrected off-axis bias | 0.052 dps ⇒ **3.4° per 65 s**, accumulating | Yes, once looked for | **Was live. Found by measuring, fixed** (§6.4) |
| Residual bias after ZUPT | bounded, ±0.33° over 120 s | Yes, directly | **Solved** |
| Sample quantisation @ 500 Hz | 0.57° | Yes | Acceptable |
| g-sensitivity (±0.1 dps/g) | ≤0.04°/turn | No | Negligible |

### A correction I had to argue for

An agent initially claimed the zero-sum test sequence (R90 → L90 → L45 → R90 →
L45) observes *scale-factor* error. The algebra says the opposite. For
**closed-loop** turns, each turn stops when the *integrated* angle reaches the
target, so with scale `k` and bias `b`:

```
net = ( T₁ − T₂ − b·(t₁ + t₂) ) / k
```

The target terms `T₁ − T₂` cancel by construction (the sequence sums to zero),
`k` divides a quantity that is already ~0, and **only the bias term survives**.
Scale factor cancels; bias does not. The agent checked and withdrew the
recommendation.

**Therefore: the zero-sum residual is a coast-asymmetry and bias meter, not a
scale calibrator.** Reading it as a scale check would have sent us chasing the
wrong error.

> **Note for the bench:** the sequence is balanced in *angle* (sums to 0°) but
> **not in stops** — 2 clockwise vs 3 counter-clockwise. Since each stop
> contributes 7–9° of coast, a non-zero residual partly reflects that
> asymmetry rather than a sensing error. Worth knowing before reading anything
> into the number. Left unchanged, since it is your test.

---

## 4. Can an external reference bound heading? The WiFi verdict

**Verdict: no. Not marginal — structurally impossible on this hardware.**

This was the most important question on the agenda, because it was the only
proposal that could bound drift *indefinitely* rather than merely slow it. It
does not survive contact with the geometry.

**The killing argument** `[DERIVED]`: rotating an antenna about its own phase
centre produces **zero** change in range-to-AP, therefore zero propagation-phase
modulation, therefore **no synthetic aperture**. Only the element pattern
rotates. Spinning in place does not create the interferometer the idea needs.

Even granting the best possible geometry — antenna mounted at the very edge, 3.5 cm
off-axis on a 7 cm robot — the aperture is `L = 7 cm = 0.57 λ` at 2.44 GHz, giving a
beamwidth of `0.886 λ/L ≈ **89°**` `[DERIVED]`. Against a baseline of ±0.05°.

Supporting findings, each independently sufficient:

- **A published negative result on nearly this exact experiment** `[SOURCED]`:
  ESP32-ROOM-DA + BNO085 IMU, **two** antennas, 7 APs, 100 training points, on
  a DJI RoboMaster S1. Best classifier reached **MCC 0.23, F1 0.39** on 4-class
  (90°-granularity) heading. DOI `10.2478/ama-2025-0079`.
- **CSI cannot do angle-of-arrival on this part** `[DERIVED]`. The ESP32-S3 has
  **one RF chain**; `esp_phy_set_ant()` is an external *switch*, not a second
  receiver. Without simultaneous cross-antenna phase there is no way to cancel
  per-packet CFO/SFO offsets, so no phase-based DoA.
- **FTM / 802.11mc RTT solves a different problem** `[DERIVED]`. The S3 does
  support it (`SOC_WIFI_FTM_SUPPORT (1)`), but RTT yields **range**, and range
  is invariant under spin-in-place — **zero yaw information**. Android quotes
  1–2 m with ≥3 APs, about 30× the robot's body length.
- **RSSI resolution is far below what is needed** `[SOURCED — wifi-rssi]`.
  WiFi-SLAM (Ferris, Fox & Lawrence, IJCAI 2007) — which *is* the original idea,
  and it is real work — is **rotation-ambiguous by construction**: it recovers a
  latent-space map up to an arbitrary rotation. A 30 cm move produces ~0.5 dB
  against 1 dB RSSI quantisation, and indoor multipath bias is *spatially fixed*,
  so averaging does not remove it.

**What WiFi *is* good for here:** room-level **place recognition** — "am I at
my usual desk spot?" That is a genuine capability and it would work. It is just
not a compass. Promiscuous beacon sniffing does work while softAP runs, so the
data is collectable if that feature is ever wanted.

### 4.1 Confirmed on this robot, not just cited

The `rfscan` command (§6.5) was built to close this question with our own data
rather than only a citation. Four back-to-back scans with the robot **physically
motionless**:

| BSSID | SSID | RSSI across 4 identical scans | Spread |
|---|---|---|---|
| `84:D8:1B:32:35:6C` | TP-LINK Power Strip | −54, −53, −53, −54 | **1 dB** |
| `6A:55:D4:13:3D:12` | DIRECT-D413BD12 | −56, −56, −57, −55 | **2 dB** |
| `1C:28:AF:31:E0:C0` | MSFTGUEST | −59, −64, −64, −64 | **5 dB** |
| `1C:28:AF:31:E0:C1` | MSFTCONNECT | −64, −64, −64, −64 | 0 dB |
| `04:D9:F5:13:A5:BB` | ECLYPSE-5CF030 | −95, −94, −94, −93 | 2 dB |

**The stationary noise floor is 1–5 dB** `[MEASURED]`. The literature says a
**30 cm translation** produces about **0.5 dB** of RSSI change — so on this
hardware, in this room, the noise from *doing nothing at all* is **2–10× larger
than the signal a 30 cm move would produce**. Rotation in place produces less
still, because it generates no range change at all (§4).

The AP *set* is not stable either: "Studio" appeared in scan 1 only, and a
second MSFTGUEST BSSID appeared in scan 4 only. Two identical scans seconds
apart do not even agree on which APs exist.

**This is the theoretical verdict reproduced on our own bench.** The idea is
closed — not on authority, but on our own measurement.

**Cost of having asked:** low, and worth it. The question had to be settled
before anything was built on top of it, and now it is — with a measured,
citable reason rather than an opinion.

---

## 5. Findings that change what we build

### 5.1 Is the gravity projection real or an artefact? — settled, but by the *other* argument

This is the one place where the research flagged a risk to something already
shipped, and it deserved the challenge.

The concern `[SOURCED — QST datasheet Rev 0.9 §3.4 Table 7]`: the
accelerometer's **Initial Offset Tolerance is ±100 mg**. The off-axis gravity
components measured *on the desk* are (·, 0.10, −0.10) g — **exactly that
magnitude**. If those readings are sensor *offset* rather than genuine *tilt*,
then single-axis integration was already correct and the projection **costs**
accuracy instead of saving it. A fair and serious objection, and a static
reading cannot resolve it.

> **A correction, recorded because it was my error.** I initially claimed this
> was settled by measuring `g = (0.97, 0.08, −0.27)` — a Z component of 270 mg,
> 2.7× the tolerance, therefore unambiguously real. **That reading was taken
> with the robot on its stand, not on the desk.** The stand tilts it ~16.5°;
> the desk pose is ~8°. Measuring a static quantity in one pose and quoting it
> for another is exactly the class of mistake this document is supposed to
> guard against. The 270 mg figure proves only that the stand tilts the robot.

**What actually settles it: the gyro-ratio check, from real desk turns**
`[MEASURED + DERIVED]`. If the tilt is real, the off-axis *gyro* components
during a turn must appear in the same ratio as the off-axis *gravity*
components. Gyro during a turn read `X = −85.2, Y = −1.7, Z = +7.6` dps against
a desk gravity ratio of 10.1 : 1:

| Ratio | Observed | Predicted if tilt is real | Reading |
|---|---|---|---|
| X : Z | **11.2 : 1** | 10.1 : 1 | **Z tilt is real** — matches |
| X : Y | **50 : 1** | 10.1 : 1 | **Y is mostly accelerometer offset** |

This is immune to the pose problem because both quantities come from the same
desk-loaded turns, and immune to the accel-offset problem because the gyro has
no such offset. **Z is genuine tilt; Y is largely sensor offset.** The
projection is net-positive — but worth **~3.5° per 360° in the desk pose**, not
the 14.3° that the stand reading suggested.

`AxisEstimator` measures the spin axis from the gyro directly, which is immune
to accelerometer offset by construction and is the right long-term answer.

**The projection gain is pose-dependent, and the firmware handles that
correctly:** `establishYawAxis()` re-reads gravity at boot and
`measureYawAxisAndBias()` re-reads it before every closed-loop turn, so a turn
always uses the gravity of the pose it is actually in. Only the *documentation*
was wrong. The one residual gap: heading held *across* a pose change (desk →
stand) keeps using the old gravity until the next turn.

**But the projection was not free** — see §6.4.

### 5.2 Scale factor is the largest sensing error — and mostly harmless here

±3% initial sensitivity tolerance `[SOURCED — QST DS Rev 0.9 §3.4 Table 8]` is
up to ±10.8° per 360°, three times the projection term. ZUPT is *structurally
blind* to it: at zero rate, a scale error produces zero output.

But it **cancels exactly for zero-sum gestures** (§3), which is what the
movement vocabulary is made of. It only matters if absolute commanded angle
matters — "turn exactly 90° and stay there."

Two routes if it ever does:

1. **N-revolution bidirectional calibration** against a mechanical reference
   (a protractor and a mark on the desk). ~15 min, one NVS constant, takes
   ±10.8° → <0.5°. Must be untethered.
2. **On-chip COD**, `CTRL9 = 0xA2`, then `0xAA` to apply the gains
   `[SOURCED — SensorLib]`. Four of the eight `COD_STATUS (0x46)` bits report
   *sensitivity* limits and the readback is `uint16_t gX/gY/gZ_gain` —
   unsigned, i.e. a multiplicative gain, not an offset. So COD calibrates
   **scale, not bias**, which is exactly the error we cannot otherwise see. Z-axis
   coverage is unconfirmed and should be checked before relying on it.

### 5.3 The only way to bound drift with no external reference

No surveyed domain bounds heading with an IMU alone `[SOURCED — domains]`.
Foot-mounted pedestrian INS, camera-free mocap suits, drones, and underwater
INS all import an external fact; **none of them out-filters the problem.** PX4
states outright that the minimum viable EKF sensor set includes "a source of
yaw (magnetometer or external vision)".

The one mechanism that bounds heading without a sensor is a **structural prior
on the world**. Heuristic Drift Elimination "maintains near-zero heading errors
in walks of unlimited duration" by assuming structured indoor environments
`[SOURCED — Borenstein & Ojeda]`.

**This robot has an unusually clean version of that prior available: its
commanded turns are quantised to multiples of 45°.** Snapping the heading
estimate to that lattice at standstill, gated on confidence, converts unbounded
drift into bounded error. Ranked "do second" by the domains agent, ~50 lines.

It must be gated and clamped — a wrong snap is worse than no snap — but it is
the only credible route to indefinitely-bounded heading on this hardware.

### 5.4 Things that are definitively not worth building

Recorded so they are not revisited:

| Rejected | Why |
|---|---|
| Mahony / Madgwick / attitude EKF for yaw | **Zero** information gain; correction is orthogonal to yaw `[DERIVED]` |
| Gyrocompassing (sensing Earth's rotation) | Needs bias < 0.186 °/h; this part measures **1908 °/h** — **10,250× short** `[DERIVED]` |
| Spin-and-measure RF bearing | 25–90° error vs a 0.05° baseline; no synthetic aperture (§4) |
| CSI angle-of-arrival | One RF chain. Not a tuning limit — a hardware one |
| FTM / RTT for heading | Yields range; range is invariant under spin-in-place |
| Multi-IMU array | Ceiling is √N; verified 16-unit array reached 3.4× `[SOURCED]`. Buys 1.4–2× on the *smallest* error term |
| Magnetometer | Ferromagnetic desk + motor currents. High risk, and it would be measuring the desk |
| AttitudeEngine (on-chip quaternion) | Coning error here is already ≈0; unverified fixed-point format |
| Active braking (reverse pulse) | Vendor: "stalling or back-driving this servo can damage it" `[SOURCED — Pololu]` |
| Lowering gyro full-scale to ±512 dps | Only 1.8× headroom ⇒ **silent clipping** risk. Gain ≤0.029° |

### 5.5 Accelerometer during motion: a trap worth recording

At 285 °/s, centripetal acceleration creates a **4.3° false tilt** `[DERIVED]`.
Any attempt to update the gravity vector *during* a turn will corrupt it.

**Rule: sample gravity only at standstill.** The current design already does
this, and it should stay that way.

---

## 6. What was implemented tonight

Only low-risk, host-tested changes were made. Nothing here has been run on the
robot — all of it builds clean and passes the host suites, and that is a
different claim.

### 6.1 Coast lead compensation — the #1 ranked change

Every agent ranked this first. The controller now stops **early** by the angle
it is predicted to coast through, so the angle it comes to *rest* at is the
target, rather than the angle at which power was cut.

```
θ_lead(ω) = k_coast · |ω_now|          k_coast ≈ 0.028 s
STOP when  progress + θ_lead ≥ target
```

The linear model is used deliberately over the quadratic alternative: it cannot
blow up, and it degrades *safely* — under-leading leaves a small overshoot,
whereas over-leading leaves an undershoot that would need a second nudge too
small for the servo deadband to deliver `[SOURCED — wheeled §6]`.

Design decisions worth recording:

- **Driven by the measured gyro rate, not the commanded speed.** This makes it
  self-correcting: a restrained, slipping or low-battery chassis turns slower,
  so it coasts less, so it leads less — automatically. That property is exactly
  what makes it robust to this robot's known failure mode.
- **Smoothed over 4 samples**, not a single reading, so gyro noise cannot move
  the stop decision.
- **Self-learning.** Every completed turn already measures its own coast, so
  `k_coast` is updated as `coast / rate_at_stop` with a 0.7/0.3 EWMA, bounded to
  0–0.12 s. The raw coast is still printed, so the compensation stays visible
  rather than silently folded away.
- **Bounded per turn** to 40% of the target angle. Without that, a small turn
  could have its target satisfied by predicted coast alone on the first fast
  sample, cutting power mid-ramp. There is a host test pinning that hazard.
- **`lead` serial command** to read, pin, or disable it (`lead`, `lead 0.03`,
  `lead off`, `lead reset`) so it can be A/B'd on the bench without a reflash.

Expected effect: **7–9° → ~1–2°**. Unverified on hardware.

### 6.2 ZUPT spread gate

An agent recommended tightening the stationary threshold from 8 → 2 dps to
close a bias-poisoning hole. **That fix has a failure mode**: the gate is
applied to the *bias-corrected* rate, so if the true bias ever drifts past the
threshold, the zero-velocity update is locked out permanently — precisely when
it is most needed. Silent, and self-reinforcing.

Implemented instead: `BiasEstimator` now tracks peak-to-peak **spread**, and a
window is rejected if it varies too much. This separates the two situations a
single magnitude threshold conflates:

| Situation | Mean | Spread | Correct action |
|---|---|---|---|
| Bias has drifted | large | **tiny** | **Accept** — this is what ZUPT is for |
| Being nudged / still coasting | ~0 | **large** | **Reject** |

The loose magnitude threshold is kept for bias-drift tolerance; spread rejects
actual motion. Rejections are counted and reported by `heading` so the gate's
effect is visible rather than silent.

### 6.3 Not done deliberately

- **Terminal speed ramp** (drop to ~15–30 °/s for the last ~15°). This is the
  other half of the top recommendation and would take coast to **<1°**, and —
  more importantly — collapse its *variance*. It changes actuation mid-turn,
  which risks the working test button, and it cannot be verified tonight. It is
  the clear next step. See §7.
- **Gyro low-pass filter** (`CTRL5`, one byte, 4.3× noise reduction). It has a
  subtlety worth stating: the ~38 ms group delay means the integrated angle
  *at the moment of the stop decision* lags truth by `τ·ω ≈ 10.8°` at full
  speed, so enabling it blind would cause a large late-stop on the first turn.
  Elegantly, the self-learning loop in §6.1 would absorb it (`k_coast` would
  converge to `k_coast + τ`), because the learner measures exactly that lag.
  But "elegantly, it should self-correct" is not a claim to ship unverified.
  Enable it with `lead reset` and watch the constant converge.

---

### 6.4 A bug the research did not predict, and measurement caught

After flashing, I checked heading drift on the robot instead of assuming the
earlier figure still held. It did not:

```
BEFORE FIX   t+15s  +0.12   t+30s  +0.94   t+45s  +1.80   t+60s  +2.78   t+65s  +3.39 deg
AFTER FIX    t+20s  -0.26   t+40s  -0.09   t+60s  +0.03   t+80s  -0.08   t+120s -0.19 deg
```

The first row **accumulates monotonically** — that is unbounded drift, 3.39° in
65 s, about 13× worse than before the gravity projection was added. The second
**oscillates around zero for twice as long**, and does so while the gyro's own
bias drifts 0.28 → 0.50 dps. Bounded.

**Cause.** The projection makes heading a weighted sum of *all three* gyro axes:

```
ω_yaw = ω · ĝ = ω_x·ĝ_x + ω_y·ĝ_y + ω_z·ĝ_z
```

so all three axis biases now reach the heading, each scaled by its gravity
component. But the zero-velocity update was only re-estimating the bias of the
*dominant* axis. With ĝ_z ≈ −0.26, an uncorrected Z bias of ~0.2 dps leaks in
as ~0.052 dps of apparent yaw — which integrates to **3.4° in 65 s**, matching
the observation almost exactly `[DERIVED]`.

**Why it hid.** A single-axis integrator never read Y or Z at all, so their
biases were harmless and nobody had reason to estimate them. Adding the
projection silently promoted two previously-irrelevant quantities into the
error path. The projection was correct; its *precondition* was not met.

**Fix.** Estimate bias on all three axes during the zero-velocity update, and
judge the spread gate on the worst axis — the robot was either still or it was
not, and motion about any axis disqualifies the window. A host test now pins
the arithmetic so this cannot silently return.

**The general lesson**, worth more than the specific bug: *a change that is
correct in isolation can invalidate an assumption made elsewhere.* No amount of
further reading would have surfaced this. Sixty seconds of looking at the actual
robot did.

### 6.5 `rfscan` — closing the WiFi question with data

Deliberately **not** the spin-and-log tool originally planned, because the
research rules that out. It is the cheap stop-and-scan pre-check the RF agent
recommended ("do, then stop"): sample every visible AP's RSSI at the current
heading, one line per AP, prefixed with the heading so a series can be grouped
by BSSID.

```
rfscan 0.46 84:D8:1B:32:35:6C -53 6 "TP-LINK_Power Strip_356C"
rfscan: heading 0.46 deg, 7 networks
```

Two hazards were hit and are worth recording:

- **A blocking scan reboots the board.** `WiFi.scanNetworks()` synchronously
  parks the loop task for seconds, which starves the task watchdog:
  `E task_wdt: Aborting` → `rst:0xc (RTC_SW_CPU_RST)`. The scan is now
  asynchronous and collected by `serviceRfScan()` from the loop.
- **`WiFi.mode(WIFI_OFF)` is not safe to call here.** `stopWifiServer()`
  documents that turning the radio fully off corrupts the netif chain on this
  core and breaks later softAP bring-ups. `rfscan` leaves the radio in idle STA
  instead, and refuses to run at all while the joystick AP is up.

It also refuses while anything is driving the motors — a multi-second scan
during a closed-loop turn would starve the controller of the samples it needs
to stop.

## 7. Ranked recommendations

Merged across all eight agents, ordered by value per unit of complexity.

| # | Action | Expected gain | Cost | Risk | Status |
|---|---|---|---|---|---|
| 1 | **Coast lead termination** | **+17.6° → −4.5° measured** (74% smaller); alpha retuned 2100→2600 | Done | Low | **Implemented and measured over 2 supervised runs** |
| 2 | **Terminal speed ramp** to 15–30 °/s for the last ~15° | ~1–2° → **<1°**, and kills the *variance* | Moderate | Low–med | **Next** |
| 3 | ~~Verify tilt via gyro-axis ratio~~ | — | — | — | **Done — tilt is real, worth ~3.5°/360° on the desk** (§5.1). The 270 mg "proof" was a stand artefact |
| 3c | **Re-measure stationary drift on the desk** | Turns a best-case number into an operating one | ~5 min | None | **Open** — all drift figures here were taken on the stand |
| 3b | **Three-axis ZUPT bias estimation** | 3.39°/65 s unbounded → ±0.33°/120 s bounded | Done | Low | **Implemented and verified on hardware** |
| 4 | **Re-measure the rate model untethered** | Removes a cable-contaminated model | One session | None | Pending |
| 5 | **Gyro scale-factor calibration** (N-rev bidirectional, or on-chip COD `0xA2`) | ±10.8° → <0.5° per 360° | ~15 min + 1 constant | Low | Only if absolute angle matters |
| 6 | **ZUPT spread gate** | Closes bias poisoning without lockout risk | Done | Low | **Implemented** |
| 7 | **HDE: snap heading to the 45° lattice at standstill** | Unbounded → **bounded** | ~50 lines | Med — must gate + clamp | The only route to bounded drift |
| 8 | **Gyro LPF** `CTRL5`, mode 0 | 4.3× noise; kills aliasing | 1 byte | Low, but see §6.3 | Enable *with* `lead reset` |
| 9 | **Re-assert `CTRL1` after reset** | Prevents silent burst-read corruption | Trivial | Very low | Cheap insurance |
| 10 | **FIFO + INT2 watermark** | 128–256 ms stall immunity | Moderate | Low–med | Insurance |
| 11 | **Slip detector** (commanded rate vs gyro) | Turns silent slip into a visible fault | Small | Low | Nice to have |
| 12 | ~~8-heading stop-and-scan RF pre-check~~ | — | — | — | **Done. `rfscan` built; noise floor 1–5 dB stationary vs 0.5 dB per 30 cm — question closed** (§4.1) |

---

## 8. What the field does that we do not

| Field practice | Our status |
|---|---|
| Allan variance characterisation of the gyro | Not done. Would replace one 48 s run with a real noise model |
| Reporting RMS over ≥10 runs, not a single number | Improved: two 120 s runs through real bias drift, not one 48 s snapshot. Still only two |
| Scale-factor calibration against an external reference | Not done — the largest sensing error is unmeasured |
| Temperature-compensated bias (this part has an on-die sensor) | Not done. Negligible given per-turn re-estimation |
| Structural priors on the world (HDE) | Not done. The 45° lattice is available and unused |
| FIFO + interrupt-driven sampling | Not done. Polling at 500 Hz works but has no stall immunity |
| Separating "estimation accuracy" from "execution accuracy" in reporting | **Now done** — and it was the single most clarifying distinction of the night |

---

## 9. References

### Load-bearing

- QST **QMI8658C datasheet** Rev 0.9, §3.4 Tables 7–8 — accel Initial Offset
  Tolerance ±100 mg; gyro Initial Sensitivity Tolerance ±3%. (`datasheets/`)
- `lewisxhe/SensorLib`, `src/SensorQMI8658.hpp` — `CTRL9` command table,
  `COD_STATUS (0x46)` bit meanings, `uint16_t` gain readback establishing that
  COD calibrates scale rather than bias.
- Ferris, Fox & Lawrence, **"WiFi-SLAM Using Gaussian Process Latent Variable
  Models"**, IJCAI 2007 — the original of the user's idea; rotation-ambiguous by
  construction.
- **DOI `10.2478/ama-2025-0079`** — ESP32 + IMU WiFi heading classification on a
  mobile robot. The directly-comparable negative result (MCC 0.23, F1 0.39).
- Borenstein & Feng, **UMBmark**, IEEE T-RA 12(6) 1996, DOI `10.1109/70.544770`
  — order-of-magnitude odometry improvement, explicitly scoped to *systematic*
  error only.
- Borenstein & Ojeda, **Heuristic Drift Elimination**, J. Navigation 2008 —
  "near-zero heading errors in walks of unlimited duration" via structural
  priors.
- **PX4 ECL EKF documentation**, `advanced_config/tuning_the_ecl_ekf.md` —
  minimum viable sensor set explicitly requires an external yaw source.
- Pololu FS90R product documentation — "stalling or back-driving this servo can
  damage it."
- ESP-IDF `soc_caps.h`, `SOC_WIFI_FTM_SUPPORT` — FTM capability on ESP32-S3.

### Background

- Madgwick (2010); Mahony et al. (2008) — reviewed and **rejected** for yaw on
  a 6-axis part; included so the rejection is traceable.
- arXiv:1910.00529 — foot-mounted INS survey; ZUPT bounds velocity, not heading.
- arXiv:1805.10410 — gyrocompassing bias requirements.
- Sensors 2024, PMC11598325 — multi-IMU arrays; √N ceiling, 16 units → 3.4×.
- PMC8859896 — camera-free mocap drift rates (−0.14 to +0.17 deg/min).
- Bahl & Padmanabhan, **RADAR** (INFOCOM 2000); Youssef & Agrawala, **Horus**
  (MobiSys 2005) — RSSI fingerprinting baselines.
- Hu et al., Sensors 26(2):595, 2026 — sub-0.15° RMS heading, but GNSS/SINS on a
  large slow vehicle. **Not** a turn-execution spec for a hobby servo.

---

## 10. Open questions

1. ~~**Is the gravity projection net-positive?**~~ **Settled: yes**, by the
   gyro-ratio check on desk turns — Z tilt is real, Y is mostly accel offset.
   Worth **~3.5° per 360° in the desk pose** `[MEASURED]`. The precondition it
   needed — bias on all three axes — is now met (§6.4). Note the gain is
   pose-dependent and re-derived at boot and before every turn.
1b. **What is the drift on the desk?** Every drift figure here was measured on
   the stand, which decouples the robot from desk vibration. Re-run the two
   120 s holds with its weight on its wheels.
2. **What is `k_coast` really?** 0.028 s is derived from a single 8°/285 °/s
   observation. The learner will refine it; the value it converges to is the
   answer. **Needs an untethered turn** — the cable restrains rotation.
3. **Is the coast linear or quadratic in rate?** Distinguishable by measuring
   coast at three speeds. Matters only if the terminal ramp is not implemented —
   the ramp makes coast small enough that the model stops mattering.
4. ~~**What is the actual stationary gyro noise σ?**~~ **Measured: peak-to-peak
   spread is 0.9–1.3 dps** over a 32-sample window `[MEASURED]`. The spread gate
   sits at 4.0 dps, so it has ~3× margin — tight enough to catch motion, loose
   enough that it has rejected only 1 window in ~100.
5. **Does servo vibration alias into the gyro band?** Unquantified, and the one
   argument for the LPF that does not depend on noise arithmetic. A 60 s log
   with servos running settles it.
