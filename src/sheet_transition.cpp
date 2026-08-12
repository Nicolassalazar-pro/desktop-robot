#include "sheet_transition.h"

namespace SheetTransition {

int16_t rubberBand(int16_t raw, int16_t h) {
  // Inside the travel range the card tracks the finger 1:1: filtering here
  // would smear touch by exactly one frame and make the sheet feel dead
  // (WWDC18-803, and the same reason a web sheet uses transform only).
  if (raw >= 0 && raw <= h) {
    return raw;
  }
  if (raw < 0) {
    // Integer division truncates toward zero, so a negative raw yields a
    // negative-but-shallower value: the top wall pulls but never stops.
    return static_cast<int16_t>(raw / kRubberDivisor);
  }
  // Only the excess past the bottom stop is damped, not the whole position,
  // so the sheet still departs from h and grows from there rather than
  // snapping back into range.
  const int16_t overshoot = static_cast<int16_t>(raw - h);
  return static_cast<int16_t>(h + overshoot / kRubberDivisor);
}

int32_t project(int16_t y, int32_t velocityMilliPxPerMs, uint16_t decayMs) {
  // 64-bit intermediates because a fast finger's velocity * decay is not
  // guaranteed to fit in int32_t if the caller ever raises the horizon: v
  // is milli-px/ms, so v * decay is milli-px-per-ms times ms which can be
  // millions before the divide-by-1000 pulls it back to pixel space.
  const int64_t delta = static_cast<int64_t>(velocityMilliPxPerMs) *
                        static_cast<int64_t>(decayMs);
  const int64_t projected = static_cast<int64_t>(y) + delta / 1000;
  return static_cast<int32_t>(projected);
}

int16_t chooseTarget(int32_t projected, int16_t h, int16_t threshold) {
  // Comparison is on the PROJECTED endpoint, not on y. This is the whole
  // point of the model: a flick from below the threshold can still commit,
  // and a stationary rest above the threshold can still close if the finger
  // was pulling back at the moment of release. Header comment covers why.
  return projected >= static_cast<int32_t>(threshold)
             ? h
             : static_cast<int16_t>(0);
}

int16_t settleStep(int16_t y, int16_t target, uint8_t percentPerTick) {
  // Widen before subtracting so 16-bit promotion cannot silently truncate
  // when target and y sit at opposite ends of the travel.
  const int32_t remaining =
      static_cast<int32_t>(target) - static_cast<int32_t>(y);
  if (remaining == 0) {
    return target;
  }
  const int32_t sign = remaining > 0 ? 1 : -1;
  const int32_t magnitude = remaining > 0 ? remaining : -remaining;
  // Percentage of remaining distance is fine for the bulk of the animation,
  // but by itself it never reaches zero: 3 * 25 / 100 = 0 and the sheet
  // parks two pixels short forever. A 1 px minimum stride guarantees the
  // settle always terminates within a bounded number of ticks.
  int32_t step = magnitude * static_cast<int32_t>(percentPerTick) / 100;
  if (step < 1) {
    step = 1;
  }
  // If the enforced minimum stride would meet or overshoot the target, snap
  // exactly. Without this the 1 px floor could ping-pong across the endpoint
  // on the very last tick.
  if (step >= magnitude) {
    return target;
  }
  return static_cast<int16_t>(y + sign * step);
}

void Controller::onPress(int16_t screenY, uint32_t nowMs) {
  // Capture BOTH the finger origin AND the sheet's current travel, so a
  // press on a half-open sheet reads the finger's motion as a delta from
  // where the card already is instead of teleporting it under the finger.
  state = State::Dragging;
  dragOriginY = screenY;
  dragBaseY = y;
  velocity = 0;
  lastSampleY = y;
  lastSampleMs = nowMs;
  // Where the finger currently is, as opposed to where the card is. Needed by
  // reachedCommit to tell "pulled as far as the panel allows" from "stopped
  // short" -- see the note there.
  lastScreenY = screenY;
  // A new interaction cancels any prior one-shot the caller never took.
  // Otherwise a forgotten poll would fire commit on the NEXT open cycle
  // and land at a state change the user did not just perform.
  justSettled = false;
  // Re-arm the crossing latch. Without this, dragging the card down to commit,
  // dragging it back up, and pulling it down again would never fire a second
  // commit -- the sheet would work exactly once per boot.
  armedOpen = false;
}

void Controller::onMove(int16_t screenY, uint32_t nowMs, int16_t h) {
  // Widen to int32_t before the finger-delta add so an origin near the edge
  // cannot overflow int16_t before rubberBand sees it. Then clamp back to
  // int16_t range explicitly for the same reason.
  int32_t raw = static_cast<int32_t>(dragBaseY) +
                (static_cast<int32_t>(screenY) -
                 static_cast<int32_t>(dragOriginY));
  if (raw > INT16_MAX) raw = INT16_MAX;
  if (raw < INT16_MIN) raw = INT16_MIN;
  y = rubberBand(static_cast<int16_t>(raw), h);
  lastScreenY = screenY;

  // R46 rollover-safe delta: subtract as UNSIGNED (well-defined modular
  // wrap) then reinterpret the result as SIGNED. Across the 49.7-day
  // millis() boundary a true small-positive dt comes out correctly this
  // way. A same-task future-stamp would appear negative and fall below
  // kMinSampleMs, safely skipping the velocity update.
  const int32_t dt = static_cast<int32_t>(nowMs - lastSampleMs);
  if (dt >= static_cast<int32_t>(kMinSampleMs)) {
    const int32_t dy =
        static_cast<int32_t>(y) - static_cast<int32_t>(lastSampleY);
    // Milli-px/ms keeps useful resolution without floats: a 30 px/s move
    // is 0.03 px/ms which would round to zero as plain px/ms. dy is
    // bounded by roughly ±h*4/3, so dy * 1000 is well within int32_t.
    velocity = dy * 1000 / dt;
    lastSampleY = y;
    lastSampleMs = nowMs;
  }

  // Arm the commit the moment the card crosses the line, and only on the
  // crossing. Commit is now a consequence of the GESTURE reaching the bottom,
  // not of letting go, so this is where it belongs -- but it has to be
  // edge-triggered or holding the card at the bottom would re-fire it on every
  // frame. `armedOpen` is the latch that makes it an edge.
  if (!armedOpen && reachedCommit(h)) {
    armedOpen = true;
    justSettled = true;
    state = State::Open;
  }
}

void Controller::onRelease(int16_t h, int16_t threshold) {
  // SNAP to whichever end the card is nearest.
  //
  // This replaces the earlier "park exactly where the finger left it" rule.
  // Park was a reasonable model -- a physical shade goes where you push it --
  // but it had a failure the metaphor does not: a card left half-open puts the
  // controller in a state that is not Closed, and the caller suppresses the
  // face renderer for exactly that condition. So a half-pull that the operator
  // simply abandoned froze the robot's face behind a stationary card, with the
  // only way out being to find the edge and drag it back. Snapping removes the
  // half-open resting state altogether, which removes the trap with it.
  //
  // 51% rather than the 40% drag threshold because this is a commitment, not a
  // hint: past half means "I meant to open it".
  //
  // Deliberately NOT velocity-projected. project()/chooseTarget() still exist
  // and still work, but a flick that lands 5 px past halfway opening the AP is
  // a surprise, and the operator asked for position to decide it.
  (void)threshold;
  velocity = 0;

  if (h <= 0) {
    state = (y <= 0) ? State::Closed : State::Parked;
    return;
  }

  const int32_t line = (static_cast<int32_t>(h) * kSnapOpenPct) / 100;
  target = (static_cast<int32_t>(y) >= line) ? h : 0;

  if (y == target) {
    // Already there; no animation to run. Route through the same latch tick()
    // uses so a commit cannot fire twice for one gesture.
    if (target == 0) {
      state = State::Closed;
    } else {
      state = State::Open;
      if (!armedOpen) {
        armedOpen = true;
        justSettled = true;
      }
    }
    return;
  }

  state = State::Settling;
}

bool Controller::reachedCommit(int16_t h) const {
  if (h <= 0) {
    return false;
  }
  const int32_t line = (static_cast<int32_t>(h) * kCommitPct) / 100;
  if (static_cast<int32_t>(y) >= line) {
    return true;
  }
  // "Pulled as far as the panel allows" also counts.
  //
  // WHY THIS SECOND RULE EXISTS
  //
  // Tracking is 1:1, so from a closed sheet the card can never exceed
  // (h - pressY). Commit at 85% of a 280 px panel is 238, which means a press
  // even 60 px down caps the card at 220 and the gesture CANNOT commit no
  // matter how far it is pulled. That is not a tuning problem, it is an
  // unreachable target -- and it is exactly why the operator found the sheet
  // took two pulls to open.
  //
  // The tempting fixes are both wrong. Lowering kCommitPct makes the sheet
  // trigger-happy everywhere to rescue one start position. Anchoring the card
  // edge to the finger makes it reachable but teleports the card by however
  // far the press landed from the edge -- which is precisely what
  // dragBaseY/dragOriginY were split up to prevent (and a test caught it).
  //
  // So instead: honour the gesture the operator actually performed. Running the
  // finger to the bottom of the panel IS pulling the shade all the way down,
  // whatever the arithmetic ceiling happens to be. Reachable from any start
  // point, still exactly 1:1, and no jump.
  //
  // The travel floor is what keeps it honest -- without it, a tap near the
  // bottom of the screen would read as a completed pull. Requiring the card to
  // be at least half-way means the operator has to have actually dragged it
  // there.
  const int32_t pullLine = (static_cast<int32_t>(h) * kPullOutCommitPct) / 100;
  const bool fingerOutOfRoom =
      (lastScreenY >= 0) &&
      (static_cast<int32_t>(lastScreenY) >=
       (static_cast<int32_t>(h) - kPullOutBandPx));
  return fingerOutOfRoom && (static_cast<int32_t>(y) >= pullLine);
}

bool Controller::tick(uint8_t percentPerTick) {
  // Any state that is not Settling has nothing to animate. tick() being
  // called during Dragging is not a caller bug -- the main loop polls
  // every frame -- so silently do nothing instead of asserting.
  if (state != State::Settling) {
    return false;
  }
  y = settleStep(y, target, percentPerTick);
  if (y != target) {
    return true;
  }
  // Reaching the target is the ONLY point at which the commit one-shot
  // fires. That is what makes the caller's rule "act on takeSettledOpen(),
  // never on state" safe: nothing here can arm mid-drag, and a settle back
  // to Closed does not arm either.
  if (target == 0) {
    state = State::Closed;
  } else {
    state = State::Open;
    // Same latch the mid-drag commit uses, so a gesture that both crosses the
    // commit line AND snaps open fires the one-shot exactly once.
    if (!armedOpen) {
      armedOpen = true;
      justSettled = true;
    }
  }
  velocity = 0;
  return false;
}

bool Controller::takeSettledOpen() {
  // One-shot semantics matter more here than the return value itself. The
  // caller polls every frame; without the clear, "true" would latch and
  // the commit action would fire again next frame.
  if (!justSettled) {
    return false;
  }
  justSettled = false;
  return true;
}

uint8_t Controller::progress(int16_t h) const {
  // No travel means no gradient to interpolate against; return the neutral
  // start value rather than divide by zero. Callers use this to fade a
  // backdrop, so 0 (fully closed) is the safe default.
  if (h <= 0) {
    return 0;
  }
  int32_t clamped = y;
  if (clamped < 0) {
    clamped = 0;
  } else if (clamped > h) {
    clamped = h;
  }
  // Widen to int32_t before the scale so clamped * 255 stays within range
  // for any travel width the display can plausibly present.
  return static_cast<uint8_t>(clamped * 255 / h);
}

void Controller::reset() {
  state = State::Closed;
  y = 0;
  dragOriginY = 0;
  dragBaseY = 0;
  target = 0;
  velocity = 0;
  lastSampleY = 0;
  lastScreenY = -1;
  lastSampleMs = 0;
  justSettled = false;
  armedOpen = false;
}

}  // namespace SheetTransition
