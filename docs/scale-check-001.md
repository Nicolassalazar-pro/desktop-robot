# Gyro scale check — 2026-08-06, 3 revolutions, untethered

The one gyro error that cannot be measured from onboard data, resolved with a
single external observation.

## Method

Mark on the floor, 3 revolutions (1080°) at full speed, untethered, triggered by
the TEST button. Multiple revolutions were used because the scale error
accumulates every turn while the human's ability to read the mark does not.

## Result

**Offset from the mark: 1–2°, estimated by eye (no protractor).**

| Interpretation | Implied scale error | Per 360° |
|---|---|---|
| Taking 1.5° at face value | 0.139% | 0.50° |
| Conservative (±3° reading precision) | 0.278% | 1.00° |
| Very conservative (±5°) | 0.463% | 1.67° |

For comparison:

| | Would have shown as |
|---|---|
| Datasheet ±3% tolerance | **32°** over 3 revs |
| The change COD wanted (15%) | **190°** over 3 revs |
| Observed | **1–2°** |

## Conclusions

**1. The factory scale is correct.** The error is at least an order of magnitude
inside the datasheet tolerance, and roughly 100× smaller than the correction COD
tried to apply.

**2. COD was wrong, and reverting it was right.** Run 004 showed it moving the
scale by 15%; this shows the scale needed no correction at all. The post-COD A/B
that read a flawless +0.02° was measuring in corrupted units — exactly the trap
of letting an instrument check its own scale.

**3. No correction was applied, deliberately.** The measurement precision (±2–3°
by eye) is coarser than the effect (1–2°), and the direction was not
distinguishable. Storing a 0.14% correction derived from a ±0.3% measurement
would be false precision, and worse, it would be recorded as "measured against a
physical reference" without the caveat. The honest record is: **scale verified
to within ~0.3%, no correction needed.**

## Secondary finding: the robot walks while it spins

**Observed: 2 cm forward, 1 cm left = 2.2 cm total over 3 revolutions**, i.e.
**~0.75 cm per revolution.**

This is *not* a rotation-centre offset. A perfect pivot returns to its exact
starting position after a full revolution regardless of where the rotation
centre sits — an offset centre just means orbiting a different point.

That argument is stronger than it first looks: **constant but unequal wheel
speeds also return to start.** Unequal speeds shift the instantaneous centre of
rotation off the robot's midpoint, but if they are constant that centre is still
*fixed*, so whole revolutions still close. So the displacement cannot be
explained by mismatched-but-steady wheels either.

What is left is the two things that are not steady:

- **Slip** — the wheels not gripping equally, varying with floor texture.
- **The ramp and the coast.** During the ~400 ms standing start and the
  coast-down, the rotation rate is low while any translation continues, so the
  instantaneous turn radius is large and the robot sweeps a wide arc. Those two
  arcs happen at different rates and do not cancel.

Both are plausible and this measurement cannot separate them. Distinguishing
them would need a spin held at constant rate with the ramp/coast excluded — not
worth doing for a 0.75 cm effect.

**The gyro cannot see it either way.** It measures rotation; this is
translation.

Impact:

| Context | Effect |
|---|---|
| Triangle survey (3 × 120° turns) | ~0.75 cm total, against 100 cm legs = 0.7% |
| `excited` gesture (360° each way) | ~0.75 cm per turn, partially cancelling |
| Turn accuracy | **none** — heading is unaffected |

Not worth acting on. Recorded so it is not rediscovered as a mystery.

## Error budget after this check

| Source | Magnitude | Status |
|---|---|---|
| Coast overshoot | 0.5° | Solved (was 16–17°) |
| Gyro scale | ≤1° per 360° | **Verified, no correction needed** |
| Heading drift (stationary) | ±0.33° / 2 min | Solved |
| Axis projection | 3.0° per 360° | Corrected |
| Spin translation | 0.75 cm/rev | Measured, not worth fixing |

Every term is now measured. Nothing in the turn path is above ~1°.
