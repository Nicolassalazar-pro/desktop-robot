# QMI8658C On-Chip Capabilities Not Currently Exploited — Waveshare ESP32-S3 Desk Robot

## Verdict

- **[DERIVED]** Given the measured error hierarchy — coast overshoot 7–9°, single-axis projection error 3.48°/360° (now fixed), heading drift ±0.05°/48 s — **no IMU register change addresses the dominant error**. Coast overshoot is a motion-control problem, not a sensing problem. Every technique below is worth ≤0.05° except one.
- **[SOURCED]** **COD (CTRL9 = 0xA2) calibrates gyro sensitivity/scale factor, not bias.** Four of eight `COD_STATUS (0x46)` bits read "COD failed for checking low/high **sensitivity limit**" and the readback is `uint16_t gX/gY/gZ_gain` (`lewisxhe/SensorLib@master:src/SensorQMI8658.hpp`, `calibration()`). This is the only unexploited feature that touches an error class the host cannot fix: a 1–3% scale tolerance is 3.6–10.8° per 360°, and ZUPT is structurally blind to it because at zero rate a scale error produces zero output.
- **[SOURCED]** **Gyro LPF (CTRL5 = 0x06) is currently OFF.** Four bandwidths at 2.66/3.63/5.39/13.37% of ODR = 26.6/36.3/53.9/133.7 Hz at gODR 1000 Hz (`enum LpfMode`). One register write; MG90D servo vibration is in-band.
- **[DERIVED]** **AttitudeEngine is not worth adopting.** Its only accuracy contribution is coning compensation, which is ≈0 for a 285 °/s single-axis yaw sampled at 1 kHz. Its robustness contribution (immunity to a starved sampler) is delivered by the 128-sample FIFO with less code and without adopting an unverified fixed-point quaternion format.
- **[DERIVED]** **Do not lower gyro full scale from ±2048 dps.** Quantization over a 1260 ms turn is 0.041° worst-case / 0.00065° RMS. Dropping to ±512 dps saves 0.031° and leaves only 1.8× headroom over the 285 °/s peak; clipping under-reads *silently*.
- **[INFERRED]** **Highest open risk: COD's Z-axis coverage.** `COD_STATUS` defines sensitivity-limit bits for gyro **X and Y only** — none for Z, the robot's turn axis.

## Findings

### Register map (addresses [SOURCED] `lewisxhe/SensorLib@v0.2.4:src/REG/QMI8658Constants.h`, cross-checked `ALICHOUCHENE/Qmi8658c@main:src/Qmi8658c.h`)

`CTRL1 0x02` (b6 SPI_AI, b5 BE, b4 INT2_EN, b3 INT1_EN, b2 FIFO_INT_SEL, b1 power-down) · `CTRL2 0x03` (b7 aST, b6:4 aFS, b3:0 aODR) · `CTRL3 0x04` (b7 gST, b6:4 gFS, b3:0 gODR) · `CTRL4 0x05` (external mag — unpopulated) · **`CTRL5 0x06`** (b6:5 gLPF_MODE, b4 gLPF_EN, b2:1 aLPF_MODE, b0 aLPF_EN) · **`CTRL6 0x07`** (b7 sMoD, b2:0 sODR) · `CTRL7 0x08` (b7 syncSample, b5 DRDY_dis, b4 gSN, b3 sEN, b2 mEN, b1 gEN, b0 aEN) · `CTRL8 0x09` (b4 Ped, b3 SigMotion, b2 NoMotion, b1 AnyMotion, b0 Tap) · `CTRL9 0x0A` · `CAL1_L…CAL4_H 0x0B–0x12` · **FIFO `0x13` WTM_TH, `0x14` CTRL, `0x15` COUNT, `0x16` STATUS, `0x17` DATA** · `STATUSINT 0x2D` (b7 CmdDone) · `STATUS0 0x2E` · `STATUS1 0x2F` · `TIMESTAMP 0x30–0x32` · `TEMP 0x33/0x34` · accel `0x35–0x3A` · gyro `0x3B–0x40` · `COD_STATUS 0x46` · `dQW/X/Y/Z 0x49–0x50` · `dVX/Y/Z 0x51–0x56` · `TAP_STATUS 0x59` · `STEP_CNT 0x5A–0x5C` · `RESET 0x60` (write 0xB0, poll `0x4D` for 0x80).

**[SOURCED]** ODR encoding (QMI8658**C**): `0011 = 1000 Hz`, `0100 = 500 Hz`, series 8000/4000/2000/1000/500/250/125/62.5/31.25 Hz. Gyro FS `111 = ±2048 dps @ 16 LSB/dps`; `101 = ±512 dps @ 64 LSB/dps` (`ALICHOUCHENE/Qmi8658c@main:src/Qmi8658c.h`, `GYRO_SCALE_SENSITIVITY_2048DPS (1<<4)`). **Footgun [SOURCED]:** `lewisxhe/SensorLib@master` documents a different gyro ODR series (7174.4/3587.2/1793.6/896.8 Hz) and omits ±2048 dps entirely — it targets the QMI8658**A**. Do not copy its constants.

### 1. AttitudeEngine (CTRL6, sEN, dQ/dV, sMoD)

**[SOURCED]** `CTRL6 0x07` = "AttitudeEngine Settings: Output Data Rate, Motion on Demand"; `CTRL7.sEN` (b3) = "Enable AttitudeEngine orientation and velocity increment computation"; outputs `dQW 0x49/0x4A, dQX 0x4B/0x4C, dQY 0x4D/0x4E, dQZ 0x4F/0x50` and `dVX 0x51/0x52, dVY 0x53/0x54, dVZ 0x55/0x56`.

**[INFERRED]** Standard sensor-data-integration (SDI) block: integrates internally at the full gyro rate, emits a **delta quaternion** (incremental rotation, host composes `q ⊗ dQ`) and **delta velocity** (integrated specific force) per AE period. Carries coning/sculling correction, so host polling rate decouples from motion bandwidth. `sMoD` (b7) accumulates continuously and returns the increment only on host request via a CTRL9 opcode, stretching the window to match a jittery loop.

**[INFERRED]** `wx_clip/wy_clip/wz_clip` live at `0x57/0x58` — the only gap between `dVZ_H 0x56` and `TAP_STATUS 0x59`. They flag rate saturation *inside* the accumulation window; necessary precisely because AE hides individual samples, so a saturated interval would otherwise read as a valid but small rotation. Triggers when instantaneous |ω| > gFS: at ±2048 dps only a violent hand flick; at ±512 dps a brisk manual rotation.

**[DERIVED] Verdict — no.** Error budget for a 1260 ms, 285 °/s single-axis turn: scale factor (1%) on 360° = **3.6°**; coast overshoot = **7–9°**; single-axis projection = **3.48°** (fixed); residual bias after ZUPT (0.05°/48 s → ~0.001 °/s) = **0.0013°**; quantization = **0.041°**; **coning ≈ 0**. Coning matters when the rotation *axis* slews fast relative to the sample interval; a differential-drive robot pivoting about a near-fixed vertical axis at 285 °/s sampled every 1 ms has none. AE fixes the one term already at zero. Extracting a turn angle from dQ requires quaternion composition plus axis-angle extraction — *more* host code than `Σ ω·Δt`, on a fixed-point format whose scaling is unverified.

### 2. FIFO

**[SOURCED]** `FIFO_CTRL 0x14`: bits [1:0] mode, [3:2] size (`_fifo_mode = (samples << 2) | mode`). Modes: **BYPASS (0)** disables; **FIFO (1)** — "once FIFO is full, the data filling will stop and new data will be discarded"; **STREAM (2)** — "the oldest data will be discarded". Depths 16/32/64/128 samples.

**[SOURCED]** Watermark unit, verbatim: *"The unit is sample, which means 6 bytes if one of accelerometer and gyroscope is enabled, and 12 bytes if both are enabled."* `getFifoNeedBytes()` = `sam[samples] * 6 * sensors` → **128 × 6 × 2 = 1536 bytes**. Frames interleave accel-then-gyro (`readFromFifo()` decodes `i % 2 == 0` → accel).

**[SOURCED]** Routing: `CTRL1 b2` set→INT1, cleared→INT2; `CTRL1 b3/b4` = INT1/INT2 enable. **[DERIVED]** INT2 is the wired pin on this build, INT1 is not connected → `CTRL1 = 0x70` (b6 SPI_AI + b5 BE + b4 INT2_EN, b2 clear), up from the current `0x60`.

**[SOURCED]** Sequence: disable sensors → `CTRL9 = 0x04` (RST_FIFO) → write `FIFO_CTRL`, `FIFO_WTM_TH` → re-enable → on IRQ `CTRL9 = 0x05` (REQ_FIFO) → burst-read `0x17`.

**[DERIVED]** Coverage: 128 samples ÷ 1000 Hz = **128 ms**; at 500 Hz = **256 ms**; at 250 Hz = **512 ms**. A 20 Hz UI loop (50 ms) fits inside 128 ms; a 200 ms display-flush or NVS-write hitch does not. Use **FIFO mode, not stream** — overflow then discards *new* data and pins `FIFO_COUNT`, making loss detectable; stream discards *old* data, silently deleting the start of a turn.

### 3. Low-pass filters (CTRL5 = 0x06)

**[SOURCED]** Bit layout from `configAccelerometer`/`configGyroscope`: b0 `aLPF_EN`, b2:1 `aLPF_MODE` (mask `0xF9`), b4 `gLPF_EN`, b6:5 `gLPF_MODE` (mask `0x9F`).

**[SOURCED]** `enum LpfMode` bandwidths as % of ODR: mode 0 = **2.66%**, mode 1 = **3.63%**, mode 2 = **5.39%**, mode 3 = **13.37%**. **[DERIVED]** At gODR 1000 Hz → **26.6 / 36.3 / 53.9 / 133.7 Hz**; at aODR 500 Hz → 13.3 / 18.2 / 27.0 / 66.9 Hz.

**[DERIVED]** Reset default is CTRL5 = 0x00 (both filters off), inferred from the driver's enable/disable symmetry; the firmware never writes CTRL5, so **this robot integrates the full 500 Hz Nyquist band**. Enabling mode 0 cuts integration bandwidth 500 → 26.6 Hz, a **√(500/26.6) ≈ 4.3×** RMS noise reduction.

**[DERIVED]** A unity-DC-gain filter satisfies ∫₀ᵀ(h∗ω)dt = ∫₀ᵀω dt whenever the turn starts and ends at zero rate and T extends past the settling tail (≈1/26.6 Hz ≈ 38 ms). Group delay shifts *when* rate appears, not *how much angle* accumulates — **zero cost in integrated angle**. Cost lands only on instantaneous-rate edge detection: ~38 ms lag. Given coast overshoot is 7–9°, any turn-stop logic keyed on instantaneous rate is already the weak link; a 38 ms lag at 285 °/s is 10.8° of additional latency-induced overshoot if the LPF output drives the stop decision. **Use filtered rate for integration, unfiltered rate for the stop trigger.**

### 4. On-chip calibration (CTRL9 = 0x0A)

**[SOURCED]** Protocol: write opcode to `CTRL9`, poll `STATUSINT (0x2D) b7 = CmdDone`, write `0x00` (ACK). Parameters in `CAL1_L…CAL4_H (0x0B–0x12)`.

**[SOURCED]** `enum CommandTable`: `0x00` ACK, `0x04` RST_FIFO, `0x05` REQ_FIFO, `0x08` WRITE_WOM_SETTING, `0x09` ACCEL_HOST_DELTA_OFFSET, `0x0A` GYRO_HOST_DELTA_OFFSET, `0x0C` CONFIGURE_TAP, `0x0D` CONFIGURE_PEDOMETER, `0x0E` CONFIGURE_MOTION, `0x0F` RESET_PEDOMETER, `0x10` COPY_USID, `0x11` SET_RPU, `0x12` AHB_CLOCK_GATING, **`0xA2` ON_DEMAND_CALIBRATION**, **`0xAA` APPLY_GYRO_GAINS**.

#### What COD corrects — scale, not bias

**[SOURCED]** Four independent strands, all from `calibration()`/`writeCalibration()`:

1. `COD_STATUS (0x46)` bits 7/6/5/4 = "COD failed for checking **low/high sensitivity limit** of X/Y axis of gyroscope". Four of eight bits are sensitivity checks; a bias routine has no reason to report sensitivity limits.
2. Readback is `uint16_t gX_gain, gY_gain, gZ_gain` from `0x51–0x56` — **unsigned**, consistent with a multiplicative gain, inconsistent with a signed offset.
3. `CTRL_CMD_APPLY_GYRO_GAINS = 0xAA` exists as a separate gain-write path (`writeCalibration(uint16_t, uint16_t, uint16_t)` → `CAL1_L/H`, `CAL2_L/H`, `CAL3_L/H`).
4. Offsets have their **own** command with **signed** arguments: `setGyroOffset(int16_t, int16_t, int16_t)` → `CTRL9 = 0x0A`.

**[SOURCED]** Remaining `COD_STATUS` bits: b3 "Accelerometer checked failed (significant vibration happened during COD)", b2 "Gyroscope startup failure", b1 "COD was called while gyroscope was enabled", b0 "COD failed; no COD correction applied".

**[SOURCED]** Procedure, verbatim: `CTRL7 = 0x00` → issue `0xA2` → *"wait about 1.5 seconds"* (driver delays 1600 ms) → read `0x46`. *"During the process, it is recommended to place the device in quiet, otherwise, the COD might fail and report error."*

**Confidence table:**

| Claim | Confidence | Basis |
|---|---|---|
| COD adjusts gyro **sensitivity/scale** | **High** | 4 sensitivity-named status bits + `uint16_t` gain readback + dedicated 0xAA apply path |
| COD does **not** adjust bias | **Medium** | Separate signed-offset command 0x0A exists; no text explicitly excludes a bias component |
| COD covers **Z axis** | **Medium-low** | `writeCalibration()` writes `CAL3_L/H` for gZ_gain, but `COD_STATUS` defines sensitivity bits for **X and Y only** |
| COD calibrates the accelerometer | **Low — probably not** | b3 says accel is *"checked"* as a vibration detector; no accel gain read back |

**[INFERRED]** Likely mechanism: COD applies the same electrostatic actuation used by `gST` self-test, measures response, computes gain = expected/measured. That would explain the stillness requirement without implying a bias measurement.

**[INFERRED]** Gains are not persistent across power cycle — the existence of `writeCalibration()`/`0xAA` implies the host must store them (NVS) and re-apply at boot. `setGyroOffset()`'s documented caveat is explicit for offsets: *"this offset change is lost when the sensor is power cycled, or the system is reset."*

**[DERIVED]** `GYRO_HOST_DELTA_OFFSET (0x0A)` is worthless here: pushing the host's 0.53 dps ZUPT estimate on-chip is arithmetically identical to subtracting it on the host, and makes raw data less inspectable. Keep bias on the host.

### 5. Self-test (aST / gST)

**[SOURCED]** Accel: `CTRL7 = 0x00` → set `CTRL2.aST (b7)` with a valid aODR → wait INT2 high or `STATUSINT.b0 = 1` → clear b7 → wait `STATUSINT.b0 = 0` → read `0x51–0x56`, *"signed U5.11, resolution 0.5 mg (1/2^11 g)"*. Pass: *"absolute results of all three axes higher than **200 mg**"*.

**[SOURCED]** Gyro: same flow via `CTRL3.gST (b7)`; results *"signed U12.4, resolution 62.5 mdps (1/2^4 dps)"*. Pass: *"higher than **300 dps**"* on all three axes.

**[DERIVED]** Verifies MEMS mechanical integrity and front-end response — catches a cracked die, broken bond wire, or cold solder joint. Verifies nothing about accuracy, bias, or scale. On this robot the existing `WHO_AM_I = 0x05` probe plus a check that the resting gravity vector magnitude ‖(1.01, 0.10, −0.10)‖ = **1.015 g** is within tolerance already catches every failure self-test would catch, at zero cost. **[SOURCED]** Additionally the reference driver's gyro decode `(rawBuffer[0] << 12) | (rawBuffer[1] >> 4)` is inconsistent with its own accel path and appears buggy — write the U12.4 decode yourself.

### 6. Temperature (0x33/0x34)

**[SOURCED]** `°C = TEMP_H + TEMP_L/256.0` (`getTemperature_C()`: `(float)buffer[1] + ((float)buffer[0]/256.0)`); resolution **1/256 °C ≈ 0.0039 °C**, corroborated by `TEMPERATURE_SENSOR_RESOLUTION (1 << 8)`. **[SOURCED]** Bug in that reference: `(float)buffer[1]` treats a signed high byte as unsigned — breaks below 0 °C; cast to `int8_t`.

**[DERIVED]** Die temperature, reading MEMS junction plus conducted heat from the 240 MHz ESP32-S3 and the MG90D drivers. Absolute accuracy is irrelevant for bias compensation — only repeatability matters, and 1/256 °C resolution is ample; regress bias against the raw int16 and skip the °C conversion.

**[DERIVED]** **Not worth implementing.** The robot re-estimates bias at standstill before *every* turn and holds ±0.05° over 48 s across 29 zero-updates. Temperature compensation predicts a quantity that is already measured directly, moments before use. It earns its keep only if a turn is ever commanded without a preceding still period.

### 7. Full-scale and ODR tradeoffs

**[DERIVED]** Quantization error over a 1260 ms turn at 1 kHz (N = 1260, Δt = 1 ms), σ = q/√12 per sample, accumulating as σ·Δt·√N:

| gFS | q (dps/LSB) | RMS angle error | Worst case (q/2 × 1.26 s) |
|---|---|---|---|
| **±2048 (current)** | 0.0625 | **0.00064°** | **0.039°** |
| ±1024 | 0.03125 | 0.00032° | 0.020° |
| ±512 | 0.015625 | 0.00016° | 0.010° |

**[DERIVED]** Maximum possible gain from ±2048 → ±512 is **0.029°** worst-case. Against 7–9° coast overshoot that is a 0.4% effect on the error budget — unmeasurable. Meanwhile quantization RMS at ±2048 (0.018 °/s) sits ~12× below the sensor's own broadband noise (≈0.22 °/s RMS assuming 0.01 °/s/√Hz over 500 Hz), contributing **0.6% of variance**; it is not the floor.

**[DERIVED]** Downside is asymmetric and severe: peak turn rate is 285 °/s, so ±512 dps leaves **1.8×** headroom and ±1024 dps **3.6×**. A hand flick is 800–1500 °/s. Clipping under-reads silently and looks like a valid small rotation. **Keep ±2048 dps.**

**[DERIVED]** Consider **gODR 500 Hz** (`CTRL3 = 0x74`) during turn measurement: doubles FIFO coverage to 256 ms, halves transfer load, still ~100× oversampled for a few-Hz signal, and LPF mode 0 then yields 13.3 Hz — still 4× the signal bandwidth. **[SOURCED]** Caveat: *"In 6DOF mode (accelerometer and gyroscope are both enabled), the output data rate is derived from the nature frequency of gyroscope"* — the robot's aODR 500 / gODR 1000 split may not deliver what it appears to; verify empirically.

### 8. Other unexploited features

**[SOURCED]** All from `lewisxhe/SensorLib@master:src/SensorQMI8658.hpp`.

| Feature | Access | Verdict for this robot |
|---|---|---|
| **24-bit timestamp** | `0x30–0x32`, `getTimestamp()` with 0x1000000 wrap | Detects dropped/duplicated samples definitively. **[DERIVED]** Do *not* use as absolute Δt — it shares the ODR oscillator, so it cannot correct oscillator-tolerance scale error; keep host `micros()` (crystal) for absolute timing. |
| **Tap (single/double)** | `CTRL8 b0`; `CTRL9 = 0x0C`; `TAP_STATUS 0x59` (b7 polarity, b5:4 axis, b1:0 type) | **Useful, non-navigation.** Non-SyncSample only. Thresholds need tuning against MG90D vibration or false taps follow. |
| **Any-Motion** | `CTRL8 b1`; `CTRL9 = 0x0E`; `STATUS1 b5` | Per-axis slope thresholds, AND/OR combining (`ANY_MOTION_LOGIC_AND`). "Picked up / bumped". |
| **No-Motion** | `CTRL8 b2`; `STATUS1 b6` | **[DERIVED]** Best use: gate the COD run and validate the ZUPT window. |
| **Significant-Motion** | `CTRL8 b3`; `STATUS1 b7` | Requires a second any-motion event inside a confirm window — rejects single bumps. |
| **Pedometer** | `CTRL8 b4`; `0x0D`/`0x0F`; `STEP_CNT 0x5A–0x5C` | **Useless.** Tuned for human gait. |
| **Wake-on-Motion** | `CTRL9 = 0x08`, threshold (mg) in `CAL1_L`, pin/blanking in `CAL1_H` | **[SOURCED]** *"Configuring Wom will reset the sensor... and there will be no data output."* Deep-sleep entry only; destructive to normal operation. |
| **Sync-sample + data locking** | `CTRL7 b7`; `CAL1_L = 0x01` + `CTRL9 = 0x12`; `STATUSINT b1/b0` | Coherent accel+gyro reads, no tearing. Mutually exclusive with tap/motion/pedometer. |
| **Gyro snooze `gSN`** | `CTRL7 b4` | **[INFERRED]** Keeps drive loop running, suppresses output — faster wake than cold gyro start. Only relevant if the gyro is ever power-gated between turns. |
| **USID** | `CTRL9 = 0x10` → 6 bytes | Key per-unit COD gains in NVS to the actual die. |
| **Soft reset** | write `0xB0` to `0x60`, poll `0x4D` for `0x80`, ≤15 ms | **[SOURCED]** Reset clears `CTRL1`; the driver re-asserts `CTRL1 b6` (SPI_AI) immediately after. Without it every burst read silently repeats one byte. |

## Applicability to this robot

**Do — enable gyro LPF mode 0 (`CTRL5 = 0x11`).** Gain: 4.3× RMS noise reduction on integrated angle; provably zero angle bias. Cost: one register write in `begin()`. **Constraint [DERIVED]:** feed the *filtered* stream to the integrator and the *unfiltered* stream to any stop-trigger logic — 38 ms of group delay at 285 °/s is 10.8° of extra coast, which would make the dominant error worse.

**Do — run COD once at commissioning, store gains in NVS keyed by USID, re-apply at boot with `0xAA`.** Gain: potentially 3.6–10.8° per 360°, the only error class the host cannot reach. Cost: 1.6 s blocking routine, `COD_STATUS` decode, NVS storage, boot re-apply path. **Gate on No-Motion and servos-idle** (b3 fails on vibration, b1 fails if gyro enabled). **Verify Z-axis coverage against the local PDF before trusting it** — `COD_STATUS` reports X and Y sensitivity only, and Z is the turn axis.

**Do — FIFO in FIFO mode (not stream), 128 samples, watermark on INT2 (`CTRL1 = 0x70`).** Gain: bounded immunity to a starved sampler, 128 ms at 1 kHz or 256 ms at 500 Hz. Cost: ISR + ring buffer + burst read, ~1.5 KB (trivial against 8 MB PSRAM). Note this is **insurance, not accuracy** — with the loop healthy it changes nothing.

**Do — re-assert `CTRL1` after any soft reset.** Latent-bug fix, one line.

**Do NOT — adopt AttitudeEngine.** Fixes coning, which is ≈0 here. Costs quaternion composition plus axis-angle extraction on an unverified fixed-point format, replacing a working `Σ ω·Δt`.

**Do NOT — lower gyro full scale.** Buys ≤0.029°; costs headroom against 800–1500 °/s hand flicks that clip silently.

**Do NOT — use `GYRO_HOST_DELTA_OFFSET (0x0A)`.** Identical arithmetic to the existing host-side subtraction of 0.53 dps, with less inspectable raw data.

**Do NOT — implement temperature compensation.** Predicts a quantity already measured directly before every turn; ±0.05°/48 s leaves nothing to recover.

**Do NOT — put self-test in the boot path.** `WHO_AM_I` plus ‖gravity‖ = 1.015 g catches the same failures for free.

## Ranked recommendations

| Technique | Expected gain | Implementation cost | Risk | Verdict |
|---|---|---|---|---|
| **COD `0xA2` + NVS + `0xAA`** | **3.6–10.8°/360°** if scale error is real and Z is covered; unquantified residual | Medium — 1.6 s routine, status decode, NVS, boot re-apply | **Medium** — Z coverage unconfirmed; residual accuracy unspecified; must be still | **Do — highest value, verify Z first** |
| **Gyro LPF mode 0 (`CTRL5 = 0x11`)** | 4.3× RMS noise reduction; 0° bias | Trivial — one write | Very low — 38 ms group delay, keep off the stop trigger | **Do first** |
| **Re-assert `CTRL1` post-reset** | Prevents silent burst-read corruption | Trivial | Very low | **Do** |
| **FIFO mode + INT2 watermark** | 128 ms (or 256 ms @ 500 Hz) stall immunity | Medium — ISR, ring buffer, overflow detect | Low-med — `FIFO_STATUS` bits unverified | **Do — insurance** |
| **gODR 500 Hz during turns** | 2× FIFO coverage, halved load | Trivial | Low — verify 6DOF ODR slaving | **Do with FIFO** |
| **24-bit timestamp** | Definitive dropped-sample detection | Low — 3 bytes + wrap | Low — not an absolute time reference | **Optional** |
| **Tap / Any-Motion / No-Motion** | Product features; No-Motion gates COD | Medium — CTRL9 configure sequences | Low-med — false triggers from servo vibration | **Optional** |
| **Temperature compensation** | ≈0 given per-turn ZUPT | Medium — per-unit characterisation | Low | **Skip** |
| **Self-test at boot** | ≈0 beyond existing checks | Medium — 100s of ms, state restore | Medium — reference gyro decode is buggy | **Skip (diagnostics menu only)** |
| **Lower gFS to ±512 dps** | ≤0.029° | Trivial | **High** — 1.8× headroom, silent clipping | **Do not** |
| **AttitudeEngine** | 0° (coning already ≈0) | High — quaternion math, unverified format | **High** — unverified fixed-point scaling | **Do not** |
| **`GYRO_HOST_DELTA_OFFSET`** | 0° (identical arithmetic) | Low | Low | **Do not** |

## Could not verify

- **The local Rev 0.6 PDF could not be read in my environment** (no code execution; Flate-compressed streams returned 0 body-text matches; `web_fetch` returns raw PDF bytes). All datasheet facts above are corroborated from two independent driver implementations and validated against the robot's confirmed register values. Your independent extraction confirmed agreement on gFS 111 = ±2048 dps, gODR 0011 = 1000 Hz, CTRL7 b0/b1/b2/b4, CTRL1 b6, accel 0x35, gyro 0x3B, WHO_AM_I 0x05.
- **COD Z-axis sensitivity coverage** — `COD_STATUS` defines X and Y bits only, while `writeCalibration()` writes a Z gain to `CAL3_L/H`. Unresolved and load-bearing.
- **COD residual scale error after calibration** — no specification found. Sub-degree accuracy on 360° needs residual <0.15%.
- **Whether COD gains persist across power cycle** — inferred non-persistent from the existence of `0xAA`.
- **CTRL6 `sODR[2:0]` encoding** (I cite 1/2/4/8/16/32/64 Hz as the commonly published set, unconfirmed) and the **CTRL9 opcode for sMoD** — absent from `enum CommandTable`.
- **dQ/dV fixed-point formats and scale factors**; **`AE_REG1`/`AE_REG2` at 0x57/0x58** and their clip-bit positions — all inferred.
- **`FIFO_STATUS (0x16)` bit layout**; **CTRL5 reset default** (inferred 0x00); **gyro noise density** (assumed 0.01 °/s/√Hz — the variance argument holds even at 3× better); **temperature absolute accuracy**; **exact LPF −3 dB criterion**.

## Adjacent, not pursued

- Coast overshoot 7–9° is a control problem: the measured servo model `rate = 0.91·speed − 24.2 dps` with deadband ~27 permits a predictive ramp-down, which no IMU register can substitute for.
- Differential-drive slip is unobservable to a 6-axis IMU without wheel encoders; gyro yaw is the *reference*, not the victim.
- Board wiring: Waveshare's stock 1.28 wiki maps INT1→GPIO4, INT2→GPIO3; this build reports INT2 on a different pin — reconcile before wiring the FIFO IRQ.
- I²C address is `0x6B` (SA0 low) or `0x6A` (SA0 high).
- `0x4D` is dual-purpose: `DQY_L` and `RST_RESULT` — an aliasing hazard only if AttitudeEngine were enabled.
- `enum TapDetectionPriority` defines six axis-priority orderings for disambiguating simultaneous multi-axis tap peaks.
- Accel low-power ODRs (128/21/11/3 Hz, codes 12–15) are available only with the gyroscope disabled.

## References

**Load-bearing**

- `lewisxhe/SensorLib@v0.2.4:src/REG/QMI8658Constants.h` — authoritative address map 0x00–0x60 (retrieved via the `v0.2.4` tag; removed from `master`). Source of the FIFO block, status block, timestamp, temperature, `COD_STATUS 0x46`, `dQ 0x49–0x50`, `dV 0x51–0x56`, `TAP_STATUS 0x59`, `STEP_CNT`, `RESET 0x60`, and LPF masks `0xF9`/`0x9F`.
- `lewisxhe/SensorLib@master:src/SensorQMI8658.hpp` — `enum LpfMode` (bandwidth percentages); `enum FIFO_Mode`/`FIFO_Samples`/`configFIFO()`/`readFromFifo()`/`getFifoNeedBytes()`; `enum CommandTable` (CTRL9 opcodes); `calibration()` and the `COD_STATUS` bit decode; `writeCalibration()`; `selfTestAccel()`/`selfTestGyro()` with verbatim pass criteria; `getTemperature_C()`; `setGyroOffset()`/`setAccelOffset()`; `enableINT()`/`enableDataReadyINT()`; `getTimestamp()`; `reset()`. **Targets the QMI8658A — its ODR/range enums do not apply to the C part.**
- `ALICHOUCHENE/Qmi8658c@main:src/Qmi8658c.h` — QMI8658**C**-specific corroboration of `CTRL4 0x05`/`CTRL5 0x06`/`CTRL6 0x07`/`CTRL7 0x08`/`CTRL9 0x0A`, the C ODR series, `GYRO_SCALE_SENSITIVITY_2048DPS (1<<4)` and `512DPS (1<<6)`, and `TEMPERATURE_SENSOR_RESOLUTION (1<<8)`.
- Local `datasheets/QMI8658C_IMU.pdf` (Rev 0.6) — primary source; unreadable in my environment, independently extracted and confirmed consistent by the orchestrator.
- On-hardware baseline (measured tonight): bias 0.53 dps; ±0.05°/48 s over 29 ZUPTs; 360° = 1260 ms at 285 °/s; coast overshoot 7–9°; gravity (1.01, 0.10, −0.10) g; single-axis understatement 3.48°/360°; `rate = 0.91·speed − 24.2 dps`, deadband ~27.

**Background**

- `waveshare.com/wiki/ESP32-S3-Touch-LCD-1.28` — QMI8658 on I²C, INT1/INT2 mapping, spec table confirming gyro ranges ±16…±2048 °/s at 16-bit resolution.
- `api.github.com/search/repositories?q=qmi8658` — discovery only; 53 public repos, of which the two above were the substantive drivers.
