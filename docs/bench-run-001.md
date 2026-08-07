# Bench run 001 — 2026-08-06, desk, untethered

First supervised untethered measurement run. Robot on the desk, on its wheels,
unplugged, triggered by the user pressing the TEST button.

This is the raw record. It is kept in the repo because the on-device copy was
destroyed when `kMaxEntries` changed from 96 to 128 — the record size changed,
so the loader correctly refused to reinterpret the old bytes rather than
misreading them. That is the storage behaving as designed, but it means a run
costs a supervised session to retake, so the numbers get committed here too.

## Pose — the question that prompted the run

| | Desk (this run) | Stand (previous overnight readings) |
|---|---|---|
| gravity | (1.0125, 0.0833, −0.1007) | (0.97, 0.06, −0.28) |
| projection gain | **1.0083** | 1.0466 |
| single-axis loss per 360° | **3.0°** | 14.7° |

**Confirms the stand/desk correction.** The overnight figure of 14.3°/360° was a
stand artefact; the operating figure is **3.0°**. Predicted ~3.5° before the run.

Also confirms the ambiguity the fusion agent raised is genuine: on the desk the
off-axis components are (0.083, −0.101) g, i.e. **right at the ±100 mg
accelerometer offset tolerance**. A static reading cannot settle whether that is
tilt or sensor offset — only the gyro-ratio check can.

## Stationary baseline (30 s, desk)

| Quantity | Value |
|---|---|
| drift | 0.4394° over 30 s |
| bias | 0.5328 dps |
| ZUPT noise spread | 0.8125 dps |

Worse per unit time than the stand figure (±0.33° over 120 s), which supports
treating the stand numbers as a best case. One 30 s sample — do not over-read.

## Coast and rate, lead OFF

| Commanded speed | Mean rate | Mean coast | alpha = ω²/2c | linear k = c/ω |
|---|---|---|---|---|
| 100 | 265.2 dps | **16.32°** | 2185 | 0.0616 s |
| 75 | 187.1 dps | **8.10°** | 2196 | 0.0433 s |
| 55 | 124.9 dps | **4.59°** | 1750 | 0.0368 s |

**Two conclusions.**

1. **Coast is ~2× larger than the figure it was designed against.** The
   inherited 7–9° was roughly half the truth, presumably taken under conditions
   that restrained rotation.
2. **Coast is NOT linear in rate.** The linear constant varies 1.7× across the
   range (0.0368 → 0.0616 s), so no single lead time fits. The Coulomb model
   `coast = ω²/(2·alpha)` gives a consistent alpha ≈ 2185/2196 for the two
   fastest blocks. Adopted **alpha = 2100 deg/s²**, which fits speeds 100 and 75
   within 3% and under-predicts at 55 — the safe direction, since under-leading
   leaves a small overshoot while over-leading leaves an undershoot too small
   for the servo deadband to correct.

## Lead A/B at speed 100

| Condition | Mean turn error | n |
|---|---|---|
| lead off | **+16.51°** | 6 |
| lead on (k = 0.030 s, linear) | **+6.83°** | 4 |

**59% reduction**, with a constant that was ~37% too small and the wrong model
shape. Consistent with the arithmetic: a 0.030 s linear lead removes
0.030 × 265 ≈ 8.0°, and 16.5 − 8.0 = 8.5° against 6.8° observed.

Extrapolating the quadratic model at alpha = 2100 predicts it should remove
≈16.7° of the 16.5°, i.e. land near zero. **Unverified — needs the next run.**

## Raw entries

```
gravity-x        param=0   cond=-        1.0125
gravity-y        param=0   cond=-        0.0833
gravity-z        param=0   cond=-       -0.1007
projection-gain  param=0   cond=-        1.0083
drift-deg        param=30  cond=-        0.4394
bias-dps         param=1   cond=-        0.5328
noise-spread-dps param=1   cond=-        0.8125

speed 100, lead-off   (coast deg / rate dps / turn error deg)
  18.7567 / 235.0726 / 18.9371
  16.8089 / 277.8892 / 17.0286
  16.6303 / 287.1711 / 16.8828
  13.9193 / 259.7245 / 14.0690
  16.6676 / 273.2553 / 16.7965
  15.1657 / 258.2479 / 15.3780

speed 75, lead-off
   7.9768 / 189.7286 /  8.1633
   7.2774 / 184.5068 /  7.3224
   8.2686 / 181.1265 /  8.3640
   7.7745 / 183.2195 /  7.9362
   9.8705 / 180.1909 / 10.0207
   7.4593 / 204.1038 /  7.5405

speed 55, lead-off
   5.3452 / 132.8242 /  5.4282
   3.6226 / 123.0828 /  3.6844
   6.0222 / 157.5930 /  6.1215
   3.9981 / 127.7558 /  4.0367
   5.8071 / 119.4564 /  5.8111
   2.7417 /  88.4799 /  2.8200

speed 100, lead-on (linear k = 0.030 s)
  17.1781 / 326.8736 /  7.5743
  11.1111 / 282.3934 /  2.8232
  18.5540 / 277.5771 / 10.2885
  13.7431 / 243.0543 /  6.6406
  19.4510 / (truncated - log hit its 96-entry cap here)
```

## Known problems with this run

- **Truncated at 96 entries**, losing the last two turns of the A/B block. Cause
  was logging `coast-sec`, which is exactly `coast/rate` and therefore derivable
  from two entries already present. Fixed: metric dropped, cap raised to 128.
- **The A/B was reported pooled across speeds**, averaging a 16° coast with a 4°
  one. Fixed: the report now splits by speed.
- **Rate varies within a block** (235–287 dps at the same commanded speed), so
  per-turn alpha is noisier than the block means suggest. Worth understanding —
  battery sag, motor warmth, or surface variation are all candidates.
