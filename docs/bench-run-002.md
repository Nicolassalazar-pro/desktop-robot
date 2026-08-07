# Bench run 002 — 2026-08-06, desk, untethered

Second supervised run. First run with coast compensation live (α = 2100,
quadratic), so this is the first real closed-loop test of the model.

Completed cleanly: **79 entries, no truncation** (run 001 truncated at 96).

## Headline: the compensation over-corrected

| Condition | Mean turn error at speed 100 | n |
|---|---|---|
| lead off | **+17.57°** | 6 |
| lead on (α = 2100) | **−4.49°** | 6 |

Magnitude improved 4×, but **the sign flipped** — it now stops short. α = 2100
predicts *more* coast than actually occurs, so it leads too much.

### The model is sound — it is the constant that was wrong

Predicted `error = actual_coast − predicted_coast`, per turn:

| ω (dps) | predicted | actual | model error | observed error |
|---|---|---|---|---|
| 326.5 | 25.38 | 16.02 | −9.35 | **−7.53** |
| 277.1 | 18.28 | 15.80 | −2.48 | **−2.38** |
| 310.3 | 22.92 | 17.77 | −5.14 | **−4.82** |
| 283.1 | 19.09 | 16.29 | −2.80 | **−1.97** |
| 310.8 | 23.00 | 16.59 | −6.41 | **−6.31** |
| 283.6 | 19.16 | 15.09 | −4.07 | **−3.94** |

Agreement within ~1° on every turn, and within 0.15° on three of six. The
closed-loop behaviour is predictable; only α needed fixing.

## Coast and rate, lead OFF — reproduces run 001

| Speed | Rate | Coast | alpha (run 002) | alpha (run 001) |
|---|---|---|---|---|
| 100 | 283.2 dps | 17.40° | **2304** | 2185 |
| 75 | 195.0 dps | 8.76° | **2169** | 2196 |
| 55 | 136.1 dps | 5.36° | **1727** | 1750 |

Speeds 75 and 55 reproduce to within 1.3%. Speed 100 differs by 5%, consistent
with its larger within-block rate scatter.

**Speed 55 is systematically lower** (~1730 vs ~2200) in *both* runs, so it is a
real effect, not noise — near the deadband the friction regime differs.

## Choosing alpha

Two estimates disagree:

- **Lead-off block means** → α ≈ 2300
- **Closed-loop A/B extrapolation** → α ≈ 2751 would zero the error

Residual error predicted from block means:

| alpha | speed 100 | speed 75 | speed 55 |
|---|---|---|---|
| 2100 | −1.69 | −0.29 | +0.95 |
| 2400 | +0.69 | +0.84 | +1.50 |
| **2600** | **+1.98** | **+1.45** | **+1.80** |
| 2750 | +2.82 | +1.85 | +1.99 |

**Adopted α = 2600.** It sits between the two estimates, and both put the
residual under 2°.

The tie-break is the safety asymmetry: **higher α means less predicted coast, so
a later stop, so a small overshoot** — recoverable. Lower α undershoots, and
correcting an undershoot needs a nudge smaller than the servo deadband can
produce. When in doubt, lead less.

Expected: |error| ≲ 2° at all speeds, against 17.6° uncompensated.

## Pose — reproduces run 001

| | Run 002 | Run 001 |
|---|---|---|
| gravity | (1.0052, 0.0836, −0.0933) | (1.0125, 0.0833, −0.1007) |
| projection gain | 1.0077 | 1.0083 |
| single-axis loss | 2.8°/360° | 3.0°/360° |

The desk figure is reproducible. Both are far from the stand's 14.7°.

## Anomalies, unresolved

- **Drift was 4× worse than run 001**: −1.8849° over 30 s, versus +0.4394°.
  Bias also differed by 18× (0.0297 vs 0.5328 dps). One 30 s sample each, so
  this may be sampling noise — but it is a large discrepancy and worth watching.
- **Rate scatter within a block persists**: 237–320 dps at a single commanded
  speed. Battery sag, motor warmth and surface variation remain candidates.
- **The lead-on turns show a higher implied alpha (~2750) than lead-off turns at
  the same commanded speed (~2304)**, despite running at *higher* rates. Higher
  rate should mean more coast, not less. Unexplained; it is the reason the two
  alpha estimates disagree.

## Tooling faults found

- **Serial dropped output.** Two lines arrived corrupted (`pff`, `pond=`) and
  the entire per-speed summary was swallowed. Cause: `Serial.setTxTimeoutMs(0)`
  makes TX non-blocking, so it *drops* bytes rather than stalling when the USB
  CDC buffer fills; ~80 lines dumped flat out overran it. Fixed by flushing and
  yielding every 4 lines.
- **The "% better" figure was computed on signed values**, so an
  over-correction would have been reported as >100% improvement. Now compared on
  magnitude, and the direction of the miss is named explicitly.
