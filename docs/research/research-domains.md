# Tracking Motion in 3D Without External References — Domain Survey, Grounded on a Slipping Desk Robot

## Verdict

- **[SOURCED]** No domain surveyed bounds heading with an IMU alone. PX4 states heading is observable only "when the vehicle is accelerating (linear acceleration) **while absolute position or velocity measurements are fused**" and that the minimum viable EKF sensor set is "IMU data, **a source of yaw** (magnetometer or external vision) and a source of height data" (PX4 `docs/en/advanced_config/tuning_the_ecl_ekf.md`). Foot-mounted INS, mocap suits, drones, and AUVs all import an external fact; none of them out-filter the problem.
- **[SOURCED]** ZUPT bounds velocity and gyro bias, not heading. Foot-mounted INS works because "dead reckoning is limited to the intervals between footfalls, as opposed to over the entire tracked motion" (arXiv:1910.00529 §II), yet Borenstein built HDR/HDE precisely because "bias drift and other gyro errors can degrade accuracy within minutes" (J. Navigation 2008, W1984754470). **This robot already has the ZUPT half and gets the same result: ±0.05° over 48 s, 29 zero-updates, bias ~0.53 dps re-estimated before every turn.**
- **[DERIVED]** The only mechanism in this survey that bounds heading without an external sensor is a **structural prior on the world**: HDE "maintains near-zero heading errors in walks of unlimited duration" by "making assumptions about walking in structured, indoor environments" (W2095620283). This robot's commanded turns are quantised to 45° multiples — an unusually clean lattice to snap to.
- **[DERIVED]** The measured **7–9° coast overshoot after motors cut** is ~150–180× the 0.05°/48 s drift budget and ~2.6× the 3.48°/360 single-axis integration deficit already fixed by gravity-projected yaw rate. Every drift-reduction technique in this survey is irrelevant until overshoot is closed. This is a *control* problem (servo deceleration, `rate = 0.91*speed − 24.2` dps, deadband ≈27), not an estimation problem.
- **[DERIVED]** Multi-IMU arrays are **not worth it here**. The verified ceiling is √N — a 16-unit calibrated array reached "3.4 times, close to the theoretical limit value of 4 times" (Sensors 2024, PMC11598325 §2). Two-to-four QMI8658C parts buy 1.4–2.0× on white noise, which is already the smallest error term by 2–3 orders of magnitude.
- **[SOURCED]** Camera-free mocap suits do not solve world position; they bound *joint angles* via a constrained skeleton, achieving 3–6° RMS with drift slopes of **−0.14 to +0.17 deg/min** over 10-minute trials (PMC8859896). The reusable idea is the constraint, not the sensing.

## Findings

### 1. Foot-mounted pedestrian INS — the canonical no-reference success

**[SOURCED]** Unaided strapdown position error "grows cubically with time" (arXiv:1910.00529 §II). The foot is special because it is the only body part provably, repeatedly, and *detectably* at zero velocity during midstance, supplying a free, high-rate, self-labelling external fact several times per second.

**[SOURCED]** Detection is a likelihood ratio test on two heuristics: (1) measured specific force equals gravity in magnitude and opposes it in direction; (2) the norm of angular velocity is zero. The threshold is tuned to minimise position error, then frozen (arXiv:1910.00529 §II).

**[SOURCED]** Detector failure is asymmetric and permanent: "A threshold that is too low leads to a failure to report detections when the foot is in fact stationary (false negatives), while a threshold that is too high leads to false reports of zero-velocity events when the foot is moving (false positives). **Both cases result in irreversible error accumulation**" (arXiv:1910.00529 §II; arXiv:1807.05275 §II).

**Accuracy.** **[SOURCED]** Foxlin's NavShoe: "meter-level error", "an extremely accurate orientation tracker on the foot", explicitly "can't be used alone for precise registration of close-range objects" (IEEE CG&A 25(6), 2005, abstract via OpenAlex W2056427752, 1386 citations). **[SOURCED]** A modern foot-mounted ZUPT INS over a **250 m** indoor trajectory with occasional fiducial-marker corrections: 0.55 m forward, 1.05 m backward, 1.68 m sideways at 90% confidence, with the central finding that "different walking types … lead to **different levels of position and heading error**" (Sensors 20(18):5031, PMC7570486). **[SOURCED]** Multi-segment lower-limb PDR without foot ZUPT: "expected positioning error is **less than 5% of the total distance travelled**" (Sensors 19(14):3140, PMC6679558). **[DERIVED]** 250 m → 0.5–1.7 m is 0.2–0.7% of distance, consistent with the widely quoted 0.3–1% figure.

**Heading specifically.** **[DERIVED]** ZUPT bounds velocity; gravity bounds roll and pitch as an absolute vector; **nothing bounds heading**. **[SOURCED]** HDR "estimates the drift component and eliminates it, reducing heading errors by **almost one order of magnitude**" (W1984754470). HDE then imports the corridor-direction prior and achieves "near-zero heading errors in walks of unlimited duration" (W2095620283). The win comes from asserting the world is rectilinear, not from sensing.

### 2. Motion capture suits without cameras

**[SOURCED]** The canonical reference is Roetenberg, Luinge & Slycke, *Xsens MVN: Full 6DOF Human Motion Tracking Using Miniature Inertial Sensors* (2009, 717 citations, OpenAlex W1215311137) — **not retrievable this session** (see Could not verify).

**[SOURCED]** The model, not the sensor, does the work. Xsens MVN Analyze against optical mocap: lumbosacral RMSE ≤9.8°, bias ≤−14.5°, LoA ≤±10°; thoracolumbar RMSE ≤5.4°, bias ≤−8.3°. Reprocessing **the same Xsens data** through a matched OpenSim model: RMSE ≤2.9°, bias ≤3°. Conclusion: "discrepancies are driven primarily by **biomechanical model differences rather than sensor or sensor fusion limitations**" (Sensors 26(9):2639, PMC13165935).

**[SOURCED]** Skeletal constraints can replace ZUPT entirely. OpenSense computes kinematics "using sensor fusion and an inverse kinematics approach with a **constrained biomechanical model**"; over two 10-minute trials with 11 subjects, "median RMS differences … between **3 and 6 degrees** for all joint angles except hip rotation", r = 0.60–0.87, and "the average slopes of the linear fits … were near zero (**−0.14–0.17 deg/min**)". The authors state this "could mitigate kinematic drift even in the trials of **continuous walking without rest**, which may obviate the need for explicit sensor recalibration (e.g. … zero-velocity updates)" (J. NeuroEng. Rehabil. 19:22, PMC8859896).

**[SOURCED]** The magnetometer is the only heading source and the fragile part: gyro orientation is "accurate on a short time scale but they drift over time", while accelerometer+magnetometer estimates are "noisy but accurate over long periods of time" (Kok, Hol & Schön, arXiv:1704.06053 §4.4). Note the asymmetry — accelerometer fixes roll/pitch; **only** the magnetometer fixes heading. The literature treats magnetometer correction *during rest* as its own technique (Sensors 19(6):1312, W2922090546), i.e. the same "inject the absolute reference at standstill" pattern this robot uses for bias.

**[DERIVED]** Mocap suits do not solve drift, they relocate it. World position and world heading remain unbounded — which is why suits are graded on joint angles, not room coordinates.

### 3. Drones and UAVs

**[SOURCED]** PX4: "The magnetometer **biases** are only observable while the drone is rotating." And: "The true heading is observable when the vehicle is accelerating (linear acceleration) while absolute position or velocity measurements are fused (e.g. GPS)."

**[SOURCED]** `EKF2_MAG_TYPE` encodes the full fallback ladder: `0` Automatic; `1` Magnetic heading only ("tilt estimate is never affected by incorrect magnetic field measurements"); `5` **None** ("useful when the data can never be trusted (e.g. high current close to the sensor, external anomalies)"); `6` **Init only** ("useful when the data can be used before arming but not afterwards").

**[SOURCED]** The no-magnetometer answer is the **Gaussian Sum Filter yaw estimator**: "multiple 3-state Extended Kalman Filters whose states are NE velocity and yaw angle … combined using a Gaussian Sum Filter … **do not rely on any prior knowledge of the yaw angle or magnetometer measurements**", triggering "Emergency yaw reset – magnetometer use stopped", and enabling operation without any magnetometer "**provided some horizontal movement after takeoff can be performed to enable the yaw to become observable**". ArduPilot mirrors this as `EK3_SRC1_YAW=8`; the compass-less arming procedure is literally "pick up the vehicle and **walk around in a circle a few meters in diameter**" to let GSF acquire alignment (`common-compassless.html`).

**[SOURCED]** Drones already run a ZUPT analogue: "the EKF relies on **constant position fusion** (fusing a synthetic position measurement at the last known position) to prevent the estimate from drifting … only active when the vehicle is detected as stationary", forceable via `EKF2_POS_LOCK`.

**[SOURCED]** Redundancy is fault isolation, not averaging: PX4 runs `MAX(EKF2_MULTI_IMU,1) × MAX(EKF2_MULTI_MAG,1)` instances (16 theoretical, 4 demonstrated on STM32F7) and with ≥3 IMUs "the EKF selector is able to apply a **median select** strategy"; ArduPilot: "only the output from a single EKF core is ever used".

**[DERIVED] Analogue for this robot:** GSF's essence is that yaw becomes observable given an independent measurement of *direction of travel*. On a differential drive that means (a) an optical-flow sensor on the desk, or (b) the non-holonomic constraint `v_body,y = 0` fused as a pseudo-measurement whenever the robot is genuinely translating and not slipping.

### 4. GPS-denied and underwater INS

**[SOURCED]** "Past approaches to solve the AUV localization problem have employed **expensive inertial sensors**, used **installed beacons** in the region of interest, or required **periodic surfacing** of the AUV. While these methods are useful, their performance is fundamentally limited" (Paull et al., IEEE JOE 2014, W1969142081, 1484 citations). **[DERIVED]** That single sentence is the complete taxonomy: better sensors, external references, or periodic re-anchoring. There is no fourth option.

**[SOURCED]** Specification semantics that matter: in-run bias stability "represents the **best possible accuracy with which a sensor's bias can be estimated**" — a floor no filter beats; turn-on bias repeatability is "most relevant for **unaided** inertial navigation systems"; ARW drift = random walk × √t; X °/s/√Hz ≡ X °/√s ≡ 60X °/√hr; a 0.1% (1000 ppm) scale-factor error turns 9.81 into 9.82 m/s²; gyros suffer *g*-sensitivity (bias shift under constant acceleration) and *g²*-sensitivity/vibration rectification (VectorNav Inertial Navigation Primer §3.1).

**Grade/cost table — every numeric cell is [INFERRED], unverified this session:**

| Grade | Cost | Gyro in-run bias | ARW | Free-inertial behaviour |
|---|---|---|---|---|
| Consumer MEMS (QMI8658C class) | $1–10 | 10–100 °/h | 0.1–0.6 °/√h | Heading useless in minutes |
| Industrial MEMS (ADIS class) | $100–1.5k | 1–10 °/h | 0.05–0.3 °/√h | Seconds–minutes |
| Tactical MEMS / small FOG | $3k–20k | 0.1–1 °/h | 0.02–0.15 °/√h | Minutes; standard for DVL-aided AUV |
| Navigation (RLG/FOG marine) | $50k–500k | 0.001–0.01 °/h | 0.001–0.01 °/√h | ≈1 nmi/hr; gyrocompassing possible |
| Strategic | $1M+ | <0.0001 °/h | — | Hours–days between fixes |

**[SOURCED] The one verified MEMS anchor:** a calibrated MEMS IMU array gave "average position error of pure inertial navigation with GNSS interrupted for **30 s** … only **1.85 m**" (PMC11598325 §2) — and required a high-precision three-axis rotary table to get there. **[DERIVED]** That is the realistic ceiling for cheap-MEMS free-inertial over tens of seconds.

### 5. Consumer robots (vacuum class) — the matching cost class

**[SOURCED]** iRobot's patented diagnosis of exactly this robot's problem: "when an autonomous robot navigates in a carpeted environment … the robot can travel a distance **greater** than the distance determined based on the wheels' rotations when the robot moves in the direction of the carpet grain … [and] a distance **less** than the distance measured … against the carpet grain" (EP2858794B2, *Carpet drift estimation and compensation using two sets of sensors*, inventor Dhiraj Goel, priority 2012-06-08, granted 2024-02-28).

**[SOURCED]** The fix: two sensor sets, one for intent and one for reality. "measurements from a sensor can provide an indication of the **desired motion or commanded motion**, and measurements from another sensor … an indication of the **true or actual motion** … odometry sensors … provide an indication of the desired or commanded motion … The true motion, such as changes in the robotic device's orientation or heading, may be estimated using … **gyroscopic sensors, image sensors** … The carpet drift … may be estimated by **comparing the desired motion and the actual motion**." The drift "may be **proportional or somewhat related to the distance traveled**", so it integrates. By-products: motor-controller compensation and carpet/hard-floor classification (EP2858794B2).

**[SOURCED]** The 1996 answer is still the best cheap one: "Gyrodometry makes use of this observation by **using odometry data only — most of the time**, while substituting gyro data only during those brief instances which gyro and odometry differ substantially. In this way the ill-effects of gyro drift are almost completely eliminated, and our method can thus make use of **inexpensive gyros with large drift rates**" (Borenstein & Feng, ICRA 1996, W2111152968, 214 citations).

**[SOURCED]** Prior art cited inside EP2858794B2 shows the industry's alternatives: WO 2011/052827 A1 (slip angle from encoders + gyro + ceiling CCD); US 2011/0125323 A1 (carpet-drift vector via EKF+SLAM with an upward optical sensor watching a ceiling light pattern); JP 2010-167055 A (CCD reading carpet pattern + directional gyro). **[SOURCED]** CN105899112B (iRobot, Schnittman, priority 2014-01-10): "The sensor system includes an **inertial measurement unit**."

### 6. Cross-cutting: what actually bounds drift, ranked

| # | Mechanism | Bounds | Evidence | Available here? |
|---|---|---|---|---|
| 1 | External absolute reference (GNSS, beacons, cameras, magnetometer) | Everything | PX4 mag/GSF; ArduPilot `EK3_SRC1_YAW`; Paull et al. | **No** |
| 2 | Map/geometry/structural prior (HDE, terrain match, wall datum) | **Heading** + position | W2095620283 | **Yes — strongest lever** |
| 3 | Kinematic model constraints (skeleton, non-holonomic) | Relative pose; couples heading to travel direction | PMC8859896; PMC13165935 | Partly |
| 4 | ZUPT / ZARU | Velocity, gyro bias, roll, pitch. **Not heading** | arXiv:1910.00529; PX4 Ground Position Lock | **Already done** |
| 5 | Velocity aiding (DVL, optical flow, odometry) | t³ → ~linear in distance | Paull; `EK3_GPS_TYPE=3` optical-flow-only | If flow sensor added |
| 6 | Calibration (scale factor, misalignment, temperature) | Coefficient, not growth law | 3.4× after compensation (PMC11598325) | **Yes — cheapest** |
| 7 | Redundancy / better sensors | Coefficient; ARW ∝ 1/√N | PMC11598325; PX4 median-select | Poor return |

**[DERIVED] The organising principle: drift is bounded only by information that does not come from the IMU.** ZUPT is no exception — its information content is the external fact "I am stationary". Skeletons are no exception — theirs is "bones do not stretch".

### 7. Multi-IMU arrays

**[SOURCED]** Origin: Bayard & Ploen (JPL, 2003) "virtual gyroscopes" — "Through computer simulations, the fused gyroscope accuracy was **improved by a factor of 173**" (PMC11598325 §2). That is a correlation-exploiting simulation result, not √N.

**[SOURCED]** The empirical ceiling is √N: Wuhan University, 2021, rotary-table calibration of a MEMS IMU array — "navigation accuracy … increased by **3.4 times, close to the theoretical limit value of 4 times**". **[DERIVED]** Limit 4 ⇒ N = 16 ⇒ √N; the 3.4 vs 4.0 gap is residual correlated/deterministic error.

**[SOURCED]** Other real arrays: 72 MEMS gyros on a three-layer board with HMM fusion, "+50% compared with Kalman filter" (Michigan, 2015); 32× MPU9250 for pedestrian navigation with complementary filter + ZUPT-interval Kalman correction (Soochow, 2020) (PMC11598325 §2).

**[SOURCED]** What does not average away, per the array error model `y_i = ω_i + b_i + n_ai` with `b_i` rate random walk and `n_ai` angle random walk, plus per-unit `K_g`, `K_a` scale/installation matrices: "the sensitive axes of the MEMS inertial sensors produced are not always perfectly aligned, resulting in problems such as **shaft misalignment errors and proportional errors**" (PMC11598325 §3). **[DERIVED]** Common-mode terms — shared die temperature, shared supply, shared PCB stress, shared *g*-sensitivity if co-oriented — do not reduce at all.

**[SOURCED]** Arrays do buy geometry that averaging cannot: with K ≥ 3 accelerometer triads spanning a 2-D plane, `ω̇ = Σₖ Aₖ(fₖ − [ω×]²rₖ)` makes angular acceleration observable from accelerometers alone, so "it is strictly speaking not necessary to include a gyroscope triad", with gyro feedback `ω̇ = … − Lω` needed only for stability; and `s = (1/K)Σₖ fₖ` (Carlsson, Skog & Jaldén, arXiv:2201.11983 §II-A, eqs. 2–7).

## Applicability to this robot

**DO — Heuristic Drift Elimination on the 45° lattice.** **[DERIVED]** Commanded turns are 45–360° multiples; snap the heading estimate to the nearest 45° when stationary and the residual is small. This is Borenstein's HDE with the corridor prior replaced by the robot's own quantisation. Gain: converts unbounded heading to bounded. Cost: ~50 lines, gated in the existing standstill state. Do **not** snap without a confidence gate and a hard ±X° clamp, or a mid-turn slip will lock in a wrong bin permanently.

**DO — fix the 7–9° coast overshoot first.** **[DERIVED]** At 285 °/s a 7–9° overshoot equals 25–32 ms of uncommanded rotation after motors cut. With `rate = 0.91*speed − 24.2` dps and deadband ≈27, the tractable fixes are (a) command a ramp-down to the deadband edge instead of a step to zero, (b) brief counter-drive pulse, (c) predictive early cut using the known 1260 ms/360° profile. Gain: 150–180× the entire remaining drift budget. Cost: control-loop work, no new hardware. **[DERIVED]** Continuing to tune estimation while overshoot dominates is optimising the 0.3% term.

**DO — gyro scale-factor and temperature calibration.** **[SOURCED]** 0.1% scale error is visible at the 9.81→9.82 m/s² level (VectorNav §3.1); the array result needed rotary-table calibration for 3.4× (PMC11598325). **[DERIVED]** For a 1260 ms 360° turn, 1% scale error = 3.6°; 0.53 dps residual bias × 1.26 s = 0.67°; ARW at 0.3 °/√h ≈ 0.006°. Scale factor dominates noise by ~600×. The QMI8658C die temperature at regs `0x33/0x34` is already exposed — index the scale correction on it. The gravity-projected yaw rate already recovered the 3.48°/360 single-axis deficit, which is the same class of fix.

**DO — gyrodometry for the slipping servos.** **[SOURCED]** Use odometry for distance most of the time; substitute the gyro during brief disagreements (W2111152968). **[SOURCED]** Or invert it as iRobot does: servo command = intent, gyro = reality, difference = slip vector (EP2858794B2). Gain: a slip detector that also gates the ZUPT so false stationarity never corrupts bias. Cost: low.

**DO NOT — add 2–4 more QMI8658C parts.** **[DERIVED]** √N gives 1.4–2.0× on white noise, the term that is already ~600× smaller than scale-factor error and ~150× smaller than overshoot. Common-mode temperature and per-part scale error are untouched. I²C address options at `0x6B` are limited, forcing a board respin. This is the worst return in the table. The only defensible version is 3+ *geometrically dispersed* units to exploit arXiv:2201.11983 §II-A — a research project, not a fix.

**DO NOT — add a magnetometer as the heading reference.** **[SOURCED]** PX4 offers `EKF2_MAG_TYPE=5` (None) specifically for when "the data can never be trusted (e.g. high current close to the sensor, external anomalies)" and `=6` (Init only) for "high current after the vehicle is armed". **[INFERRED]** A desk with steel, monitors, and two MG90D motors inside a 1.69" chassis is the exact failure case those options exist for. Low BOM cost, high silent-failure risk.

**DO NOT — expect anything from Mahony/Madgwick, and do not expect an EKF to invent heading.** **[SOURCED]** Complementary filters explicitly high-pass the gyro and low-pass "the combination of the accelerometer **and the magnetometer**" (Kok et al. §4.4). With no magnetometer the low-pass branch supplies roll and pitch only; yaw is passed through unmodified. **[DERIVED]** Confirms the prior finding: exactly zero yaw benefit on a 6-axis part.

## Ranked recommendations

| Technique | Expected gain | Implementation cost | Risk | Verdict |
|---|---|---|---|---|
| Close coast overshoot (ramp-down / counter-pulse / predictive cut) | 7–9° → <1°; ~150–180× the drift budget | Medium (control loop, servo model already measured) | Low — reversible, testable per turn | **Do first** |
| HDE snap to 45° lattice at standstill | Unbounded → bounded heading | Low (~50 lines) | Medium — must gate on confidence + clamp | **Do second** |
| Gyro scale-factor + temperature calibration (regs `0x33/0x34`) | ~3.6° per 360° at 1% error → sub-degree | Low–medium (fixture + LUT) | Low | **Do** |
| Gyrodometry / slip detection (servo command vs gyro) | Distance accuracy + ZUPT gating | Low | Low | **Do** |
| Non-holonomic constraint `v_body,y = 0` | Weak heading observability while translating | Medium (filter work) | Medium — worthless during slip; needs slip detector | Conditional |
| Wall/dock geometric datum (periodic re-anchor) | True absolute bound | Medium (behaviour + mechanical) | Low technical, high UX | Conditional |
| Optical-flow ground sensor (1×, or 2× for yaw rate) | Slip-free translation; independent yaw-rate channel | Medium (SPI + BOM) | Medium — fails on gloss/glass desks | Consider later |
| 2–4× QMI8658C array | 1.4–2.0× on the smallest error term | Medium (board respin, I²C addressing) | Medium | **No** |
| Magnetometer | Restores absolute heading in clean fields | Low BOM | **High** — ferromagnetic desk + motor currents | **No** |
| Mahony / Madgwick for yaw | Zero | Low | — | **No** |

## Could not verify

- **Foxlin's "0.3% of distance travelled".** The 2005 abstract says "meter-level error" only; IEEE full text paywalled, `openshoe.org` DNS-dead, DuckDuckGo/Brave/Mojeek/Bing all blocked, rate-limited, or returning unrelated results. Do not quote 0.3% as sourced.
- **Xsens MVN white paper full text.** 404 at both `movella.com/hubfs/…/MVN_white_paper.pdf` and `xsens.com/hubfs/…`; `xsens.com/white-papers` 404; the sole OpenAlex-indexed mirror (`edge.rit.edu`) DNS-failed. Therefore XKF/XME engine naming, magnetic field mapping, "external contact points"/foot-contact detection, and any Xsens-published drift figure are unverified. Perception Neuron and Rokoko internals: entirely unverified.
- **All grade/cost numbers in §4.** `sensonor.com/…/stim300` redirected to a generic Safran page; Teledyne, Sonardyne, Advanced Navigation URLs 404; El-Sheimy & Youssef (*Satellite Navigation* 1:2, 2020), which carries the standard table, returned HTTP 406 from both SpringerOpen and link.springer.com; Woodman UCAM-CL-TR-696 fetched as unparseable binary PDF.
- **Roomba-specific hardware.** Which retail models carry a gyro, which part, and achieved heading performance — no teardown or vendor doc retrievable. Patent evidence (§5) is solid; product evidence is absent.
- **DVL-aided AUV drift as % of distance travelled.** No primary source retrieved.
- **Whether OpenSense's −0.14–0.17 deg/min was magnetometer-aided.** Methods section not retrieved; treat as "IMU + constrained model", not "magnetometer-free".
- **Skog et al. (2010) ZV-detector evaluation and Nilsson et al. OpenShoe** primary texts — identified, not retrieved (Semantic Scholar HTTP 429; arXiv API timeout/429 twice).
- **QMI8658C datasheet numbers** (ARW, in-run bias stability, scale-factor tolerance, tempco). File exists at `datasheets/QMI8658C_IMU.pdf`; my tooling cannot extract PDF text. **This is the highest-value unverified item for this robot** — the scale-factor tolerance directly sets the calibration payoff.

## Adjacent, not pursued

- Visual-inertial odometry (VINS-Mono, W2745859992, 4488 citations) — excluded by the no-camera constraint; the field default for GPS-denied 3D.
- Magnetic-field SLAM (Sensors 22(8):2833, W4224303268) — mapping indoor magnetic anomalies; irrelevant without a magnetometer.
- Learned inertial odometry (IONet, RoNIN, CNN stride regression) — cited throughout arXiv:1807.05275 §II.
- Cooperative localisation via inter-agent ranging (EURASIP JASP 2013:164, W2159502462) — needs a second robot.
- Terrain-aided navigation and synthetic-aperture-sonar micronavigation (IEEE JOE 2021, W3199832698) — underwater map-matching.
- Robust filtering for slip: chi-square fault detection + IGG re-weighting gave ~82.4% positioning-error reduction over SINS/OD baseline (Sensors 26(8):2457, PMC13120419).
- UWB/acoustic beacons, ActionSLAM landmark re-observation (W2022304611) — external references by definition.

## References

### Load-bearing

1. PX4 Autopilot, *Using PX4's Navigation Filter (EKF2)*, `PX4/PX4-Autopilot:docs/en/advanced_config/tuning_the_ecl_ekf.md` (main) — Magnetometer (`EKF2_MAG_TYPE` 0/1/5/6); Yaw From GPS Velocity (GSF); Ground Position Lock (`EKF2_POS_LOCK`); Running Multiple EKF Instances.
2. ArduPilot, `common-apm-navigation-extended-kalman-filter-overview.html`; `common-ekf-sources.html` (`EK3_SRC1_YAW` 1/2/3/6/8); `common-compassless.html`.
3. Wagstaff, Peretroukhin & Kelly, *Robust Data-Driven Zero-Velocity Detection for Foot-Mounted Inertial Navigation*, arXiv:1910.00529 §II.
4. Wagstaff & Kelly, *LSTM-Based Zero-Velocity Detection*, arXiv:1807.05275 §II.
5. Borenstein & Ojeda, *Heuristic Drift Elimination for Personnel Tracking Systems*, J. Navigation 2010, DOI 10.1017/S0373463310000184 (OpenAlex W2095620283).
6. Borenstein & Ojeda, *Heuristic Reduction of Gyro Drift*, J. Navigation 2008, DOI 10.1017/S0373463308005043 (W1984754470).
7. Foxlin, *Pedestrian Tracking with Shoe-Mounted Inertial Sensors*, IEEE CG&A 25(6), 2005, DOI 10.1109/MCG.2005.140 (W2056427752).
8. Al Borno et al., *OpenSense*, J. NeuroEng. Rehabil. 19:22, 2022, PMC8859896.
9. Snyder, Mannby & Martelli, Sensors 26(9):2639, 2026, PMC13165935.
10. iRobot Corp. (Goel), *Carpet drift estimation and compensation using two sets of sensors*, EP2858794B2, granted 2024-02-28.
11. Borenstein & Feng, *Gyrodometry*, ICRA 1996, DOI 10.1109/ROBOT.1996.503813 (W2111152968).
12. Xuan, Zhu, Peng, Sun & Dong, *A Review on the IMU Array of MEMS*, Sensors 24(22):7140, 2024, PMC11598325 §§2–3.
13. Carlsson, Skog & Jaldén, *Inertial Navigation Using an Inertial Sensor Array*, arXiv:2201.11983 §II-A.
14. VectorNav, *Inertial Navigation Primer* §3.1 IMU Specifications.
15. Local hardware: `HARDWARE.md:98-110` (accelerometer finds yaw axis from gravity at rest; full-turn gyro calibration; **no magnetometer**), `:295-306` (6-axis, verified register map), `:316-318` (gyro scale 32768/FS_dps), `:62` (I²C `0x6B`), `:100` (die temp `0x33/0x34`).

### Background

16. Kok, Hol & Schön, *Using Inertial Sensors for Position and Orientation Estimation*, arXiv:1704.06053 §4.4.
17. Paull, Saeedi, Seto & Li, *AUV Navigation and Localization: A Review*, IEEE JOE 39(1), 2014 (W1969142081).
18. Khan, Clark, Woodward & Lindeman, Sensors 20(18):5031, 2020, PMC7570486.
19. Tjhai & O'Keefe, Sensors 19(14):3140, 2019, PMC6679558.
20. Liu et al., Sensors 26(8):2457, 2026, PMC13120419.
21. Fischer, Sukumar & Hazas, *Tutorial: Implementing a Pedestrian Tracker Using Inertial Sensors*, IEEE Pervasive Computing 11(2), 2012 (W2088803835).
22. Roetenberg, Luinge & Slycke, *Xsens MVN*, 2009 (W1215311137) — cited, not retrieved.
23. iRobot Corp. (Schnittman), *Autonomous mobile robot*, CN105899112B — snippet only.
24. Prior art within EP2858794B2: WO 2011/052827 A1; US 2011/0125323 A1; JP 2010-167055 A.
