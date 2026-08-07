# Research: Drift Compensation
## Executive summary
## 1. ZUPT: theory, detectors, and critique of the current implementation
## 2. ZARU vs ZUPT
## 3. Heuristic drift elimination (HDE/HDR) and applicability here
## 4. Allan variance: method, interpretation, and prediction vs our measurement
## 5. Temperature-dependent bias
## 6. Bias re-estimation cadence
## 7. Sample rate interaction

STOP after section 7. If too long, send 1-4 first labelled "PART 1 OF 2".

(2) I want to challenge recommendation #8 before it goes into the synthesis, because I derive the opposite conclusion and one of us is wrong.

You wrote: "Calibrate gyro SCALE FACTOR from the zero-sum gestures. After a pattern guaranteed to net zero heading, the residual is an almost pure scale observation - bias cancels across the opposed legs, scale accumulates."

My derivation says the reverse. Let scale factor k (gyro measures k times true rate) and bias b. For a CLOSED-LOOP turn the controller drives until the measured integral reaches target T, so:

  Leg 1, clockwise, target T1, duration t1:  k*theta1 + b*t1 = T1   =>  theta1 = (T1 - b*t1)/k
  Leg 2, counter-clockwise, target T2, duration t2:  -k*theta2 + b*t2 = -T2  =>  theta2 = (T2 + b*t2)/k

  Net true rotation = theta1 - theta2 = (T1 - T2 - b*(t1+t2)) / k

With T1 = T2 (as in our R90/L90 pairs) the numerator loses the target terms entirely and becomes -b*(t1+t2). So:
  - SCALE k appears only as a divisor of a quantity that is already zero when b=0. Scale error alone produces NO residual - it is unobservable in a zero-sum closed-loop sequence.
  - BIAS b produces a residual proportional to total elapsed time. Bias IS observable.

That is exactly backwards from your #8. Intuition: a closed-loop turn stops on MEASURED angle, so a gyro reading 1% high makes the robot physically turn 1% less on EVERY leg, in both directions, and 0.99*(+90) + 0.99*(-90) is still 0.

Please either correct me or correct #8, showing the algebra. This matters a lot: an independent agent concluded scale factor is the dominant remaining error (1-3% = 3.6-10.8 deg per 360) and that only an external angle reference can fix it. If you are right, we have a free on-board scale calibration. If I am right, scale is genuinely unobservable without an external reference and we must not pretend otherwise.

Also please state explicitly what the zero-sum residual DOES measure, given the robot also has a fixed coast overshoot of roughly 7-9 degrees per stop at full speed (which is direction-dependent in sign and would partly cancel across opposed legs, but not exactly).

[Response]
# Research: Drift Compensation

**PART 1 OF 2 — Executive summary and sections 1–4. Sections 5–7 follow as PART 2 OF 2 on request.**

## Executive summary (max 10 bullets, each tagged)

- **The mechanism in the firmware is ZARU, not ZUPT.** It applies a zero *angular-rate* pseudo-measurement to re-estimate gyro bias; there is no velocity or position state anywhere in the pipeline. [SOURCED — ZARU defined as "for stationary phases, a zero-angular rate update (ZARU) is implemented to eliminate gyroscope bias," PMC13030354 §2.3; code at `desktop-robot:src/main.cpp:5139-5160`]
- **The current detector is the weakest link, but not because of the threshold value — because it is a single-sample, single-axis, rate-only test.** Literature-standard detectors are windowed multi-condition tests (accel magnitude + accel variance + angular-rate energy, all simultaneously, over W≈100 samples). [SOURCED — PMC11435479 §3.2; PMC12031195 §2.1]
- **Knowing the motion command is worth a lot, but not in the way it seems: it does not make the detector unnecessary, it makes a much *stricter* detector free.** Pedestrian systems must trade precision against recall because stance windows are ~0.3 s and scarce; this robot has stationary windows of seconds-to-minutes and thousands of them, so recall is worthless and precision is everything. [INFERRED, grounded on SOURCED precision-weighted evaluation rationale in PMC11435479 §4.1.2]
- **The on-chip low-pass filter (CTRL5) is never configured, so the firmware subsamples a 1000 Hz unfiltered stream at 100 Hz.** This aliases ~500 Hz of noise bandwidth into 50 Hz and multiplies angle random walk and bias-estimator noise by √(1000/100) ≈ **3.2×**. [DERIVED from `desktop-robot:src/imu_qmi8658.cpp:97-100` writing only CTRL1/2/3/7; SOURCED constraint from Kalibr IMU noise model: "If you simply 'subsample' your gyro or accel, you are not allowed to scale your white noise density in that way"]
- **Allan variance predicts ~0.02 deg of ARW over a 1.3 s turn and ~0.1 deg over 48 s for a consumer part; the measured ±0.05 deg over 48 s is plausible and sits at the good end of that bracket.** Against the no-ZUPT prediction of 0.5 dps × 48 s = **24 deg**, the loop is delivering ~3 orders of magnitude. [DERIVED, using SOURCED noise densities 0.007 °/s/√Hz (BMI270) and 0.014 °/s/√Hz (BMI088)]
- **Temperature compensation is not worth implementing here.** Typical consumer gyro TCO is 0.015–0.02 °/s/K; a desk-scale thermal ramp is ~0.05 K/s, so bias moves ~0.001 dps/s — i.e. **0.002 dps of staleness across a 2 s ZARU interval, ≈0.003 deg on a 1.3 s turn.** ZARU already tracks thermal drift ~100× faster than it happens. [DERIVED from SOURCED Bosch BMI270 TCO ±0.02 °/s/K and BMI088 ±0.015 °/s/K]
- **2 s re-estimation is right, arguably conservative.** The Allan-deviation minimum for consumer MEMS sits at τ ≈ 10–100 s, so 2 s is on the white-noise side where longer averaging still helps — but the estimator noise is already ~0.02 dps ⇒ ~0.03 deg on a turn, so there is nothing left to win. The real defect is that the bias is *hard-replaced* rather than blended. [DERIVED; optimal-τ principle SOURCED from Kalibr/IEEE Std 952-1997]
- **1 kHz buys nothing over 200 Hz for a 1.3 s turn.** Trapezoidal integration error at 100 Hz on this turn profile is ~0.02 deg, and ARW is rate-independent at fixed noise density. Rate matters here *only* through aliasing. [DERIVED]
- **HDE does not apply, but there is a stronger analogue.** Borenstein-style snapping to building axes needs an external structural prior on absolute heading, which a desk robot lacks — but the movement vocabulary's guaranteed zero-sum gestures are a hard loop-closure constraint, and they observe **scale factor**, which is where the remaining error actually lives. [INFERRED; HDE definition SOURCED PMC13030354 §2.3] ⚠️ **See correction below — this bullet is wrong and is withdrawn.**
- **After ZARU, bias is no longer the dominant turn error — scale factor is.** Residual bias contributes ~0.1 deg to a 370 deg sweep; a 0.4–1% scale error contributes **1.5–3.7 deg**. Further bias engineering has almost no headroom. [DERIVED, using SOURCED BMI270 sensitivity error ±0.4% and TCS ±0.02 %/K] ⚠️ **See correction below — needs qualification.**

---

## 1. ZUPT: theory, detectors, and critique of the current implementation

### 1.1 What a ZUPT formally corrects

In a strapdown INS, a zero-velocity update is a **pseudo-measurement** injected into a Kalman filter: when the platform is detected stationary, the filter is told `v = 0`, and the innovation `z_k = v_k^-` is fed through the measurement matrix `H = [0₃ₓ₃ I₃ₓ₃ 0₃ₓ₉]` against a 15-state error vector `[δp, δv, δa, δg_b, δw_b]` [SOURCED — PMC11435479 §3.1]. Because the error-state covariance couples velocity to attitude and to sensor biases, a velocity-only observation *indirectly* corrects tilt and accelerometer bias, and prevents the cubic growth of position drift [SOURCED — PMC13030354 §2.1: "resets the integration of errors at each step and prevents the cubic growth of positioning drift over time"].

The critical structural fact for this robot: **ZUPT does not observe yaw.** In a 6-axis system with no magnetometer, heading is unobservable from a velocity pseudo-measurement. This is precisely why ZARU and HDE exist as separate mechanisms in the pedestrian literature [INFERRED from the fact that PMC13030354 §2.3 introduces ZARU and HDE specifically under *Heading/Orientation Estimation*, separately from ZUPT under §2.1].

### 1.2 Detection criteria used in the literature

**(a) SHOE — Stance Hypothesis Optimal dEtection** (Skog et al.), the reference detector [SOURCED — full formulation retrieved from Wagstaff & Kelly, arXiv:1707.01152 §III-A]:

```
T_n(a,ω) = (1/W) Σ_{k=n}^{n+W-1} [ (1/σ_a²)‖a_k − g·ā_n/‖ā_n‖‖²  +  (1/σ_ω²)‖ω_k‖² ]  <  γ
```

Note the construction: gravity is removed *without* a global orientation estimate by subtracting a vector of magnitude g along the mean acceleration direction, and the two terms are normalised by their respective noise variances so the statistic is a proper likelihood ratio.

**(b) The multi-condition ("3C") detector**, the most commonly deployed practical form [SOURCED — PMC12031195 §2.1]:

| Condition | Statistic | Rule |
|---|---|---|
| C1 | acceleration magnitude `‖a_k‖` | must lie **between** a lower and an upper threshold (bracketing g) |
| C2 | acceleration **local variance** over a half-window of s samples | must be **below** threshold |
| C3 | angular-rate magnitude `‖ω_k‖` | must be **below** threshold |

"The gait is considered stationary only if the above three conditions are met simultaneously" [SOURCED — PMC12031195 §2.1].

**(c) The MV / SHOE / ARE triple-condition detector** used for slow industrial platforms — the closest published analogue to a desk robot, because the carrier is a machine with commanded start/stop cycles rather than a human [SOURCED — PMC11435479 §3.2]:
- **C1 = MV (Acceleration Moving Variance)** detector on `‖f_k‖`, threshold γ_a
- **C2 = SHOE**, combining acceleration and angular-rate energy
- **C3 = ARE (Angular Rate Energy)** detector on `‖ω_k‖`, threshold γ_ω
- **Sliding window W = 100 samples at 100 Hz sampling = 1.0 s**, followed by a **median filter of length L = 100** to suppress burr/chatter in the binary decision [SOURCED — PMC11435479 §3.2]

**(d) Sliding-variance segmentation on angular-rate magnitude** is the standard way to split stationary from moving intervals for calibration purposes [SOURCED — PMC11014127, Abstract: "using a sliding variance detector to segment the angular velocity magnitude into stationary and motion intervals"].

**Typical windows and thresholds.** W ≈ 100 samples ≈ 1 s at 100 Hz is the published operating point [SOURCED — PMC11435479 §3.2]; window sizes in foot-mounted work are shorter (0.1–0.3 s) because stance phases are short. Absolute threshold values are **platform- and noise-specific** and are always tuned per-IMU — note that in SHOE the thresholds enter only through σ_a and σ_ω, and "the noise ratio σ_a²/σ_ω² will affect the performance of the zero-velocity detector" [SOURCED — PMC11435479 §3.2]. I found no universal numeric threshold that transfers across platforms, and would distrust any that claimed to.

**The asymmetry that matters most.** The literature is explicit that ZUPT detection is **not** a balanced classification problem: "false positives significantly impact navigation performance because they imply that non-zero-velocity points are incorrectly identified as zero-velocity points. This incorrect correction in the inertial navigation solution can lead to greater errors. Therefore, in this experiment, more emphasis was placed on improving precision" — they evaluate with **F0.5**, not F1 [SOURCED — PMC11435479 §4.1.2].

### 1.3 Critique of the current detector

The implementation is `desktop-robot:src/main.cpp:5143-5149`:

```cpp
const bool settled = !g_driveRailOn && !g_movementSequencer.active() &&
                     (dps < kHeadingStationaryDps) &&
                     (dps > -kHeadingStationaryDps);
if (!settled) { g_headingZupt.reset(); return; }
```

with `kHeadingStationaryDps = 8.0f` and `kHeadingZuptPeriodMs = 2000` at `desktop-robot:src/main.cpp:350-353`, accumulating at `kHeadingIdleSampleMs = 10` (100 Hz) per `desktop-robot:src/main.cpp:348` and `:5093-5098`.

**Is it sound? Yes — it works, and the measured bounded drift proves it. But it is sound by luck of duty cycle, not by construction.** Five specific findings:

1. **It is a single-sample instantaneous test, not a window statistic.** Every published detector integrates a statistic over W samples [SOURCED — PMC11435479, PMC12031195]. A single-sample test is maximally sensitive to individual outliers in *both* directions: one noise spike resets a good window (harmless), and one quiet sample in the middle of slow real motion admits it (harmful).

2. **The threshold is ~20–60× the noise σ.** Per-sample σ is ~0.14 dps if properly band-limited, or ~0.44 dps as currently subsampled (see §7). An 8 dps gate is therefore enormously permissive. [DERIVED, using SOURCED noise density 0.014 °/s/√Hz]. The realistic failure is not a transient — a single 8 dps sample in a 200-sample mean shifts the estimate by only 0.04 dps — it is a **sustained sub-threshold rotation**: the robot slipping on a smooth desk, settling on a soft mat, or being slowly rotated by a cable at 3–5 dps. That is absorbed *entirely* into the bias, and it is exactly the false-positive case the literature weights most heavily. [DERIVED]

3. **It is yaw-axis only, and the accelerometer — already configured and running at 500 Hz per `desktop-robot:src/imu_qmi8658.cpp:31-33` — is unused for this.** A translation (robot slid across the desk), a tap, or a lift produces little or no yaw rate but a large accelerometer signature. Every standard detector includes an acceleration magnitude and an acceleration variance condition [SOURCED — PMC12031195 §2.1].

4. **Latent lock-out: the threshold is applied to the bias-*corrected* rate.** `dps` at `desktop-robot:src/main.cpp:5108` is `rawDps − g_turnBias[...]`, and it is `dps` that is tested at `:5144-5145` while `rawDps` is what gets accumulated at `:5151`. Accumulating raw is correct. But testing corrected means that if the stored bias is ever badly wrong (a stale cross-session bias, a corrupt calibration, a large thermal excursion after a long powered-down soak), the detector can never fire and therefore can never repair itself. A **variance**-based condition is bias-independent by construction and cannot lock out. [DERIVED from code]

5. **No post-motion settle dwell on the free-running heading path.** The sequenced paths do enforce settle — `kTurnCoastMs = 400` at `desktop-robot:src/main.cpp:376`, and comments at `:426` ("Long enough for the chassis to stop coasting before bias is re-measured") and `:4332` ("Long enough for the chassis to stop rocking before the next bias sample") — but the ZARU accumulator at `:5143` begins the instant `g_driveRailOn` clears and the sequencer goes idle, so chassis rocking enters the first window. The 2 s apply-period partially masks this by diluting it. [INFERRED from code; not directly observed on hardware]

### 1.4 What a better detector looks like for this robot

```
settled  ⟺   (¬railOn) ∧ (¬sequencerActive) ∧ (t − t_motionEnd > T_dwell)
             ∧ var_W(ω_yaw) < γ_ω                 // bias-independent
             ∧ | ‖a‖ − g | < γ_mag                // not being lifted/tilted
             ∧ var_W(‖a‖)  < γ_var                // not being slid or tapped
             ∧ |ω_yaw − b̂| < γ_rate               // keep as a cheap outer guard
```
with W ≈ 50–100 samples (0.5–1.0 s at 100 Hz, matching the published W = 100 [SOURCED — PMC11435479 §3.2]), `T_dwell` ≈ 300–500 ms to match `kTurnCoastMs`, and `γ_rate` tightened from 8 dps to **~1 dps** — still 3–7σ, and affordable precisely because recall is free here.

### 1.5 How much does knowing the command help?

**A great deal, but it changes the design objective rather than replacing the detector.**

- *What the command gives you:* ground truth that **no actuation torque is being applied**. Pedestrian and vehicle systems have no equivalent and must infer everything from the IMU. Machine platforms that *do* have this — e.g. pipe-jacking rigs, which stop for 10–15 min every 3 m — are described in the literature as having "the ideal conditions for zero-velocity correction" [SOURCED — PMC11435479 §2].
- *What the command does not give you:* ground truth that the robot is **not moving**. Coasting (explicitly handled at `desktop-robot:src/main.cpp:5116-5122`), chassis rocking, desk vibration from a nearby servo, being picked up, and being nudged by a human all occur with the rail off.
- *The quantitative consequence:* pedestrian ZUPT lives or dies on threshold tuning because stance windows are ~0.2–0.5 s inside a ~1 s gait cycle — miss too many and the filter starves. This robot is stationary the overwhelming majority of the time and produced **29 ZARU applications in 48 s**. Rejecting 90% of candidate windows would still leave ~3 updates per 48 s, which is more than enough given that bias moves on thermal timescales of minutes (§5). **So: set the detector for near-100% precision and accept whatever recall falls out.** That is a design freedom pedestrian systems do not have, and it is the single highest-leverage consequence of knowing the command. [INFERRED, grounded on SOURCED precision-over-recall rationale, PMC11435479 §4.1.2]

---

## 2. ZARU vs ZUPT

| | ZUPT | ZARU |
|---|---|---|
| Pseudo-measurement | `v = 0` (linear velocity) | `ω = 0` (angular rate) |
| Directly corrects | velocity error; via covariance coupling: position, tilt, accel bias | **gyro bias** |
| Corrects heading? | No — yaw is unobservable from velocity in a 6-axis INS | Yes, indirectly: removing bias removes the linear-in-time heading ramp |
| Needs a Kalman filter? | Effectively yes (it is a filter measurement) | No — a windowed mean of the rate *is* the estimator |

[SOURCED — ZUPT mechanics and H-matrix, PMC11435479 §3.1; ZARU purpose, PMC13030354 §2.3: "For stationary phases, a zero-angular rate update (ZARU) is implemented to eliminate gyroscope bias, whereas linear movement enables a straight-line constraint to prevent heading drift." Both appear as distinct blocks in the canonical inertial-tracking block diagram — SOURCED, PMC9318359 Figure 4 caption: "Abbreviations: ZARU: Zero Angular Rate Update; HDE: Heuristic Drift Elimination."]

**Which is this robot doing? ZARU, unambiguously.** The code averages `rawDps` over a stationary window and writes the mean directly into `g_turnBias[yaw]` (`desktop-robot:src/main.cpp:5151-5159`), using `TurnMeasure::BiasEstimator`, which is a plain running mean in double precision (`desktop-robot:src/turn_measure.cpp:53-76`). There is no velocity state, no position state, and no Kalman filter anywhere in the heading path. The name `g_headingZupt` and the comment at `desktop-robot:src/main.cpp:5139` ("Zero-velocity update") are a misnomer. [SOURCED — local code]

**Why this distinction is not pedantic:** it tells you which failure modes you inherit. A ZUPT-based system fights *position* divergence and would still need a separate heading mechanism. A ZARU-based system like this one has exactly one job — keep the gyro bias fresh — and once it does that, the residual error is no longer bias at all (see §4.4 and §7).

**Naming recommendation:** rename `g_headingZupt` → `g_headingZaru` and `kHeadingZuptPeriodMs` → `kHeadingZaruPeriodMs`. Zero functional change, but it stops the next reader from looking for a velocity state that does not exist. *(Read-only task — recorded as a recommendation, not applied.)*

---

## 3. Heuristic drift elimination (HDE/HDR) and applicability here

### 3.1 What HDE is

Borenstein & Ojeda's Heuristic Drift Elimination exploits the observation that indoor built environments are overwhelmingly rectilinear: corridors, walls and paths align to a small number of dominant, mutually orthogonal directions. When the system detects that the user is walking in a straight line, HDE nudges the estimated heading toward the nearest dominant direction, which cancels slowly accumulating gyro drift without any external sensor. HDR (Heuristic Drift *Reduction*) is the weaker, earlier variant that damps rather than eliminates. [SOURCED for the mechanism — PMC13030354 §2.3: "In structured environments, heuristic drift elimination (HDE) further limits heading divergency by aligning the estimated trajectory with the dominant headings, such as parallel or orthogonal corridor paths." Primary source is Borenstein & Ojeda, *Journal of Navigation* 63(4), 2010 — see "Could not verify".]

The two structural preconditions are: **(P1)** a long-running, unaided heading estimate that actually drifts, and **(P2)** an external prior that constrains the *true* heading to a discrete set.

### 3.2 Does it apply to a desk robot?

**No, not as formulated.**

- **P1 fails.** Heading here is explicitly relative and re-zeroable (`desktop-robot:src/main.cpp:318-321`, `desktop-robot:README.md:96-104`), the longest continuous rotation is ~1.3 s, and drift is already bounded by ZARU to ±0.05 deg over 48 s (`desktop-robot:README.md:111-118`). There is no accumulating heading error for HDE to eliminate.
- **P2 fails.** A robot sitting on a desk has no corridor geometry. It is not translating along building axes; it pivots in place (`desktop-robot:src/movement_expression.cpp:405` — "Pivots in place: the wheels always oppose"). There is no absolute frame — no magnetometer (`desktop-robot:HARDWARE.md:295-304`) — so there is nothing to snap *to*.

### 3.3 Is there an analogous exploitable structure? Yes — three, ranked by value

**(1) Guaranteed zero-sum gesture sequences — the strongest analogue, and better than HDE.**
The movement vocabulary is constructed so that patterns net to zero heading change: "biggest movement in the vocabulary it still finishes on its start heading" (`desktop-robot:src/movement_expression.cpp:34`), "identical decomposition, so the heading still nets out exactly" (`:224`), "The two turns are opposite, so the heading still nets out" (`desktop-robot:src/movement_expression.h:177`), and the test sequence R90/L90/L45/R90/L45 sums to zero specifically so that "did it come back to where it started" is the pass/fail (`desktop-robot:src/main.cpp:391-394`).

This is a **hard loop-closure constraint on absolute truth**, which is strictly stronger than HDE's *heuristic* assumption that a corridor is axis-aligned. And crucially, because both legs of a zero-sum pattern are executed at the same rate in opposite directions, a bias error **cancels** across the pair while a **scale-factor** error **accumulates**. So the residual at the end of a zero-sum gesture is an almost pure observation of gyro scale error — which §4.4 shows is now the dominant term. [INFERRED from code structure; the bias-cancels/scale-accumulates decomposition is DERIVED]

> ⚠️ **This paragraph is WRONG and is withdrawn — see the correction below. The decomposition is exactly backwards: for a closed-loop turn, bias is observable and scale is not.**

**(2) Command quantization as "dominant directions."**
The robot only ever commands turns from a small discrete set (90, 45, 360 — `desktop-robot:src/main.cpp:391-394`). Snapping the post-turn heading to the nearest multiple of the command quantum *is* HDE, with the movement vocabulary playing the role of building geometry. **Caveat: this is the dangerous version.** It would make every turn *look* perfect while hiding the coast-overshoot and scale error the design deliberately exposes (`desktop-robot:src/main.cpp:359-363` — "Without this the coast is invisible and every turn looks more accurate than it is"). Recommend **against** snapping; recommend using the same residual as a *measurement* per (1). [INFERRED]

**(3) Gravity as an absolute reference for the other two axes.**
Roll and pitch are already directly observable from the accelerometer at rest, and this is already exploited to pick the yaw axis (`desktop-robot:src/turn_measure.cpp:40-47`, `desktop-robot:src/main.cpp:4610`). Only yaw is unobservable — the standard 6-axis result, and the reason ZARU and HDE exist at all.

**Verdict:** HDE is not applicable and should not be implemented. Its *spirit* — "exploit known structure in the task to observe an otherwise-unobservable error" — applies strongly, and the right structure to exploit is the zero-sum gesture, used to calibrate **scale factor**, not bias.

> ⚠️ **Verdict amended: the first clause stands (HDE does not apply). The final clause is withdrawn — the zero-sum gesture observes coast asymmetry and bias, not scale factor.**

---

## 4. Allan variance: method, interpretation, and prediction vs our measurement

### 4.1 What it is

Allan variance (two-sample variance) characterises how a sensor's noise averages down as a function of averaging time τ. You cluster a long stationary record into bins of length τ, take the mean of each bin, and compute half the mean-squared difference between successive bin means; Allan **deviation** σ(τ) is its square root, plotted log-log against τ. Its value is that different physical noise processes produce different, separable **slopes**, so one plot decomposes the total noise into its components. The reference procedure for gyros is IEEE Std 952-1997 [SOURCED — Kalibr IMU Noise Model, reference [1]; the log-log slope decomposition is stated there: white noise = slope −1/2, random walk = slope +1/2].

The regime structure is empirically standard and is reproduced across sensor domains [SOURCED — PMC10484910 §Noise analysis, which states it cleanly: short-τ random walk "decreases with the sampling time τ as σ ∝ τ^(−1/2)"; intermediate-τ "saturation value ... indicative of the bias instability (BI) noise ... traced to the random flickering of electronics"; long-τ "σ = α·τ ... typical of a drift rate ramp (DRR) noise associated with the presence of systematic (deterministic) errors"].

### 4.2 The five regions

| Region | Slope on log-log ADEV | Physical cause | What it costs you |
|---|---|---|---|
| **Quantization noise (Q)** | −1 | finite ADC/LSB step | matters only if LSB ≳ analog noise |
| **Angle random walk (ARW, N)** | **−1/2** | white noise on the rate output | heading error grows as **N√t** |
| **Bias instability (B)** | **0** (the minimum) | 1/f flicker in the electronics/MEMS | **the floor on how well bias can ever be known** |
| **Rate random walk (K)** | **+1/2** | Brownian wander of the bias | bias must be re-estimated to track it |
| **Rate ramp (R)** | +1 | deterministic drift — in practice, **temperature** | needs compensation or re-estimation |

Reading the parameters off the plot [SOURCED — Kalibr IMU Noise Model, "From the Allan standard deviation"]:
- **N (noise density, °/s/√Hz ≡ °/√s)** = value of ADEV at **τ = 1 s** — valid "since the noise power in most inertial sensors is dominated by white noise at a frequency of approximately 1 Hz."
- **K (rate random walk)** = value of the fitted **+1/2** diagonal at **τ = 3 s**.
- **B (bias instability)** = the minimum of the curve, divided by the standard 0.664 flicker coefficient.

Two important framing points, both explicitly stated by the same source [SOURCED — Kalibr IMU Noise Model, closing section]:
- In-run bias stability "indicates (approximately) the accuracy with which the bias can be determined" — it is the floor, not a typical error.
- "The IMU measurement error model used here is derived from a sensor which does not undergo motion, and at constant temperature. Hence scale factor errors and bias variation caused by temperature changes are not accounted for. So clearly, the model is **optimistic**... for lowest-cost sensors, increasing the noise model parameters by a factor of **10× or more** may be necessary."

### 4.3 How to run the test on *this* hardware

[DERIVED — a concrete procedure sized to the ESP32-S3R8 / QMI8658C / 8 MB PSRAM]

**Setup**
1. Robot stationary on a heavy, non-resonant surface. **Not** the desk you type on. Motors and servos completely unpowered; the drive rail off. LCD in a *fixed* mode for the whole run.
2. **Critical:** run in the LCD mode that leaves the loop free-running. `desktop-robot:HARDWARE.md:181-191` documents that any non-FACE LCD mode caps the entire loop at 20 Hz via `delay(50)`, producing 20 gyro samples per probe instead of 990. An Allan run from the wrong mode would silently characterise the *scheduler*, not the gyro.
3. USB-powered, on charge, thermally settled for ≥30 min before logging starts, and **log the die temperature (regs 0x33/0x34) alongside every sample** — this is the one place the currently-unused temperature sensor earns its keep (see §5.5).

**Logging**
4. **Rate:** log at the rate you will actually run in production, with the sensor configured exactly as production configures it. Do **two** runs: (a) as-is (CTRL5 unwritten, read at 100 Hz) and (b) with the gyro LPF enabled, read at 100 Hz. The difference between the two ADEV curves at small τ *is* the aliasing penalty of §7, measured rather than predicted.
5. **Duration:** Kalibr's recommendation is a **15–24 hour stationary recording** [SOURCED]. The practical constraint is that ADEV estimates become unreliable for τ beyond ~T/10, so a 3 h run gives usable τ to ~1000 s and an overnight 12 h run to ~1.2 h — enough to see the bias-instability minimum and the onset of the temperature-driven rate ramp. **Log overnight.**
6. **Volume:** yaw axis only, int16 at 100 Hz = 200 B/s = **8.6 MB for 12 h**. Stream as CSV over USB CDC to the host; do not buffer in PSRAM (8 MB is only ~11 h of one axis and you lose everything on a reset). If you want all three axes at 1000 Hz it is 6 kB/s = 260 MB/night — streaming only, and it will contend with the touch controller on the shared 400 kHz I²C bus (`desktop-robot:HARDWARE.md:52`, `:633`).

**Compute** (host-side, Python + `numpy`/`allantools`)
7. Convert raw counts to °/s with the documented 16 LSB/°/s (`desktop-robot:src/imu_qmi8658.h:34`, `desktop-robot:HARDWARE.md:318-319`).
8. Compute the **overlapping** Allan deviation for τ = m·τ₀ with m log-spaced from 1 to N/10.
9. Plot log-log, fit −1/2 and +1/2 diagonals, read N at τ=1 s, K at τ=3 s, and B at the minimum.
10. **Also** plot ADEV against the co-logged die temperature. If the long-τ upturn correlates with dT/dt, it is a rate ramp (temperature) rather than intrinsic rate random walk — which is the actual question §5 needs answered.

**One thing not to bother with:** quantization at ±2048 dps is 0.0625 °/s/LSB, giving σ_q = q/√12 = **0.018 dps** [DERIVED]. That is well below the expected per-sample white noise of 0.14–0.44 dps, so the −1 slope region will be buried and narrowing the full-scale range would buy nothing. The existing reasoning at `desktop-robot:src/imu_qmi8658.h:29-33` — that silent clipping is a worse failure than coarse resolution — stands unchallenged.

### 4.4 Prediction vs measurement

**Assumed parameters.** I could not obtain QMI8658C datasheet noise figures (see "Could not verify"), so I bracket using two directly-sourced consumer 6-axis parts:

| Part | Gyro noise density (N) | Gyro zero-offset over temperature (TCO) | Sensitivity error |
|---|---|---|---|
| Bosch **BMI270** | **0.007 °/s/√Hz** | **±0.02 °/s/K** | ±0.4% |
| Bosch **BMI088** | **0.014 °/s/√Hz** | **±0.015 °/s/K** | — |

[SOURCED — bosch-sensortec.com BMI270 and BMI088 product specification tables]

**Predictions** [DERIVED, σ_θ = N·√t]

| Horizon | N = 0.007 | N = 0.014 | Note |
|---|---|---|---|
| **1.3 s turn** | **0.008 deg** | **0.016 deg** | vs 370 deg swept — utterly irrelevant |
| **48 s hold** | **0.049 deg** | **0.097 deg** | 1σ of a random walk |

Add the bias-estimator contribution: with a 2 s / 200-sample window, σ_b̂ = σ_sample/√200 ≈ 0.010 dps ideal, ≈0.031 dps as currently subsampled [DERIVED]. Propagated across 24 windows this contributes a further ~0.1–0.2 deg (1σ) over 48 s.

**Total predicted 1σ heading excursion at 48 s: roughly 0.1–0.3 deg.**

**Measured:** +0.05, −0.02, +0.01, −0.01 deg at 12/24/36/48 s (`desktop-robot:README.md:115-118`).

**Does the measurement look plausible? Yes.** Three checks:

1. **Magnitude.** Measured excursions sit at or just below the optimistic end of the prediction. Being a factor of ~2–4 better than a bracketed estimate is entirely ordinary — it implies either a quieter-than-BMI088 part (N ≈ 0.005–0.008 °/s/√Hz, i.e. BMI270-class) or partial cancellation between successive independent bias estimates, or both. It is **not** implausibly good; that would be a reading of 0.001 deg, which would indicate the heading was being reset somewhere. [DERIVED]

2. **The bias spread is estimator noise, not real bias motion.** Bias moved 0.476 → 0.539 dps across 29 estimates in 48 s — a range of 0.063 dps. For n≈29 normal samples, range ≈ 3.5σ ⇒ σ_b̂ ≈ **0.018 dps**, which matches the predicted 0.010–0.031 dps estimator noise almost exactly. **The gyro's true bias is not wandering measurably over 48 s; you are watching your own estimator's noise floor.** [DERIVED] This is a useful and reassuring result: it means rate random walk and temperature are *not* limiting on this timescale.

3. **The non-monotonic sequence is not evidence of anything.** 0.05 → 0.02 → 0.01 → 0.01 shrinking looks mean-reverting, but nothing in the code resets heading, so it cannot be. Four draws from a random walk with 1σ ≈ 0.05 deg staying small is unremarkable. **The falsifiable test:** ARW predicts σ(600 s) = σ(48 s)·√(600/48) = **3.5× larger ≈ 0.2 deg**. Run one 10-minute stationary hold and ten repeats of the 48 s hold. If the 10-minute drift is ~0.2 deg and the 48 s spread across repeats is ~0.05 deg, the model is confirmed. If the 10-minute drift is much worse than 0.2 deg, temperature or rate random walk is present and §5 needs revisiting. [DERIVED]

**The number that actually matters:** without a fresh bias, 0.5 dps × 48 s = **24 deg**. Measured: 0.01 deg. **The ZARU loop is delivering a factor of ~2000.** [DERIVED from `desktop-robot:README.md:115-118` and the stated baseline]

**And the consequence for turns:** on a 370 deg sweep, residual bias now contributes ~0.1 deg while a 0.4% scale-factor error contributes **1.5 deg** and a 1% error contributes **3.7 deg** [DERIVED, using SOURCED BMI270 sensitivity error ±0.4%]. **Bias is solved. Scale is not.**

> ⚠️ **Amended: "scale is not solved" is true but incomplete — scale is not *observable* on-board at all, and it is *zero* for zero-sum gestures. See correction below.**

**— END OF PART 1 OF 2. Sections 5–7 available on request. —**

---
---

# Response to challenge on recommendation #8

**You are right. I was wrong. Recommendation #8 is withdrawn.** Your algebra is correct, your intuition is correct, and the error in #8 was mine — I imported the bias-cancels/scale-accumulates decomposition from the *open-loop* case (where you command a fixed duration) without re-deriving it for the *closed-loop* case (where the controller terminates on measured angle). Closed-loop inverts it.

## 1. Your derivation, confirmed

Let measured rate `ω_m = k·ω_t + b`, where `k` is scale factor and `b` is residual bias *after* ZARU subtraction (`b = b_true − b̂`). Leg `i` has direction `s_i = ±1` and target `T_i`.

Your two lines are exactly right:

```
Leg 1 (CW) :   k·θ₁ + b·t₁ =  T₁   ⟹  θ₁ = (T₁ − b·t₁)/k
Leg 2 (CCW):  −k·θ₂ + b·t₂ = −T₂   ⟹  θ₂ = (T₂ + b·t₂)/k

net = θ₁ − θ₂ = [T₁ − T₂ − b(t₁+t₂)] / k
```

With `T₁ = T₂` the target terms vanish and `net = −b(t₁+t₂)/k`. **`k` appears only as a divisor of a quantity that is already zero when `b = 0`.** Scale error alone produces no residual. Bias does.

Your intuition is the cleanest statement of it and I will use it verbatim in the synthesis: *a closed-loop turn terminates on measured angle, so a gyro reading 1% high makes the robot physically turn 1% less on every leg in both directions, and 0.99(+90) + 0.99(−90) = 0.*

(Strictly, `k` has a second-order effect — it scales the bias-induced residual by `1/k` — but that is a 1% modulation of an already-negligible term. Unobservable for any practical purpose.)

## 2. One refinement that strengthens your result

You derived the **true physical** residual. The number the robot actually *prints* is different, and it is worth having both, because they are not the same quantity and the difference matters when someone tries to use the printed value.

The closed-loop control law has a structural consequence: **the measured contribution of a drive phase is exactly `s_i·T_i`, by construction.** That is what "closed loop" means — the controller keeps integrating until the measurement hits the target, so neither `k` nor `b` can survive into the *reported* drive-phase contribution; they are absorbed into *when* it decides to stop.

So decompose the whole sequence into drive / coast / settle phases and sum what the heading integrator records:

```
θ_rep = Σ(drive) + Σ(coast) + Σ(settle)
      = Σ s_i T_i  +  Σ (k·s_i·c_i + b·t_i^coast)  +  Σ b·t_i^settle
      = 0          +  k·Δc                         +  b·T_notdriving
```

where `Δc ≡ Σ s_i c_i` is the **signed sum of coast overshoots**. So:

| | expression | what it is |
|---|---|---|
| **True** residual | `−b·T_drive/k + Δc` | where the robot physically ends up |
| **Reported** residual | `k·Δc + b·T_notdriving` | what `heading` prints |

Both confirm you: **`k` never appears as an isolated observable term.** In the true residual it divides zero; in the reported residual it multiplies `Δc`, so if coast were symmetric (`Δc = 0`) scale would be *completely* invisible.

Note the bias lever arm differs between the two: bias enters the true residual through *driving* time and the reported residual through *non-driving* time. Either way it is observable and scale is not.

## 3. Scale is genuinely unobservable on-board — your other agent is right

I checked whether any variant rescues it. None do:

- **Open-loop (fixed-duration) opposed legs.** True net ≈ 0 by drivetrain symmetry, so measured net = `k·0 + b·Σt = b·Σt`. `k` multiplies zero again. No help.
- **No magnetometer** (`desktop-robot:HARDWARE.md:295-304`) → no absolute yaw reference.
- **Gravity gives roll/pitch only**; yaw is the unobservable axis (`desktop-robot:src/turn_measure.cpp:40-47`). This is the standard 6-axis result.
- **No wheel encoders** → no independent odometric angle to difference against the gyro.
- **The drive rate model** `rate = 0.91·speed − 24.2` (`desktop-robot:src/main.cpp:378-382`) was itself measured *with this gyro*, so comparing predicted vs actual turn duration is circular and observes nothing.
- **The full-turn spin calibration** is likewise gyro-referenced — a 1% scale error propagates straight through it.

**Conclusion: scale factor requires an external angle reference. There is no free on-board calibration. Do not pretend otherwise in the synthesis.**

## 4. But the priority should change: scale error is largely *harmless* here

This is the part that I think improves on both prior positions, and it falls straight out of your own algebra.

The same cancellation that makes scale unobservable also makes it **not matter** for the zero-sum gesture vocabulary. Every pattern built to net zero (`desktop-robot:src/movement_expression.cpp:34`, `:224`; `desktop-robot:src/movement_expression.h:177`) physically returns to its start heading *regardless of `k`*. Scale error only bites on **net-nonzero** commanded turns, as a proportional shortfall:

- 1% scale error → 0.9 deg per 90 deg turn, 3.6 deg per 360 deg turn.

So my §4.4 claim needs qualifying rather than deleting: scale is the dominant remaining error **only for absolute, net-nonzero heading commands**, and it is **exactly zero** for the zero-sum gestures. For an expressive desk robot whose vocabulary is deliberately zero-sum, that means scale is close to a non-problem. If absolute accuracy ever does matter, the fix is one line of human-in-the-loop calibration: mark the robot and the desk, command a 360, have a human read the shortfall, store `1/k`. One number, once — but it is an external reference, exactly as your other agent said.

## 5. What the zero-sum residual DOES measure

From §2: `θ_rep = k·Δc + b·T_notdriving`.

- **Term 1 — `k·Δc`, the signed sum of coast overshoots.** At 7–9 deg per full-speed stop this dominates by two orders of magnitude. If coast is symmetric CW/CCW it cancels within an opposed pair; the residue is the **CW/CCW coast asymmetry**.
- **Term 2 — `b × non-driving time`.** This is precisely the ZARU-bounded idle drift you already measured: ±0.05 deg over 48 s. For a test sequence with ~10–20 s of settle, ~±0.03 deg. Negligible.

**So the zero-sum test is a coast-asymmetry meter, plus an end-to-end integrity check on the whole chain.** That is a genuinely useful thing for it to be — it is just not a scale calibrator.

**Two caveats you should carry into the synthesis:**

**(a) The current test sequence is angle-balanced but not stop-balanced.** R90(CW), L90(CCW), L45(CCW), R90(CW), L45(CCW) is **2 CW stops vs 3 CCW stops**. So `Δc = 2c₉₀ᶜʷ − (c₉₀ᶜᶜʷ + 2c₄₅ᶜᶜʷ)`, which does **not** vanish even if coast is perfectly symmetric. At 7–9 deg per full-speed stop that is potentially several degrees of *systematic* offset with nothing to do with drift. It probably passes today because a 45 deg turn never completes its 400 ms ramp (`kTurnRampMs = 400`, `desktop-robot:src/main.cpp:387` — a full ramp to 285 deg/s sweeps ~57 deg, more than 45), so it stops at much lower speed and coasts far less. If you want the residual as a clean measurement, use a **stop-balanced** pattern: R90/L90 repeated, or R360/L360.

**(b) You do not need the zero-sum residual for coast at all — the firmware already prints it per turn.** `finishClosedLoopTurn` computes `coast = turned − g_turnCmdAtStopDeg` and logs `"turn: %s target %.0f, stopped at %.1f, coasted %+.1f, final %.1f deg"` (`desktop-robot:src/main.cpp:5045-5063`, with the stop angle latched at `:5032`). So CW/CCW coast asymmetry can be read directly by aggregating the printed `coasted` values by direction and speed — no calibration scheme, no zero-sum pattern, just parse the existing log. That is strictly better than inferring it from a residual where it is confounded with bias.

I also confirmed the coast is **not** lead-compensated: `beginTurnCoast` cuts the motors when the powered phase reaches target and success is then judged on `magnitude >= target` *including* the coast (`desktop-robot:src/main.cpp:5049-5054`). So the overshoot is real and uncorrected, consistent with your 7–9 deg figure.

## 6. Revised recommendation #8

**Withdraw:** "the residual is an almost pure scale observation — bias cancels, scale accumulates." False. It is the reverse, and the residual is in practice dominated by neither — it is dominated by coast.

**Replace with:**

- **#8a — Measure and lead-compensate coast per direction.** Aggregate the already-printed `coasted %+.1f` values by direction and commanded speed; lead the target by the predicted coast. **Gain: attacks a 7–9 deg per stop error — roughly 300× larger than anything left in the drift budget.** Cost: low (the data already exists in the log). Risk: low, but it must not be allowed to hide the coast from the report — keep printing measured coast so it stays visible, per the design intent at `desktop-robot:src/main.cpp:359-363`.
- **#8b — One-time human-in-the-loop scale calibration against a physical mark**, *only* if any behaviour depends on absolute physical angle. Otherwise skip: scale cancels in the zero-sum vocabulary.
- **#8c — Use a stop-balanced zero-sum pattern** (R90/L90 or R360/L360) as an end-to-end integrity check, and subtract the ZARU-known `b̂·T_notdriving` term before interpreting the residual as coast asymmetry.

## 7. The corrected error budget

This is the headline for the synthesis, and it is a better story than either of the two we started with:

| Error source | Magnitude | Observable on-board? | Status |
|---|---|---|---|
| **Coast overshoot** | **7–9 deg per stop** | **Yes — already measured and printed per turn** | Uncompensated. The dominant error by ~300×. A *control* problem. |
| Scale factor | 0.9 deg per 90, 3.6 deg per 360 — **and exactly 0 for zero-sum gestures** | **No** — needs an external reference | Unobservable but mostly harmless. Fix only if absolute angle matters. |
| Residual bias (post-ZARU) | ~0.02 deg per turn | Yes — directly, σ ≈ 0.018 dps | **Solved.** |

**Drift compensation is done.** The remaining error on this robot is mechanical and control-theoretic, not inertial. That reframing is worth more than any further work on the bias path — and it only became visible because you pushed back on the algebra. Thank you for that; the corrected version is materially better than what I would have shipped.