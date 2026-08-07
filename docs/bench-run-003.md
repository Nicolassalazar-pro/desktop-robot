# Bench run 003 — 2026-08-06, desk, untethered

First run with **α = 2600** (quadratic coast model, retuned from run 002).

## Coast compensation is solved

| Condition | Mean turn error at speed 100 | n |
|---|---|---|
| lead off | **+16.67°** | 6 |
| lead on (α = 2600) | **−0.49°** | 6 |

**97.0% reduction. Half a degree of residual**, with no over/under-correction
flag raised. The dominant error on the robot — the one every research agent
independently ranked first — is now smaller than the run-to-run scatter of
almost everything else.

Progression across the three runs:

| Run | α | lead-off | lead-on | verdict |
|---|---|---|---|---|
| 001 | 0.030 s (linear) | +16.51° | +6.83° | under-corrected |
| 002 | 2100 (quadratic) | +17.57° | **−4.49°** | over-corrected |
| 003 | **2600** | +16.67° | **−0.49°** | **on target** |

## Alpha estimates are converging

| Speed | run 001 | run 002 | run 003 |
|---|---|---|---|
| 100 | 2185 | 2304 | **2481** |
| 75 | 2196 | 2169 | **2203** |
| 55 | 1750 | 1727 | **2262** |

Speed 75 is remarkably stable (2169–2203, 1.6% spread across three runs).

**The speed-55 anomaly did not reproduce.** It read ~1730 in both earlier runs
and 2262 here — so the "friction regime changes near the deadband" explanation
in `bench-run-002.md` is wrong, or at least incomplete. Note the measured rate
at speed 55 also rose (146 dps here vs 125/136 before), so the low-speed
behaviour is simply less repeatable than the high-speed behaviour.

The learner converged to **α = 2484** live, from a 2600 start — consistent with
the block estimate at speed 100.

## Stationary baseline

| | run 001 | run 002 | run 003 |
|---|---|---|---|
| drift over 30 s | +0.44° | −1.88° | **+0.63°** |
| bias | 0.533 dps | 0.030 dps | **−0.023 dps** |

Run 002's −1.88° now looks like an outlier rather than a trend. Three 30 s
samples is still too few to quote an interval from.

## What this closes, and what it opens

**Closed:** coast overshoot, which was ~2× worse than originally believed
(16–17°, not 7–9°) and is now ~0.5°.

**Still open:**

- **Rate scatter within a block** — 237–320 dps at one commanded speed, across
  all three runs. Unexplained. Battery sag, motor warmth and surface variation
  remain the candidates, and it is now the largest source of variability left.
- **Low-speed repeatability** — speed 55 gives inconsistent α between runs.
- **Drift needs more than one 30 s sample per run** to be quotable.
