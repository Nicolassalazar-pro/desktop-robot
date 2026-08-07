#include "EyeEngine.h"

#include <Arduino.h>
#include <math.h>

namespace {
const uint8_t kBlinkHeight = 4;
const uint16_t kBlinkHoldMs = 60;
const uint16_t kThinkStepMs = 1000;
const int16_t kDirtyMargin = 2;
// MOOD_BORED lash extension geometry — kept here so drawEye and
// computeEyeBounds can't drift out of agreement.
constexpr int16_t kBoredLashExtend = 6;     // px past outer corner
constexpr int16_t kBoredLashThickness = 3;  // px tall
// MOOD_ANGRY brow geometry — sits ABOVE each eye, slopes parallel to the
// inner-corner cut so the brow and cut read as the same angry angle.
constexpr int16_t kAngryBrowExtend = 6;     // px past outer corner
constexpr int16_t kAngryBrowThickness = 3;  // px tall
constexpr int16_t kAngryBrowGap = 2;        // px between brow bottom and eye top
constexpr int16_t kAngryBrowRise = 12;      // outer end is this many px higher than inner end
constexpr int16_t kAngryBrowOutline = 1;    // px-wide black halo around the brow
constexpr int16_t kEyeOutline = 1;          // px-wide black halo around every eye shape

// MOOD_ERROR: two rotated skinny pills crossed at the eye center to read as
// a softer X. Pill thickness is a fraction of the eye width; tilt angle is
// fixed at ±15° (sin/cos baked as constants so no trig at runtime).
constexpr int16_t kErrorAngleDeg = 30;
constexpr float kErrorSin = 0.5000000000f;   // sin(30°)
constexpr float kErrorCos = 0.8660254038f;   // cos(30°)
constexpr int16_t kErrorEyeOffset = 8;       // px to push each X outward from its base center
constexpr int16_t kErrorShakeAmp = 3;        // px max shake amplitude (per axis)
constexpr uint16_t kErrorShakeStepMs = 70;   // tick period for shake redraw
// 8-step chaotic-ish (dx,dy) shake table. Both axes capped at ±kErrorShakeAmp.
// Designed so consecutive steps are NEVER the same value → always reads as
// "vibrating", and the average over the full cycle is ~0 → no perceived drift.
constexpr int8_t kErrorShakeDx[8] = { +2, -3, +1, +3, -2,  0, +2, -3 };
constexpr int8_t kErrorShakeDy[8] = { -1, +2, -3, +1, +3, -2,  0, +2 };

// Fill a rotated pill (thick line with round caps) centered at (cx,cy).
// `length` is tip-to-tip; `thickness` is perpendicular width.
// `tiltLeft` chooses which way the TOP of the pill leans (left = -15°,
// right = +15°). Direction vector + perpendicular are computed from the
// baked sin/cos so this is allocation-/trig-free at call time.
inline void fillTiltedPill(Arduino_GFX* gfx, int16_t cx, int16_t cy,
                           int16_t length, int16_t thickness,
                           bool tiltLeft, uint16_t color) {
  const int16_t r = thickness / 2;
  const float halfSpine = (length > thickness) ? ((length - thickness) * 0.5f) : 0.0f;
  // Spine direction (from bottom end to top end). Top tilts left = negative X;
  // top tilts right = positive X. Y is always negative (top is "up", lower Y).
  const float dirX = tiltLeft ? -kErrorSin : +kErrorSin;
  const float dirY = -kErrorCos;
  const int16_t e1x = static_cast<int16_t>(cx + dirX * halfSpine);  // top end-cap center
  const int16_t e1y = static_cast<int16_t>(cy + dirY * halfSpine);
  const int16_t e2x = static_cast<int16_t>(cx - dirX * halfSpine);  // bottom end-cap center
  const int16_t e2y = static_cast<int16_t>(cy - dirY * halfSpine);
  // Perpendicular to spine, scaled to r (rotate dir 90° clockwise on screen).
  const int16_t px = static_cast<int16_t>(-dirY * r);
  const int16_t py = static_cast<int16_t>(+dirX * r);
  // Body = parallelogram = two filled triangles.
  gfx->fillTriangle(static_cast<int16_t>(e1x + px), static_cast<int16_t>(e1y + py),
                    static_cast<int16_t>(e2x + px), static_cast<int16_t>(e2y + py),
                    static_cast<int16_t>(e1x - px), static_cast<int16_t>(e1y - py), color);
  gfx->fillTriangle(static_cast<int16_t>(e2x + px), static_cast<int16_t>(e2y + py),
                    static_cast<int16_t>(e2x - px), static_cast<int16_t>(e2y - py),
                    static_cast<int16_t>(e1x - px), static_cast<int16_t>(e1y - py), color);
  // Rounded end-caps.
  gfx->fillCircle(e1x, e1y, r, color);
  gfx->fillCircle(e2x, e2y, r, color);
}

// THINK indicator: 3 dots above each eye, one "lit" cycling every kThinkDotPeriodMs.
constexpr int16_t kThinkDotRadius = 2;
constexpr int16_t kThinkDotSpacing = 6;     // px between dot centers
constexpr int16_t kThinkDotGap = 6;         // px between top of eye and bottom of dot row
constexpr uint16_t kThinkDotPeriodMs = 333;
// MOOD_CURIOUS brow geometry — single raised brow above LEFT eye only.
constexpr int16_t kCuriousBrowExtend = 4;   // px past outer corner
constexpr int16_t kCuriousBrowThickness = 3;
constexpr int16_t kCuriousBrowGap = 3;
constexpr int16_t kCuriousBrowRise = 8;     // outer end this much higher than inner
// MOOD_LOVE heart-lobe geometry. Each lobe is a small filled circle; lobes
// bulge ~kLoveBulge px above the eye top.
constexpr int16_t kLoveBulge = 3;
// MOOD_SLEEPING / MOOD_WINK closed-eye line thickness.
constexpr int16_t kClosedEyeLineThickness = 3;
}

void EyeEngine::begin(Arduino_GFX* gfx,
                      const EyeEngineGeom& geom,
                      uint16_t bgColor,
                      uint16_t mainColor) {
  _gfx = gfx;
  _geom = geom;
  _bgColor = bgColor;
  _mainColor = mainColor;
  _browColor = mainColor;
  _left.curX = geom.leftCenterX;
  _left.curY = geom.leftCenterY;
  _left.nextX = geom.leftCenterX;
  _left.nextY = geom.leftCenterY;
  _right.curX = geom.rightCenterX;
  _right.curY = geom.rightCenterY;
  _right.nextX = geom.rightCenterX;
  _right.nextY = geom.rightCenterY;
  _position = POS_DEFAULT;
  _currentMood = MOOD_DEFAULT;
  _targetMood = MOOD_DEFAULT;
  _renderMood = MOOD_DEFAULT;
  _pendingMood = MOOD_DEFAULT;
  _moodPhase = MOOD_PHASE_STEADY;
  _curW = geom.baseW;
  _curH = geom.baseH;
  _nextW = geom.baseW;
  _nextH = geom.baseH;
  _currentOverlayScale = 0;
  _nextOverlayScale = 0;
  _dirtyX = 0;
  _dirtyY = 0;
  _dirtyW = 0;
  _dirtyH = 0;
  _lastFrameX = 0;
  _lastFrameY = 0;
  _lastFrameW = 0;
  _lastFrameH = 0;
  _pendingPriorX = 0;
  _pendingPriorY = 0;
  _pendingPriorW = 0;
  _pendingPriorH = 0;
  _dirty = false;
  _forceRedraw = true;
  _begun = true;
  _blinkActive = false;
  _lastFrameValid = false;
  _thinkTowardWest = true;
  _havePendingPrior = false;
  syncWidthTarget();

  const uint32_t now = millis();
  _lastFrameMs = (now >= _frameIntervalMs) ? (now - _frameIntervalMs) : 0U;
  _lastInteractionMs = now;
  _nextBlinkMs = 0;
  _nextIdleMs = 0;
  _nextThinkMs = 0;
  _blinkReleaseMs = 0;
}

void EyeEngine::setBackgroundSource(const uint16_t* pixels, int16_t originX, int16_t originY,
                                    int16_t srcW, int16_t srcH) {
  _bgSource = pixels;
  _bgSourceX = originX;
  _bgSourceY = originY;
  _bgSourceW = srcW;
  _bgSourceH = srcH;
}

void EyeEngine::clearRect(int16_t x, int16_t y, int16_t w, int16_t h) const {
  if ((w <= 0) || (h <= 0) || (_gfx == 0)) {
    return;
  }
  if (_bgSource == nullptr) {
    _gfx->fillRect(x, y, w, h, _bgColor);
    return;
  }
  // Clip target rect to bg source bounds.  Margins outside the source
  // (rare — mascot covers most of the screen) fall back to bgColor fill.
  const int16_t sx0 = _bgSourceX;
  const int16_t sy0 = _bgSourceY;
  const int16_t sx1 = static_cast<int16_t>(sx0 + _bgSourceW);
  const int16_t sy1 = static_cast<int16_t>(sy0 + _bgSourceH);
  int16_t cx0 = (x < sx0) ? sx0 : x;
  int16_t cy0 = (y < sy0) ? sy0 : y;
  int16_t cx1 = ((x + w) > sx1) ? sx1 : static_cast<int16_t>(x + w);
  int16_t cy1 = ((y + h) > sy1) ? sy1 : static_cast<int16_t>(y + h);

  if (y < cy0) {
    _gfx->fillRect(x, y, w, static_cast<int16_t>(cy0 - y), _bgColor);
  }
  if ((y + h) > cy1) {
    _gfx->fillRect(x, cy1, w, static_cast<int16_t>((y + h) - cy1), _bgColor);
  }
  if ((cx1 > cx0) && (cy1 > cy0)) {
    if (cx0 > x) {
      _gfx->fillRect(x, cy0, static_cast<int16_t>(cx0 - x),
                     static_cast<int16_t>(cy1 - cy0), _bgColor);
    }
    if ((x + w) > cx1) {
      _gfx->fillRect(cx1, cy0, static_cast<int16_t>((x + w) - cx1),
                     static_cast<int16_t>(cy1 - cy0), _bgColor);
    }
    // Blit the source region row-by-row from the clean mascot bitmap.
    const int16_t copyW = static_cast<int16_t>(cx1 - cx0);
    const int16_t copyH = static_cast<int16_t>(cy1 - cy0);
    for (int16_t row = 0; row < copyH; ++row) {
      const int32_t srcOffset = (static_cast<int32_t>(cy0 - sy0 + row) * _bgSourceW) + (cx0 - sx0);
      _gfx->draw16bitRGBBitmap(cx0, static_cast<int16_t>(cy0 + row),
                               const_cast<uint16_t*>(&_bgSource[srcOffset]),
                               static_cast<uint16_t>(copyW), 1);
    }
  }
}

bool EyeEngine::update() {
  if (!_begun || !_gfx) {
    _dirty = false;
    return false;
  }

  const uint32_t now = millis();
  if (!_forceRedraw && !timeReached(now, _lastFrameMs + _frameIntervalMs)) {
    _dirty = false;
    return false;
  }
  _lastFrameMs = now;

  bool hadEvent = false;
  if (_blinkActive && timeReached(now, _blinkReleaseMs)) {
    _blinkActive = false;
    _curH = _nextH;
    hadEvent = true;
  }

  const bool errorMode = (_targetMood == MOOD_ERROR) ||
                         (_renderMood == MOOD_ERROR) ||
                         (_currentMood == MOOD_ERROR);
  if (errorMode && (_nextErrorShakeMs != 0U) && timeReached(now, _nextErrorShakeMs)) {
    _errorShakeStep = static_cast<uint8_t>((_errorShakeStep + 1U) & 0x07U);
    _nextErrorShakeMs = now + kErrorShakeStepMs;
    hadEvent = true;
  }

  if (_autoblinkOn && !_blinkActive && (_nextBlinkMs != 0U) && timeReached(now, _nextBlinkMs)) {
    blink();
    hadEvent = true;
  }

  if (_idleWanderOn && (_nextIdleMs != 0U) && timeReached(now, _nextIdleMs)) {
    setPosition(static_cast<EyePosition>(random(9L)));
    hadEvent = true;
  }

  if ((_moodPhase == MOOD_PHASE_FADING_OUT) && (_currentOverlayScale == 0U)) {
    if (_pendingMood == MOOD_DEFAULT) {
      _renderMood = MOOD_DEFAULT;
      _currentMood = MOOD_DEFAULT;
      _nextOverlayScale = 0;
      _moodPhase = MOOD_PHASE_STEADY;
    } else {
      _renderMood = _pendingMood;
      _nextOverlayScale = 255;
      _moodPhase = MOOD_PHASE_FADING_IN;
    }
  }

  bool changed = false;

  int16_t oldCoord = _left.curX;
  _left.curX = tweenCoord(_left.curX, _left.nextX);
  changed = changed || (oldCoord != _left.curX);

  oldCoord = _left.curY;
  _left.curY = tweenCoord(_left.curY, _left.nextY);
  changed = changed || (oldCoord != _left.curY);

  oldCoord = _right.curX;
  _right.curX = tweenCoord(_right.curX, _right.nextX);
  changed = changed || (oldCoord != _right.curX);

  oldCoord = _right.curY;
  _right.curY = tweenCoord(_right.curY, _right.nextY);
  changed = changed || (oldCoord != _right.curY);

  uint8_t oldByte = _curW;
  _curW = tweenByte(_curW, _nextW);
  changed = changed || (oldByte != _curW);

  if (_blinkActive) {
    if (_curH != kBlinkHeight) {
      _curH = kBlinkHeight;
      changed = true;
    }
  } else {
    oldByte = _curH;
    _curH = tweenByte(_curH, _nextH);
    changed = changed || (oldByte != _curH);
  }

  oldByte = _currentOverlayScale;
  _currentOverlayScale = tweenByte(_currentOverlayScale, _nextOverlayScale);
  changed = changed || (oldByte != _currentOverlayScale);

  if ((_moodPhase == MOOD_PHASE_FADING_OUT) && (_currentOverlayScale == 0U)) {
    if (_pendingMood == MOOD_DEFAULT) {
      _renderMood = MOOD_DEFAULT;
      _currentMood = MOOD_DEFAULT;
      _nextOverlayScale = 0;
      _moodPhase = MOOD_PHASE_STEADY;
    } else {
      _renderMood = _pendingMood;
      _nextOverlayScale = 255;
      _moodPhase = MOOD_PHASE_FADING_IN;
    }
  }

  if ((_moodPhase == MOOD_PHASE_FADING_IN) && (_currentOverlayScale == 255U)) {
    _renderMood = _targetMood;
    _currentMood = _targetMood;
    _moodPhase = MOOD_PHASE_STEADY;
  } else if ((_moodPhase == MOOD_PHASE_STEADY) &&
             (_targetMood == MOOD_DEFAULT) &&
             (_currentOverlayScale == 0U)) {
    _renderMood = MOOD_DEFAULT;
    _currentMood = MOOD_DEFAULT;
  }

  // Brow lag — brows chase the eye gaze with a slower 1/3-per-frame
  // tween so they trail slightly behind the eyes (more "alive" feel).
  // Always run the tween, even when brows aren't visible, so that
  // switching INTO a brow-bearing mood doesn't snap the brow in from a
  // stale gaze.  Only mark `changed` if a brow-bearing mood is active —
  // otherwise we'd trigger pointless redraws of brow-less moods.
  const int16_t eyeGazeX = static_cast<int16_t>(_left.curX - _geom.leftCenterX);
  const int16_t eyeGazeY = static_cast<int16_t>(_left.curY - _geom.leftCenterY);
  const bool browWasOffTarget = (_browGazeX != eyeGazeX) || (_browGazeY != eyeGazeY);
  if (_browGazeX != eyeGazeX) {
    int32_t stepX = (static_cast<int32_t>(eyeGazeX) - static_cast<int32_t>(_browGazeX)) / 3;
    if (stepX == 0) {
      stepX = (eyeGazeX > _browGazeX) ? 1 : -1;
    }
    _browGazeX = static_cast<int16_t>(_browGazeX + stepX);
  }
  if (_browGazeY != eyeGazeY) {
    int32_t stepY = (static_cast<int32_t>(eyeGazeY) - static_cast<int32_t>(_browGazeY)) / 3;
    if (stepY == 0) {
      stepY = (eyeGazeY > _browGazeY) ? 1 : -1;
    }
    _browGazeY = static_cast<int16_t>(_browGazeY + stepY);
  }
  if (browWasOffTarget) {
    const bool browVisible = (_currentMood == MOOD_ANGRY) ||
                              (_targetMood == MOOD_ANGRY) ||
                              (_renderMood == MOOD_ANGRY) ||
                              (_currentMood == MOOD_SAD) ||
                              (_targetMood == MOOD_SAD) ||
                              (_renderMood == MOOD_SAD);
    if (browVisible) {
      changed = true;
    }
  }

  if (!_forceRedraw && !changed && !hadEvent && _lastFrameValid) {
    _dirty = false;
    return false;
  }

  int16_t frameX = 0;
  int16_t frameY = 0;
  int16_t frameW = 0;
  int16_t frameH = 0;
  computeCurrentBounds(frameX, frameY, frameW, frameH);

  int16_t dirtyX = frameX;
  int16_t dirtyY = frameY;
  int16_t dirtyW = frameW;
  int16_t dirtyH = frameH;
  if (_lastFrameValid) {
    expandUnion(_lastFrameX, _lastFrameY, _lastFrameW, _lastFrameH, dirtyX, dirtyY, dirtyW, dirtyH);
  }
  if (_havePendingPrior) {
    expandUnion(_pendingPriorX, _pendingPriorY, _pendingPriorW, _pendingPriorH, dirtyX, dirtyY, dirtyW, dirtyH);
    _havePendingPrior = false;
  }

  dirtyX = static_cast<int16_t>(dirtyX - kDirtyMargin);
  dirtyY = static_cast<int16_t>(dirtyY - kDirtyMargin);
  dirtyW = static_cast<int16_t>(dirtyW + (kDirtyMargin * 2));
  dirtyH = static_cast<int16_t>(dirtyH + (kDirtyMargin * 2));

  _dirtyX = dirtyX;
  _dirtyY = dirtyY;
  _dirtyW = dirtyW;
  _dirtyH = dirtyH;

  if ((dirtyW > 0) && (dirtyH > 0)) {
    clearRect(dirtyX, dirtyY, dirtyW, dirtyH);
  }

  // 5-pass dilation: 4 outline-color halos at axis-aligned ±1 offsets, then
  // a final center pass in the real colors. The 4 shifted draws form a 1px
  // ring of _outlineColor around every edge of the eye+brow+cut shape; the
  // center pass overdraws everything except that outer ring. If
  // _outlineColor == 0 (disabled), skip the halo and draw a single pass.
  if (_outlineColor != 0) {
    static const int16_t kHaloOffsets[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (uint8_t i = 0; i < 4; ++i) {
      const int16_t dx = kHaloOffsets[i][0];
      const int16_t dy = kHaloOffsets[i][1];
      drawEye(_left.curX,  _left.curY,  _curW, _curH, true,
              _outlineColor, _outlineColor, _outlineColor, dx, dy);
      drawEye(_right.curX, _right.curY, _curW, _curH, false,
              _outlineColor, _outlineColor, _outlineColor, dx, dy);
    }
  }
  drawEye(_left.curX,  _left.curY,  _curW, _curH, true,
          _mainColor, _browColor, _bgColor, 0, 0);
  drawEye(_right.curX, _right.curY, _curW, _curH, false,
          _mainColor, _browColor, _bgColor, 0, 0);

  _lastFrameX = frameX;
  _lastFrameY = frameY;
  _lastFrameW = frameW;
  _lastFrameH = frameH;
  _lastFrameValid = true;
  _dirty = true;
  _forceRedraw = false;
  return true;
}

void EyeEngine::setMood(EyeMood m) {
  _targetMood = m;
  _pendingMood = m;

  if (m == _renderMood) {
    if (m == MOOD_DEFAULT) {
      _nextOverlayScale = 0;
      _moodPhase = (_currentOverlayScale == 0U) ? MOOD_PHASE_STEADY : MOOD_PHASE_FADING_OUT;
      if (_currentOverlayScale == 0U) {
        _currentMood = MOOD_DEFAULT;
        _renderMood = MOOD_DEFAULT;
      }
    } else {
      _nextOverlayScale = 255;
      _moodPhase = (_currentOverlayScale == 255U) ? MOOD_PHASE_STEADY : MOOD_PHASE_FADING_IN;
    }
  } else if ((_currentOverlayScale > 0U) || (_nextOverlayScale > 0U) || (_renderMood != MOOD_DEFAULT)) {
    _nextOverlayScale = 0;
    _moodPhase = MOOD_PHASE_FADING_OUT;
  } else {
    _renderMood = m;
    if (m == MOOD_DEFAULT) {
      _currentMood = MOOD_DEFAULT;
      _nextOverlayScale = 0;
      _moodPhase = MOOD_PHASE_STEADY;
    } else {
      _nextOverlayScale = 255;
      _moodPhase = MOOD_PHASE_FADING_IN;
    }
  }

  syncWidthTarget();

  if (!_begun) {
    return;
  }

  const uint32_t now = millis();
  _lastInteractionMs = now;
  if (_idleWanderOn) {
    scheduleNextIdle(now);
  }

  // MOOD_THINK is currently a placeholder rendering as DEFAULT, so we no
  // longer auto-pan the eyes when it's selected. Keep _nextThinkMs cleared
  // so the per-tick check in update() short-circuits.
  _nextThinkMs = 0;

  if (m == MOOD_ERROR) {
    _errorShakeStep = 0;
    _nextErrorShakeMs = now + kErrorShakeStepMs;
  } else {
    _nextErrorShakeMs = 0;
    _errorShakeStep = 0;
  }

  // Re-schedule the next blink so the per-mood blink-feel kicks in
  // immediately on mood change (rather than waiting for whatever was
  // scheduled under the old mood). scheduleNextBlink() consults _targetMood
  // (set above) and handles MOOD_SLEEPING suppression.
  if (_autoblinkOn) {
    scheduleNextBlink(now);
  }

  _forceRedraw = true;
}

void EyeEngine::snapMood(EyeMood m) {
  _targetMood = m;
  _pendingMood = m;
  _renderMood = m;
  _currentMood = m;
  const uint8_t fullScale = (m == MOOD_DEFAULT) ? 0U : 255U;
  _currentOverlayScale = fullScale;
  _nextOverlayScale = fullScale;
  _moodPhase = MOOD_PHASE_STEADY;

  syncWidthTarget();

  if (!_begun) {
    return;
  }

  const uint32_t now = millis();
  _lastInteractionMs = now;
  if (_idleWanderOn) {
    scheduleNextIdle(now);
  }

  _nextThinkMs = 0;

  if (m == MOOD_ERROR) {
    _errorShakeStep = 0;
    _nextErrorShakeMs = now + kErrorShakeStepMs;
  } else {
    _nextErrorShakeMs = 0;
    _errorShakeStep = 0;
  }

  if (_autoblinkOn) {
    scheduleNextBlink(now);
  }

  _forceRedraw = true;
}

void EyeEngine::setPosition(EyePosition p) {
  setPositionInternal(p, true);
}

void EyeEngine::blink() {
  if (!_begun) {
    return;
  }

  const uint32_t now = millis();
  _blinkActive = true;
  _blinkReleaseMs = now + kBlinkHoldMs;
  _curH = kBlinkHeight;
  _lastInteractionMs = now;
  if (_autoblinkOn) {
    scheduleNextBlink(now);
  }
  if (_idleWanderOn) {
    scheduleNextIdle(now);
  }
  _forceRedraw = true;
}

void EyeEngine::setFramerate(uint8_t fps) {
  if (fps == 0U) {
    fps = 1U;
  }
  _fps = fps;
  _frameIntervalMs = static_cast<uint16_t>(1000U / fps);
  if (_frameIntervalMs == 0U) {
    _frameIntervalMs = 1U;
  }
}

void EyeEngine::setAutoblinker(bool on, uint16_t baseMs, uint16_t jitterMs) {
  _autoblinkOn = on;
  _autoblinkBaseMs = baseMs;
  _autoblinkJitterMs = jitterMs;
  if (!_begun) {
    return;
  }
  if (on) {
    scheduleNextBlink(millis());
  } else {
    _nextBlinkMs = 0;
  }
}

void EyeEngine::setIdleWander(bool on, uint16_t baseMs, uint16_t jitterMs) {
  _idleWanderOn = on;
  _idleBaseMs = baseMs;
  _idleJitterMs = jitterMs;
  if (!_begun) {
    return;
  }
  if (on) {
    scheduleNextIdle(millis());
  } else {
    _nextIdleMs = 0;
  }
}

void EyeEngine::setDims(uint8_t w, uint8_t h, uint8_t radius) {
  int16_t priorX = 0;
  int16_t priorY = 0;
  int16_t priorW = 0;
  int16_t priorH = 0;
  const bool capturePrior = _begun;
  if (capturePrior) {
    computeCurrentBounds(priorX, priorY, priorW, priorH);
  }

  const uint8_t maxRadius = static_cast<uint8_t>(((w < h) ? w : h) / 2U);
  if (radius > maxRadius) {
    radius = maxRadius;
  }

  _geom.baseW = w;
  _geom.baseH = h;
  _geom.borderRadius = radius;
  syncWidthTarget();

  if (!_begun) {
    _curW = _nextW;
    _curH = _nextH;
  } else if (capturePrior && (priorW > 0) && (priorH > 0)) {
    if (_havePendingPrior) {
      expandUnion(priorX, priorY, priorW, priorH,
                  _pendingPriorX, _pendingPriorY, _pendingPriorW, _pendingPriorH);
    } else {
      _pendingPriorX = priorX;
      _pendingPriorY = priorY;
      _pendingPriorW = priorW;
      _pendingPriorH = priorH;
      _havePendingPrior = true;
    }
  }

  _forceRedraw = true;
}

void EyeEngine::setMotionRange(int8_t x, int8_t y) {
  int16_t nextRangeX = x;
  int16_t nextRangeY = y;
  if (nextRangeX < 0) {
    nextRangeX = 0;
  } else if (nextRangeX > 63) {
    nextRangeX = 63;
  }
  if (nextRangeY < 0) {
    nextRangeY = 0;
  } else if (nextRangeY > 31) {
    nextRangeY = 31;
  }

  _geom.motionRangeX = static_cast<int8_t>(nextRangeX);
  _geom.motionRangeY = static_cast<int8_t>(nextRangeY);
  setPositionInternal(_position, false);
  _forceRedraw = true;
}

void EyeEngine::setMainColor(uint16_t color) {
  _mainColor = color;
  _forceRedraw = true;
}

void EyeEngine::setOutlineColor(uint16_t color) {
  _outlineColor = color;
  _forceRedraw = true;
}

uint16_t EyeEngine::getOutlineColor() const {
  return _outlineColor;
}

void EyeEngine::setCenters(int16_t leftCenterX, int16_t leftCenterY,
                           int16_t rightCenterX, int16_t rightCenterY) {
  int16_t priorX = 0;
  int16_t priorY = 0;
  int16_t priorW = 0;
  int16_t priorH = 0;
  const bool capturePrior = _begun;
  if (capturePrior) {
    computeCurrentBounds(priorX, priorY, priorW, priorH);
  }

  _geom.leftCenterX = leftCenterX;
  _geom.leftCenterY = leftCenterY;
  _geom.rightCenterX = rightCenterX;
  _geom.rightCenterY = rightCenterY;
  // Snap both X and Y to the new centers so we don't tween from the old pose.
  _left.curX = leftCenterX;
  _left.curY = leftCenterY;
  _left.nextX = leftCenterX;
  _left.nextY = leftCenterY;
  _right.curX = rightCenterX;
  _right.curY = rightCenterY;
  _right.nextX = rightCenterX;
  _right.nextY = rightCenterY;

  if (capturePrior && (priorW > 0) && (priorH > 0)) {
    if (_havePendingPrior) {
      expandUnion(priorX, priorY, priorW, priorH,
                  _pendingPriorX, _pendingPriorY, _pendingPriorW, _pendingPriorH);
    } else {
      _pendingPriorX = priorX;
      _pendingPriorY = priorY;
      _pendingPriorW = priorW;
      _pendingPriorH = priorH;
      _havePendingPrior = true;
    }
  }

  _forceRedraw = true;
}

void EyeEngine::forceRedraw() {
  _forceRedraw = true;
}

EyeMood EyeEngine::getMood() const {
  return _targetMood;
}

EyePosition EyeEngine::getPosition() const {
  return _position;
}

void EyeEngine::getDims(uint8_t& w, uint8_t& h, uint8_t& r) const {
  w = _geom.baseW;
  h = _geom.baseH;
  r = _geom.borderRadius;
}

void EyeEngine::getMotionRange(int8_t& x, int8_t& y) const {
  x = _geom.motionRangeX;
  y = _geom.motionRangeY;
}

uint16_t EyeEngine::getMainColor() const {
  return _mainColor;
}

bool EyeEngine::isAutoblinker() const {
  return _autoblinkOn;
}

void EyeEngine::getAutoblinkInterval(uint16_t& base, uint16_t& jitter) const {
  base = _autoblinkBaseMs;
  jitter = _autoblinkJitterMs;
}

bool EyeEngine::isIdleWander() const {
  return _idleWanderOn;
}

void EyeEngine::getIdleWanderInterval(uint16_t& base, uint16_t& jitter) const {
  base = _idleBaseMs;
  jitter = _idleJitterMs;
}

uint8_t EyeEngine::getFramerate() const {
  return _fps;
}

int16_t EyeEngine::gazeOffsetX() const {
  return static_cast<int16_t>(_left.curX - _geom.leftCenterX);
}

int16_t EyeEngine::gazeOffsetY() const {
  return static_cast<int16_t>(_left.curY - _geom.leftCenterY);
}

void EyeEngine::getDirtyRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h) const {
  x = _dirtyX;
  y = _dirtyY;
  w = _dirtyW;
  h = _dirtyH;
}

bool EyeEngine::isDirty() const {
  return _dirty;
}

void EyeEngine::clearDirty() {
  _dirty = false;
}

bool EyeEngine::isAnimating() const {
  if (!_begun || !_gfx) {
    return _forceRedraw;
  }

  if (_forceRedraw ||
      (_moodPhase != MOOD_PHASE_STEADY) ||
      (_left.curX != _left.nextX) || (_left.curY != _left.nextY) ||
      (_right.curX != _right.nextX) || (_right.curY != _right.nextY) ||
      (_curW != _nextW) || (_curH != _nextH) ||
      (_currentOverlayScale != _nextOverlayScale)) {
    return true;
  }

  const uint32_t now = millis();
  const uint32_t horizon = now + _frameIntervalMs;

  if (_autoblinkOn && (_nextBlinkMs != 0U) && timeReached(horizon, _nextBlinkMs)) {
    return true;
  }
  if (_blinkActive && (_blinkReleaseMs != 0U) && timeReached(horizon, _blinkReleaseMs)) {
    return true;
  }
  const bool errorMode = (_targetMood == MOOD_ERROR) ||
                         (_renderMood == MOOD_ERROR) ||
                         (_currentMood == MOOD_ERROR);
  if (errorMode && (_nextErrorShakeMs != 0U) && timeReached(horizon, _nextErrorShakeMs)) {
    return true;
  }
  if (_idleWanderOn && (_nextIdleMs != 0U) && timeReached(horizon, _nextIdleMs)) {
    return true;
  }
  return false;
}

void EyeEngine::drawEye(int16_t eyeX, int16_t eyeY, uint8_t eyeW, uint8_t eyeH, bool isLeft,
                        uint16_t eyeColor, uint16_t browColor, uint16_t cutColor,
                        int16_t ox, int16_t oy) const {
  if (!_gfx || (eyeW == 0U) || (eyeH == 0U)) {
    return;
  }

  // Bake the halo offset into x0/y0/x1/y1 so every shape (round-rect, cut,
  // lash, brow) shifts together. eyeX/eyeY stay un-shifted so the angry-brow
  // gaze-lag math (which compares this eye's gaze to _browGazeX/Y) still
  // produces the same browDx/browDy on every pass.
  const int16_t x0 = static_cast<int16_t>(eyeX - (eyeW / 2) + ox);
  const int16_t y0 = static_cast<int16_t>(eyeY - (eyeH / 2) + oy);
  const int16_t x1 = static_cast<int16_t>(x0 + eyeW - 1);
  const int16_t y1 = static_cast<int16_t>(y0 + eyeH - 1);

  uint8_t radius = _geom.borderRadius;
  if (radius > (eyeW / 2U)) {
    radius = static_cast<uint8_t>(eyeW / 2U);
  }
  if (radius > (eyeH / 2U)) {
    radius = static_cast<uint8_t>(eyeH / 2U);
  }

  // MOOD_SLEEPING replaces the eye with a single dome-down arc and we
  // don't want the rounded-rect base (or its outline-halo ring) showing
  // through as an "eye shadow". Skip the base fill so only the arc and
  // its own halo get drawn — the dirty-rect clearRect already wiped the
  // area to the mascot bg.
  if (_renderMood != MOOD_SLEEPING && _renderMood != MOOD_THINK) {
    _gfx->fillRoundRect(x0, y0, eyeW, eyeH, radius, eyeColor);
  }

  const uint8_t scale = _currentOverlayScale;
  if ((scale == 0U) || (_renderMood == MOOD_DEFAULT)) {
    return;
  }

  (void)cutColor;  // currently always equal to _bgColor — kept as a param so
                   // callers can later request a tinted "outline pass" without
                   // another signature change.

  switch (_renderMood) {
    case MOOD_BORED: {
      // Erase the top half of the eye with mascot-bg so the eye reads as a
      // shorter, flat-topped half-blink (rounded bottom only).  Then draw
      // a thin eye-colored lash on the OUTER side of each eye only,
      // sitting just below the cut line.
      const int16_t coverH = scale255(static_cast<int16_t>(eyeH / 2U), scale);
      if (coverH > 0) {
        _gfx->fillRect(x0, y0, eyeW, static_cast<uint16_t>(coverH), cutColor);

        const int16_t lashThickness =
            (coverH < kBoredLashThickness) ? coverH : kBoredLashThickness;
        // Lash bottom edge sits 3 px BELOW the cut line so it overlaps
        // the top of the visible eye (cleaner read of the lash line).
        const int16_t lashY = static_cast<int16_t>(y0 + coverH - lashThickness + 3);
        int16_t lashX = 0;
        if (isLeft) {
          lashX = static_cast<int16_t>(x0 - kBoredLashExtend);
        } else {
          lashX = static_cast<int16_t>(x1 + 1);
        }
        _gfx->fillRect(lashX, lashY, static_cast<uint16_t>(kBoredLashExtend),
                       static_cast<uint16_t>(lashThickness), eyeColor);
      }
      break;
    }
    case MOOD_ANNOYED: {
      // Like BORED but with lashes on BOTH sides of each eye — reads as
      // a more active half-lidded glare instead of zoned-out boredom.
      const int16_t coverH = scale255(static_cast<int16_t>(eyeH / 2U), scale);
      if (coverH > 0) {
        _gfx->fillRect(x0, y0, eyeW, static_cast<uint16_t>(coverH), cutColor);

        const int16_t lashThickness =
            (coverH < kBoredLashThickness) ? coverH : kBoredLashThickness;
        const int16_t lashY = static_cast<int16_t>(y0 + coverH - lashThickness + 3);
        const int16_t leftLashX = static_cast<int16_t>(x0 - kBoredLashExtend);
        const int16_t rightLashX = static_cast<int16_t>(x1 + 1);
        _gfx->fillRect(leftLashX, lashY, static_cast<uint16_t>(kBoredLashExtend),
                       static_cast<uint16_t>(lashThickness), eyeColor);
        _gfx->fillRect(rightLashX, lashY, static_cast<uint16_t>(kBoredLashExtend),
                       static_cast<uint16_t>(lashThickness), eyeColor);
      }
      break;
    }
    case MOOD_TIRED: {
      // Outer-corner droop. cutW = 2/3 eye-width, cutH = 1/3 eye-height
      // gives a gentle baseline slope (~1.5). Then a second triangle
      // bulges the cut's midpoint INTO the eye so the eyelid edge reads
      // as a curved droop instead of a straight diagonal — the heavy-lid
      // look of "sleepy" eyes.
      const int16_t cutW = scale255(static_cast<int16_t>((eyeW * 2U + 2U) / 3U), scale);
      const int16_t cutH = scale255(static_cast<int16_t>((eyeH + 2U) / 3U), scale);
      const int16_t bulge = (cutH > 8) ? static_cast<int16_t>(cutH / 8) : 1;
      if (isLeft) {
        // Main outer-corner cut (straight diagonal hypotenuse).
        _gfx->fillTriangle(x0, y0, static_cast<int16_t>(x0 + cutW), y0,
                           x0, static_cast<int16_t>(y0 + cutH), cutColor);
        // Bulge: extra triangle whose apex sits inside the eye,
        // pulling the visible eyelid edge into a smooth curve.
        const int16_t mx = static_cast<int16_t>(x0 + cutW / 2 + bulge);
        const int16_t my = static_cast<int16_t>(y0 + cutH / 2 + bulge);
        _gfx->fillTriangle(static_cast<int16_t>(x0 + cutW), y0,
                           mx, my,
                           x0, static_cast<int16_t>(y0 + cutH), cutColor);
      } else {
        _gfx->fillTriangle(x1, y0, static_cast<int16_t>(x1 - cutW), y0,
                           x1, static_cast<int16_t>(y0 + cutH), cutColor);
        const int16_t mx = static_cast<int16_t>(x1 - cutW / 2 - bulge);
        const int16_t my = static_cast<int16_t>(y0 + cutH / 2 + bulge);
        _gfx->fillTriangle(static_cast<int16_t>(x1 - cutW), y0,
                           mx, my,
                           x1, static_cast<int16_t>(y0 + cutH), cutColor);
      }
      break;
    }
    case MOOD_ANGRY: {
      // Shallower inner-corner cut (slope ~0.75 vs the original ~3.6).
      // cutW covers ~80% of eyeW; cutH ~17% of eyeH.
      const int16_t cutW = scale255(static_cast<int16_t>((eyeW * 4U + 4U) / 5U), scale);
      const int16_t cutH = scale255(static_cast<int16_t>((eyeH + 5U) / 6U), scale);
      // Brow gaze lag: shift brow position by (brow_gaze - eye_gaze) so the
      // brow trails the eye and only catches up over several frames.
      const int16_t eyeAnchorX = isLeft ? _geom.leftCenterX : _geom.rightCenterX;
      const int16_t eyeAnchorY = isLeft ? _geom.leftCenterY : _geom.rightCenterY;
      const int16_t thisEyeGazeX = static_cast<int16_t>(eyeX - eyeAnchorX);
      const int16_t thisEyeGazeY = static_cast<int16_t>(eyeY - eyeAnchorY);
      const int16_t browDx = static_cast<int16_t>(_browGazeX - thisEyeGazeX);
      const int16_t browDy = static_cast<int16_t>(_browGazeY - thisEyeGazeY);
      // Bring brows closer together — inner end extends inward (toward the
      // nose bridge) by 1/4 of the inter-eye gap, which halves the empty
      // space between the two brows.
      const int16_t centerGap = static_cast<int16_t>(_geom.rightCenterX - _geom.leftCenterX);
      const int16_t innerBaseGap = static_cast<int16_t>(centerGap - static_cast<int16_t>(_geom.baseW));
      const int16_t browInset = (innerBaseGap > 0) ? static_cast<int16_t>(innerBaseGap / 4) : 0;
      // Brow sits above the eye, slopes parallel to the cut hypotenuse,
      // and runs from the inner edge of the eye to just past the outer edge.
      const int16_t innerTopY = static_cast<int16_t>(y0 - kAngryBrowGap - kAngryBrowThickness + browDy);
      const int16_t outerTopY = static_cast<int16_t>(innerTopY - kAngryBrowRise);
      if (isLeft) {
        _gfx->fillTriangle(x1, y0, static_cast<int16_t>(x1 - cutW), y0,
                           x1, static_cast<int16_t>(y0 + cutH), cutColor);
        // Outer (left, lower x) is HIGHER; inner (right) is LOWER. Inner end
        // extends inward by browInset px past the eye's inner edge.
        const int16_t bxOuter = static_cast<int16_t>(x0 - kAngryBrowExtend + browDx);
        const int16_t bxInner = static_cast<int16_t>(x1 + browInset + browDx);
        const int16_t outerBotY = static_cast<int16_t>(outerTopY + kAngryBrowThickness);
        const int16_t innerBotY = static_cast<int16_t>(innerTopY + kAngryBrowThickness);
        _gfx->fillTriangle(bxOuter, outerTopY, bxInner, innerTopY, bxInner, innerBotY, browColor);
        _gfx->fillTriangle(bxOuter, outerTopY, bxInner, innerBotY, bxOuter, outerBotY, browColor);
      } else {
        _gfx->fillTriangle(x0, y0, static_cast<int16_t>(x0 + cutW), y0,
                           x0, static_cast<int16_t>(y0 + cutH), cutColor);
        // Inner (left) is LOWER; outer (right, higher x) is HIGHER. Inner
        // end extends inward by browInset past the eye's inner edge.
        const int16_t bxInner = static_cast<int16_t>(x0 - browInset + browDx);
        const int16_t bxOuter = static_cast<int16_t>(x1 + kAngryBrowExtend + browDx);
        const int16_t outerBotY = static_cast<int16_t>(outerTopY + kAngryBrowThickness);
        const int16_t innerBotY = static_cast<int16_t>(innerTopY + kAngryBrowThickness);
        _gfx->fillTriangle(bxInner, innerTopY, bxOuter, outerTopY, bxOuter, outerBotY, browColor);
        _gfx->fillTriangle(bxInner, innerTopY, bxOuter, outerBotY, bxInner, innerBotY, browColor);
      }
      break;
    }
    case MOOD_HAPPY: {
      const int16_t halfH = static_cast<int16_t>((eyeH + 1U) / 2U);
      const int16_t coverH = scale255(halfH, scale);
      if (coverH > 0) {
        uint8_t coverRadius = radius;
        if (coverRadius > static_cast<uint8_t>(coverH / 2)) {
          coverRadius = static_cast<uint8_t>(coverH / 2);
        }
        _gfx->fillRoundRect(x0, static_cast<int16_t>(y1 - coverH + 1), eyeW,
                            static_cast<uint16_t>(coverH), coverRadius, cutColor);
      }
      break;
    }
    case MOOD_SAD: {
      // Mirror of ANGRY: brows angle UP toward the nose (inner end HIGH,
      // outer end LOW but still above the eye) and the inner-corner eye
      // cut is mirrored to the OUTER corner so the eye's top edge slopes
      // down-and-out to match the brow — classic concerned /\ shape.
      const int16_t cutW = scale255(static_cast<int16_t>((eyeW * 4U + 4U) / 5U), scale);
      const int16_t cutH = scale255(static_cast<int16_t>((eyeH + 5U) / 6U), scale);
      const int16_t eyeAnchorX = isLeft ? _geom.leftCenterX : _geom.rightCenterX;
      const int16_t eyeAnchorY = isLeft ? _geom.leftCenterY : _geom.rightCenterY;
      const int16_t thisEyeGazeX = static_cast<int16_t>(eyeX - eyeAnchorX);
      const int16_t thisEyeGazeY = static_cast<int16_t>(eyeY - eyeAnchorY);
      const int16_t browDx = static_cast<int16_t>(_browGazeX - thisEyeGazeX);
      const int16_t browDy = static_cast<int16_t>(_browGazeY - thisEyeGazeY);
      const int16_t centerGap = static_cast<int16_t>(_geom.rightCenterX - _geom.leftCenterX);
      const int16_t innerBaseGap = static_cast<int16_t>(centerGap - static_cast<int16_t>(_geom.baseW));
      const int16_t browInset = (innerBaseGap > 0) ? static_cast<int16_t>(innerBaseGap / 4) : 0;
      // Anchor the LOW end (outer) just above the eye, then lift the inner
      // end by kAngryBrowRise. This keeps the entire brow above the eye
      // (no overlap artifacts) while still showing the /\ slope.
      const int16_t outerTopY = static_cast<int16_t>(y0 - kAngryBrowGap - kAngryBrowThickness + browDy);
      const int16_t innerTopY = static_cast<int16_t>(outerTopY - kAngryBrowRise);
      if (isLeft) {
        // Cut on OUTER top corner (x0, y0) sloping out-and-down.
        _gfx->fillTriangle(x0, y0, static_cast<int16_t>(x0 + cutW), y0,
                           x0, static_cast<int16_t>(y0 + cutH), cutColor);
        const int16_t bxOuter = static_cast<int16_t>(x0 - kAngryBrowExtend + browDx);
        const int16_t bxInner = static_cast<int16_t>(x1 + browInset + browDx);
        const int16_t outerBotY = static_cast<int16_t>(outerTopY + kAngryBrowThickness);
        const int16_t innerBotY = static_cast<int16_t>(innerTopY + kAngryBrowThickness);
        _gfx->fillTriangle(bxOuter, outerTopY, bxInner, innerTopY, bxInner, innerBotY, browColor);
        _gfx->fillTriangle(bxOuter, outerTopY, bxInner, innerBotY, bxOuter, outerBotY, browColor);
      } else {
        // Cut on OUTER top corner (x1, y0) sloping out-and-down.
        _gfx->fillTriangle(x1, y0, static_cast<int16_t>(x1 - cutW), y0,
                           x1, static_cast<int16_t>(y0 + cutH), cutColor);
        const int16_t bxInner = static_cast<int16_t>(x0 - browInset + browDx);
        const int16_t bxOuter = static_cast<int16_t>(x1 + kAngryBrowExtend + browDx);
        const int16_t outerBotY = static_cast<int16_t>(outerTopY + kAngryBrowThickness);
        const int16_t innerBotY = static_cast<int16_t>(innerTopY + kAngryBrowThickness);
        _gfx->fillTriangle(bxInner, innerTopY, bxOuter, outerTopY, bxOuter, outerBotY, browColor);
        _gfx->fillTriangle(bxInner, innerTopY, bxOuter, outerBotY, bxInner, innerBotY, browColor);
      }
      break;
    }
    case MOOD_ERROR: {
      // X-cross of two skinny rotated pills, both centered at the eye
      // center (shifted outward by kErrorEyeOffset so the two X's read as
      // more separated than the default eye spacing). Top of one pill
      // tilts LEFT (-30°), top of the other tilts RIGHT (+30°). Pill
      // thickness is 3/4 of the original eye width so it reads as
      // "skinnier than default". Length is 4/3 of the eye height so the X
      // extends slightly past the base eye footprint vertically.
      // Erase the base eye first — the cross lives directly on the bg.
      _gfx->fillRect(x0, y0, eyeW, eyeH, cutColor);
      const int16_t thickness = static_cast<int16_t>((eyeW * 3U + 2U) / 4U);
      const int16_t fullLen   = static_cast<int16_t>((eyeH * 4U + 2U) / 3U);
      const int16_t length    = scale255(fullLen, scale);
      if (length < thickness) {
        break;  // fade-in not far enough to render the pills cleanly
      }
      const int16_t baseCx = static_cast<int16_t>((x0 + x1) / 2);
      const int16_t baseCy = static_cast<int16_t>((y0 + y1) / 2);
      // Push left eye further left, right eye further right.
      // Then add the per-frame shake offset so both eyes vibrate in sync.
      const uint8_t step = static_cast<uint8_t>(_errorShakeStep & 0x07U);
      const int16_t shakeDx = static_cast<int16_t>(kErrorShakeDx[step]);
      const int16_t shakeDy = static_cast<int16_t>(kErrorShakeDy[step]);
      const int16_t cx = static_cast<int16_t>(
          baseCx + (isLeft ? -kErrorEyeOffset : +kErrorEyeOffset) + shakeDx);
      const int16_t cy = static_cast<int16_t>(baseCy + shakeDy);
      const uint16_t errorColor = 0xF800;  // pure RGB565 red — angry alarm look
      fillTiltedPill(_gfx, cx, cy, length, thickness, /*tiltLeft=*/true, errorColor);
      fillTiltedPill(_gfx, cx, cy, length, thickness, /*tiltLeft=*/false, errorColor);
      break;
    }
    case MOOD_THINK: {
      // Base starting animation: both eyes closed — same dome-down arc (∪)
      // as MOOD_SLEEPING. The base rounded-rect is skipped above for THINK
      // so only the arc renders. Visual indicator (dots/?/bubble) layered
      // on top is TBD per Nick iteration.
      uint8_t eyeRcorner = _geom.borderRadius;
      if (eyeRcorner > (eyeW / 2U)) {
        eyeRcorner = static_cast<uint8_t>(eyeW / 2U);
      }
      if (eyeRcorner > (eyeH / 2U)) {
        eyeRcorner = static_cast<uint8_t>(eyeH / 2U);
      }
      constexpr int16_t kThinkArcGap = 5;
      int16_t rOuterInt = static_cast<int16_t>(eyeRcorner + kThinkArcGap);
      const int16_t maxR = static_cast<int16_t>(eyeW / 2U);
      if (rOuterInt > maxR) {
        rOuterInt = maxR;
      }
      if (rOuterInt < 2) {
        rOuterInt = 2;
      }
      const int16_t bcx = static_cast<int16_t>((x0 + x1) / 2);
      const float rOuter = static_cast<float>(rOuterInt);
      float rInner = rOuter - static_cast<float>(kAngryBrowThickness);
      if (rInner < 1.0f) {
        rInner = 1.0f;
      }
      const int16_t bcy = static_cast<int16_t>(y1 - rOuterInt);
      const int16_t thickness = scale255(static_cast<int16_t>(rOuter - rInner), scale);
      const float rInnerScaled = static_cast<float>(rOuter) - static_cast<float>(thickness);
      _gfx->fillArc(bcx, bcy, rOuter, rInnerScaled, -1.0f, 181.0f, eyeColor);
      break;
    }
    case MOOD_SURPRISED: {
      // OUTER ring drawn around the eye (eyeColor) with a bg gap between eye
      // edge and ring. Then we paint the LEFT, RIGHT, and BOTTOM straight
      // segments of that ring back to bg — leaves just the top bar + the 4
      // rounded corner brackets. Small bg pupil dot in the eye center.
      constexpr int16_t kSurprisedGap = 5;                               // 5px gap
      constexpr int16_t kSurprisedRingThickness = 5;
      constexpr int16_t kSurprisedRingExtent = kSurprisedGap + kSurprisedRingThickness;  // 10px past eye
      constexpr int16_t kSurprisedPupilR = 3;
      const int16_t cx = static_cast<int16_t>((x0 + x1) / 2);
      const int16_t cy = static_cast<int16_t>((y0 + y1) / 2);

      // Step 1: outer rect (eyeColor) — bigger rounded rect surrounding eye+gap.
      const int16_t outerX = static_cast<int16_t>(x0 - kSurprisedRingExtent);
      const int16_t outerY = static_cast<int16_t>(y0 - kSurprisedRingExtent);
      const int16_t outerW = static_cast<int16_t>(eyeW + (2 * kSurprisedRingExtent));
      const int16_t outerH = static_cast<int16_t>(eyeH + (2 * kSurprisedRingExtent));
      const int16_t outerR = static_cast<int16_t>(radius + kSurprisedRingExtent);
      _gfx->fillRoundRect(outerX, outerY,
                          static_cast<uint16_t>(outerW),
                          static_cast<uint16_t>(outerH),
                          static_cast<uint8_t>(outerR), eyeColor);

      // Step 2: gap rect (cutColor) — slightly-smaller rounded rect. Covers
      // the gap area AND erases the eye (which we restore in step 3).
      const int16_t gapX = static_cast<int16_t>(x0 - kSurprisedGap);
      const int16_t gapY = static_cast<int16_t>(y0 - kSurprisedGap);
      const int16_t gapW = static_cast<int16_t>(eyeW + (2 * kSurprisedGap));
      const int16_t gapH = static_cast<int16_t>(eyeH + (2 * kSurprisedGap));
      const int16_t gapR = static_cast<int16_t>(radius + kSurprisedGap);
      _gfx->fillRoundRect(gapX, gapY,
                          static_cast<uint16_t>(gapW),
                          static_cast<uint16_t>(gapH),
                          static_cast<uint8_t>(gapR), cutColor);

      // Step 3: restore the eye fill (eyeColor) — same shape as the base eye.
      _gfx->fillRoundRect(x0, y0, eyeW, eyeH, radius, eyeColor);

      // Step 4: remove L/R/B straight segments of the outer ring by restoring
      // the underlying mascot bg pixels there (clearRect blits from _bgSource
      // so the strips truly "disappear" against the face rather than showing
      // as flat-color blobs). Leaves the top bar + 4 rounded corner brackets.
      const int16_t cornerSize = outerR;
      const int16_t stripV_H = static_cast<int16_t>(outerH - (2 * cornerSize));
      if (stripV_H > 0) {
        // LEFT straight strip of outer ring
        clearRect(outerX,
                  static_cast<int16_t>(outerY + cornerSize),
                  kSurprisedRingThickness,
                  stripV_H);
        // RIGHT straight strip
        clearRect(static_cast<int16_t>(outerX + outerW - kSurprisedRingThickness),
                  static_cast<int16_t>(outerY + cornerSize),
                  kSurprisedRingThickness,
                  stripV_H);
      }
      const int16_t stripH_W = static_cast<int16_t>(outerW - (2 * cornerSize));
      if (stripH_W > 0) {
        // BOTTOM straight strip
        clearRect(static_cast<int16_t>(outerX + cornerSize),
                  static_cast<int16_t>(outerY + outerH - kSurprisedRingThickness),
                  stripH_W,
                  kSurprisedRingThickness);
      }

      // Step 4b: trim the 2 BOTTOM corner brackets from 90° to 45° with an
      // asymmetric rule based on whether the corner is on the INSIDE (facing
      // the other eye) or OUTSIDE (facing away). Top corners stay full 90°.
      //   INSIDE  bottom corner → remove 45° from the TOP    of the arc
      //   OUTSIDE bottom corner → remove 45° from the BOTTOM of the arc
      // Inside vs outside is per-eye via isLeft:
      //   LEFT  eye: INSIDE = BR    OUTSIDE = BL
      //   RIGHT eye: INSIDE = BL    OUTSIDE = BR
      {
        const int16_t innerR = static_cast<int16_t>(radius + kSurprisedGap);
        const int16_t cxL = static_cast<int16_t>(outerX + outerR);
        const int16_t cxR = static_cast<int16_t>(outerX + outerW - outerR - 1);
        const int16_t cyT = static_cast<int16_t>(outerY + outerR);
        const int16_t cyB = static_cast<int16_t>(outerY + outerH - outerR - 1);
        const int32_t rOutSq = static_cast<int32_t>(outerR + 1) * (outerR + 1);
        const int32_t rInSq  = static_cast<int32_t>(innerR) * innerR;
        const int16_t half = static_cast<int16_t>((outerR / 2) + 2);

        // Clear pixels in the ring band [innerR, outerR] around (cx_, cy_)
        // whose atan2 angle (screen-polar, east=0°, south=90°, west=180°,
        // north=270°) falls inside [tMin, tMax]. Searches local bbox only.
        auto clearWedge = [&](int16_t cx_, int16_t cy_,
                              int16_t xMin, int16_t xMax,
                              int16_t yMin, int16_t yMax,
                              float tMin, float tMax) {
          for (int16_t dy = yMin; dy <= yMax; ++dy) {
            for (int16_t dx = xMin; dx <= xMax; ++dx) {
              const int32_t d2 =
                  (static_cast<int32_t>(dx) * dx) + (static_cast<int32_t>(dy) * dy);
              if ((d2 < rInSq) || (d2 > rOutSq)) {
                continue;
              }
              float t = atan2f(static_cast<float>(dy), static_cast<float>(dx)) *
                        (180.0f / 3.14159265f);
              if (t < 0.0f) {
                t += 360.0f;
              }
              if ((t >= tMin) && (t <= tMax)) {
                clearRect(static_cast<int16_t>(cx_ + dx),
                          static_cast<int16_t>(cy_ + dy), 1, 1);
              }
            }
          }
        };

        if (isLeft) {
          // LEFT eye — outside = BL (remove bottom); inside = BR (remove top)
          // BL outside-bottom: arc 90°-180°. "Bottom" = near S. Remove 90°-135°.
          clearWedge(cxL, cyB,
                     static_cast<int16_t>(-outerR), static_cast<int16_t>(0),
                     static_cast<int16_t>(0),      outerR,
                     89.0f, 136.0f);
          // BR inside-bottom: arc 0°-90°. "Top" = near E. Remove 0°-45°.
          clearWedge(cxR, cyB,
                     static_cast<int16_t>(0), outerR,
                     static_cast<int16_t>(0), outerR,
                     -1.0f, 46.0f);
        } else {
          // RIGHT eye — inside = BL (remove top); outside = BR (remove bottom)
          // BL inside-bottom: arc 90°-180°. "Top" = near W. Remove 135°-180°.
          clearWedge(cxL, cyB,
                     static_cast<int16_t>(-outerR), static_cast<int16_t>(0),
                     static_cast<int16_t>(0),       outerR,
                     134.0f, 181.0f);
          // BR outside-bottom: arc 0°-90°. "Bottom" = near S. Remove 45°-90°.
          clearWedge(cxR, cyB,
                     static_cast<int16_t>(0), outerR,
                     static_cast<int16_t>(0), outerR,
                     44.0f, 91.0f);
        }

        // Step 4c: round end-caps. Every exposed stroke endpoint gets a
        // small filled disc of eyeColor centered on the stroke's centerline
        // at the endpoint, with radius = half stroke thickness. This rounds
        // off the otherwise-square arc cuts.
        //   2 caps from top corners' free ends (W of TL, E of TR — where the
        //     L/R side strips were removed).
        //   4 caps from bottom-corner half-arcs (each kept half has 2 ends).
        // ring band is d ∈ {16..20} (5px), centerline at d=18.
        const float rCenter = 18.0f;
        const int16_t capR = 2;
        auto drawCap = [&](int16_t cx_, int16_t cy_, float thetaDeg) {
          const float rad = thetaDeg * (3.14159265f / 180.0f);
          const int16_t px = static_cast<int16_t>(
              roundf(static_cast<float>(cx_) + (rCenter * cosf(rad))));
          const int16_t py = static_cast<int16_t>(
              roundf(static_cast<float>(cy_) + (rCenter * sinf(rad))));
          _gfx->fillCircle(px, py, capR, eyeColor);
        };
        // Top corners: free endpoints at W and E (where side strips were).
        drawCap(cxL, cyT, 180.0f);
        drawCap(cxR, cyT, 0.0f);
        if (isLeft) {
          // LEFT eye bottom: BL kept 136°-180°, BR kept 46°-90°.
          drawCap(cxL, cyB, 136.0f);
          drawCap(cxL, cyB, 180.0f);
          drawCap(cxR, cyB, 46.0f);
          drawCap(cxR, cyB, 90.0f);
        } else {
          // RIGHT eye bottom: BL kept 90°-134°, BR kept 0°-44°.
          drawCap(cxL, cyB, 90.0f);
          drawCap(cxL, cyB, 134.0f);
          drawCap(cxR, cyB, 0.0f);
          drawCap(cxR, cyB, 44.0f);
        }
      }

      // Step 5: small bg pupil dot in the eye center, fade-in via scale.
      int16_t pupilR = scale255(kSurprisedPupilR, scale);
      if (pupilR < 1) {
        pupilR = 1;
      }
      _gfx->fillCircle(cx, cy, pupilR, cutColor);
      break;
    }
    case MOOD_SCARED: {
      // Small bg-colored "pupil" circle in the eye center reads as wide-eyed
      // shock/terror. The base eye is already drawn; we just punch a hole.
      const int16_t cx = static_cast<int16_t>((x0 + x1) / 2);
      const int16_t cy = static_cast<int16_t>((y0 + y1) / 2);
      int16_t minDim = (eyeW < eyeH) ? static_cast<int16_t>(eyeW) : static_cast<int16_t>(eyeH);
      int16_t pupilR = scale255(static_cast<int16_t>(minDim / 4), scale);
      if (pupilR < 1) {
        pupilR = 1;
      }
      _gfx->fillCircle(cx, cy, pupilR, cutColor);

      // Half-dome brow above each eye. Radius = R_corner + kScaredBrowGap,
      // so the curve matches the curvature an offset-border at that gap
      // distance would naturally have at the eye's rounded corners. The
      // dome sits entirely ABOVE the eye (chord at y = y0 - gap), so it
      // never paints inside the eye region. Same gap as the angry/sad
      // brows' high end (kAngryBrowGap + kAngryBrowRise).
      uint8_t eyeRcorner = _geom.borderRadius;
      if (eyeRcorner > (eyeW / 2U)) {
        eyeRcorner = static_cast<uint8_t>(eyeW / 2U);
      }
      if (eyeRcorner > (eyeH / 2U)) {
        eyeRcorner = static_cast<uint8_t>(eyeH / 2U);
      }
      constexpr int16_t kScaredBrowGap = 8;
      const int16_t bcx = cx;
      const int16_t bcy = static_cast<int16_t>(y0 - kScaredBrowGap);
      const float rOuter = static_cast<float>(eyeRcorner + kScaredBrowGap);
      float rInner = rOuter - static_cast<float>(kAngryBrowThickness);
      if (rInner < 1.0f) {
        rInner = 1.0f;
      }
      _gfx->fillArc(bcx, bcy, rOuter, rInner, 181.0f, 359.0f, browColor);
      break;
    }
    case MOOD_WINK: {
      // Left eye renders normal (no overlay); right eye is "closed": erase
      // the eye, then draw a thin horizontal line at the eye's vertical
      // center. The line tweens in via scale (grows from 0 width).
      if (!isLeft) {
        _gfx->fillRect(x0, y0, eyeW, eyeH, cutColor);
        const int16_t lineW = scale255(static_cast<int16_t>(eyeW), scale);
        const int16_t lineY = static_cast<int16_t>((y0 + y1) / 2 - (kClosedEyeLineThickness / 2));
        const int16_t lineX = static_cast<int16_t>(x0 + (static_cast<int16_t>(eyeW) - lineW) / 2);
        if (lineW > 0) {
          _gfx->fillRect(lineX, lineY, static_cast<uint16_t>(lineW),
                         static_cast<uint16_t>(kClosedEyeLineThickness), eyeColor);
        }
      }
      break;
    }
    case MOOD_SLEEPING: {
      // Both eyes closed — base rounded-rect is skipped above. Draw a
      // dome-down arc (∪) at the bottom of where each eye was, same
      // shape/style as the CONFUSED left eyebrow.
      uint8_t eyeRcorner = _geom.borderRadius;
      if (eyeRcorner > (eyeW / 2U)) {
        eyeRcorner = static_cast<uint8_t>(eyeW / 2U);
      }
      if (eyeRcorner > (eyeH / 2U)) {
        eyeRcorner = static_cast<uint8_t>(eyeH / 2U);
      }
      constexpr int16_t kSleepingArcGap = 5;
      // Target radius matches the CONFUSED left brow, but clamped so the
      // arc's diameter never exceeds the eye width — otherwise the arc
      // spills past the eye edges and leaves stale arc-colored pixels
      // when the eye gaze-wanders.  kEyeOutline already pads the dirty
      // rect by 1, which absorbs the +1px halo on each side.
      int16_t rOuterInt = static_cast<int16_t>(eyeRcorner + kSleepingArcGap);
      const int16_t maxR = static_cast<int16_t>(eyeW / 2U);
      if (rOuterInt > maxR) {
        rOuterInt = maxR;
      }
      if (rOuterInt < 2) {
        rOuterInt = 2;
      }
      const int16_t bcx = static_cast<int16_t>((x0 + x1) / 2);
      const float rOuter = static_cast<float>(rOuterInt);
      float rInner = rOuter - static_cast<float>(kAngryBrowThickness);
      if (rInner < 1.0f) {
        rInner = 1.0f;
      }
      // Position so the bottom of the arc band sits at y1 (eye bottom).
      const int16_t bcy = static_cast<int16_t>(y1 - rOuterInt);
      // Tween the band thickness with scale so the arc grows in / out
      // smoothly on mood enter/leave.
      const int16_t thickness = scale255(static_cast<int16_t>(rOuter - rInner), scale);
      const float rInnerScaled = static_cast<float>(rOuter) - static_cast<float>(thickness);
      _gfx->fillArc(bcx, bcy, rOuter, rInnerScaled, -1.0f, 181.0f, eyeColor);
      break;
    }
    case MOOD_LOVE: {
      // Heart shape: erase the eye, draw two filled circles at upper-left
      // and upper-right (the heart's lobes), then a downward triangle
      // pointing to the bottom center. Lobes bulge above the eye top by
      // kLoveBulge px — computeEyeBounds adds matching pad.
      _gfx->fillRect(x0, y0, eyeW, eyeH, cutColor);
      const int16_t lobeR = scale255(static_cast<int16_t>((eyeW + 2) / 4), scale);
      if (lobeR < 1) {
        break;
      }
      const int16_t cy = static_cast<int16_t>(y0 + lobeR - kLoveBulge);
      const int16_t leftCx = static_cast<int16_t>(x0 + lobeR);
      const int16_t rightCx = static_cast<int16_t>(x1 - lobeR);
      _gfx->fillCircle(leftCx, cy, lobeR, eyeColor);
      _gfx->fillCircle(rightCx, cy, lobeR, eyeColor);
      // Triangle from outer-bottom of each lobe to the bottom-center of eye.
      const int16_t triTopY = cy;
      const int16_t triLeftX = static_cast<int16_t>(leftCx - lobeR);
      const int16_t triRightX = static_cast<int16_t>(rightCx + lobeR);
      const int16_t triBotX = static_cast<int16_t>((x0 + x1) / 2);
      const int16_t triBotY = y1;
      _gfx->fillTriangle(triLeftX, triTopY, triRightX, triTopY, triBotX, triBotY, eyeColor);
      break;
    }
    case MOOD_CONFUSED: {
      // SCARED-based asymmetric "huh??" look:
      //   - No pupil (unlike SCARED).
      //   - Right eye: normal SCARED-style dome-up brow (∩).
      //   - Left eye: same brow but flipped upside-down (∪), occupying
      //     the same vertical band above the eye.
      //   - Left eye is half-closed from the top.
      uint8_t eyeRcorner = _geom.borderRadius;
      if (eyeRcorner > (eyeW / 2U)) {
        eyeRcorner = static_cast<uint8_t>(eyeW / 2U);
      }
      if (eyeRcorner > (eyeH / 2U)) {
        eyeRcorner = static_cast<uint8_t>(eyeH / 2U);
      }
      constexpr int16_t kConfusedBrowGap = 5;  // slightly tighter than SCARED
      const int16_t bcx = static_cast<int16_t>((x0 + x1) / 2);
      const float rOuter = static_cast<float>(eyeRcorner + kConfusedBrowGap);
      float rInner = rOuter - static_cast<float>(kAngryBrowThickness);
      if (rInner < 1.0f) {
        rInner = 1.0f;
      }
      if (isLeft) {
        // Half-close from the top FIRST, so the brow draws on top of
        // the cover (otherwise the cover wipes the brow's lower edge
        // when the brow drops into the top-half eye band).
        const int16_t coverH = scale255(static_cast<int16_t>(eyeH / 2U), scale);
        if (coverH > 0) {
          _gfx->fillRect(x0, y0, eyeW, static_cast<uint16_t>(coverH), cutColor);
        }

        // Flipped brow: arc 1°-179° draws the lower half of the band
        // (screen-down). Setting bcy = y0 - gap - rOuter puts that
        // lower half-band into y ∈ [y0 - gap - rOuter, y0 - gap] —
        // the same vertical band SCARED's dome-up brow would occupy.
        // kConfusedLeftBrowDrop slides the whole flipped brow down a
        // few px so it sits a touch closer to the eye than the right.
        constexpr int16_t kConfusedLeftBrowDrop = 10;
        const int16_t bcy = static_cast<int16_t>(y0 - kConfusedBrowGap - static_cast<int16_t>(rOuter) + kConfusedLeftBrowDrop);
        // Extend the arc by 2° on each side (-1° → 181°, library wraps
        // the negative via +360 and handles the cross-zero sweep).
        _gfx->fillArc(bcx, bcy, rOuter, rInner, -1.0f, 181.0f, browColor);
      } else {
        // Normal SCARED dome-up brow above right eye, extended ±2°.
        const int16_t bcy = static_cast<int16_t>(y0 - kConfusedBrowGap);
        _gfx->fillArc(bcx, bcy, rOuter, rInner, 179.0f, 361.0f, browColor);
      }
      break;
    }
    case MOOD_CURIOUS: {
      // Single raised brow above LEFT eye only — short slanted bar that
      // rises toward the outer end.
      if (isLeft) {
        const int16_t bxOuter = static_cast<int16_t>(x0 - kCuriousBrowExtend);
        const int16_t bxInner = x1;
        const int16_t innerTopY = static_cast<int16_t>(y0 - kCuriousBrowGap - kCuriousBrowThickness);
        const int16_t outerTopY = static_cast<int16_t>(innerTopY - kCuriousBrowRise);
        const int16_t outerBotY = static_cast<int16_t>(outerTopY + kCuriousBrowThickness);
        const int16_t innerBotY = static_cast<int16_t>(innerTopY + kCuriousBrowThickness);
        // Two-triangle quad fill (same trick as ANGRY brow).
        _gfx->fillTriangle(bxOuter, outerTopY, bxInner, innerTopY, bxInner, innerBotY, browColor);
        _gfx->fillTriangle(bxOuter, outerTopY, bxInner, innerBotY, bxOuter, outerBotY, browColor);
      }
      break;
    }
    default:
      break;
  }
}

void EyeEngine::setPositionInternal(EyePosition p, bool markInteraction) {
  int16_t gx = 0;
  int16_t gy = 0;
  switch (p) {
    case POS_N:
      gy = -_geom.motionRangeY;
      break;
    case POS_NE:
      gx = _geom.motionRangeX;
      gy = -_geom.motionRangeY;
      break;
    case POS_E:
      gx = _geom.motionRangeX;
      break;
    case POS_SE:
      gx = _geom.motionRangeX;
      gy = _geom.motionRangeY;
      break;
    case POS_S:
      gy = _geom.motionRangeY;
      break;
    case POS_SW:
      gx = -_geom.motionRangeX;
      gy = _geom.motionRangeY;
      break;
    case POS_W:
      gx = -_geom.motionRangeX;
      break;
    case POS_NW:
      gx = -_geom.motionRangeX;
      gy = -_geom.motionRangeY;
      break;
    case POS_DEFAULT:
    default:
      break;
  }

  _position = p;
  _left.nextX = clampCoord(static_cast<int16_t>(_geom.leftCenterX + gx),
                           static_cast<int16_t>(_geom.leftCenterX - _geom.motionRangeX),
                           static_cast<int16_t>(_geom.leftCenterX + _geom.motionRangeX));
  _left.nextY = clampCoord(static_cast<int16_t>(_geom.leftCenterY + gy),
                           static_cast<int16_t>(_geom.leftCenterY - _geom.motionRangeY),
                           static_cast<int16_t>(_geom.leftCenterY + _geom.motionRangeY));
  _right.nextX = clampCoord(static_cast<int16_t>(_geom.rightCenterX + gx),
                            static_cast<int16_t>(_geom.rightCenterX - _geom.motionRangeX),
                            static_cast<int16_t>(_geom.rightCenterX + _geom.motionRangeX));
  _right.nextY = clampCoord(static_cast<int16_t>(_geom.rightCenterY + gy),
                            static_cast<int16_t>(_geom.rightCenterY - _geom.motionRangeY),
                            static_cast<int16_t>(_geom.rightCenterY + _geom.motionRangeY));

  if (markInteraction && _begun) {
    const uint32_t now = millis();
    _lastInteractionMs = now;
    if (_idleWanderOn) {
      scheduleNextIdle(now);
    }
  }

  _forceRedraw = true;
}

void EyeEngine::syncWidthTarget() {
  _nextW = _geom.baseW;
  _nextH = _geom.baseH;
}

void EyeEngine::scheduleNextBlink(uint32_t now) {
  // Per-mood blink-feel overrides: each mood scales the user's autoblink
  // baseline so the same eye visually breathes differently across moods.
  // User's `blinkrate` is still the baseline for MOOD_DEFAULT; other moods
  // are derived as a fraction/multiple of it. MOOD_SLEEPING suppresses
  // autoblink entirely (the eyes are already closed).
  uint16_t base = _autoblinkBaseMs;
  uint16_t jitter = _autoblinkJitterMs;
  bool suppress = false;
  switch (_targetMood) {
    case MOOD_SLEEPING:  suppress = true; break;
    case MOOD_THINK:     suppress = true; break; // eyes already closed
    case MOOD_TIRED:     base = static_cast<uint16_t>(_autoblinkBaseMs / 2U);
                          jitter = static_cast<uint16_t>(_autoblinkJitterMs / 3U); break; // slow heavy blinks
    case MOOD_BORED:     base = static_cast<uint16_t>((_autoblinkBaseMs * 5U) / 6U);
                          jitter = static_cast<uint16_t>(_autoblinkJitterMs * 4U / 3U); break;
    case MOOD_ANGRY:     base = static_cast<uint16_t>(_autoblinkBaseMs * 2U);
                          jitter = static_cast<uint16_t>(_autoblinkJitterMs / 2U); break; // intense, rarely blinks
    case MOOD_HAPPY:     base = static_cast<uint16_t>((_autoblinkBaseMs * 2U) / 3U); break; // lively
    case MOOD_SAD:       base = static_cast<uint16_t>((_autoblinkBaseMs * 3U) / 5U); break; // welling up
    case MOOD_SCARED:    base = static_cast<uint16_t>(_autoblinkBaseMs * 3U);
                          jitter = 0; break; // staring in terror
    case MOOD_LOVE:      base = static_cast<uint16_t>(_autoblinkBaseMs / 2U); break; // fluttery
    case MOOD_CONFUSED:  jitter = static_cast<uint16_t>(_autoblinkJitterMs * 3U / 2U); break;
    case MOOD_CURIOUS:   base = static_cast<uint16_t>((_autoblinkBaseMs * 5U) / 6U); break;
    default: break; // MOOD_DEFAULT/SURPRISED/ANNOYED/ERROR/WINK keep user settings
  }
  if (suppress) {
    _nextBlinkMs = 0; // skip; update() guards against scheduling==0
    return;
  }
  _nextBlinkMs = now + randomCentered(base, jitter);
}

void EyeEngine::scheduleNextIdle(uint32_t now) {
  _nextIdleMs = now + 2000UL + randomCentered(_idleBaseMs, _idleJitterMs);
}

void EyeEngine::scheduleNextThink(uint32_t now) {
  _nextThinkMs = now + kThinkStepMs;
}

void EyeEngine::computeEyeBounds(int16_t eyeX, int16_t eyeY, uint8_t eyeW, uint8_t eyeH,
                                 int16_t& x, int16_t& y, int16_t& w, int16_t& h) const {
  if ((eyeW == 0U) || (eyeH == 0U)) {
    x = eyeX;
    y = eyeY;
    w = 0;
    h = 0;
    return;
  }

  x = static_cast<int16_t>(eyeX - (eyeW / 2));
  y = static_cast<int16_t>(eyeY - (eyeH / 2));
  w = eyeW;
  h = eyeH;

  // Every eye now has a 1px BLACK outline around its full footprint.
  // Pad on all sides so the dirty rect / clear-rect covers the outline ring.
  x = static_cast<int16_t>(x - kEyeOutline);
  y = static_cast<int16_t>(y - kEyeOutline);
  w = static_cast<int16_t>(w + (2 * kEyeOutline));
  h = static_cast<int16_t>(h + (2 * kEyeOutline));

  // MOOD_BORED and MOOD_ANNOYED both draw lash extensions past each eye's
  // outer corner.  Pad both sides (the side that doesn't have a lash just
  // over-includes a few mascot-bg pixels — harmless) so the dirty rect and
  // the prior-region restore cover the lash strip.  Use the union of
  // current / target / render mood so we keep padding during fade
  // transitions.
  const bool needsLashPad =
      (_renderMood == MOOD_BORED) || (_currentMood == MOOD_BORED) || (_targetMood == MOOD_BORED) ||
      (_renderMood == MOOD_ANNOYED) || (_currentMood == MOOD_ANNOYED) || (_targetMood == MOOD_ANNOYED);
  if (needsLashPad) {
    x = static_cast<int16_t>(x - kBoredLashExtend);
    w = static_cast<int16_t>(w + (2 * kBoredLashExtend));
  }

  // MOOD_ANGRY and MOOD_SAD both draw brows above each eye that extend past
  // the outer corner AND rise above the eye top.  Pad horizontally (both
  // sides; inner over-pad is harmless) and vertically up by
  // gap+thickness+rise.  Also include +motionRange on each axis: the brow
  // lags the gaze, so during a gaze shift the brow can sit up to
  // motionRange away from the eye's current position — the dirty rect
  // needs to cover that swept area or ghost-brows are left behind.
  const bool needsBrowPad =
      (_renderMood == MOOD_ANGRY) || (_currentMood == MOOD_ANGRY) || (_targetMood == MOOD_ANGRY) ||
      (_renderMood == MOOD_SAD)   || (_currentMood == MOOD_SAD)   || (_targetMood == MOOD_SAD);
  if (needsBrowPad) {
    const int8_t mxRaw = _geom.motionRangeX;
    const int8_t myRaw = _geom.motionRangeY;
    const int16_t motionX = static_cast<int16_t>(mxRaw < 0 ? -mxRaw : mxRaw);
    const int16_t motionY = static_cast<int16_t>(myRaw < 0 ? -myRaw : myRaw);
    const int16_t sidePad = static_cast<int16_t>(kAngryBrowExtend + kAngryBrowOutline + motionX);
    const int16_t topPad = static_cast<int16_t>(kAngryBrowGap + kAngryBrowThickness + kAngryBrowRise + kAngryBrowOutline + motionY);
    x = static_cast<int16_t>(x - sidePad);
    w = static_cast<int16_t>(w + (2 * sidePad));
    y = static_cast<int16_t>(y - topPad);
    h = static_cast<int16_t>(h + topPad + motionY);
  }

  // MOOD_CURIOUS draws a single raised brow above the LEFT eye that extends
  // past its outer corner. Pad both eye dirty rects (right-eye over-pad is
  // harmless) so the brow's footprint is always covered.
  const bool needsCuriousPad =
      (_renderMood == MOOD_CURIOUS) || (_currentMood == MOOD_CURIOUS) || (_targetMood == MOOD_CURIOUS);
  if (needsCuriousPad) {
    const int16_t sidePad = static_cast<int16_t>(kCuriousBrowExtend + kAngryBrowOutline);
    const int16_t topPad = static_cast<int16_t>(kCuriousBrowGap + kCuriousBrowThickness + kCuriousBrowRise + kAngryBrowOutline);
    x = static_cast<int16_t>(x - sidePad);
    w = static_cast<int16_t>(w + (2 * sidePad));
    y = static_cast<int16_t>(y - topPad);
    h = static_cast<int16_t>(h + topPad);
  }

  // MOOD_THINK is currently a placeholder rendering as DEFAULT — no extra padding needed.

  // MOOD_SURPRISED draws an OUTER ring around the eye that extends 8px past
  // the eye on all sides (3px gap + 5px ring). Pad the dirty rect so the
  // clear-rect covers the ring footprint on mood transitions / motion.
  const bool needsSurprisedPad =
      (_renderMood == MOOD_SURPRISED) || (_currentMood == MOOD_SURPRISED) || (_targetMood == MOOD_SURPRISED);
  if (needsSurprisedPad) {
    constexpr int16_t kSurprisedRingPad = 8;
    x = static_cast<int16_t>(x - kSurprisedRingPad);
    y = static_cast<int16_t>(y - kSurprisedRingPad);
    w = static_cast<int16_t>(w + (2 * kSurprisedRingPad));
    h = static_cast<int16_t>(h + (2 * kSurprisedRingPad));
  }

  // MOOD_SCARED draws a half-dome brow with radius = R_corner +
  // kScaredBrowGap above each eye. Horizontal extent past the eye is
  // (rOuter - eyeW/2); vertical extent is kScaredBrowGap + rOuter above
  // the eye top.  MOOD_CONFUSED reuses the same brow band (one dome-up,
  // one dome-down), so it shares this padding.
  const bool needsScaredPad =
      (_renderMood == MOOD_SCARED) || (_currentMood == MOOD_SCARED) || (_targetMood == MOOD_SCARED) ||
      (_renderMood == MOOD_CONFUSED) || (_currentMood == MOOD_CONFUSED) || (_targetMood == MOOD_CONFUSED);
  if (needsScaredPad) {
    uint8_t eyeRcorner = _geom.borderRadius;
    if (eyeRcorner > (_geom.baseW / 2U)) {
      eyeRcorner = static_cast<uint8_t>(_geom.baseW / 2U);
    }
    if (eyeRcorner > (_geom.baseH / 2U)) {
      eyeRcorner = static_cast<uint8_t>(_geom.baseH / 2U);
    }
    constexpr int16_t kScaredBrowGap = 8;
    const int16_t rOuter = static_cast<int16_t>(eyeRcorner + kScaredBrowGap);
    const int16_t halfBaseW = static_cast<int16_t>(_geom.baseW / 2U);
    const int16_t sidePad = (rOuter > halfBaseW) ? static_cast<int16_t>(rOuter - halfBaseW + 1) : 0;
    const int16_t topPad  = static_cast<int16_t>(kScaredBrowGap + rOuter + 1);
    x = static_cast<int16_t>(x - sidePad);
    w = static_cast<int16_t>(w + (2 * sidePad));
    y = static_cast<int16_t>(y - topPad);
    h = static_cast<int16_t>(h + topPad);
  }

  // MOOD_LOVE draws heart lobes that bulge above the eye top by kLoveBulge px.
  const bool needsLovePad =
      (_renderMood == MOOD_LOVE) || (_currentMood == MOOD_LOVE) || (_targetMood == MOOD_LOVE);
  if (needsLovePad) {
    const int16_t topPad = static_cast<int16_t>(kLoveBulge + 1);
    y = static_cast<int16_t>(y - topPad);
    h = static_cast<int16_t>(h + topPad);
  }

  // MOOD_ERROR draws two rotated pills crossed at the eye center, with
  // the center itself shifted outward by kErrorEyeOffset (left eye left,
  // right eye right). Each pill's rotated bounding box also extends past
  // the base eye footprint both horizontally and vertically. We don't
  // know isLeft here, so pad both sides by max(geometric, offset).
  const bool needsErrorPad =
      (_renderMood == MOOD_ERROR) || (_currentMood == MOOD_ERROR) || (_targetMood == MOOD_ERROR);
  if (needsErrorPad) {
    const int16_t thickness = static_cast<int16_t>((static_cast<uint16_t>(_geom.baseW) * 3U + 2U) / 4U);
    const int16_t fullLen   = static_cast<int16_t>((static_cast<uint16_t>(_geom.baseH) * 4U + 2U) / 3U);
    const int16_t halfLen   = static_cast<int16_t>(fullLen / 2);
    const int16_t halfThick = static_cast<int16_t>(thickness / 2);
    // sin(30°) = 0.5 → ×256 = 128, cos(30°) ≈ 0.8660 → ×256 ≈ 222.
    const int16_t pillHalfW = static_cast<int16_t>(
        ((halfLen * 128) + (halfThick * 222) + 128) / 256);
    const int16_t pillHalfH = static_cast<int16_t>(
        ((halfLen * 222) + (halfThick * 128) + 128) / 256);
    const int16_t baseHalfW = static_cast<int16_t>(_geom.baseW / 2);
    const int16_t baseHalfH = static_cast<int16_t>(_geom.baseH / 2);
    const int16_t geomSide = (pillHalfW > baseHalfW)
                                 ? static_cast<int16_t>(pillHalfW - baseHalfW + 1)
                                 : 0;
    const int16_t sidePad = static_cast<int16_t>(geomSide + kErrorEyeOffset + kErrorShakeAmp);
    const int16_t geomVert = (pillHalfH > baseHalfH)
                                 ? static_cast<int16_t>(pillHalfH - baseHalfH + 1)
                                 : 0;
    const int16_t vertPad = static_cast<int16_t>(geomVert + kErrorShakeAmp);
    if (sidePad > 0) {
      x = static_cast<int16_t>(x - sidePad);
      w = static_cast<int16_t>(w + (2 * sidePad));
    }
    if (vertPad > 0) {
      y = static_cast<int16_t>(y - vertPad);
      h = static_cast<int16_t>(h + (2 * vertPad));
    }
  }
}

void EyeEngine::computeCurrentBounds(int16_t& x, int16_t& y, int16_t& w, int16_t& h) const {
  int16_t leftX = 0;
  int16_t leftY = 0;
  int16_t leftW = 0;
  int16_t leftH = 0;
  int16_t rightX = 0;
  int16_t rightY = 0;
  int16_t rightW = 0;
  int16_t rightH = 0;
  computeEyeBounds(_left.curX, _left.curY, _curW, _curH, leftX, leftY, leftW, leftH);
  computeEyeBounds(_right.curX, _right.curY, _curW, _curH, rightX, rightY, rightW, rightH);
  x = leftX;
  y = leftY;
  w = leftW;
  h = leftH;
  expandUnion(rightX, rightY, rightW, rightH, x, y, w, h);
}

void EyeEngine::expandUnion(int16_t x, int16_t y, int16_t w, int16_t h,
                            int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH) {
  if ((w <= 0) || (h <= 0)) {
    return;
  }
  if ((outW <= 0) || (outH <= 0)) {
    outX = x;
    outY = y;
    outW = w;
    outH = h;
    return;
  }

  const int16_t minX = (x < outX) ? x : outX;
  const int16_t minY = (y < outY) ? y : outY;
  const int16_t maxX = static_cast<int16_t>(((x + w) > (outX + outW)) ? (x + w) : (outX + outW));
  const int16_t maxY = static_cast<int16_t>(((y + h) > (outY + outH)) ? (y + h) : (outY + outH));
  outX = minX;
  outY = minY;
  outW = static_cast<int16_t>(maxX - minX);
  outH = static_cast<int16_t>(maxY - minY);
}

uint8_t EyeEngine::clampByte(int16_t value) {
  if (value < 0) {
    return 0;
  }
  if (value > 255) {
    return 255;
  }
  return static_cast<uint8_t>(value);
}

int16_t EyeEngine::clampCoord(int16_t value, int16_t minValue, int16_t maxValue) {
  if (value < minValue) {
    return minValue;
  }
  if (value > maxValue) {
    return maxValue;
  }
  return value;
}

int16_t EyeEngine::scale255(int16_t value, uint8_t scale) {
  if ((value <= 0) || (scale == 0U)) {
    return 0;
  }

  int32_t scaled = static_cast<int32_t>(value) * static_cast<int32_t>(scale);
  scaled = (scaled + 254) / 255;
  if (scaled < 1) {
    scaled = 1;
  }
  return static_cast<int16_t>(scaled);
}

uint16_t EyeEngine::randomCentered(uint16_t baseMs, uint16_t jitterMs) {
  if (baseMs == 0U && jitterMs == 0U) {
    return 1U;
  }

  long value = static_cast<long>(baseMs);
  if (jitterMs > 0U) {
    const long span = static_cast<long>(jitterMs) * 2L + 1L;
    value += random(span) - static_cast<long>(jitterMs);
  }
  if (value < 1L) {
    value = 1L;
  }
  return static_cast<uint16_t>(value);
}

bool EyeEngine::timeReached(uint32_t now, uint32_t target) {
  return static_cast<int32_t>(now - target) >= 0;
}

uint8_t EyeEngine::tweenByte(uint8_t current, uint8_t target) {
  if (current == target) {
    return current;
  }

  int16_t next = static_cast<int16_t>((static_cast<int16_t>(current) + static_cast<int16_t>(target)) / 2);
  if (next == current) {
    next += (target > current) ? 1 : -1;
  }
  return clampByte(next);
}

int16_t EyeEngine::tweenCoord(int16_t current, int16_t target) {
  if (current == target) {
    return current;
  }

  int32_t next = (static_cast<int32_t>(current) + static_cast<int32_t>(target)) / 2;
  if (next == current) {
    next += (target > current) ? 1 : -1;
  }
  return static_cast<int16_t>(next);
}
