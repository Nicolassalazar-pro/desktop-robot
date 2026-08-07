# Gyro/Accel Fusion Fundamentals for a 6-Axis Desk Robot (QMI8658C, No Magnetometer)

## Verdict

- **`ω_yaw = ω · ĝ` is exactly correct here, not an approximation.** The closed form is `ω·ĝ_down = ψ̇ − φ̇·sin θ`; the residual vanishes when roll rate is zero or pitch is zero, both true for a differential-drive robot with fixed body tilt on a flat desk. [DERIVED]
- **The 3.48°/360° arithmetic is right but the premise is unverified, and the fix is a coin-flip until it is.** QMI8658C accel **Initial Offset Tolerance is ±100 mg** and the observed Y/Z components are **exactly ±100 mg**. If those are offsets rather than tilt, single-axis X was already exact and the live projection *costs* 3.48°/360°. [SOURCED: QST DS Rev 0.9 §3.4 Table 7 + DERIVED]
- **Yaw is structurally unobservable from gravity: `∂f_b/∂ψ ≡ 0`, because `g_n` is the +1 eigenvector of `R_z(ψ)`.** No filter, no averaging, no added 6-axis data recovers it. Gyrocompassing needs bias < 0.186 °/h; this part's measured bias is 0.53 dps = **1908 °/h**, i.e. **10,250× short**. [DERIVED + SOURCED: arXiv:1805.10410]
- **The dominant *sensing* error is gyro scale factor: ±3% Initial Sensitivity Tolerance = ±10.8° per 360°, three times the projection term, and it does NOT self-cancel through `spin auto`.** [SOURCED: QST DS Rev 0.9 §3.4 Table 8 + DERIVED]
- **But the dominant error overall is mechanical: 7–9° of coast.** That is ~2.6× the projection term, ~300× the residual drift budget, and larger than everything except uncalibrated scale factor. Fusion work past scale-factor calibration is chasing noise. [DERIVED from measured baseline]
- **g-sensitivity (±0.1 dps/g) is negligible here (≤0.04°/turn) and mostly pre-absorbed by the ZUPT.** The unquantified risk is g²/VRE, which QST does not specify at all. [SOURCED + DERIVED]

## Findings

### 1. Gravity projection — derivation, sign trap, and when it breaks

ZYX Euler kinematics with body rate `ω = (p,q,r)`:
```
p = φ̇ − ψ̇ sinθ ;  q = θ̇ cosφ + ψ̇ cosθ sinφ ;  r = −θ̇ sinφ + ψ̇ cosθ cosφ
ĝ_down = Rᵀ[0,0,1]ᵀ = (−sinθ, cosθ sinφ, cosθ cosφ)ᵀ
```
The θ̇ terms cancel identically; the ψ̇ terms collect to `ψ̇(sin²θ + cos²θ)`. Result:

> **`ω · ĝ_down = ψ̇ − φ̇ · sinθ`** — exact, no small-angle approximation. [DERIVED]

`ω·ĝ` *is* the component of angular velocity about the local vertical by definition; it differs from the Euler derivative ψ̇ only by the gimbal term. With `φ̇ = θ̇ = 0` on a desk, the projection is exact. [DERIVED]

**Sign.** The accelerometer measures specific force `f_b = −Rᵀg_n`, which points **up**. The firmware projects onto the raw resting accel (`src/turn_measure.cpp:49-60`, fed by `g_turnGravity` from `src/main.cpp:5107-5112`), so it computes `ω·â_up = −(ψ̇ − φ̇ sinθ)` — an ENU-style yaw, positive counter-clockwise from above, opposite in sign to NED. Self-consistent because the same code performed `spin auto`, and `g_turnGravity[0] = +1.01` keeps it in phase with `+ω_x`. **Negating the vector "to make it gravity" would silently invert every heading.** [DERIVED from source]

**Assumptions and their failure magnitudes.** (A1) `ĝ` is the true local vertical — the live risk, ±100 mg offset spec = up to 5.7° direction error [SOURCED]. (A2) `φ̇ = 0` or `θ = 0` — broken transiently by torque-induced pitch-back, but integrates to ≈0 over a symmetric manoeuvre [INFERRED]. (A3) Gyro triad orthogonal and aligned — cross-axis ±2% costs up to 1.4°/360° through the projection (§4) [DERIVED]. (A4) Yaw means rotation about local vertical — driving onto a 5° ramp with a stale `ĝ` costs `1−cos5° = 0.38%` = 1.4°/360°, second-order and benign [DERIVED].

**Low-pass filtering: not needed for the snapshot design, and it would not help.** Accel noise density 200 µg/√Hz [SOURCED: QST DS Table 10]; at the configured aODR (code `0100`, nominal 500 Hz → **actually 470 Hz** with the gyro co-enabled) and default 27.5%-of-ODR bandwidth ≈ 129 Hz, per-sample σ = `200e-6 × √129 = 2.27 mg` → 0.18° of direction noise → gain error `1−cos(0.18°) = 5e-6`. **The dominant `ĝ` error is a bias, and averaging cannot reduce a bias.** [DERIVED]

**In motion, filtering is necessary but insufficient.** At 285 °/s (4.974 rad/s) with a 40 mm lever arm, centripetal `ω²r = 0.99 m/s² = 0.101 g` — a 5.8° apparent tilt, **DC for the whole turn**, so no LPF removes it. Correct treatment is gyro-based compensation of `ω×(ω×r) + ω̇×r` [SOURCED: Arzberger & Nüchter, arXiv July 2026, which notes the effect "gets amplified the further away the IMU is from the base" and that Madgwick/Mahony are "prone to accumulate long term error especially around the gravity vector"]. [SOURCED + DERIVED]

**Maintaining `ĝ`: snapshot at rest, refreshed on every ZUPT.** Current code takes a *single* accel sample once (`src/main.cpp:5106-5115`); averaging ~200 samples over the existing 2 s stillness window is free and yields a vibration detector via sample variance. Never recompute in motion. [INFERRED]

### 2. Validating the 3.48°/360° claim

```
|g|² = 1.01² + 0.10² + 0.10² = 1.0401 ;  |g| = 1.0198529   ✓ matches 1.01985
k = 1.01/1.0198529 = 0.9903393                             ✓ the "99.034%" claim
(1−k) × 360 = 3.4778°                                      ✓ the "~3.48°" claim
tilt = acos(0.9903393) = 7.971° ;  1−cos(7.971°) = 0.009679 ≈ θ²/2, θ=0.13913 rad
```
All correct. The loss is `1 − cos(tilt)`, **quadratic in tilt**: 0.97% at 8°, 0.24% at 4°. [DERIVED]

**Sign trap — 3.48 and 3.51 are different numbers.** A true 360° reads as `360 × 0.9903393 = 356.52°` (understates by 3.478°). But closing the loop at 360 *measured* degrees — which is what `TurnController` does (`src/turn_measure.cpp:311-313`, stops on `progressDegrees() >= target_`) — physically turns `360/0.9903393 = 363.51°`, **over-rotating by 3.512°**. Quote 3.51° when discussing turn behaviour. [DERIVED]

**Could it be accel bias rather than tilt? Yes — this is the weak point.** QST DS Rev 0.9 §3.4 Table 7: Initial Offset Tolerance **±100 mg**, Initial Sensitivity Tolerance **±6%**, Cross-Axis ±1%, TCO ±1 mg/°C, TCS ±0.04 %/°C, Non-Linearity ±0.75% [SOURCED]. The observed 0.10 g on Y and Z **is exactly the offset tolerance**. If true gravity is (1.00, 0, 0), then `ω·ĝ = 0.99034·ω_x` and the projection **under-reads by 0.966%** where plain X was exact — a symmetric −3.48°/360° penalty. [DERIVED]

Constraints that narrow it: accel cross-axis (±1%) leaks ≤10 mg from 1 g, one tenth of the observed 100 mg — ruled out as sole cause [DERIVED]. **|g| = 1.01985 ≠ 1.000 proves instrument error is present**, since rotation cannot change a vector's magnitude — but a common-mode +1.985% scale error would inflate magnitude while preserving direction perfectly, so it does not settle offset-vs-tilt [DERIVED]. Requiring *both* Y and Z at 100% of spec with opposite signs is possible but atypical, so evidence leans real tilt [INFERRED].

**The decisive test — the gyro arbitrates.** During a pure yaw about a tilted `ĝ`, the gyro must read `ω_yaw × (0.99034, +0.09805, −0.09805)`: Y and Z at ±9.805% of X, opposite signs, fixed ratio → **10.2:1**. Gyro cross-axis is only **±2%** [SOURCED: DS Table 8], so instrument cross-talk alone can produce at most 50:1. The hypotheses are 5× apart. Untethered, spin steadily and check `ω̂ · ĝ_accel > 0.999`. [DERIVED]

**This test may already have passed.** `HARDWARE.md:270-271` records gravity `(1.02, 0.12, −0.09)` and "**X rotated 10× more than Y or Z**" — matching the 10.2:1 prediction and 5× beyond what cross-axis can produce. **Caveat: that run was USB-tethered and cable-restrained (`HARDWARE.md:252-269`), so the motion was not a clean pure yaw.** Re-run untethered. [SOURCED + INFERRED]

Secondary test: 180° flip about a vertical axis. Bias is common-mode and survives; the gravity projection reverses. `b = (a₁+a₂)/2`. Only Y and Z need it — `ĝ` is normalised, so accel scale error cancels identically. [INFERRED]

### 3. Why accel cannot correct yaw

`f_b = Rᵀ(a_n − g_n)`; at rest `f_b = −Rᵀg_n`. Decompose `R = R_z(ψ)R_y(θ)R_x(φ)`. Since `g_n ∥ ê_z`, it is an eigenvector of `R_z(ψ)` with eigenvalue 1 for all ψ:
```
f_b(ψ) = −R_x(φ)ᵀ R_y(θ)ᵀ R_z(ψ)ᵀ g_n = −R_x(φ)ᵀ R_y(θ)ᵀ g_n     ← ψ vanishes identically
```
> **`∂f_b/∂ψ ≡ 0`** — a structural rank deficiency, not a noise limit. [DERIVED]

Group-theoretically, `Stab(g_n) = {R : R g_n = g_n} ≅ SO(2)`: gravity fixes 2 attitude DOF (the direction of `g` on S²), leaving exactly 1 — rotation about gravity — unconstrained. **This is why Mahony/Madgwick buy exactly zero for yaw, and why an attitude EKF buys nothing beyond what ZUPT already gives.** [DERIVED]

Literature: Hartley et al., arXiv:1805.10410 — with IMU and contact sensors "the absolute position of the robot and **a rotation about the gravity vector (yaw) are unobservable**." Yang & Huang, arXiv:1805.05876 — aided INS with point features has "**4 unobservable directions, corresponding to the global yaw and the global position**." Yang, Geneva, Zuo & Huang, arXiv:2201.09170 — VINS with full calibration has "four unobservable directions, corresponding to the system's global yaw and translation." **Even with a camera bolted on, global yaw stays unobservable.** [SOURCED]

**Can 6 axes ever see absolute yaw?** (a) **Gyrocompassing** is real but dead here. Earth rate `360°/86164.0905 s × 3600 = 15.0411 °/h`; horizontal component 10.63 °/h at 45° latitude; 1° heading needs bias uncertainty `< 10.63 × tan(1°) = 0.186 °/h`. Measured 0.53 dps = **1908 °/h → 10,250× short**; even a perfect ZUPT residual (~0.017 dps = 61 °/h) is **330× short** [SOURCED + DERIVED]. (b) **In-motion alignment** makes yaw observable *only* when the horizontal specific force direction is independently known in the nav frame (GNSS/odometry/vision). A bare 6-axis IMU cannot supply it — the whole configuration rotates rigidly with ψ. **Sustained linear acceleration does not make yaw observable.** [DERIVED] (c) **Centripetal exploitation** gives `ω = √(a_c/r)` — a genuine independent measure of rate *magnitude*, but every term is body-frame and ψ-invariant. Sanity check (±5–10%, limited by lever-arm knowledge and ±6% accel scale), never heading. [DERIVED]

`src/main.cpp:3023-3025` already states this correctly: *"there is no compass, so this is not a bearing."* Do not soften it. [SOURCED from source]

### 4. Scale factor and cross-axis

QST DS Rev 0.9 §3.4 Table 8 (gyro): **Initial Sensitivity Tolerance ±3%**, Initial Offset Tolerance ±10 dps, **Cross-Axis ±2%**, Non-Linearity ±0.2%, TCS ±0.05 %/°C, TCO ±0.1 dps/°C, Noise Density 15 mdps/√Hz, Natural Frequency 23.5 kHz. QST's own copy calls ±3% "tight board-level gyroscope sensitivity" (p.1). [SOURCED]

**±3% = ±10.8° per 360°, and it does NOT self-cancel.** Trace it: gyro under-reads 3% (`S=0.97`) → `spin auto` runs until the integrator reads 360, so the robot physically turned `360/0.97 = 371°` → it saves 1260 ms as "one full turn," now **11° too long** → every open-loop `excited` gesture over-rotates by 11°. The cancellation would need the *task* denominated in gyro-degrees; it is not. **Scale error maps one-for-one onto physical turn error.** [DERIVED]

**Cross-axis through the projection.** `gyro_x = ω_yaw(0.990 + 0.098ε_xy − 0.098ε_xz)`; worst case `0.098 × 0.02 × 2 = 0.0039` → 0.39% → **1.4°/360°** — the same order as the projection benefit itself. Not separately worth chasing. [DERIVED]

**Calibration without a rate table — N-revolution bidirectional test.** Mark floor and chassis, spin N≈10 revolutions each way:
```
I_cw = +Sθ + bT ;  I_ccw = −Sθ + bT
S = (I_cw − I_ccw)/(2θ)     ← bias cancels EXACTLY
b = (I_cw + I_ccw)/(2T)     ← free bias estimate
```
Reading the mark to ±5° over θ = 3600° gives S to **±0.14%** — 20× better than spec, taking worst-case turn error from 10.8° to 0.5°. **This end-to-end calibration absorbs scale factor, cross-axis AND the projection/tilt term simultaneously**, because it calibrates the whole chain against physical truth in the actual motion geometry. [DERIVED]

**Tedaldi's static multi-pose method** (ICRA 2014) calibrates the accel from static intervals enforcing `|a| = 1 g`, then calibrates the gyro by requiring integration between static poses to reproduce the accel-measured gravity-direction change. Webering et al. (arXiv July 2022) refine it: **N = 12 orientations suffice** vs Tedaldi's 37–50, cutting time from ~5 min to <2 min [SOURCED]. **Critical caveat: it uses gravity-direction change as reference, so it cannot calibrate rotation about gravity — the unobservable direction. The yaw axis can only be calibrated with the board on its side.** [DERIVED]

**Standard model.** `a_meas = T_a K_a a_true + b_a`; `ω_meas = T_g K_g ω_true + b_g + G·f`. **12-parameter** per triad = 3 bias + 3 scale + 6 misalignment; **9-parameter** = 3 bias + 3 scale + 3 skew, with `T` triangular because the triad's absolute orientation is unobservable without external reference (fix the body frame to the accel x-axis and x–y plane). Adding the gyro's 3×3 g-sensitivity matrix `G` takes a 6-axis model to ~21–24 parameters. [INFERRED from standard model; terms per VectorNav Primer §3.1, which distinguishes **cross-axis sensitivity** from **misalignment**] [SOURCED]

**Full calibration is structurally impossible on this robot.** Yang et al. (arXiv:2201.09170) prove intrinsics are observable only "given fully-excited 6-axis motion" and identify degenerate profiles. **A planar differential-drive robot exciting one rotation axis is exactly degenerate.** Any real calibration must be done by hand, off-robot. [SOURCED + INFERRED]

### 5. G-sensitivity

VectorNav Primer §3.1: gyros are sensitive to linear acceleration "due to their asymmetrical design and manufacturing tolerances"; **g-sensitivity** is bias shift under *constant* acceleration, **g²-sensitivity / vibration rectification (VRE)** under *oscillatory* acceleration [SOURCED]. The distinction is the whole story: g-sensitivity is linear so symmetric vibration averages out; **g² rectifies vibration into a DC offset that does not** [DERIVED].

**QMI8658C g-Sensitivity: ±0.1 dps/g** [SOURCED: DS Table 8] — typical consumer grade (class spans ~0.005–0.1 dps/g).

**Negligible here, by two independent arguments.** (i) A 0.5 g transient gives `0.05 dps × 1.26 s = 0.063°` even if it lasted the entire turn — **160× smaller than the coast overshoot**. (ii) The ZUPT bias is estimated at rest under a full 1 g `(1.01, 0.10, −0.10)`, so the g-sensitivity contribution at that operating point is **already folded into the bias estimate**. Only `Δf` matters; during a pivot that is ~0.1 g centripetal plus pitch-back → `Δbias ≤ 0.03 dps` → **≤0.04°/turn**. [DERIVED]

**Vibration: probably fine, but the one unquantified term.** QST specifies **no g²/VRE figure at all**, substituting a mounting caveat: "do not place the QMI8658C where strong vibrations may occur or could be amplified" (§3.3 p.16) [SOURCED]. Mitigating: MEMS natural frequency **23.5 kHz**, an order of magnitude above MG90D PWM (50 Hz), gear-mesh and chassis resonances — direct resonant excitation unlikely [DERIVED]. Aggravating: **CTRL5 is never written** (`src/imu_qmi8658.cpp:16-39` writes only CTRL1/2/3/7), so gyro bandwidth is the default 258.5 Hz while the host samples at 100 Hz (`kHeadingIdleSampleMs = 10`, `src/main.cpp:354`) — **anything from 50–258 Hz aliases straight into the integration band**, independent of the sensor's own VRE [DERIVED]. Settle it empirically: park against a wall, run servos 60 s, watch heading.

### 6. Honest performance envelope

> **"Yaw (Heading) Unreferenced — 5–25 deg/h — From Allan Variance bias instability. Does not require a magnetometer."** — QST DS Rev 0.9 §3.3 Table 6, p.16 [SOURCED]

The same table gives ±5° yaw referenced to north but *"requires... magnetometer, in a homogenous Earth magnetic field"* — unavailable on this board by construction. [SOURCED]

Budget [SOURCED specs, DERIVED propagation]: ARW 15 mdps/√Hz = 0.015 °/√s = **0.9 °/√h** → 0.116° @ 1 min, 0.9° @ 1 h. Turn-on bias uncorrected: ±10 dps spec → up to 600 °/min; **measured 0.53 dps = 31.8 °/min**, 19× better than spec. Thermal TCO ±0.1 dps/°C → 10 °C self-heat = up to 1 dps = **60 °/min**. **Thermal drift and bias are the enemy, not noise**: noise adds 0.017° to a 1.26 s turn while 1 dps of thermal shift adds 1.26°.

**ZUPT analysis with the live implementation** (`src/main.cpp:5168-5189`, causal — bias from window N applied in window N+1; `kHeadingZuptPeriodMs = 2000`; gate `!g_driveRailOn && !sequencer.active() && |dps| < 8.0`). Sensor runs gODR `0011` = nominally 1000 Hz but **actually 940 Hz** with accel co-enabled (DS Table 10: "ODR (Accel + Gyro) 7520/3760/1880/**940**/470/…"), default BW 27.5% = **258.5 Hz**, sampled at 100 Hz → **aliasing**:
```
per-sample σ = 0.015 × √258.5              = 0.241 dps
ZUPT bias σ (200 samples / 2 s)            = 0.0170 dps
angle RW over 48 s = 0.241 × 0.01 × √4800  = 0.167° (1σ)
ZUPT-residual RW = √24 × (0.0170 × 2 s)    = 0.167° (1σ)
predicted total                            = 0.236° (1σ)
```
[DERIVED] Extrapolated: `√1800 × 0.034° = 1.44 °/h` + 0.9 °/h ARW → **~1.7 °/h**, legitimately better than QST's 5–25 °/h because ZUPT is a stronger constraint than any in-motion filter. Fixing the aliasing (write CTRL5, or drain the 1536-byte FIFO) removes the `√(BW·2Δt) = 2.27` factor: ARW term drops 0.167° → **0.104°**. [DERIVED]

**Assessment of ±0.05° over 48 s with 29 zero-updates** (29 ZUPTs over 48 s ≈ one per 1.66 s, consistent with the 2000 ms period plus settling): measured = 0.0625 °/min = **3.75 °/h**, versus 0.236° predicted 1σ → **0.21σ, P(|X| < 0.21σ) ≈ 17%**. Verdict: **plausible as one lucky run, not a spec.** It is not physically impossible — it sits inside QST's 5–25 °/h band and within 2× of my ZUPT-aided prediction — but a single 48 s run landing at 0.21σ happens ~1 time in 6, and less often still if ±0.05° is a peak excursion rather than an endpoint error. Re-run ≥10× and report RMS. Also confirm no integration freeze and no rate deadband; and note **`kHeadingStationaryDps = 8.0f` is a 33σ gate** (per-sample σ = 0.241 dps) that will accept a genuine 7 dps nudge as "still" — 2 dps is still 8σ. [DERIVED]

**Envelope:** single manoeuvre with prior ZUPT **0.1–1°**; stationary with continuous ZUPT **~2–8 °/h**; driving with frequent stops **~0.5–2 °/min**; driving continuously **~1–10 °/min** (TCO-dominated); **absolute heading: never**. Turn accuracy is limited by scale factor and coast, not by IMU noise or bias. [DERIVED]

## Applicability to this robot

**DO — verify the tilt (Test §2), ~10 min, log-only.** Gain: decides the *sign* of a ±3.5°/360° term that is currently live in production code. Cost: zero code change. This is a validation task, not a proposal — `projectedYawRate()` already ships at `src/main.cpp:5125-5134`.

**DO — N-revolution bidirectional scale calibration, ~15 min untethered.** Gain: ±3% → ±0.14%, i.e. 10.8° → 0.5° per 360°, and it absorbs cross-axis and projection error too. Cost: one NVS constant. Must be run unplugged — `HARDWARE.md:252-293` documents that the cable made the chassis look 4× slower (fitted `rate = 0.91×speed − 24.2 dps`, predicting 5548 ms for a full turn versus the true 1260 ms).

**DO — write CTRL5 or drain the FIFO.** Gain: ~1.6× less stationary drift (0.167° → 0.104° per 48 s) plus closure of the vibration-aliasing path. Cost: one byte. LPF has unity DC gain, so the *angle integral* is untouched; only instantaneous rate latency grows ~6–10 ms, affecting only the stop decision.

**DO — average `ĝ` over the existing ZUPT window; tighten the stationarity gate to 2 dps.** Gain: fresh `ĝ` across surface changes, free vibration detector, closes a 33σ→8σ hole. Cost: ~20 lines and one constant.

**DO NOT — build a Mahony/Madgwick/EKF attitude filter.** `∂f_b/∂ψ ≡ 0` means it recovers **exactly zero** yaw information. It would improve roll/pitch, which this robot does not use. Cost: days. Gain: nil.

**DO NOT — attempt gyrocompassing.** 10,250× short on bias, 330× short even with a perfect ZUPT. Not a tuning problem.

**DO NOT — implement a 9/12-parameter calibration.** Planar differential-drive motion is a degenerate excitation (arXiv:2201.09170); on-robot self-calibration is structurally impossible, and the off-robot rig cost is unjustified when one axis is ever used.

**DO NOT — model g-sensitivity.** ≤0.04°/turn, ~200× below the coast. Measure VRE once (wall test) and close the question.

**DO NOT — chase drift further after the above.** Coast is 7–9°, ~300× the residual drift budget. The binding constraint is control, not sensing.

## Ranked recommendations

| Technique | Expected gain | Implementation cost | Risk | Verdict |
|---|---|---|---|---|
| Verify tilt via gyro ratio (untethered spin) | Decides sign of ±3.5°/360° | ~10 min, log-only | None | **Do first** |
| N-revolution bidirectional scale calibration | 10.8° → 0.5° per 360° | ~15 min + 1 NVS constant | Low (must be unplugged) | **Do — highest value** |
| Coast-overshoot compensation (predictive stop) | 7–9° → ~2° | Moderate, control work | Low | **Do — largest single error** |
| Enable CTRL5 LPF / drain FIFO | 1.6× stationary drift; kills aliasing | 1 byte | Low | **Do** |
| Average `ĝ` over ZUPT window | Robust `ĝ` + vibration detector | ~20 lines | Low | **Do** |
| Tighten `kHeadingStationaryDps` 8.0 → 2.0 | Closes 33σ bias-poisoning hole | 1 constant | Low | **Do** |
| Re-run 48 s hold ×10, report RMS | Replaces 1-in-6 run with a real number | ~15 min | None | **Do** |
| Wall vibration test (60 s servos) | Quantifies the only unspecified term | ~10 min | None | **Do** |
| Temperature-compensated bias (regs `0x33/0x34`, 256 LSB/°C) | Up to 60 °/min on long drives | Moderate + soak test | Medium (overfitting) | Only if driving >60 s without stopping |
| Gyro FS ±2048 → ±1024 dps (`kCtrl3Value`) | Halves %FS-referred non-linearity | 1 byte | Low–medium | Marginal; recalibrate scale after |
| Mahony / Madgwick / attitude EKF | **Zero for yaw** | Days | High | **No** |
| Gyrocompassing | **Impossible (10,250× short)** | — | — | **No** |
| 9/12-parameter IMU calibration | Unobtainable on-robot | High | High | **No** |
| g-sensitivity modelling | ≤0.04°/turn | Moderate | — | **No** |
| Centripetal scale cross-check | ±5–10% sanity only | Low | Low | Diagnostic only |

## Could not verify

- **QMI8658C in-run bias stability / Allan-variance floor** is not tabulated in Rev 0.9; only the derived 5–25 °/h system figure is published.
- **g²-sensitivity / VRE is not specified at all** for this part. Largest unquantified term in §5; closable only by the wall test.
- **Gyro non-linearity units ambiguous** — "±0.2 %" with no %FS/%-of-reading qualifier. At ±2048 dps FS and 285 dps operating point the readings differ 7× (0.2% vs 1.4% of rate). Affects the FS-reduction recommendation.
- **Local `datasheets/QMI8658C_IMU.pdf` is Rev 0.6 and not machine-readable** (compressed streams; no PDF parser available). All specs here are from QST's public **Rev 0.9**. The register map cross-checks exactly against `HARDWARE.md:306-319` (WHO_AM_I `0x00`→`0x05`, CTRL1 bit6 SPI_AI, CTRL2 `aFS<6:4>/aODR<3:0>`, CTRL3 `gFS<6:4>/gODR<3:0>`, CTRL7 gEN/aEN, AX_L `0x35`, GX_L `0x3B`) so the revisions agree structurally — **but Rev 0.6's spec tables were not confirmed.** If Rev 0.6 differs on the ±100 mg or ±3% figures, §2 and §4 shift.
- **Tedaldi et al. (ICRA 2014) full text not retrieved** — host served the author homepage; extraction proxy returned HTTP 422. Method described via Webering et al. (2022). The 37–50 vs 12 orientation counts are Webering's, sourced; the algorithmic detail is [INFERRED].
- **arXiv IDs**: Hartley **1805.10410** ✓, Yang & Huang **1805.05876** ✓, Yang/Geneva/Zuo/Huang **2201.09170** ✓. Arzberger & Nüchter (28 July 2026) — **ID not retrieved**, cited by title/authors/date.
- **Search infrastructure degraded**: Google, Bing, DuckDuckGo (HTML + lite), Marginalia, MDPI (403), Semantic Scholar (429), arXiv export API (429) all blocked or rate-limited. Only arXiv's HTML search worked. **Consequence: no accessible primary source for the classical GNSS/INS in-motion alignment result** (Rhee/Abdel-Hafez/Speyer 2004; Hong et al. 2005) — §3(b) rests on my derivation plus arXiv:2201.09170.
- **Provenance of ±0.05°/48 s unknown** — endpoint vs peak, single run vs ensemble. Changes the §6 verdict materially.
- **Verified negative:** integration uses `micros()` deltas (`src/turn_measure.cpp:132-142`), so the 1000-vs-940 Hz ODR discrepancy introduces **no** scale error. Do not "fix" it by assuming `dt` from the ODR.

## Adjacent, not pursued

- FIFO-based sampling (1536 bytes, DS §8) — better than CTRL5 for both aliasing and shared-I²C load, but needs its own design pass.
- QMI8658C AttitudeEngine / XKF3 on-chip fusion — wants a magnetometer for its headline specs; would displace a well-understood integrator.
- Wheel-odometry / gyro cross-fusion to bound scale error in real time — no encoders on this chassis, and the drive demonstrably slips.
- Non-holonomic constraints (NHC) and ZARU — bound bias, never heading; marginal over the existing ZUPT.
- Heuristic Drift Elimination (Borenstein & Ojeda) — exploits corridor structure; meaningless for a desk robot.
- Allan-variance characterisation of this specific unit (2–4 h static log) — would replace every "typical" spec with measured values.
- Accelerometer 6-position tumble — unnecessary for yaw, since `ĝ` is normalised and accel scale cancels.
- Gyro self-test (DS §11.2) as a periodic health check — unrelated to accuracy, cheap insurance.

## References

### Load-bearing

1. **QST Corporation, *QMI8658C Datasheet Rev 0.9*, 10 Jan 2022**, 68 pp. §3.3 Table 6 p.16 (yaw unreferenced **5–25 °/h**; yaw-to-north ±5° requires magnetometer; vibration mounting caveat); §3.4 Table 7 p.17 (accel: **offset ±100 mg**, **sensitivity ±6%**, cross-axis ±1%, TCO ±1 mg/°C, TCS ±0.04 %/°C, non-linearity ±0.75%); §3.4 Table 8 p.18 (gyro: **sensitivity ±3%**, offset ±10 dps, **cross-axis ±2%**, **g-sensitivity ±0.1 dps/g**, TCO ±0.1 dps/°C, TCS ±0.05 %/°C, non-linearity ±0.2%, **noise density 15 mdps/√Hz**, natural frequency 23.5 kHz); Table 10 p.19 (ODR accel+gyro 7520/3760/1880/**940**/470 Hz; accel noise 200 µg/√Hz; default BW 27.5% of ODR = **258.5 Hz**); p.1 ("tight board-level gyroscope sensitivity of ±3%").
2. **VectorNav, *Inertial Navigation Primer* §3.1 "IMU Specifications."** Definitions of noise density, ARW (`X °/s/√Hz ≡ X °/√s`), in-run bias stability, turn-on bias repeatability, scale factor error and non-linearity, **cross-axis sensitivity vs misalignment**, **g-sensitivity**, **g²-sensitivity / vibration rectification**.
3. **Hartley, Ghaffari Jadidi, Grizzle, Eustice (2018).** *Contact-Aided Invariant EKF for Legged Robot State Estimation.* **arXiv:1805.10410**.
4. **Yang & Huang (2018).** *Observability Analysis of Aided INS with Heterogeneous Features of Points, Lines and Planes.* **arXiv:1805.05876**.
5. **Yang, Geneva, Zuo, Huang (2022).** *Online Self-Calibration for VINS: Models, Analysis and Degeneracy.* **arXiv:2201.09170** v3.
6. **Webering, Kleinjohann, Stanislawski, Blume (2022).** *Improved Calibration Procedure for Wireless IMUs without Precision Equipment.* arXiv, 11 July 2022. **N = 12 orientations sufficient** vs Tedaldi's 37–50.
7. **Arzberger & Nüchter (2026).** *Motion-Acceleration Calibration and Compensation in IMUs without External Equipment.* arXiv, 28 July 2026.
8. **Robot firmware (read-only).** `src/turn_measure.cpp:49-60` (`yawRateFromVector`), `:122-143` (trapezoidal integration on `micros()`), `:311-313` (`reachedTarget`); `src/main.cpp:5125-5134` (`projectedYawRate`, live), `:5106-5115` (single-sample `ĝ`), `:5168-5189` (causal ZUPT), `:354-359` (100 Hz idle, 2000 ms period, 8 dps gate), `:3023-3025` ("not a bearing"); `src/imu_qmi8658.cpp:16-39` (**CTRL5 never written**; accel ±2 g @ `0100`, gyro ±2048 dps @ `0011`); `HARDWARE.md:252-293` (cable trap; 1260 ms/360° untethered; `rate = 0.91×speed − 24.2 dps`), `:270-271` ("**X rotated 10× more than Y or Z**"), `:295-319` (6-axis, no magnetometer; verified register map).

### Background

9. **Woodman (2007).** *An Introduction to Inertial Navigation.* Univ. Cambridge **UCAM-CL-TR-696**. Retrieved but not machine-readable; not quoted.
10. **Tedaldi, Pretto, Menegatti (2014).** *A Robust and Easy to Implement Method for IMU Calibration without External Equipments.* IEEE ICRA 2014, pp. 3042–3049. Full text not retrieved; described via ref. 6.
11. **IEEE Std 952-1997 (R2008).** Origin of the Allan-variance terminology (ARW, bias instability, rate random walk) used by every IMU datasheet including QST's. Paywalled; not consulted directly.
12. **Earth rotation rate** 15.0411 °/h = `360°/86164.0905 s × 3600`, from the sidereal day. [DERIVED]
