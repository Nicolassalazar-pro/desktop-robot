# Research: Attitude Estimation Filters

## Executive summary (max 10 bullets, each tagged)

- **[DERIVED] The single largest correctable error on this robot is not drift — it is a static 8° axis misalignment.** Resting gravity (1.01, 0.10, −0.10) g normalises to û = (0.9903, 0.0981, −0.0981); the angle between body-X and true vertical is **acos(0.9903) ≈ 7.97°**. Integrating raw gyro-X therefore under-reads a pure-vertical rotation by a factor of 0.9903 → **−0.966%, i.e. 3.48° short on a 360° turn**. That is ~2,600× the measured stationary drift.
- **[SOURCED] With no magnetometer, gravity is the only absolute reference, and gravity is a single vector — it constrains 2 of 3 DOF. Rotation about gravity is exactly the unobservable direction.** The AHRS reference implementation states it flatly: tilt from accel gives "only the roll, θx, and pitch, θy … leaving the yaw angle, θz, equal to zero" (ahrs.readthedocs.io/filters/complementary.html); Mahony's doc: "Two degrees of freedom … are resolved using the acceleration readings (tilt) and the final degree of freedom is resolved using the magnetometer (heading)."
- **[DERIVED] Worse: Mahony/Madgwick cannot even estimate the *yaw-axis gyro bias*.** Their correction term is ω_mes = Σ kᵢ vᵢ × v̂ᵢ, a cross product of two near-parallel vectors along ĝ, which is **structurally orthogonal to ĝ**. The k_I bias integrator can therefore never accumulate a component along the yaw axis. For a yaw-only application on a 6-axis part these filters buy **exactly zero** on the quantity of interest.
- **[DERIVED] During a 285 °/s turn the accelerometer is actively lying about which way is down.** Centripetal acceleration ω²r at 4.974 rad/s with the IMU 30 mm off the pivot is 0.742 m/s² = **0.076 g → a 4.3° false tilt**, plus a comparable tangential term during the ~200 ms ramp. The pseudo-tilt is **larger than the real weight-transfer tilt it would be trying to track**. Any accel-fusing filter degrades the answer precisely during the burst.
- **[DERIVED] Verdict on Q4: a full attitude filter is not justified; a 5-flop gravity projection is, and it is worth ~70× more.** Replace `componentFor(axis, …)` with ω·û. Gain 3.48°/360°, cost 3 multiplies + 2 adds, risk near zero. The three gyro axes are **already in RAM and discarded** (`src/main.cpp:5100-5105`), so there is no extra I²C traffic.
- **[DERIVED] Q5: dynamic tilt tracking is real but small — ~0.1–0.3° on a 360° turn.** Loss is (1−cos φ): φ=2° → 0.22°/360°, φ=5° → 1.37°/360°, scaled by the fraction of the burst spent tilted (~30–40%). It must be recovered by **gyro dead-reckoning of ĝ**, not accel fusion — over 1.26 s the gyro propagates ĝ with only ~0.0013° of error.
- **[SOURCED/DERIVED] Q3: Euler angles are not merely inelegant here, they are actively broken.** Standard ZYX pitch from accel is atan2(−a_x, √(a_y²+a_z²)) = atan2(−1.01, 0.1414) = **−82.0°, i.e. 8° from the ±90° gimbal-lock singularity**. Yaw extraction carries a 1/cos θ = **7.19× noise/bias amplification**; an unsubtracted 0.5 °/s bias on gyro-Y would inject ≈3.2° of error into a 1.26 s turn. Use a gravity unit vector (3 floats) or a quaternion — never Euler.
- **[DERIVED] Q6: integration order is irrelevant at 1 kHz.** First-order quaternion update with renormalisation has per-step angle error θ³/12, i.e. relative error θ²/12 = **2.28 ppm at 0.3°/step → 0.0008° over a full 360° turn**. Un-normalised, the quaternion norm grows 0.43% per turn — fixed by one rsqrt. Coning is bounded at **<0.01°**. RK4 and closed-form exponentials are wasted cycles.
- **[DERIVED] The real update-rate risk is the documented 20 Hz LCD-mode collapse, not the filter.** At Δt = 50 ms trapezoidal error through the rate ramp is order **~1–2°** (vs 0.0005° at 1 kHz) and sample count falls to ~26 against `Integrator::kMinSamples = 24`. `HARDWARE.md:183-191` already records 990 samples free-running vs 20 from the gesture menu.
- **[INFERRED] The one error nobody can filter away is gyro scale factor.** It passes 1:1 into `correctTurn()` (`ideal = testedMs·360/|measured|` ⇒ robot physically turns 360/k), so ±1–3% MEMS sensitivity tolerance = **3.6–10.8° per 360°**. Only an external angle reference fixes it. This, not filtering, is where the remaining accuracy lives.

---

## 1. Filter landscape and comparison

Compute cost is quoted as flops/sample and as % of one 240 MHz ESP32-S3 core at 1 kHz, assuming ~1 single-precision flop/cycle on the hardware FPU. **[DERIVED]**

| Filter | Estimates | Needs | Cost/sample | % core @1 kHz | Tuning burden | Known failure modes |
|---|---|---|---|---|---|---|
| **Single-axis integration** (current) | 1 angle | 1 gyro axis, bias | ~5 flops | <0.01% | 1 knob (ZUPT threshold) | Axis misalignment; scale factor; unbounded drift without ZUPT |
| **Gravity-projected integration** (proposed) | 1 angle | 3 gyro axes, ĝ at rest | ~10 flops | <0.01% | Same 1 knob | Tilt *during* motion untracked; scale factor |
| **ĝ-propagating integrator** (proposed) | 1 angle + ĝ in body frame | 3 gyro, accel at rest only | ~30 flops | 0.013% | Same 1 knob | Long bursts (>10 s) would let ĝ drift; not a concern at 1.3 s |
| **Complementary (linear/Euler)** | roll, pitch (+yaw only w/ mag) | 3 gyro, 3 accel | ~40 flops | 0.017% | 1 gain α | Euler singularity; **no yaw correction** |
| **Mahony (explicit complementary, SO(3))** | quaternion + gyro bias ⊥ ĝ | 3 gyro, 3 accel | ~80–100 flops | 0.04% | 2 gains (k_P, k_I) | **Yaw & yaw-bias unobservable**; accel corruption under acceleration |
| **Madgwick** | quaternion (+bias in MARG form) | 3 gyro, 3 accel | ~110–150 flops | 0.06% | 1 gain β | Same as Mahony; β trades convergence vs noise |
| **Quaternion EKF (4-state, naive)** | quaternion | 3 gyro, 3 accel | ~1–2 k flops | ~1% | Q, R matrices | **Singular covariance** — the unit-norm constraint removes a DOF, so P is rank-deficient. This is the classic reason MEKF exists |
| **ESKF / MEKF (6–7 state)** | ref quaternion × 3-param error + 3 bias | 3 gyro, 3 accel | ~1.5–3 k flops | ~1.5% | Q, R, P₀ | Yaw still unobservable; bias states along ĝ go unobservable and their covariance grows without bound unless constrained |
| **UKF / USQUE** | same as MEKF | same | ~10–20 k flops | ~10% | Q, R, P₀, α/β/κ | Same observability limits; Cholesky can lose positive-definiteness in float32 |

### Which are equivalent in practice **[SOURCED + INFERRED]**

Four equivalences matter, and they collapse most of the landscape:

1. **ESKF ≡ MEKF.** These are two names for one algorithm — carry a unit quaternion as the reference and a 3-parameter *error* state, correcting multiplicatively. "Multiplicative EKF" is the aerospace name (Markley, Crassidis); "error-state KF" is the robotics/vision name (Solà, *Quaternion kinematics for the error-state Kalman filter*, arXiv:1711.02508). There is no algorithmic difference to choose between.
2. **Mahony ≡ steady-state ESKF.** With stationary noise statistics a Kalman filter converges to a constant gain. Mahony's explicit complementary filter is exactly that constant-gain solution expressed on SO(3): `q̇̂ = ½ q̂ p(Ω_y − b̂ + k_P ω_mes)`, `ḃ̂ = −k_I ω_mes` (ahrs.readthedocs.io/filters/mahony.html). k_P is the steady-state Kalman gain, k_I is the bias-estimator gain.
3. **Madgwick ≈ Mahony.** Madgwick's own derivation ends in a complementary filter, `q_t = γ_t q_∇,t + (1−γ_t) q_ω,t`, which he then reduces to `q_t = q_{t−1} + (q̇_ω,t − β ∇f/‖∇f‖)Δt` (ahrs.readthedocs.io/filters/madgwick.html, after Madgwick 2010). Mahony's correction is a cross product; Madgwick's is a normalised gradient. To first order in the tilt error these are the same vector; the practical difference is that Madgwick's step is *fixed magnitude* (β) and Mahony's is *proportional to the error*.
4. **UKF ≈ MEKF for small errors.** The unscented transform only pays for itself when the attitude error is large enough that the Jacobian linearisation breaks (tens of degrees). This robot starts every burst from a known, freshly-anchored attitude, so errors are sub-degree throughout. The UKF's 5–13× cost buys nothing.

**Net:** for this application the entire right-hand side of the table degenerates to the same estimator, and that estimator does not observe yaw. See §2.

---

## 2. What filters buy WITHOUT a magnetometer (verdict)

### The verdict, plainly

**Running Madgwick or Mahony on this 6-axis part for a yaw-only application is pointless — and during the turn it is worse than pointless, it is harmful.** Do not do it.

### Why — three independent arguments

**(a) Yaw is structurally unobservable from accelerometer data. [SOURCED]**

The accelerometer's measurement model is h(q) = R(q)ᵀ ĝ — one vector. A single vector observation fixes 2 of the 3 attitude DOF; the null direction of the observability matrix lies exactly along ĝ. Every reference implementation says so explicitly:

- Complementary filter: tilt angles are computed as θ_x = atan2(a_y, a_z), θ_y = atan2(−a_x, √(a_y²+a_z²)), and then — "Only the roll, θx, and pitch, θy, angles are computed, **leaving the yaw angle, θz, equal to zero**." (ahrs.readthedocs.io/en/latest/filters/complementary.html)
- Mahony: "Two degrees of freedom in the rotation matrix are resolved using the acceleration readings (*tilt*) and **the final degree of freedom is resolved using the magnetometer** (*heading*)." (ahrs.readthedocs.io/en/latest/filters/mahony.html)
- Madgwick: "they don't hold for longer periods of time, **especially estimating the heading orientation of the system**, as the gyroscope measurements, prone to drift, are instantaneous and local, while **the accelerometer computes the roll and pitch orientations only**. Therefore, it is always very convenient to add a reference that provides constant information about the heading angle (a.k.a. yaw)." (ahrs.readthedocs.io/en/latest/filters/madgwick.html, after Madgwick 2010)

Consequence: the yaw output of a 6-axis Madgwick/Mahony/EKF is **byte-for-byte gyro integration**, dressed in a quaternion. It cannot be otherwise.

**(b) They cannot even estimate the yaw-axis gyro bias. [DERIVED]**

This is the sharper point and it is usually missed. Mahony's bias estimator is driven by

```
ω_mes = Σ kᵢ (vᵢ × v̂ᵢ)          ḃ̂ = −k_I ω_mes
```

With only an accelerometer, v₁ = measured gravity direction and v̂₁ = predicted gravity direction — two nearly-parallel unit vectors both lying along ĝ. Their cross product is **identically perpendicular to ĝ**. Therefore ω_mes has *no component along the yaw axis*, ever, and `ḃ̂ · ĝ = 0` for all time. The filter's integral term can learn roll and pitch gyro bias and is structurally incapable of learning yaw gyro bias.

Your existing ZUPT (`src/main.cpp:5139-5160`) *does* learn yaw bias, directly, by averaging the gyro at rest. **The ZUPT is strictly more capable than the filter on the axis you care about.** Measured evidence: heading holds ±0.05° over 48 s ⇒ residual yaw bias ≤ 0.05/48 = **0.00104 °/s**, contributing **0.0013°** over a 1.26 s turn. No filter can improve on that.

**(c) During the burst the accelerometer is worse than useless. [DERIVED]**

Filters gate accel corrections when ‖a‖ deviates from 1 g — but the disturbance here is *lateral*, so the norm barely changes while the *direction* swings hard.

At ω = 285 °/s = 4.9742 rad/s, centripetal acceleration at radius r is ω²r:

| IMU offset from pivot | a_centripetal | False tilt = atan(a_c/g) |
|---|---|---|
| 10 mm | 0.0252 g | 1.45° |
| 30 mm | **0.0757 g** | **4.33°** |
| 50 mm | 0.126 g | 7.24° |

Plus tangential α·r: ramping 0→285 °/s in ~200 ms gives α = 24.87 rad/s², so at r = 30 mm that is another 0.076 g. Total in-plane disturbance ≈ 0.1 g ≈ 6° of apparent tilt — **larger than the 2–4° of real weight-transfer tilt the filter would be trying to observe.** Signal-to-disturbance is below 1. The correct gain during the burst is *zero*, which means the filter reduces to open-loop gyro propagation anyway.

### What a filter *does* legitimately buy

One thing only: **it tracks where gravity is in the body frame while the body moves**, which lets you project the gyro onto the true vertical. That is worth ~0.1–0.3° here (§5). And you can get the same benefit *without* any accel fusion, by propagating ĝ with the gyro and re-anchoring it at rest — which is what your ZUPT already provides a hook for.

---

## 3. Attitude representation

### Euler angles — actively dangerous on THIS robot **[DERIVED]**

The board is upright and gravity lies on body-X. Applying the standard ZYX tilt extraction to the measured resting vector (1.01, 0.10, −0.10):

- pitch θ = atan2(−a_x, √(a_y² + a_z²)) = atan2(−1.01, 0.1414) = **−82.0°**
- roll φ = atan2(a_y, a_z) = atan2(0.10, −0.10) = **135°**

So the robot's *nominal resting pose* sits **8° from the ZYX gimbal-lock singularity at θ = ±90°**. This is the textbook failure — "If the aircraft pitches up 90 degrees, the aircraft and platform's yaw axis gimbal becomes parallel to the roll axis gimbal, and changes about yaw can no longer be compensated for" (en.wikipedia.org/wiki/Gimbal_lock) — except here it is the *default* attitude, not an edge case.

Two concrete consequences:

1. **Roll is noise-amplified.** φ = atan2(a_y, a_z) is computed from two components totalling only 0.14 g. At ±0.005 g of accel noise, roll jitters by **±2°**.
2. **Yaw extraction is amplified 7.19×.** ψ̇ = (q sin φ + r cos φ)/cos θ, and 1/cos(82°) = 7.19. With gyro-Y bias of 0.5 °/s left unsubtracted (which it currently is — see below), the injected yaw-rate error is 0.5 × sin(135°) × 7.19 = 2.54 °/s ⇒ **3.2° of error over a 1.26 s turn**. That single mistake would be worse than every other error on this robot combined.

Wikipedia's own prescription is the right one: "Modern practice is to avoid the use of gimbals entirely… mounting the inertial sensors directly to the body of the vehicle (this is called a strapdown system) and **integrating sensed rotation and acceleration digitally using quaternion methods**."

### DCM (3×3 rotation matrix) **[SOURCED]**

No singularities; the inverse rotation is just the transpose because R ∈ SO(3) (ahrs.readthedocs.io/filters/aqua.html). But it carries 9 numbers under 6 orthonormality constraints, so numerical drift shows up as loss of orthogonality, and re-orthonormalisation (Gram–Schmidt or a Björck iteration) is materially more expensive than a quaternion renormalise. Rotating a vector costs 9 mul + 6 add vs the quaternion's ~15–30. Fine, but strictly dominated by the quaternion here.

### Quaternion **[SOURCED]**

4 numbers, 1 constraint (‖q‖ = 1), no singularities, composition is a 16-mul Hamilton product, and renormalisation is one rsqrt + 4 multiplies. Numerical drift is a **pure norm error, not an orientation error** — the direction in ℍ⁴ stays correct, only the length wanders, so normalising recovers the exact attitude. This is the key structural advantage over DCM: the error mode is benign and cheaply nulled.

Quantified for this robot **[DERIVED]**: un-normalised first-order integration grows the norm by √(1 + θ²/4) ≈ 1 + θ²/8 per step. At θ = 0.3°/step (300 °/s, 1 kHz) that is 1 + 3.43e-6, so over a 1260-step full turn the norm grows to exp(1260 × 3.43e-6) = **1.00433, i.e. +0.43%**. Normalising even once every 64 steps keeps it below 0.03%. In float32 (24-bit mantissa, ε ≈ 6e-8) round-off contributes ~√1260 × 6e-8 ≈ 2e-6 — three orders below the truncation term and utterly irrelevant.

### Recommendation for this application **[INFERRED]**

**Neither quaternion nor DCM — carry a 3-vector.**

The robot needs one scalar (how far it turned about vertical). The minimal sufficient state is the **body-frame gravity unit vector ĝ_b** plus a scalar heading accumulator:

```
ĝ_b  ←  ĝ_b − (ω × ĝ_b)·Δt        // 6 mul + 3 add + 3 mul + 3 add
ĝ_b  ←  ĝ_b / ‖ĝ_b‖               // 1 rsqrt + 3 mul (every N steps)
ψ    +=  (ω · ĝ_b)·Δt              // 3 mul + 2 add
```

~30 flops/sample = **0.013% of one core at 1 kHz**. It is singularity-free (it is a point on S², not a chart), it has one benign constraint (unit norm), and it discards the one DOF you provably cannot observe (rotation about gravity) instead of carrying it as dead weight. A quaternion is a perfectly fine alternative and costs ~40 flops; use it if you ever want roll/pitch for the eye animation. Euler angles: **never**, for the reasons above.

**Rigour note on `∫(ω·ĝ_b)dt` [DERIVED].** In ZYX Euler terms, ω·ẑ_world = ψ̇ − φ̇ sin θ. So the gravity-projected integral equals the true heading change minus ∫φ̇ sin θ dt. If tilt θ is roughly constant over the burst, that term is sin θ · Δφ, and Δφ = 0 for any roll that returns to its start value — so it **cancels exactly** for the transient weight-transfer case. The residual is second order in (tilt variation × roll variation), bounded by ~0.0076 rad ≈ 0.44° in an absolute worst case with no cancellation, and realistically <0.1°.

---

## 4. Is a filter justified for this robot? (verdict with reasoning)

### The full error budget for one 360° turn (1260 ms @ ~285 °/s) **[DERIVED]**

| # | Error source | Magnitude on 360° | Fixed by | Cost of fix |
|---|---|---|---|---|
| 1 | **Gyro scale-factor error (±1–3%)** | **3.6–10.8°** | External angle reference | Procedure only |
| 2 | **Static axis misalignment (7.97°)** | **3.48°** | Gravity projection ω·û | **5 flops** |
| 3 | Dynamic tilt during turn (2–4° transient, ~35% of burst) | 0.1–0.3° | Gyro-propagated ĝ | ~25 flops |
| 4 | Cross-axis gyro bias on Y/Z (un-ZUPTed, 0.5 °/s) | 0.12° | ZUPT all 3 axes | ~0 (already computed) |
| 5 | Gyro quantisation, worst-case DC (q/2 = 0.031 °/s) | 0.039° | — | — |
| 6 | Coning at 1 kHz, first-order algorithm | <0.01° | — | — |
| 7 | Residual yaw bias after ZUPT (≤0.00104 °/s) | 0.0013° | Already solved | — |
| 8 | Quaternion truncation, first-order @1 kHz | 0.0008° | — | — |
| 9 | Quantisation random walk (σ_q/√12 · √(ΔtT)) | 0.0006° | — | — |

Derivations for the small terms: quantisation step is q = 2048/32768 = **0.0625 °/s per LSB** (`src/imu_qmi8658.h:34`), so σ = q/√12 = 0.01804 °/s and the integrated random walk is σ√(Δt·T) = 0.01804 × √0.00126 = 0.00064°. Residual bias comes straight from the measured ±0.05° over 48 s.

### The case FOR a full attitude filter

- It is the "correct" thing to do in the textbook sense, and compute is genuinely free — even a 6-state ESKF is ~1.5% of one core.
- It would track tilt during motion, recovering rows 3.
- It gives you roll/pitch for free, which the EyeEngine could use for expressive behaviour.
- It handles the case where the robot is picked up, tipped, or driven onto an incline — situations where the fixed-axis assumption silently degrades with no error flag.
- It is a single well-tested library rather than bespoke code.

### The case AGAINST

- **It does not touch rows 1 or 2, which are 95–99% of the error.** Row 1 no filter can fix. Row 2 is fixed by 5 flops of arithmetic that is not a filter.
- **It only addresses row 3, worth 0.1–0.3°** — and only if it can see the tilt, which §2(c) shows it cannot during the burst (4.3° of centripetal pseudo-tilt vs 2–4° of real tilt).
- It introduces **new** failure modes with no compensating benefit: gain tuning (β or k_P/k_I), accel-rejection gating thresholds, initialisation transients, and the Euler/quaternion conversion trap of §3.
- It replaces code whose failure modes are enumerated and host-unit-tested (`src/turn_measure.{h,cpp}` — the header explicitly isolates "axis choice, bias handling, integration, saturation, the correction arithmetic" for host testing, `src/turn_measure.h:15-18`) with a library whose behaviour on an 8°-tilted, 285 °/s, magnetometer-free platform is untested.
- Every degree of freedom it estimates beyond the one you need is a degree of freedom that can go wrong.

### Verdict

**No full attitude filter. Yes to a gravity projection and a gyro-propagated gravity vector — neither of which is a filter.**

The existing bias-corrected single-axis integration is *already near-optimal on drift* — the measured 0.0013° per turn from residual bias is smaller than the gyro's own quantisation floor. There is nothing left to win there, and every filter in §1 targets exactly that (already-solved) problem while ignoring the two that dominate.

Precisely what to do, in order:

1. **Replace `componentFor(axis, gx, gy, gz)` with `ω · û`**, where û is the normalised resting accelerometer vector. Recovers **3.48° per 360°**. The three gyro components are already read and discarded at `src/main.cpp:5100-5105`, so this adds zero I²C traffic and 5 flops. *This is the single highest-value change in the entire investigation.*
2. **ZUPT all three gyro axes, not just the yaw axis.** `src/main.cpp:5156` currently writes only `g_turnBias[yawAxis]`. Once you take a 3-axis dot product, the Y and Z biases leak in weighted by 0.098 each: with 0.5 °/s on each, that is 0.098 °/s ⇒ **0.124° per turn**. The calibration path at `src/main.cpp:4578` already computes `BiasEstimator bias[3]` and throws two of them away — the data is free.
3. **Propagate ĝ with the gyro during the burst** (~25 more flops). Recovers rows 3, worth 0.1–0.3°. Do this second; it is a genuine but small win.
4. **Then go measure scale factor against an external reference** — that is where the remaining 3.6–10.8° lives, and it is now the dominant term by a wide margin.

**If someone insists on a filter anyway**, the least-bad choice is a **Mahony (explicit complementary) filter with k_P gated to zero whenever the drive rail is on** — i.e. accel correction only at rest. But notice that this is *definitionally identical* to recommendation 3 plus your existing ZUPT, with extra tuning knobs bolted on. Prefer the explicit version: it is smaller, testable on the host, and has no hidden gains.

---

## 5. Tilt tracking during motion

### The geometry **[DERIVED]**

If the robot tilts by angle φ between the integrated axis and true vertical while spinning at Ω about the world vertical, the body-frame rate about the integrated axis is Ω·cos φ. The under-read is (1 − cos φ):

| Tilt φ held for whole turn | Fractional loss | Error on 360° | Error on 90° |
|---|---|---|---|
| 1° | 1.52e-4 | 0.055° | 0.014° |
| 2° | 6.09e-4 | 0.219° | 0.055° |
| 3° | 1.37e-3 | 0.494° | 0.123° |
| 5° | 3.81e-3 | 1.370° | 0.342° |
| 10° | 1.52e-2 | 5.469° | 1.367° |

For a *transient* tilt — which is what weight transfer produces — multiply by the fraction of the burst spent tilted. A desk robot's realistic profile is a 2–4° peak during the ~200 ms accel and ~200 ms decel phases of a 1260 ms turn, i.e. **f ≈ 0.3–0.4**:

- φ_peak = 2°, f = 0.35 → **~0.08° per 360°**
- φ_peak = 4°, f = 0.35 → **~0.31° per 360°**

**So the honest number is 0.1–0.3° recovered on a full turn, and 0.02–0.08° on a 90° turn.** Compare that to the 3.48° from the *static* misalignment. **Getting the axis right at rest is worth 10–35× more than tracking how it moves.**

### Which filters track it, and whether they can

| Approach | Tracks tilt during burst? | Actually works here? |
|---|---|---|
| Current single-axis | No | — |
| Fixed gravity projection | No | Captures the static 8°, which is the big win |
| **Gyro-propagated ĝ** | **Yes, by dead reckoning** | **Yes — this is the right answer** |
| Mahony / Madgwick / ESKF / UKF with accel correction on | Nominally yes | **No — accel is corrupted by 0.076 g of centripetal + tangential acceleration, injecting 4.3° of false tilt** |
| Mahony / ESKF with accel gated off during motion | Yes, by dead reckoning | Yes — but this is mathematically identical to gyro-propagated ĝ, with more code |

### Why dead reckoning wins over the full 1.3 s **[DERIVED]**

The reason a filter normally needs accel corrections is that gyro integration drifts. Over a 1.3 s burst it does not. With residual bias ≤0.00104 °/s (from the measured ±0.05°/48 s), the accumulated error in the *direction of ĝ* over 1.26 s is:

```
0.00104 °/s × 1.26 s = 0.0013°
```

An error of 0.0013° in ĝ changes the projection by 1 − cos(0.0013°) ≈ 2.6e-10 — **twelve orders of magnitude below the effect being measured**. Gyro propagation of the gravity vector over this burst duration is, for all practical purposes, exact. Accel correction contributes nothing except contamination.

This is the crisp architectural statement: **the correct algorithm is a filter whose measurement gain is 1 at rest and 0 during motion** — which is not Mahony, Madgwick, EKF, ESKF, MEKF or UKF. It is a ZUPT/ZARU-anchored attitude propagator, and you already have the ZUPT.

**Caveat [INFERRED]:** this reasoning depends on the burst being short. If the robot ever needed to hold heading through a continuous 30–60 s manoeuvre, ĝ drift would reach 0.03–0.06° and the sums would begin to change — but the stated motion profile (0.3–1.3 s bursts, still in between) is three orders of magnitude away from that regime.

---

## 6. Embedded implementation notes

### Fixed vs floating point **[DERIVED]**

Use **float32 throughout the vector/quaternion math**. The ESP32-S3 has a single-precision hardware FPU, so float ops are ~1 cycle; there is no reason to consider fixed point, and fixed point would introduce scaling bugs in exchange for nothing.

**Do not silently promote to `double`.** The S3's FPU is single-precision only, so `double` is software-emulated at roughly 50–200 cycles per operation. The current code uses `double` for the accumulators (`Integrator::degrees_` at `src/turn_measure.h:111`, `BiasEstimator::sum_` at line 67). At 1 kHz × ~6 double ops that is ~0.4% of one core — affordable, and a *good* choice for the scalar accumulator, since it removes any question about summing 1260 increments. Keep it there. But if you add ĝ propagation or a quaternion, keep **that** math in float32 and let only the final heading accumulator be double.

Float32 sanity check for the accumulator: 1260 increments of ~0.285° summing to 360°, with ε = 6e-8, gives accumulated round-off ≈ √1260 × 6e-8 × 360 = **7.7e-4°**. Even pure float32 would be fine; double just makes it a non-question.

### Update rate requirements **[DERIVED]**

- **Signal bandwidth:** the rate profile ramps over ~100–250 ms, so its content is ~5–10 Hz. Nyquist would be satisfied by 50 Hz.
- **Trapezoidal truncation** is what actually sets the rate. Per-step error is −(Δt³/12)·ω″. Taking ω″ ≈ 285/(0.1)² ≈ 28,500 °/s³ through the ramp:

| Sample rate | Δt | Per-step error | Total through both ramps |
|---|---|---|---|
| 1000 Hz | 1 ms | 2.4e-6 ° | **~0.0005°** |
| 100 Hz | 10 ms | 2.4e-3 ° | **~0.05°** |
| 20 Hz | 50 ms | 0.30 ° | **~1–2°** (sign partially cancels) |

**100 Hz is already adequate.** 1 kHz is free and gives 100× margin.

- **The real risk is the documented rate collapse, not the choice of rate.** `HARDWARE.md:183-191` records that `loop()` returns early with `delay(50)` whenever `g_lcdMode != LCD_MODE_FACE`, capping the whole loop at 20 Hz, and measured **990 gyro samples per probe free-running vs 20 from the gesture menu** — with on-device calibration failing "too few samples" while byte-identical code passed over serial. At 20 Hz a 1.3 s turn yields ~26 samples against `Integrator::kMinSamples = 24` (`src/turn_measure.h:101`) — passing the guard while carrying ~1–2° of truncation error. **This is a larger and more likely error than anything in the filter literature.** `Integrator::worstGapUs()` (`src/turn_measure.cpp:139-141`) already instruments it; consider making a large worst-gap fail the measurement rather than just reporting it.

### Integration schemes and truncation error **[SOURCED + DERIVED]**

The exact update, assuming ω constant over the step, has a closed form (ahrs.readthedocs.io/filters/angular.html, after Spence & Markley, *Attitude Propagation*, in Wertz ed., 1978):

```
q_{t+1} = [ cos(‖ω‖Δt/2)·I₄ + (1/‖ω‖)·sin(‖ω‖Δt/2)·Ω(ω) ] q_t
```

The cheap first-order alternative is q ← q + ½·Ω(ω)·q·Δt followed by renormalisation.

**Truncation error of first-order + renormalise [DERIVED].** The normalised first-order quaternion (1, θ/2·n̂)/√(1+θ²/4) represents a rotation of θ′ = 2·atan(θ/2). The exact rotation is θ. So:

```
error per step = θ − 2·atan(θ/2) = θ³/12 + O(θ⁵)
relative (scale) error = θ²/12
```

At **300 °/s and 1 kHz**, θ = 0.3° = 5.236e-3 rad:

- per-step error = θ³/12 = **1.196e-8 rad = 6.85e-7 °**
- relative error = θ²/12 = **2.28 ppm**
- accumulated over a full 360° turn (1260 steps): **1.51e-5 rad = 0.00086°**

**That is the answer to "does integration order matter at 1 kHz for rates up to ~300 °/s": no. First-order costs you 0.0009° per full turn** — 4,000× smaller than the misalignment error and 45× smaller than the gyro's own DC quantisation floor. RK4 and the closed-form exponential reduce a term that is already invisible.

For contrast, at **100 Hz** the same rate gives θ = 3°, relative error θ²/12 = 228 ppm ⇒ **0.082° per 360°**. Still acceptable, but 100× worse — worth knowing if the loop ever degrades.

**Coning [DERIVED].** Coning error only exists when the rotation axis moves within the interval; a pure single-axis turn produces none. Bounding the first-order algorithm's residual at (1/12)|Δθ_{k−1} × Δθ_k| with a 5.236e-3 rad/step yaw increment and a generous 30 °/s tilt rate (5.236e-4 rad/step) gives 2.29e-7 rad per sample pair; over 630 pairs that is 1.44e-4 rad = **0.008°**. Negligible, and this is a loose upper bound since oscillatory tilt partially cancels.

### Numerical conditioning **[DERIVED]**

- **Renormalise the quaternion or ĝ.** Un-normalised, the norm grows +0.43% per full turn at 1 kHz (§3). One rsqrt every 32–64 steps is ample; every step costs nothing anyway.
- **Never extract Euler angles on this platform.** The resting pose is 8° from the ZYX pitch singularity, with a 7.19× amplification factor on yaw (§3). If a display needs a heading number, integrate the scalar directly — do not round-trip through Euler.
- **If you ever do implement an ESKF**, use Joseph form for the covariance update and keep P symmetric (P ← ½(P + Pᵀ)) — in float32, the standard (I − KH)P form loses positive-definiteness quickly. The bias states along ĝ are unobservable, so their covariance grows without bound unless you cap it. *This is a reason not to implement one, not a recipe.*

### I²C bandwidth — the actual bottleneck **[DERIVED]**

Compute is free; **the shared 400 kHz I²C bus is not**. `HARDWARE.md:52-63` puts the CST816T touch controller (0x15), the QMI8658C (0x6B) and the PCF85063A RTC (0x51) on one bus at ≤400 kHz.

A 6-byte burst read costs START + addr/W + reg + repeated-START + addr/R + 6 data ≈ 81 bit-times ≈ **225 µs at 400 kHz**. At 1 kHz sampling that is **~22% bus occupancy for the gyro alone**. Two separate transactions (gyro + accel) would be ~45%.

**Useful register-map fact:** `kRegAccelX = 0x35` and `kRegGyroX = 0x3B` (`src/imu_qmi8658.cpp:20-21`) are **contiguous** — 0x35 + 6 = 0x3B. A single **12-byte burst from 0x35 returns both accel and gyro**, costing ~135 bit-times ≈ **340 µs** instead of 450 µs for two transactions. That is a 25% saving if you ever need accel at rate. (The current driver reads them separately via `readVector` at `src/imu_qmi8658.cpp:112-118`, which is correct today because accel is only read at rest.)

For the recommended architecture you do **not** need accel during the burst at all — read it only while still, to anchor ĝ. Bus cost during a turn stays at today's ~22%.

Also note the accel is configured to **500 Hz ODR** (`CTRL2 = 0x04`, `src/imu_qmi8658.cpp:33`) against the gyro's 1000 Hz (`CTRL3 = 0x73`, line 36). Any accel-fusing filter would be reading duplicate samples half the time — another reason the accel-fusion path is a poor fit.

---

## Recommendations for THIS robot (ranked: gain, cost, risk)

| # | Change | Gain (360° turn) | Cost | Risk | Verdict |
|---|---|---|---|---|---|
| **1** | **Project the gyro onto the normalised resting gravity vector**: replace `componentFor(g_turnYawAxis, gx,gy,gz)` at `src/main.cpp:5103-5105` with `ω·û`, û = normalise(accel at rest). Store û alongside/instead of `g_turnYawAxis`. | **3.48°** | 3 mul + 2 add; data already in RAM; ~15 lines incl. host tests | Low — pure arithmetic, unit-testable in `turn_measure.cpp` with no Arduino deps | **Do this first** |
| **2** | **ZUPT all three gyro axes.** `src/main.cpp:5156` writes only `g_turnBias[yawAxis]`; extend `g_headingZupt` to a 3-element estimator (the calibration path at `src/main.cpp:4578` already builds `BiasEstimator bias[3]`). | 0.12° | ~free; 3× the accumulator state | Low | **Prerequisite for #1** — without it, Y/Z bias leaks in weighted 0.098 each |
| **3** | **Calibrate gyro scale factor against an external reference.** Hand-rotate the robot N=10 full turns against a desk index mark (3600° truth) and compare the integrated total. Store the ratio in NVS beside the existing turn calibration. | **3.6–10.8°** (the largest remaining term) | Procedure + 1 NVS float + 1 multiply | Low, but needs care: do it *unplugged* (`HARDWARE.md:252-293` — the USB cable makes the chassis read 2–4× slow) | **Highest absolute gain, but requires a manual procedure** |
| **4** | **Propagate ĝ with the gyro during the burst**; re-anchor from accel only at rest. `ĝ −= (ω×ĝ)Δt`, renormalise, `ψ += (ω·ĝ)Δt`. | 0.1–0.3° | ~25 flops/sample = 0.013% core | Low–medium — new state to reset correctly on `begin()`/`stop()` | Do after #1–#3 |
| **5** | **Harden against the 20 Hz sampling collapse.** Make `Integrator::worstGapUs()` (`src/turn_measure.cpp:139-141`) a *failure* condition, not just telemetry — e.g. reject if worst gap > 20 ms. | Prevents ~1–2° silent errors | ~5 lines | Low | Cheap insurance; the failure is already documented as having occurred |
| **6** | **Validate that the 8° is real geometry, not accel bias.** Rotate the robot 180° about its vertical axis on the same surface and re-read gravity: a fixed accel bias stays put in body frame, real tilt reverses its horizontal components. | Confidence in #1 | 2 minutes | None | **Do before #1** — see caveat below |
| **7** | Mahony / Madgwick / complementary filter | **0** on yaw; negative during the burst | 80–150 flops + 1–2 gains | Medium (see §2) | **Do not** |
| **8** | EKF / ESKF / MEKF / UKF | 0 on yaw | 1.5k–20k flops + Q/R/P₀ tuning | High (tuning, conditioning, unobservable bias states) | **Do not** |

### Important caveat on recommendation #1 **[INFERRED]**

The 8° figure assumes the (0.10, −0.10) g horizontal components are **real geometry** — either true platform tilt or fixed PCB mounting misalignment. Either of those is fine: both mean the accelerometer correctly reports where vertical sits relative to the gyro axes, which is exactly what the projection needs.

It would be **wrong** only if those components were accelerometer *zero-g bias*. Two pieces of evidence say they are not:
- Magnitude: 100 mg is well above typical MEMS accel zero-g offset (tens of mg).
- Repeatability: `HARDWARE.md:270` independently records `(1.02, 0.12, −0.09)` in a separate session, closely matching the `(1.01, 0.10, −0.10)` baseline. A stable, repeatable vector.

Recommendation #6 discriminates the two cases definitively in about two minutes. Additionally, the prediction is directly falsifiable: with the current code, a calibrated "360°" turn should physically **overshoot by ~3.5°**, which accumulates to ~35° over 10 repeats and is trivially visible against a desk mark. If you observe that overshoot, #1 is confirmed empirically.

---

## Could not verify

- **QMI8658C gyro noise density / ARW / bias-instability specification.** Attempted four sources: `qstcorp.com` (404), `mouser.com` datasheet (timeout), `files.waveshare.com` (404), and the local `datasheets/QMI8658C_IMU.pdf` (no text extraction available with my toolset — grep for "noise density", "ARW", "bias instability" returned no matches, consistent with a compressed-stream PDF). **Mitigation:** I substituted the *measured* baseline, which is stronger evidence anyway — ±0.05° over 48 s bounds the effective residual bias at ≤0.00104 °/s directly, with no datasheet needed. The quantisation figure (0.0625 °/s/LSB) is verified from firmware constants at `src/imu_qmi8658.h:34` and `HARDWARE.md:318`.
- **QMI8658C gyro sensitivity (scale-factor) tolerance.** Same sources unreachable. The ±1–3% range in §4 row 1 is a **[INFERRED]** typical-MEMS figure, not a part-specific spec. This is the single largest uncertainty in this report and the reason recommendation #3 is framed as "measure it" rather than "assume it".
- **Madgwick's original 2010 technical report** (x-io.co.uk/downloads/madgwick_internal_report.pdf) — fetched successfully but returned as unparsed PDF binary; only the section headings were readable (confirming §3.2 "Orientation from vector observations", §3.5 "Gyroscope bias drift compensation", Appendix A "IMU filter implementation optimised in C" vs Appendix B "MARG filter"). **Mitigation:** the substantive claims are cited instead from the AHRS reference implementation's derivation pages, which reproduce Madgwick's equations and cite him directly.
- **Crassidis, Markley & Cheng, "Survey of Nonlinear Attitude Estimation Methods" (JGCD 2007)** and **Markley, "Attitude Error Representations for Kalman Filtering" (JGCD 2003)** — not retrieved (NTRS PDF returned unparsed binary). The MEKF ≡ ESKF equivalence and the naive-quaternion-EKF singular-covariance claim in §1 are therefore **[INFERRED]** from standard practice plus Solà arXiv:1711.02508 (abstract confirmed reachable; full text returned as unparsed PDF). These are uncontroversial and well-established, but I did not read the primary text.
- **The IMU's physical offset from the robot's pivot axis** is not documented in `HARDWARE.md`. The centripetal-acceleration figures in §2(c) are therefore given as a table across r = 10/30/50 mm rather than a single number. The *conclusion* (accel is unusable during the burst) holds across that whole range.
- **Actual tilt amplitude during a real turn.** No measurement exists — the 2–4° peak assumed in §5 is **[INFERRED]** from typical desk-robot weight transfer. This is measurable with the existing hardware (log all three accel axes through a turn and look at the low-frequency component after subtracting the ω²r term) and would sharpen recommendation #4's payoff estimate.

---

## Adjacent, not pursued

- `HARDWARE.md:252-293` documents that a USB cable physically restrains the pivot, making measured turns 2–4× short and varying 2× between identical commands — a far larger error source than anything in this report, already mitigated by putting calibration on the touch menu.
- `correctTurn()` (`src/turn_measure.cpp:313-343`) snaps to a 10 ms tick grid before clamping, quantising the achievable turn to ~2.9° at 285 °/s — a floor on closed-loop turn precision independent of sensing.
- `TurnController` (`src/turn_measure.h:193`+) closes the loop on integrated angle with a timeout backstop; a rate-feedforward or trapezoidal-velocity profile would reduce overshoot but is a control problem, not an estimation one.
- The QMI8658C exposes INT2 on GPIO38 (`HARDWARE.md:62`) but the driver polls; a data-ready interrupt would give deterministic sample timing and eliminate the jitter that trapezoidal integration currently has to absorb.
- The QMI8658C has on-chip attitude-engine / pedometer features and die-temperature registers 0x33/0x34 (`HARDWARE.md:100`); gyro scale factor is temperature-dependent and a temp-indexed scale correction is possible but almost certainly below the noise floor of the other errors.
- `MovementExpression`'s EXCITED pattern relies on two 360° spins cancelling exactly (`src/movement_expression.cpp:32-39`); any systematic per-turn bias like the 3.48° misalignment cancels there by symmetry but not in single-direction turns.
- Wheel-odometry differencing could provide an independent yaw estimate for cross-checking gyro scale factor, but a differential-drive desk robot on a smooth surface slips enough to make this unreliable.

---

## References

### Load-bearing (claims in this report depend on these)

**Primary — this robot's firmware and hardware documentation**
- `desktop-robot:src/imu_qmi8658.h:29-35` — `kGyroDpsPerLsb = 2048/32768 = 0.0625`, `kAccelGPerLsb = 2/32768`. Sets the quantisation floor.
- `desktop-robot:src/imu_qmi8658.cpp:20-21, 29-39` — register map (`kRegAccelX = 0x35`, `kRegGyroX = 0x3B` — contiguous), CTRL2 = 0x04 (accel ±2 g @ 500 Hz), CTRL3 = 0x73 (gyro ±2048 dps @ 1000 Hz).
- `desktop-robot:src/imu_qmi8658.cpp:68-81, 112-118` — `readVector` returns all three axes; gyro and accel are separate transactions today.
- `desktop-robot:src/turn_measure.cpp:40-47` — `yawAxisFromGravity` picks argmax|component|, discarding the 8°.
- `desktop-robot:src/turn_measure.cpp:99-120` — trapezoidal integration against measured elapsed µs.
- `desktop-robot:src/turn_measure.cpp:313-343` — `correctTurn`: `ideal = testedMs·360/|measured|`; shows sensor scale error passes 1:1 into physical turn error.
- `desktop-robot:src/turn_measure.h:52-69, 101, 111` — `BiasEstimator` (kMinSamples = 32), `Integrator::kMinSamples = 24`, `double degrees_`.
- `desktop-robot:src/main.cpp:5100-5112` — three gyro axes read, two discarded; the projection input is already free.
- `desktop-robot:src/main.cpp:5139-5160` — ZUPT: 2 s period, 8 dps stillness gate, writes only `g_turnBias[yawAxis]`.
- `desktop-robot:src/main.cpp:4573-4598, 5075-5079` — `BiasEstimator bias[3]` computed, only the yaw element used; resting gravity printed.
- `desktop-robot:HARDWARE.md:95-111, 295-318` — 6-axis part, no magnetometer populated; verified register map; 16 LSB per °/s.
- `desktop-robot:HARDWARE.md:183-191` — the 20 Hz LCD-mode loop cap; 990 vs 20 samples measured.
- `desktop-robot:HARDWARE.md:252-293` — USB-cable restraint trap; independent gravity reading `(1.02, 0.12, −0.09)`.

**Secondary — filter theory**
- Solà, J. (2017). *Quaternion kinematics for the error-state Kalman filter.* arXiv:1711.02508. Canonical ESKF reference; establishes ESKF ≡ MEKF framing.
- Mahony, R., Hamel, T., & Pflimlin, J.-M. (2008). *Nonlinear complementary filters on the special orthogonal group.* IEEE TAC 53(5):1203-1218. Via ahrs.readthedocs.io/en/latest/filters/mahony.html — source of the ω_mes = Σkᵢ vᵢ × v̂ᵢ structure and the explicit statement that the magnetometer resolves the final (heading) DOF.
- Madgwick, S. (2010). *An efficient orientation filter for inertial and inertial/magnetic sensor arrays.* Univ. of Bristol tech. report. Via ahrs.readthedocs.io/en/latest/filters/madgwick.html — "the accelerometer computes the roll and pitch orientations only".
- AHRS documentation, *Complementary Filter*. ahrs.readthedocs.io/en/latest/filters/complementary.html — "Only the roll, θx, and pitch, θy, angles are computed, leaving the yaw angle, θz, equal to zero." Also the ZYX tilt formulas used to compute the −82° pitch.
- AHRS documentation, *Attitude from angular rate*. ahrs.readthedocs.io/en/latest/filters/angular.html — closed-form quaternion exponential (after Spence & Markley, *Attitude Propagation*, in Wertz ed., *Spacecraft Attitude Determination and Control*, 1978, ch. 17).
- Wikipedia, *Gimbal lock*. en.wikipedia.org/wiki/Gimbal_lock — the ±90° singularity and the strapdown/quaternion prescription.

### Background (context; no claim rests solely on these)

- AHRS documentation, *Extended Kalman Filter*. ahrs.readthedocs.io/en/latest/filters/ekf.html — standard predict/correct equations and the note that gyro data enters as process noise rather than measurement noise.
- AHRS documentation, *Algebraic Quaternion Algorithm (AQUA)*. ahrs.readthedocs.io/en/latest/filters/aqua.html — quaternion↔DCM identities; AQUA's separation of tilt and heading sub-problems illustrates the same 2-DOF/1-DOF split.
- Valenti, Dryanovski & Xiao (2015). *Keeping a good attitude: a quaternion-based orientation filter for IMUs and MARGs.* Sensors 15(8):19302-19330.
- Crassidis, Markley & Cheng (2007). *Survey of nonlinear attitude estimation methods.* JGCD 30(1). **Not retrieved** — cited for completeness on MEKF/UKF equivalence.
- Markley, F. L. (2003). *Attitude error representations for Kalman filtering.* JGCD 26(2). **Not retrieved** — cited for completeness on the singular-covariance argument against naive quaternion EKFs.
- Savage, P. (1998). *Strapdown inertial navigation integration algorithm design, Part 1: Attitude algorithms.* JGCD 21(1). **Not retrieved** — the standard reference for coning-error analysis.