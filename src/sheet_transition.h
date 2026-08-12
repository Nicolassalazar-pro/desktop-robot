#pragma once

// Sheet transition physics — the pairing card that slides down over the face.
//
// WHY THIS IS ITS OWN TRANSLATION UNIT
//
// It has no Arduino dependency: integer maths over positions, velocities and
// timestamps. That keeps it host-testable against the REAL shipped code rather
// than a modelled copy of the policy, which is the compromise
// test/ws_owner_reaper_test.cpp has to live with. It also stops main.cpp
// growing again (READ-12).
//
// THE MODEL, WHICH IS THE WEB'S MODEL
//
// A sheet is two layers. The backdrop (the robot's face) does not move at all;
// the card travels over it. On the web that is `transform: translateY` on a
// separate compositing layer -- transform only, never layout. Here it is a
// second canvas composited by row.
//
//   1. 1:1 tracking. Inside range the card follows the finger exactly.
//   2. Rubber-band past the end stops, so it resists but still gives, instead
//      of hitting an invisible wall.
//   3. ONE number drives everything: progress = y / h. Coherence comes from
//      every layer reading the same value.
//   4. Release decides on VELOCITY, not position. The target comes from the
//      PROJECTED endpoint (y + v*decay), which can flip the verdict in BOTH
//      directions: a fast downward flick released below the threshold still
//      opens, and a release above the threshold that is already pulling back
//      still closes. Position alone can do neither, and that is most of what
//      makes a sheet feel alive. (Apple call it projection, WWDC18-803.)
//
//      Note this is NOT "a slow drag past the threshold fails to commit" --
//      a stationary release above the threshold commits, as it should. The
//      asymmetry is contributed by velocity, not by making position unreliable.
//   5. State changes ONLY on settle. Nothing commits mid-drag.
//
// WATCHDOG CONSTRAINT
//
// settleStep() is a fraction-of-remaining-distance move, deliberately NOT a
// timed tween. The loop cadence is the clock, so there is no delay() and no
// timer anywhere in this file. On a board with a 5 s task watchdog and a
// history of loop-stall blackouts, an animation that sleeps is not a slow UI,
// it is a pending reset (design-craft 6b).
//
// INTEGER MATHS ONLY
//
// Velocity is carried as milli-pixels per millisecond (px/ms * 1000) so the
// projection has useful resolution without floats on the hot path.
//
// Timestamps must be handled rollover-safely: subtract into a SIGNED type.
// This project has already shipped one unsigned-underflow bug that cut drive
// power mid-run because a value stamped by another task read as 4,294,967,295
// ms of silence (PROJECT-HISTORY R46). Do not repeat it here.

#include <stdint.h>

namespace SheetTransition {

enum class State : uint8_t {
  Closed,     // fully retracted, face visible, not tracking
  Dragging,   // finger down, card following it
  Settling,   // finger released, animating to the chosen target
  // Finger released with the card left exactly where it was. No longer reached
  // by the normal release path (which snaps); retained only for the degenerate
  // h <= 0 case, where there is no travel to snap along.
  Parked,
  Open,       // fully extended, card covering the face
};

// Distance at which the card counts as pulled "all the way down", as a
// percentage of travel.
//
// It is 85% and not 100% for a geometric reason that is easy to miss. Tracking
// is 1:1, so the furthest the card can travel is (H - startY): a drag begun
// 30 px below the top edge can never push the card past 250 of 280. At a
// literal 100% the sheet would be uncommittable from any start point except the
// exact top row. 85% is reachable from anywhere sensible, and in practice the
// finger runs off the bottom of the panel first, which reads as finger-gone and
// parks the card near maximum anyway.
constexpr int16_t kCommitPct = 85;

// Second commit route: the finger ran out of panel.
//
// kCommitPct alone is unreachable from most start points because tracking is
// 1:1 (see the long note in reachedCommit). These let "pulled to the bottom
// edge" count as a completed pull, with a travel floor so a tap near the
// bottom cannot masquerade as one.
constexpr int16_t kPullOutCommitPct = 50;   // card must be at least half out
constexpr int32_t kPullOutBandPx    = 10;   // how close to the bottom counts

// Release snaps to whichever end the card is nearest. Past this, it opens.
constexpr int16_t kSnapOpenPct = 51;

// ---------------------------------------------------------------------------
// project() and chooseTarget() are NOT used by the release path.
//
// Release chooses its target from POSITION alone (kSnapOpenPct), not from the
// projected endpoint -- a flick that lands just past halfway starting the AP is
// a surprise, and position is what was asked for. These two are kept because
// they are tested, reviewed and correct, and the velocity model is the obvious
// next refinement if the snap ever feels too literal. Nothing calls them today.
//
// settleStep() IS live again: it drives the snap animation via tick().
// ---------------------------------------------------------------------------

// Damp a raw finger offset into sheet travel.
//   0 <= raw <= h  -> returned unchanged (true 1:1 tracking)
//   raw < 0        -> overshoot above the top, damped by kRubberDivisor
//   raw > h        -> overshoot past the bottom, damped by kRubberDivisor
// Result is therefore bounded to roughly [-h/3, h + h/3].
int16_t rubberBand(int16_t raw, int16_t h);

// Where the sheet would come to rest if released now.
//   velocityMilliPxPerMs : px/ms * 1000, signed (positive = moving down)
//   decayMs              : projection horizon
// NOT clamped -- the caller decides what to do with an out-of-range endpoint.
int32_t project(int16_t y, int32_t velocityMilliPxPerMs, uint16_t decayMs);

// Settle target for a projected endpoint: h (open) or 0 (closed).
int16_t chooseTarget(int32_t projected, int16_t h, int16_t threshold);

// One settle tick: move a fixed PERCENTAGE of the remaining distance.
// Produces an ease-out whose duration emerges from the loop rate. Guarantees
// forward progress of at least 1 px so it always terminates rather than
// asymptotically crawling.
int16_t settleStep(int16_t y, int16_t target, uint8_t percentPerTick);

// Drag threshold as a fraction of travel. 40% rather than 50% because velocity
// projection already handles the deliberate flick, so the positional bar can
// sit lower without becoming trigger-happy.
constexpr int16_t  kThresholdPct  = 40;
// Projection horizon. Long enough that a flick carries, short enough that a
// slow drag is still decided by where the finger actually stopped.
constexpr uint16_t kProjectMs     = 120;
// Fraction of remaining distance consumed per settle tick.
constexpr uint8_t  kSettlePercent = 25;
// Overshoot divisor past the end stops.
constexpr uint8_t  kRubberDivisor = 3;
// Velocity samples closer together than this are ignored: dt in the denominator
// makes a 1 ms gap produce meaningless magnitudes.
constexpr uint32_t kMinSampleMs   = 8;

// Drag controller. Fed screen-space Y and a millisecond clock by the caller;
// owns no hardware and reads no globals.
struct Controller {
  State    state        = State::Closed;
  int16_t  y            = 0;   // current sheet travel, 0 = closed
  int16_t  dragOriginY  = 0;   // finger Y when the press started
  int16_t  dragBaseY    = 0;   // sheet travel when the press started
  int16_t  target       = 0;   // settle destination
  int32_t  velocity     = 0;   // milli-px per ms, signed
  int16_t  lastSampleY  = 0;
  // Last finger position in screen space, distinct from `y` (the card edge).
  // -1 means "no sample yet", so a fresh controller cannot satisfy the
  // pulled-to-the-bottom commit route by accident.
  int16_t  lastScreenY  = -1;
  uint32_t lastSampleMs = 0;
  bool     justSettled  = false;  // one-shot, consumed by takeSettled()
  // Latch so the commit fires on the CROSSING only. Without it, holding the
  // card at the bottom would re-arm the one-shot on every frame and the caller
  // would start the AP repeatedly. Cleared by onPress and reset.
  bool     armedOpen    = false;

  // Finger down. Captures the origin so a drag can begin from a partly-open
  // sheet without jumping.
  void onPress(int16_t screenY, uint32_t nowMs);

  // Finger moved. Updates travel with rubber-banding and accumulates velocity.
  void onMove(int16_t screenY, uint32_t nowMs, int16_t h);

  // Finger lifted. SNAPS to whichever end the card is nearest: past
  // kSnapOpenPct it settles fully open (and commits), otherwise it settles
  // closed. Nothing rests half-open any more -- see the note in the definition
  // for why parking there was a trap.
  void onRelease(int16_t h, int16_t threshold);

  // True once the card has been pulled far enough to count as fully down.
  // Evaluated DURING the drag, which is what makes commit a consequence of the
  // gesture rather than of letting go.
  bool reachedCommit(int16_t h) const;

  // Advance one settle frame. Returns true while still moving.
  //
  // Only meaningful if something explicitly puts the controller into Settling;
  // the release path no longer does. Kept for the same reason as project() --
  // see the note above.
  bool tick(uint8_t percentPerTick);

  // True exactly once, on the frame the card first reaches the commit
  // distance. This is the only commit signal, and being edge-triggered is what
  // stops it firing every frame while the operator holds the card at the
  // bottom.
  bool takeSettledOpen();

  // Progress 0..255 for anything that wants to interpolate alongside the sheet.
  uint8_t progress(int16_t h) const;

  void reset();
};

}  // namespace SheetTransition
