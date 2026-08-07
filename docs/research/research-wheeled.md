# Research: Wheeled-Robot Heading Techniques

## Executive summary (max 10 bullets, each tagged)

- **[SOURCED]** Gyrodometry (Borenstein & Feng, ICRA 1996, DOI `10.1109/ROBOT.1996.503813`) trusts *odometry* by default and substitutes the *gyro* only during brief instants when the two disagree — its stated purpose is to suppress **gyro drift**, not slip. On a robot with no encoders and a demonstrably excellent gyro, the method is architecturally inverted and does not apply.
- **[DERIVED]** The one transferable idea from gyrodometry is its **disagreement threshold**: compare gyro-measured yaw rate against the rate *predicted from the wheel command* and use the residual as a slip/stall detector — never as a heading correction.
- **[SOURCED + DERIVED]** Non-holonomic constraints observe heading **only in proportion to forward speed**; the heading row of the NHC measurement Jacobian is ≈ `v_forward · δψ`. A robot whose signature motion is turn-in-place (`v_forward = 0`) gets **exactly zero heading information** from NHC. Its useful cousins — ZUPT and ZARU — are already what is working (±0.05° / 48 s).
- **[SOURCED]** Slip handling in the literature splits into: reject-the-update (threshold), estimate-and-subtract (current-based, model-based), and declare-immobilised. Ward & Iagnemma's immobilisation detector works **without GPS**; Ojeda/Borenstein's current-based detector works **only along the direction of motion, not laterally**.
- **[SOURCED]** Continuous-rotation servos are vendor-characterised exactly as **linear-with-deadband**: "the speed response is nicely linear across 1280 µs to 1720 µs. The rest point is at 1500 µs, with a **20 µs deadband on either side**" (Parallax Feedback 360°, via Pololu). The measured deadband of "27" on this robot is strikingly consistent with that if the command unit is microseconds.
- **[SOURCED]** Vendor warning directly relevant to active braking: "as with most hobby servos, **stalling or back-driving this servo can damage it**" (Pololu FS90R page). Active braking is the wrong answer here.
- **[DERIVED]** The measured coast implies a clean constant: `k_coast = 8° / 285 °/s ≈ **0.028 s**`. The chassis behaves as though it keeps turning at its current rate for ~28 ms after cut-off. Recommended termination rule: **stop when `progress + k_coast·|ω_now| ≥ target`**, using instantaneous gyro rate.
- **[DERIVED]** Combining lead termination with a **terminal speed ramp** is the single highest-leverage fix: drop to a command giving ω ≈ 15–30 °/s for the last stretch, and the coast collapses from 7–9° to **0.4–0.85°**.
- **[DERIVED]** **±1° per commanded turn is realistic** after coast compensation + gyro scale-factor calibration + ≥200 Hz sampling. **±0.1° is not** — it is below one sample-interval of rotation at full speed and below the run-to-run variance of the coast. ±0.05° is achievable for *knowing* heading while stationary, which is a different quantity and is already demonstrated.
- **[DERIVED]** The supplied tethered rate model `rate = 0.91·speed − 24.2` is **internally inconsistent** with the 285 °/s full-speed figure unless `speed` is in microseconds of pulse offset; either way it was measured under cable restraint and must be re-measured untethered before being trusted.

---

## 1. Gyrodometry (and applicability without encoders)

**Canonical source.** J. Borenstein and L. Feng, *"Gyrodometry: a new method for combining data from gyros and odometry in mobile robots,"* Proc. IEEE Int. Conf. Robotics and Automation (ICRA), Minneapolis, 1996, vol. 1, pp. 423–428, DOI `10.1109/ROBOT.1996.503813`. **[SOURCED]** — metadata verified via Crossref (`api.crossref.org/works?query=gyrodometry`) and OpenAlex work `W2111152968`; 214 citations, top-1% citation percentile.

**Mechanism, in the authors' own abstract [SOURCED, OpenAlex `W2111152968` `abstract_inverted_index`]:**

> "…combining measurements from a gyro with measurements from wheel encoders (odometry)… non-systematic odometry error sources (such as bumps) impact the vehicle only during short periods; typically a fraction of a second for each encounter. During these short instances the readings from the gyro and from odometry differ significantly, while in the absence of the large non-systematic errors the readings are very similar. **Gyrodometry makes use of this observation by using odometry data only — most of the time — while substituting the gyro data only during those brief instances during which the gyro and odometry differ substantially.** In this way the ill-effects of gyro drift are almost completely eliminated, and our method can thus make use of very inexpensive gyros with large drift rates."

**Algorithm, restated [DERIVED from the above]:**

```
Δθ_gyro(k) = ω_gyro(k) · Δt          // gyro-derived heading increment
Δθ_odo(k)  = (Δs_R − Δs_L)/b         // encoder-derived heading increment
d(k)       = Δθ_gyro(k) − Δθ_odo(k)  // the "gyrodometry" discrepancy

Δθ_used(k) = |d(k)| > τ  ?  Δθ_gyro(k)  :  Δθ_odo(k)
θ(k)       = θ(k−1) + Δθ_used(k)
```

The gyro is on duty for a tiny duty cycle, so its bias only integrates over that tiny fraction of total time — that is the entire trick.

**Claimed accuracy.** The abstract makes only the qualitative claim above ("ill-effects of gyro drift are almost completely eliminated"). **The specific numeric error reductions in the paper body could not be verified** — see *Could not verify*.

**Does it apply here? No, and it is worth being precise about why. [DERIVED]**

1. **The odometry half does not exist.** No encoders, no wheel-position feedback of any kind. There is nothing to be the default channel.
2. **The trust ordering is inverted.** Gyrodometry assumes odometry is the accurate channel and the gyro is the cheap drifty one that must be rationed. On this robot the measured facts are the opposite: heading holds ±0.05° over 48 s stationary with ZUPT, and gyro-measured turns were "accurate and repeatable… the gyro was NOT fooled by the slip." Running the published algorithm would degrade heading by preferring a worse channel.
3. **The error regime is different.** Borenstein & Feng's non-systematic errors are *transient* (a bump, a fraction of a second). This robot's error is *sustained* restraint/slip across an entire 0.3–1.3 s burst (600 ms → 50–84°; 2000 ms → 55–101°). A "brief instances" detector is the wrong shape for a persistent fault.

**What replaces the odometry half? [DERIVED]**

The only available second channel is the **commanded-motion model** — a *kinematic prior* rather than a measurement: `ω_pred = f(command)` from the calibrated rate model. Use it as gyrodometry's *detector*, with the roles reversed:

```
r(k) = ω_gyro(k) − ω_pred(command(k))       // residual, evaluated in the steady window only
|r| small   → commanded motion happened as modelled
|r| large   → slip, restraint, stall, or an invalid rate model
```

Heading always comes from the gyro. `r` never touches heading; it drives **behaviour** (abort, retry, flag the distance estimate as untrustworthy) and **diagnostics**. This is the residue of gyrodometry that is actually useful without encoders.

**Related Borenstein work worth knowing.** UMBmark — J. Borenstein and L. Feng, *"Measurement and correction of systematic odometry errors in mobile robots,"* IEEE Trans. Robotics and Automation, 12(6), 1996, DOI `10.1109/70.544770` (795 citations). Abstract **[SOURCED, OpenAlex `W2102145772`]**: targets the two dominant differential-drive error sources — "1) uncertainty about the effective wheelbase; 2) unequal wheel diameters" — and reports "consistent improvement of at least one order of magnitude in odometric accuracy (with respect to systematic errors)." Encoder-based, therefore not directly applicable, but its **bidirectional square-path test protocol** (run the same square clockwise *and* counter-clockwise to separate systematic from random error) transfers directly as a test harness for this robot **[DERIVED]**.

---

## 2. Nonholonomic constraints — what they really bound

**Canonical source.** G. Dissanayake, S. Sukkarieh, E. Nebot, H. Durrant-Whyte, *"The aiding of a low-cost strapdown inertial measurement unit using vehicle model constraints for land vehicle applications,"* IEEE Trans. Robotics and Automation, 17(5), 2001, DOI `10.1109/70.964672`, 469 citations. **[SOURCED — OpenAlex `W2160626949`]**

Key sentences from the abstract **[SOURCED]**:

> "The algorithm **exploits nonholonomic constraints that govern the motion of a vehicle on a surface to obtain velocity observation measurements** which aid in the estimation of the alignment of the IMU as well as the forward velocity of the vehicle. It is shown that this can be achieved **without any external sensing provided certain observability conditions are met**… The results show that **use of these constraints and vehicle speed guarantees the observability of the attitude and velocity and hence bounds the errors associated with these states**."

**The measurement model. [DERIVED — standard form, consistent with the above]**

Two pseudo-measurements, applied in the **body frame**:

```
z = [ v_y^b ]   =  [ 0 ]  + noise      (no sideways translation)
    [ v_z^b ]      [ 0 ]              (no vertical translation / no leaving the ground)

with  v^b = C_n^b · v^n
```

**What it actually bounds — being precise, because this is routinely overstated:**

- **[DERIVED]** Linearising the lateral row about a nominal heading, the residual is
  `δv_y^b ≈ δv_lateral + v_forward · δψ_rel`,
  where `δψ_rel` is the **misalignment between the body x-axis and the velocity vector** (boresight + heading error). The heading sensitivity is **multiplied by forward speed**. Consequences:
  - **At `v_forward = 0` the heading row of the Jacobian is identically zero.** NHC contributes *nothing* to heading at standstill, and nothing during a pure turn-in-place. Applying it anyway just re-asserts what the filter already believes, and (worse) can make an EKF over-confident.
  - What NHC most directly observes is the **body-to-velocity-vector misalignment**, not absolute heading. It contains **no north reference whatsoever** — it can never fix an initial heading offset.
- **[DERIVED]** Its real, reliable payoff is **velocity and position**: it clamps lateral and vertical velocity error, which is what suppresses cross-track position drift and the vertical divergence of a free-running INS. Roll and pitch benefit too (in combination with the accelerometer/gravity), which is the "alignment of the IMU" in Dissanayake's abstract.
- **[DERIVED]** Heading benefit is **second-order and conditional**: it accrues only while translating, and it grows with speed and with manoeuvring (turns, accelerations) that make the coupling observable. Straight-line constant-speed driving is close to the degenerate case.
- **[DERIVED]** NHC is a **modelling assumption that is violated exactly when there is side-slip** — skid-steering, sliding on a smooth desk, being pushed or lifted. Feeding NHC during those events injects error rather than removing it. This is the same failure mode as feeding odometry during slip.

**A strong-sounding number, correctly caveated. [SOURCED]** Hu et al., *"Adaptive Trajectory-Constrained Heading Estimation for Tractor GNSS/SINS Integrated Navigation,"* Sensors 26(2):595, 2026, DOI `10.3390/s26020595` (PMC12845551), introduction, reports that "the use of virtual velocity constructed from Non-Holonomic Constraints (NHC) as Kalman Filter observations has shown promising results, with one study achieving a measurement azimuth accuracy of **0.0358° (RMS)**." **[DERIVED caveat]** That is a GNSS-aided SINS on a moving tractor. It is *not* evidence that NHC yields heading on a stationary or turn-in-place platform; the GNSS velocity vector is doing the heavy lifting there.

**Verdict for THIS robot. [DERIVED]** Do not implement NHC. It is aimed at a failure mode this robot does not have (INS velocity/position divergence) and is blind in the regime this robot lives in (`v_forward = 0`). The genuinely applicable members of the same family are:

- **ZUPT** (zero-velocity update) — already implemented and demonstrably working.
- **ZARU** (zero angular-rate update) — assert `ω = 0` while stationary to continuously re-estimate gyro bias. This is what actually preserves heading between bursts, and the ±0.05° / 48 s result implies the residual bias is already ≲0.001 °/s.
- **Planar / gravity constraint** — on a desk, roll and pitch are near-constant, so the yaw axis is well determined from gravity. The firmware already does this properly: it projects the gyro vector onto the measured gravity unit vector rather than picking one axis (`desktop-robot:src/turn_measure.h:41-63`), and documents that the single-axis shortcut would cost 0.97% ≈ 3.5° per 360° on this build (`desktop-robot:src/turn_measure.h:46-57`). **[SOURCED — repo]**

---

## 3. Slip detection and handling

**Family 1 — model/dynamics-based, IMU + encoders (+ optional GPS).**
C. Ward and K. Iagnemma, *"A Dynamic-Model-Based Wheel Slip Detector for Mobile Robots on Outdoor Terrain,"* IEEE Trans. Robotics, 2008, DOI `10.1109/TRO.2008.924945`, 146 citations. Abstract **[SOURCED — OpenAlex `W2130187425`]**: a "novel tire traction/braking model… used to calculate vehicle dynamic forces in an extended Kalman filter framework. Estimates of external and resistive forces are derived using measurements from wheel encoders, inertial measurement unit, and GPS. **Weak constraints are used to constrain the evolution of the resistive force estimate based upon physical reasoning.**… the technique accurately and rapidly detects robot immobilization… **Immobilization detection is shown to be robust to uncertainty in tire model parameters. Accurate immobilization detection is demonstrated in the absence of GPS.**"

**Family 2 — actuator-signal-based (motor current).**
L. Ojeda, D. Cruz, G. Reina, J. Borenstein, *"Current-Based Slippage Detection and Odometry Correction for Mobile Robots and Planetary Rovers,"* IEEE Trans. Robotics, 22(2), 2006, DOI `10.1109/TRO.2005.862480`, 152 citations. Abstract **[SOURCED — OpenAlex `W2097108504`]**: "estimates wheel slippage from motor current measurements, and adjusts encoder readings affected by slippage accordingly. The method… **works only in the direction of motion, but not laterally**, and it requires some knowledge of the terrain. However, this knowledge does not have to be provided ahead of time by human operators" — three auto-tuning techniques, two requiring GPS ground truth, "the third technique does not require any position ground truth, but is less accurate."

**Family 3 — threshold-based reject-the-update, and learned continuous compensation.**
Reginald, Al-Buraiki, Choopojcharoen, Fidan, Hashemi, *"Visual-Inertial-Wheel Odometry with Slip Compensation and Dynamic Feature Elimination,"* Sensors 25(5):1537, 2025, DOI `10.3390/s25051537` (PMC11902339). Introduction **[SOURCED]**: "Liu et al. address slip effects by **detecting and omitting updates during slip events using a threshold-based method**, ensuring real-time performance but leaving the underlying slip dynamics unmodeled. In contrast, our work utilizes GPR alongside other tools to estimate wheel slippage continuously." Abstract **[SOURCED]**: Gaussian-process regression with deep-kernel/LSTM layers models slippage-induced error inside an MSCKF; the paper also notes that existing VIWO schemes "often assume little to no wheel slippage, which does not hold in practice."

**What systems actually DO once slip is detected [DERIVED — synthesis of the three families]:**

| Response | Used by | Applicable here? |
|---|---|---|
| Reject / skip the odometry update (or inflate `R`) | threshold methods | N/A — no odometry update exists |
| Substitute a different channel for the interval | gyrodometry, VIWO | N/A — gyro is already the only channel |
| Estimate a slip ratio and subtract it | Ojeda, Ward, GPR/LSTM | N/A — nothing to correct |
| Declare **immobilisation** and change behaviour | Ward & Iagnemma | **Yes — this is the one that transfers** |

**Encoder-free detection for THIS robot — four channels. [DERIVED unless noted]**

- **Channel A — command-vs-gyro rate residual (primary).** In the steady window of a burst, compute `r = ω_measured − ω_pred(command)`. The tether experiment is the textbook positive case: 2000 ms commanded produced only 55–101°, i.e. `ω_measured ≈ 28–50 °/s` against a full-command prediction of ~285 °/s — a residual so large no careful thresholding is needed. Gate this to the steady window to avoid firing on the ramp; the firmware already has exactly the right primitive (`TurnMeasure::WindowedRate`, which skips `skipUs` of ramp and averages over `holdUs`) — `desktop-robot:src/turn_measure.h:155-185`. **[SOURCED — repo]**
- **Channel B — burst-integral shortfall.** Compare integrated degrees over the burst against `predictDurationMs`-implied degrees. Catches slow-onset restraint that a rate threshold might ride through. The prediction primitive already exists: `desktop-robot:src/turn_measure.h:207-212`. **[SOURCED — repo]**
- **Channel C — accelerometer transients.** Do not try to double-integrate accelerometer data for distance on a 0.3–1.3 s burst; that is hopeless with a consumer MEMS part. Do use the **start and stop jerk transients**: when the wheels bite, there is a distinct acceleration step at motor-on and motor-off. Their *absence* under a non-zero command means the chassis never accelerated — free-spinning wheels, held robot, or blocked drive. The accelerometer is already initialised and streaming at 500 Hz and is currently sampled only once per calibration, so this is free (`desktop-robot:HARDWARE.md:116-118`). **[SOURCED for the hardware availability; DERIVED for the technique]**
- **Channel D — battery-rail sag as a proxy for motor current (the encoder-free analogue of Ojeda's current method).** Ojeda et al.'s insight is that *motor current* reveals load. This robot has no per-servo current sense, but it does have `BAT_ADC` on GPIO1 behind a 200 kΩ/100 kΩ divider, and the servos hang off an external buck-boost enabled by GPIO3 (`desktop-robot:HARDWARE.md:72,82`). Sampling battery voltage during a burst should show a load-dependent sag. **[SOURCED for the hardware; INFERRED for the signal — not measured, needs a bench test]**

**Resulting classification table [DERIVED]:**

| Command | Rail sag (D) | Gyro rate (A) | Diagnosis | Action |
|---|---|---|---|---|
| ≠ 0 | yes | ≈ 0 | Stalled / blocked / held | Abort burst, back off, report. Do **not** keep driving into a stall (servo damage risk) |
| ≠ 0 | yes | < predicted | Slipping or restrained | Closed loop already extends the burst; flag distance estimate as invalid |
| ≠ 0 | no | ≈ 0 | Power/enable fault (buck-boost EN, brownout) | Hard fault, do not retry blindly |
| = 0 | no | ≠ 0 | Robot pushed / picked up / knocked | Heading is still valid (gyro sees it); position is not |

**Crucial framing point [DERIVED].** On this robot slip does **not** corrupt heading — the gyro measures the actual rotation regardless of what the wheels did. Slip corrupts *the plan*: distance travelled, whether a pattern returned to its start, and whether a timed fallback is safe. So the correct response to detected slip is **behavioural and diagnostic**, not a state correction. This is the opposite of the encoder-robot case and it is why importing slip-correction machinery wholesale would be wasted effort.

---

## 4. Open-loop servo drive: state of practice

**What a continuous-rotation servo is, from the vendor.** Pololu FS90R product page (`https://www.pololu.com/product/2820`) **[SOURCED]**:

> "The FS90R continuous rotation servo **converts standard RC servo position pulses into continuous rotation speed**. The default rest point is 1.5 ms, but this can be adjusted by using a small screwdriver to turn the **middle-point adjustment potentiometer**. Pulse widths above the rest point result in counterclockwise rotation, with speed increasing as the pulse width increases; pulse widths below the rest point result in clockwise rotation…"

and, directly relevant to §6:

> "Note that, **as with most hobby servos, stalling or back-driving this servo can damage it**."

**Three tiers of practice [DERIVED, anchored on the vendor pages]:**

1. **Pure open-loop timed bursts + hand-trimmed centre.** The beginner default. Non-repeatable across battery voltage, surface friction, servo temperature and load. This robot's own data is the indictment: 600 ms → 50–84°, 2000 ms → 55–101°, not monotonic in time.
2. **Closed loop on an external sensor — integrate gyro yaw rate, stop at the target angle.** *This is the standard and correct answer for heading on a position-less drivetrain*, and it is what has been built. The firmware's own reasoning is exactly right and worth preserving as documentation: "strictly better than a timed turn for accuracy — it is immune to battery level, surface grip, motor warmth and wheel slip, none of which a fixed duration can adapt to. The tradeoff is that net-zero stops being a compile-time property of the pattern and becomes a runtime property of the sensor, so **the timeout is not optional**" (`desktop-robot:src/turn_measure.h:214-224`). **[SOURCED — repo]**
3. **Replace the actuator with one that has feedback.** The industry answer to "CR servos have no feedback" is a CR servo *with* feedback. Parallax Feedback 360°, via Pololu (`https://www.pololu.com/product/3432`) **[SOURCED]**: "a return signal line from an internal Hall effect sensor system that provides **digital angular position feedback**… your application can make the servo turn to and hold any angle, much like a standard servo but with an unlimited range of motion. Or, you can **rotate the servo continuously at a controlled, verifiable speed as a robot drive motor**. The feedback signal is a 3.3 V PWM with a fixed period of approximately 1.1 ms and a duty cycle that corresponds to the rotational position of the servo."

**Is closed-loop-on-gyro the best answer? [DERIVED]**

- **For heading: yes.** Nothing cheaper is better. A gyro is the *right* instrument for rotation; and specifically on this robot a magnetometer would be worse (two motors, a LiPo under load and steel gearing inches away — the firmware already documents this at `desktop-robot:src/turn_measure.h:7-13`), and there is no magnetometer fitted anyway (`desktop-robot:HARDWARE.md:108-110`).
- **For translation: no — and this is the gap.** A gyro cannot observe forward slip. Drive 30 cm forward on a slippery desk and the gyro reports the same near-zero yaw whether you moved 30 cm or 3 cm. Closed-loop-on-gyro solves half the problem and leaves the other half completely open. The standard fixes are tier 3 (feedback servos) or an **optical-flow / mouse sensor** looking at the desk surface.
- **Refinements over naive gyro closed loop [DERIVED]:** (a) predict-and-lead termination rather than react-and-stop (§6); (b) a terminal speed ramp so the loop closes at low rate; (c) deadband-inverse command mapping so the ramp is actually achievable (§5); (d) sample at the IMU's native 500 Hz rather than at the main-loop rate, since the loop's angular resolution is `ω · Δt`.

---

## 5. Deadband and rate modelling

**Is linear-with-deadband the right form? Yes — it is literally how the class of device is specified. [SOURCED]** Parallax Feedback 360° via Pololu: "the **speed response is nicely linear** across the control signal range of **1280 µs to 1720 µs**. The **rest point is at 1500 µs, with a 20 µs deadband on either side**." That is the textbook dead-zone nonlinearity:

```
ω(u) =  k⁺ · (u − d⁺)     u > d⁺
        0                 d⁻ ≤ u ≤ d⁺
        k⁻ · (u − d⁻)     u < d⁻
```

**The measured model, read carefully. [DERIVED]**
`rate = 0.91·speed − 24.2 °/s` puts the zero-crossing at `speed = 24.2/0.91 = 26.6 ≈ 27`. Two observations:

1. **Unit reconciliation.** If `speed` were a −100…100 or −127…127 command, full command would predict only 67–91 °/s, which contradicts the measured 285 °/s at full command. If `speed` is **microseconds of pulse offset from the 1500 µs rest point**, full offset ≈ 340 µs gives exactly 285 °/s, and the deadband lands at **27 µs** — remarkably close to the vendor-published **20 µs** for a comparable CR servo. The µs reading is almost certainly correct and is independently corroborated by the vendor spec. **[DERIVED / INFERRED]**
2. **The model is contaminated.** It was fitted while USB-tethered, and the tether demonstrably restrained rotation. A restraint depresses the slope and inflates the apparent deadband (extra static friction to overcome). **Re-fit untethered before using it for anything.** **[DERIVED]**

**Should it be per-direction and per-side? Yes — for three independent reasons. [DERIVED]**

1. The nonlinearity is intrinsically two-sided with **independent breakpoints** `d⁺` and `d⁻`. The mechanical trim pot centres the *rest point*, not the midpoint between the breakpoints, so a perfectly trimmed servo can still be asymmetric.
2. Each servo has its own pot, its own gear friction, and its own wear → per-side asymmetry on top of per-direction asymmetry.
3. For **turn-in-place specifically**, both wheels must break free simultaneously and against opposite chassis reaction, so the *effective* system deadband is not either servo's deadband but roughly `max(d_L, d_R) + chassis static friction`. Command 27 is a **system** deadband, not a servo deadband — which is also why it should be re-measured for spin and for straight-line drive separately.

**This robot already has the right data structure for it. [SOURCED — repo]** `DriveCalibration::MotorCalibration` stores five independent breakpoints per motor:

```c
struct MotorCalibration {
  uint16_t stopUs;       // rest point
  uint16_t fwdStartUs;   // forward deadband breakpoint  d⁺
  uint16_t revStartUs;   // reverse deadband breakpoint  d⁻
  uint16_t fwdMaxUs;
  uint16_t revMaxUs;
};
```
`desktop-robot:src/drive_calibration.h:31-37`, with per-side `Left`/`Right` (`:7-10`) and the documented rule "Every field must be 1000..2000; stop must be 1450..1550. **Direction polarity derives from each start pulse relative to stop**" (`:72-77`). The calibration model is already correct; what is missing is *using* it as a dead-zone inverse.

**How people calibrate deadband [DERIVED, anchored on the vendor pages]:**

1. **Hardware centring** — trim the middle-point potentiometer until the wheel is stationary at 1500 µs. **[SOURCED — Pololu FS90R page]** Do this once, first; software cannot undo a badly centred pot cleanly.
2. **Software staircase, per side and per direction** — step the pulse away from `stopUs` in ~2 µs increments, dwell ~200 ms, and record the **first command that produces gyro rate above a noise floor** (e.g. 5× the resting gyro σ, measured with the existing `BiasEstimator`, `desktop-robot:src/turn_measure.h:87-101`). That value is `fwdStartUs` / `revStartUs`. Do it for one wheel at a time (drive one, hold the other at stop) so you measure each servo, then repeat with counter-rotation to get the *system* spin deadband.
3. **Software compensation — the dead-zone inverse.** Given a desired normalised effort `u ∈ [−1, 1]`, do not emit `stop + u·span`; emit from the breakpoint:

```
pulse(u) =  stop                                        u == 0
            fwdStart + u  · (fwdMax − fwdStart)         u  > 0
            revStart + |u|· (revMax − revStart)         u  < 0     (revStart/revMax on the far side of stop)
```

**Does compensating improve small/slow motions? Yes — that is precisely and only what it buys. [DERIVED]**

- Without it, the lowest ~8% of the command range (27/340) is dead, and the response has a **step discontinuity** at the breakpoint: small commanded efforts either do nothing or jump straight to ~0.91·(first-live-command) °/s. That makes any terminal ramp, gentle settle, or fine trim impossible.
- With it, the effective map is continuous and monotone from zero, which is exactly what the §6 terminal ramp needs.
- **Residual limitation:** the inverse cannot give you speeds *below* the breakpoint speed. The minimum non-zero rate is quantised. If you need finer control than that, the fix is **time-quantisation** — short pulses of above-breakpoint motion with off time between — not smaller amplitudes.
- **Drift:** breakpoints move with battery voltage, temperature and wheel load, so either recalibrate periodically or (better) let the gyro closed loop absorb the residual — which it does, provided the closed loop can actually command low rates, which requires the inverse. **[INFERRED]**

---

## 6. Coast/overshoot compensation (with recommended formula)

**Measured:** 7–9° of coast after motors stop, at full speed (~285 °/s).

**Option assessment [DERIVED unless noted]:**

| Approach | Fit for hobby CR servos on a light chassis | Verdict |
|---|---|---|
| **Lead termination** (stop early by predicted coast) | No extra actuator stress; uses the sensor already in the loop; degrades gracefully | **Primary recommendation** |
| **Deceleration ramp** (reduce speed before the target) | Reduces `ω` at cut-off, so the coast shrinks *and* its variance shrinks; needs deadband inverse to be achievable | **Strong secondary — combine with lead** |
| **Active braking** (drive reverse briefly) | Vendor: "as with most hobby servos, **stalling or back-driving this servo can damage it**" **[SOURCED — Pololu FS90R page]**. Reversal at speed is a current spike through a micro geartrain; and with no encoder you cannot verify the brake pulse's magnitude, so it can bite and produce a *reverse* overshoot that is less repeatable than the coast it replaced | **Avoid.** At most a very short fixed de-energise pulse, and only if lead+ramp prove insufficient |
| **Closed-loop settling** (re-nudge after the coast) | The minimum achievable nudge is a deadband-speed pulse, which itself coasts; creates limit-cycle hunting | **Fallback only**, and only as time-quantised micro-pulses |

**Recommended concrete approach.**

**Step 1 — Calibrate the coast constant. [DERIVED]** For each of ~3 speeds (min-usable, mid, max): spin to steady state, cut the motors, and integrate gyro from cut-off until `|ω| < ε` (ε ≈ 3σ of the resting gyro). This yields pairs `(ω_cut, θ_coast)`. Fit both candidate models and keep the better fit:

- Coulomb-friction-dominant: `θ_coast = ω²/(2α)`, `t_coast = ω/α`
- Viscous / first-order-dominant: `θ_coast = k_coast · ω`, with `k_coast = τ` (an effective time constant)

**Step 2 — Use the measured numbers.** From the supplied data (`θ_coast ≈ 8°` at `ω ≈ 285 °/s`) **[DERIVED]**:

```
Linear model     k_coast = 8 / 285          ≈ 0.028 s     (≈ 28 ms of "extra rotation")
Quadratic model  α       = 285² / (2 · 8)   ≈ 5077 °/s²   (t_coast ≈ 56 ms)
```

Prefer the **linear model as the default**: it is robust, it cannot blow up, and it degrades safely (it under-leads rather than over-leads at high rate if the true behaviour is quadratic — under-leading leaves a small overshoot, over-leading leaves an undershoot that requires a second nudge you cannot make small).

**Step 3 — The lead-angle formula and termination rule. [DERIVED]**

```
θ_lead(ω_now) = k_coast · |ω_now|                      // k_coast ≈ 0.028 s

STOP  when   progress + θ_lead(ω_now) ≥ target
```

Critical detail: use the **instantaneous measured gyro rate** `ω_now` (a 3–5 sample moving average to keep noise out), **not the commanded speed**. This makes the lead self-correcting: a slipping, restrained, or low-battery chassis is turning slower, so it coasts less, so it leads less — automatically. This is the property that makes the whole scheme robust to the exact failure mode this robot exhibits.

**Step 4 — Terminal speed ramp (this is where the accuracy actually comes from). [DERIVED]**
For the last `max(2·θ_lead_full, 15°)` of the turn, drop the wheel command to just above the deadband — a command giving `ω ≈ 15–30 °/s`. Then:

```
θ_coast_terminal = 0.028 · 15  ≈ 0.42°
θ_coast_terminal = 0.028 · 30  ≈ 0.85°
```

**The coast collapses from 7–9° to well under 1°**, and — more importantly — so does its *variance*. Lead termination alone caps the mean error; the ramp caps the spread. Do both.

**Step 5 — Guards. [DERIVED]**
- Clamp `θ_lead` to `[0, θ_lead_max]`.
- Never let `θ_lead` exceed ~half the target; for targets smaller than `θ_lead_full`, start at the reduced speed rather than trying to lead more than the turn contains.
- Keep the existing mandatory timeout (`desktop-robot:src/turn_measure.h:214-224`, `TurnController::timedOut`) — the lead logic must never be able to defer a stop indefinitely.
- Report both mean and standard deviation of final error over ≥10 runs. After lead+ramp, the residual is dominated by coast *variance* and gyro *scale-factor* error, not by the coast mean.

---

## 7. Achievable turn-in-place accuracy

**Open loop, timed, on this platform — tens of degrees. [SOURCED — supplied hardware measurements]** 600 ms commanded → 50–84°; 2000 ms commanded → 55–101°; rotation did not scale with time. Any accuracy claim for open-loop timed turns on a slipping/restrainable chassis is unfounded.

**Encoder odometry, calibrated — an order of magnitude better than uncalibrated, for *systematic* error only. [SOURCED]** Borenstein & Feng, UMBmark, IEEE T-RA 12(6) 1996, DOI `10.1109/70.544770`: "consistent improvement of at least one order of magnitude in odometric accuracy (with respect to systematic errors) in a mobile robot calibrated with our method." Not applicable directly (no encoders), and note the explicit scoping to systematic errors — UMBmark does not help with slip.

**Gyro closed loop — full error budget. [DERIVED]**

| Term | Magnitude on this robot | Notes |
|---|---|---|
| Coast / termination (current) | **7–9°** | Dominant today; reducible to <1° per §6 |
| Gyro scale-factor error, uncalibrated | ~1–3% → **3–11° per 360°** | Consumer MEMS sensitivity tolerance; calibrate against a known 360° |
| Gyro scale-factor error, calibrated | <0.5% → **<1.8° per 360°**, typically far less | Second-largest term after coast is fixed |
| Axis-projection error (if single-axis were used) | 0.97% → **3.5° per 360°** | Already eliminated by projecting onto measured gravity — `desktop-robot:src/turn_measure.h:46-57` **[SOURCED — repo]** |
| Bias × duration | 1.3 s × residual bias | At a sloppy 0.5 °/s → 0.65°. With the demonstrated ZUPT quality (±0.05° over 48 s ⇒ residual bias ≲0.001 °/s) this term is **<0.01° — negligible** |
| Sample quantisation | `ω · Δt` | At 285 °/s: 1.4° at 200 Hz, **0.57° at 500 Hz**. The IMU already streams at 500 Hz (`desktop-robot:HARDWARE.md:116-118`) |

**Answers to the direct questions [DERIVED]:**

- **Is ±1° realistic?** **Yes**, for a single commanded turn — but only with *all* of: lead termination + terminal ramp (kills the 7–9°), gyro scale-factor calibration (kills the 1–3%), and ≥200 Hz (preferably 500 Hz) sampling in the turn loop (kills the 1.4°). Miss any one and you are at several degrees.
- **Is ±0.1° realistic?** **No, not for a commanded turn.** It is below one sample-interval of rotation at full speed (0.57° at 500 Hz), below the run-to-run *variance* of the coast, and below what a deadband-limited actuator can trim. You could approach it by ending every turn at a crawl and accepting a much longer settle, at which point you are trading time for a precision the chassis cannot mechanically hold anyway (a light robot on a desk gets nudged by its own cable and by surface texture).
- **The distinction that matters most.** Separate **"how well do I know my heading"** from **"how accurately can I execute a commanded turn."** The former is already ±0.05° over 48 s stationary — excellent, and a genuine achievement of the ZUPT design. The latter is currently ~8° and is limited by actuator termination, not by the sensor. Reporting them as one number would badly mislead.
- **Comparable literature figures, correctly caveated. [SOURCED]** Hu et al., Sensors 26(2):595, 2026 report that agricultural navigation requires heading accuracy better than 0.5°, and their method achieves "RMS heading error below 0.15° for straight-line tracking and 0.25° for curved paths." **[DERIVED caveat]** That is GNSS/SINS heading *estimation* on a large, slow, heavy vehicle — do **not** read it as a turn-execution specification for a hobby-servo desk robot.

---

## Recommendations for THIS robot (ranked: gain, cost, risk)

| # | Recommendation | Gain | Cost | Risk |
|---|---|---|---|---|
| 1 | **Lead-angle termination** `θ_lead = 0.028·\|ω_now\|` **plus terminal speed ramp** to ω ≈ 15–30 °/s for the last ~15–20° | **Highest.** Removes the dominant 7–9° error → ~±1° | ~30 lines in `TurnController` + a coast-calibration routine (3 speeds × 5 runs) | Low. Guard with clamps and keep the existing timeout |
| 2 | **Re-measure the rate model untethered** | Removes a known-contaminated model; also resolves the 285 °/s vs `0.91·s−24.2` unit inconsistency | One calibration session | None |
| 3 | **Calibrate gyro scale factor** against a mechanical 360° reference, untethered, at operating rate | Removes the second-largest term (1–3% → <0.5%) | A manual fixture/protractor procedure | Low. Must be done near the operating rate; check `isSaturated` never trips (`src/turn_measure.h:75-76`) |
| 4 | **Run the turn loop at the IMU's native 500 Hz** (or interpolate the threshold crossing) | Removes up to 1.4° of quantisation → 0.57° or better | Loop timing / sampling path | Low. `Integrator::worstGapUs()` already exists to verify (`src/turn_measure.h:123`) |
| 5 | **Dead-zone inverse using existing `fwdStartUs`/`revStartUs`** | Makes small/slow motion monotone and possible — a *precondition* for #1's ramp | A mapping function; per-side/per-direction staircase calibration | Low. Keep raw calibration values authoritative; never let the inverse write back |
| 6 | **Command-vs-gyro slip/stall detector** (Channel A), gated to the steady window, with behavioural response | Turns silent slip failures into visible ones; protects against driving into a stall (servo damage) | Small — `WindowedRate` + rate model already exist | Low. Gate to steady window or it false-fires on the ramp |
| 7 | **Battery-sag cross-check** (Channel D) to separate "no drive" from "blocked drive" | Diagnostics; distinguishes power fault from mechanical fault | ADC sampling during bursts | **Unverified** — bench-test the sag signal before relying on it |
| 8 | **Do NOT implement NHC. Do NOT implement gyrodometry as published. Do NOT active-brake the servos.** | Avoided harm and avoided wasted effort | Zero | Zero |
| 9 | *If translation accuracy ever matters:* feedback servos (Parallax Feedback 360°) or an optical-flow/mouse sensor | Real odometry; the gyro fundamentally cannot see forward slip | Hardware + mechanical redesign | Medium-high. Only justified if straight-line distance becomes a requirement |

---

## Could not verify

- **The numeric accuracy claims inside Borenstein & Feng 1996.** The paper is closed access: Unpaywall (`api.unpaywall.org/v2/10.1109/robot.1996.503813`) returns `is_oa: false`, `oa_status: "closed"`, `has_repository_copy: false`, `oa_locations: []`. The author's UMich page (`.../~johannb/Papers/paper63.pdf`) returns HTTP 403. Only the abstract's qualitative claim ("ill-effects of gyro drift are almost completely eliminated") is verified.
- **Deadband width and per-direction asymmetry specs for the MG90D in continuous-rotation form.** Manufacturer datasheets in reach are PDFs, and this environment cannot text-extract PDFs (they return raw binary). The 20 µs figure I cite is for a *comparable* CR servo (Parallax Feedback 360°), used as a sanity anchor, not as a spec for the MG90D.
- **Any peer-reviewed head-to-head "open-loop vs gyro-closed-loop turn accuracy" benchmark for sub-kilogram hobby-servo robots.** Searched Europe PMC for `"turning angle" AND "gyroscope" AND "mobile robot"` (2 hits, neither relevant) and `"continuous rotation servo"` (10 hits, none robotics-accuracy). The ±1° / ±0.1° verdicts in §7 are a **derived error budget**, not a cited benchmark.
- **The QMI8658C scale-factor accuracy on this specific unit** — not measured, and the largest un-retired error term after coast.
- **The battery-sag slip channel (Channel D)** — inferred from `HARDWARE.md` (GPIO1 divider, GPIO3 servo buck-boost EN); no measurement exists.
- **Tooling limits encountered:** Google, Bing, DuckDuckGo, Mojeek and Ecosia all returned 403 / error pages; MDPI direct returned 403; arXiv and ar5iv returned title-only stubs; the arXiv API timed out and Semantic Scholar's API rate-limited (429). All literature was therefore obtained via **OpenAlex, Crossref, Unpaywall and Europe PMC REST APIs** plus vendor HTML. GitHub tooling (`github/get_me` etc.) was not available in this environment.

---

## Adjacent, not pursued

- **Wheel-mounted IMU dead reckoning (Wheel-INS, Niu et al., arXiv:1912.07805 / IEEE TVT 2021 DOI `10.1109/TVT.2021.3108008`)** — gets odometry without encoders by putting an IMU *on the wheel*; needs a second IMU plus wireless/slip-ring, out of scope here.
- **Visual-inertial-wheel odometry with learned slip compensation (GPR + LSTM deep kernel inside an MSCKF)** — Sensors 25(5):1537 — far beyond this MCU's compute and sensor budget.
- **Optical-flow / mouse-sensor desk odometry** — the natural encoder replacement for *translation*, the one thing the gyro cannot supply.
- **UMBmark bidirectional square-path protocol** as a repeatable test harness for separating systematic from random error.
- **Allan-variance characterisation of the QMI8658C** to set ZUPT/ZARU process noise on evidence rather than by feel.
- **Formal dead-zone inverse adaptive control (Tao & Kokotović lineage)** — the rigorous version of §5's hand-built inverse, only worth it if the breakpoints drift enough to need online adaptation.
- **PCF85063A RTC (fitted, never initialised)** — could timestamp calibration runs so scale-factor/deadband drift can be tracked over sessions.

---

## References

### Load-bearing

1. **J. Borenstein, L. Feng**, "Gyrodometry: a new method for combining data from gyros and odometry in mobile robots," *Proc. IEEE ICRA 1996*, vol. 1, pp. 423–428. DOI `10.1109/ROBOT.1996.503813`. Abstract via OpenAlex `W2111152968`; metadata via Crossref; OA status via Unpaywall (closed). — §1
2. **G. Dissanayake, S. Sukkarieh, E. Nebot, H. Durrant-Whyte**, "The aiding of a low-cost strapdown inertial measurement unit using vehicle model constraints for land vehicle applications," *IEEE Trans. Robotics and Automation* 17(5), 2001. DOI `10.1109/70.964672`. OpenAlex `W2160626949`, 469 citations. — §2
3. **C. Ward, K. Iagnemma**, "A Dynamic-Model-Based Wheel Slip Detector for Mobile Robots on Outdoor Terrain," *IEEE Trans. Robotics*, 2008. DOI `10.1109/TRO.2008.924945`. OpenAlex `W2130187425`. — §3
4. **L. Ojeda, D. Cruz, G. Reina, J. Borenstein**, "Current-Based Slippage Detection and Odometry Correction for Mobile Robots and Planetary Rovers," *IEEE Trans. Robotics* 22(2), 2006. DOI `10.1109/TRO.2005.862480`. OpenAlex `W2097108504`. — §3
5. **J. Borenstein, L. Feng**, "Measurement and correction of systematic odometry errors in mobile robots" (UMBmark), *IEEE Trans. Robotics and Automation* 12(6), 1996. DOI `10.1109/70.544770`. OpenAlex `W2102145772`, 795 citations. — §1, §7
6. **Pololu**, FEETECH FS90R micro continuous rotation servo product page — `https://www.pololu.com/product/2820` (rest point, middle-point trim pot, back-driving/stall damage warning). — §4, §5, §6
7. **Pololu**, Parallax Feedback 360° High-Speed Servo product page — `https://www.pololu.com/product/3432` (linear 1280–1720 µs response, 1500 µs rest, ±20 µs deadband, Hall-effect position feedback). — §4, §5
8. **N. Reginald, O. Al-Buraiki, T. Choopojcharoen, B. Fidan, E. Hashemi**, "Visual-Inertial-Wheel Odometry with Slip Compensation and Dynamic Feature Elimination," *Sensors* 25(5):1537, 2025. DOI `10.3390/s25051537`, PMC11902339. — §3
9. **S. Hu et al.**, "Adaptive Trajectory-Constrained Heading Estimation for Tractor GNSS/SINS Integrated Navigation," *Sensors* 26(2):595, 2026. DOI `10.3390/s26020595`, PMC12845551. — §2, §7
10. **This repository** — `desktop-robot:src/turn_measure.h:7-13, 41-63, 75-82, 87-101, 155-185, 187-212, 214-224`; `desktop-robot:src/drive_calibration.h:7-10, 31-37, 72-77`; `desktop-robot:HARDWARE.md:72, 82, 108-110, 116-118`. — §2, §3, §4, §5, §6, §7

### Background

- **K.-W. Chiang et al.**, "Navigation Error Characteristics of LIO-, VIO-, and RIMU-Assisted INS/GNSS Multi-Sensor Fusion Schemes in a GNSS-Denied Environment," *Sensors* 26(7):2068, 2026. DOI `10.3390/s26072068`, PMC13074926 — heading-*change* updates vs heading updates; odometry-platform lever-arm/boresight formulation.
- **M. Elsheikh, W. Abdelfatah, A. Noureldin, U. Iqbal, M. Korenberg**, "Low-Cost Real-Time PPP/INS Integration for Automated Land Vehicles," *Sensors* 19(22):4896, 2019. DOI `10.3390/s19224896`, PMC6891817 — INS mechanisation in the local-level frame.
- **M. Moussa, S. Zahran, M. Mostafa, A. Moussa, N. El-Sheimy, M. Elhabiby**, "Optical and Mass Flow Sensors for Aiding Vehicle Navigation in GNSS Denied Environment," *Sensors* 20(22):6567, 2020. DOI `10.3390/s20226567`, PMC7698455 — "differential wheel odometry" heading-change concept implemented with non-wheel sensors; useful analogue for encoder-free heading-rate estimation.
- **D. Guffanti, W. Pavon**, "Multivariate System Identification of Differential Drive Robot: Comparison Between State-Space and LSTM-Based Models," *Sensors* 25(18):5821, 2025. DOI `10.3390/s25185821`, PMC12473858 — v/ω coupling and identification for differential drive.
- **X. Niu et al.**, "Wheel-INS: A Wheel-Mounted MEMS IMU-Based Dead Reckoning System," *IEEE TVT*, 2021. DOI `10.1109/TVT.2021.3108008`; preprint arXiv:1912.07805 (OpenAlex `W3115461992`) — encoder-free odometry via wheel-mounted IMU.
- **A. Khumaidi**, "Pemetaan Posisi Robot Soccer Menggunakan Gyrodometry dan Trigonometry…," *Jurnal Poli-Teknologi* 19(3), 2021. DOI `10.32722/pt.v19i3.2864` — a secondary application of gyrodometry (PDF only; not text-extractable in this environment).