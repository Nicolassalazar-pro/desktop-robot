# Bench run 004 — 2026-08-06, desk, untethered, AFTER `cod run`

First run after Calibration On Demand. The purpose was to measure what COD
actually did, since the chip cannot check its own scale.

**Outcome: COD changed the gyro scale by ~15%, which is 5× the datasheet
tolerance. It has been reverted pending a physical check.**

## The A/B looked perfect — and that is the trap

| Condition | Mean turn error at speed 100 | n |
|---|---|---|
| lead off | +14.79° | 6 |
| lead on (α = 2600) | **+0.02°** | 6 |

99.9% reduction. But **that error is measured in gyro units, and the gyro units
are exactly what is in question.** A scale error cannot be detected by the
instrument carrying it.

## What actually happened

| Speed | rate 003 | rate 004 | ratio | α 003 | α 004 | ratio |
|---|---|---|---|---|---|---|
| 100 | 283.2 | 240.2 | **0.848** | 2304 | 1971 | **0.855** |
| 75 | 195.0 | 165.8 | **0.850** | 2169 | 1891 | 0.872 |
| 55 | 136.1 | 130.2 | 0.957 | 1727 | 1709 | 0.990 |

The model predicts that if scale changes by `k`, then rate → `k·rate` **and**
α → `k·α`. Observed at speed 100: **0.848 and 0.855.** They match.

Nothing physical changed between runs — same motors, same surface, same
commands. So the 15% drop in reported rate is entirely a scale change.

Confirmed against the gain values: with a nominal of 12288, `10301/12288 =
0.838`, and X is the yaw axis. That matches the observed 0.848.

## Why it was reverted

The datasheet initial sensitivity tolerance is **±3%**. A 15% correction is 5×
outside it. Two possibilities:

- **(a)** the part was within spec and COD has now broken it
- **(b)** the part was 15% out of spec and COD fixed it

**(a) is far more likely a priori**, and the 19.6% spread across the three axis
gains is itself hard to reconcile with three axes each within ±3%.

Neither is decidable from onboard data. Reverting restores the state in which
three bench runs of tuning are valid.

## The physical test that would settle it

The two scales disagree about something a human can see:

| | says at full speed | so a real 360° takes |
|---|---|---|
| OLD (pre-COD) | 283 dps | **1271 ms** |
| NEW (post-COD) | 240 dps | 1499 ms |

The stored calibration, measured pre-COD, is **1260 ms** — agreeing with the old
scale. That is circular on its own, but it means `turn 360` now commands a
physically different rotation.

**Test:** mark the floor, `turn 360`, see whether it returns to the mark. A 15%
scale error is **54° of visible offset** — impossible to miss. Repeat with
`cod run` applied and pick whichever comes back square.

## Other observations

- **Bias was 1.41 dps**, far higher than runs 001–003 (0.53, 0.03, −0.02). Also
  consistent with a scale change: bias in dps is raw counts × scale.
- **Drift −0.32° over 30 s**, in family with runs 001 and 003.
- The α convergence toward ~1971 while the compiled value stayed 2600 is why the
  error landed at +0.02 rather than −0.49: under-leading nudged it positive.
  **That is luck, not design.**
