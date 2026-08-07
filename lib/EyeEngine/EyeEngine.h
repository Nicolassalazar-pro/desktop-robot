#pragma once
#include <Arduino_GFX.h>

enum EyeMood : uint8_t {
  MOOD_DEFAULT   = 0,
  MOOD_BORED     = 1,  // half-height eye + outer lash only
  MOOD_ANGRY     = 2,
  MOOD_HAPPY     = 3,
  MOOD_THINK     = 5,  // (renders as DEFAULT — placeholder for future indicator)
  MOOD_ERROR     = 6,  // red vibrating X (two rotated pills per eye)
  MOOD_TIRED     = 7,  // outer-corner triangle droop
  MOOD_ANNOYED   = 8,  // half-height eye + lashes on BOTH sides
  MOOD_SAD       = 9,  // /\ concerned brows + outer-corner eye cut
  MOOD_SURPRISED = 10, // wide 36x36 square eyes pushed outward; outer eyeColor ring around each eye with a bg gap, L/R/B straight segments removed, tiny pupil
  MOOD_WINK      = 11, // right eye closed (horizontal line); left eye normal
  MOOD_SLEEPING  = 12, // both eyes closed (horizontal lines); autoblink disabled
  MOOD_LOVE      = 13, // heart shape per eye (2 lobes + downward triangle)
  MOOD_CONFUSED  = 14, // right eye squinted (top-half cut); left normal
  MOOD_CURIOUS   = 15, // raised brow above LEFT eye only
  MOOD_SCARED    = 16, // small bg "pupil" circle inside a wider eye (was old SURPRISED)
};

enum EyePosition : uint8_t {
  POS_DEFAULT = 0, // centered
  POS_N  = 1, POS_NE = 2, POS_E = 3, POS_SE = 4,
  POS_S  = 5, POS_SW = 6, POS_W = 7, POS_NW = 8,
};

struct EyeEngineGeom {
  int16_t leftCenterX,  leftCenterY;   // bitmap-local center of left eye
  int16_t rightCenterX, rightCenterY;  // bitmap-local center of right eye
  uint8_t baseW, baseH;                // default rest size (e.g. 16 x 24)
  uint8_t borderRadius;                // rounded-rect corner radius (e.g. 6)
  int8_t  motionRangeX, motionRangeY;  // max gaze offset (e.g. 18, 10)
};

class EyeEngine {
public:
  void begin(Arduino_GFX* gfx,
             const EyeEngineGeom& geom,
             uint16_t bgColor,
             uint16_t mainColor);

  // call once per loop iteration; returns true if it redrew this frame
  bool update();

  void setMood(EyeMood m);
  // Snap mood state to m instantly, bypassing the overlay fade. Use this
  // for transitions where the eye geometry/centers also change (e.g.
  // MOOD_SURPRISED <-> anything), since fading the old overlay at the
  // new centers would draw decorations in the wrong place.
  void snapMood(EyeMood m);
  void setPosition(EyePosition p);
  // Set the clean background pixel source.  EyeEngine uses this to restore
  // mascot pixels when clearing the per-frame dirty rect, so mood overlays
  // and dirty-rect padding don't leave bg-colored "mask" rectangles over
  // the mascot's body.  Pass nullptr to disable (falls back to bgColor fill).
  void setBackgroundSource(const uint16_t* pixels, int16_t originX, int16_t originY,
                           int16_t srcW, int16_t srcH);
  void blink();                  // one-shot blink
  void setFramerate(uint8_t fps);
  void setAutoblinker(bool on, uint16_t baseMs = 6000, uint16_t jitterMs = 3000);
  void setIdleWander(bool on,    uint16_t baseMs = 4000, uint16_t jitterMs = 2000);
  void setDims(uint8_t w, uint8_t h, uint8_t radius);
  void setMotionRange(int8_t x, int8_t y);
  void setMainColor(uint16_t color);
  // Color of the 1px ring drawn around every eye/brow/cut shape. Pass 0 to
  // disable (no outline). Default disabled.
  void setOutlineColor(uint16_t color);
  uint16_t getOutlineColor() const;
  // Update eye centers in-flight (after begin). Snaps eye state to new
  // centers (no tween) and forces a full eye redraw.
  void setCenters(int16_t leftCenterX, int16_t leftCenterY,
                  int16_t rightCenterX, int16_t rightCenterY);
  // Force a redraw of both eyes on the next update() call.
  void forceRedraw();
  EyeMood getMood() const;
  EyePosition getPosition() const;
  void getDims(uint8_t& w, uint8_t& h, uint8_t& r) const;
  void getMotionRange(int8_t& x, int8_t& y) const;
  uint16_t getMainColor() const;
  bool isAutoblinker() const;
  void getAutoblinkInterval(uint16_t& base, uint16_t& jitter) const;
  bool isIdleWander() const;
  void getIdleWanderInterval(uint16_t& base, uint16_t& jitter) const;
  uint8_t getFramerate() const;

  // for sympathetic head shift - caller reads these AFTER update()
  int16_t gazeOffsetX() const;
  int16_t gazeOffsetY() const;

  // dirty-rect bbox in canvas coords (caller pushes this to LCD)
  void getDirtyRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h) const;
  bool isDirty() const;          // false if no redraw this frame
  void clearDirty();             // call after pushing
  bool isAnimating() const;

private:
  enum MoodPhase : uint8_t {
    MOOD_PHASE_STEADY = 0,
    MOOD_PHASE_FADING_OUT = 1,
    MOOD_PHASE_FADING_IN = 2,
  };

  struct EyeState {
    int16_t curX;
    int16_t curY;
    int16_t nextX;
    int16_t nextY;
  };

  void drawEye(int16_t eyeX, int16_t eyeY, uint8_t eyeW, uint8_t eyeH, bool isLeft,
               uint16_t eyeColor, uint16_t browColor, uint16_t cutColor,
               int16_t ox, int16_t oy) const;
  void clearRect(int16_t x, int16_t y, int16_t w, int16_t h) const;
  void setPositionInternal(EyePosition p, bool markInteraction);
  void syncWidthTarget();
  void scheduleNextBlink(uint32_t now);
  void scheduleNextIdle(uint32_t now);
  void scheduleNextThink(uint32_t now);
  void computeEyeBounds(int16_t eyeX, int16_t eyeY, uint8_t eyeW, uint8_t eyeH,
                        int16_t& x, int16_t& y, int16_t& w, int16_t& h) const;
  void computeCurrentBounds(int16_t& x, int16_t& y, int16_t& w, int16_t& h) const;
  static void expandUnion(int16_t x, int16_t y, int16_t w, int16_t h,
                          int16_t& outX, int16_t& outY, int16_t& outW, int16_t& outH);
  static uint8_t clampByte(int16_t value);
  static int16_t clampCoord(int16_t value, int16_t minValue, int16_t maxValue);
  static int16_t scale255(int16_t value, uint8_t scale);
  static uint16_t randomCentered(uint16_t baseMs, uint16_t jitterMs);
  static bool timeReached(uint32_t now, uint32_t target);
  static uint8_t tweenByte(uint8_t current, uint8_t target);
  static int16_t tweenCoord(int16_t current, int16_t target);

  Arduino_GFX* _gfx = 0;
  EyeEngineGeom _geom = {0, 0, 0, 0, 0, 0, 0};
  uint16_t _bgColor = 0;
  uint16_t _mainColor = 0;
  uint16_t _browColor = 0;
  uint16_t _outlineColor = 0;  // 0 = no outline; otherwise color of 1px ring
  const uint16_t* _bgSource = nullptr;
  int16_t _bgSourceX = 0;
  int16_t _bgSourceY = 0;
  int16_t _bgSourceW = 0;
  int16_t _bgSourceH = 0;
  EyeState _left = {0, 0, 0, 0};
  EyeState _right = {0, 0, 0, 0};
  EyePosition _position = POS_DEFAULT;
  EyeMood _currentMood = MOOD_DEFAULT;
  EyeMood _targetMood = MOOD_DEFAULT;
  EyeMood _renderMood = MOOD_DEFAULT;
  EyeMood _pendingMood = MOOD_DEFAULT;
  MoodPhase _moodPhase = MOOD_PHASE_STEADY;
  uint8_t _curW = 0;
  uint8_t _curH = 0;
  uint8_t _nextW = 0;
  uint8_t _nextH = 0;
  uint8_t _currentOverlayScale = 0;
  uint8_t _nextOverlayScale = 0;
  uint8_t _fps = 50;
  uint16_t _frameIntervalMs = 20;
  uint16_t _autoblinkBaseMs = 6000;
  uint16_t _autoblinkJitterMs = 3000;
  uint16_t _idleBaseMs = 4000;
  uint16_t _idleJitterMs = 2000;
  uint32_t _lastFrameMs = 0;
  uint32_t _lastInteractionMs = 0;
  uint32_t _nextBlinkMs = 0;
  uint32_t _nextIdleMs = 0;
  uint32_t _nextThinkMs = 0;
  uint32_t _blinkReleaseMs = 0;
  uint32_t _nextErrorShakeMs = 0;
  int16_t _dirtyX = 0;
  int16_t _dirtyY = 0;
  int16_t _dirtyW = 0;
  int16_t _dirtyH = 0;
  int16_t _browGazeX = 0;
  int16_t _browGazeY = 0;
  int16_t _lastFrameX = 0;
  int16_t _lastFrameY = 0;
  int16_t _lastFrameW = 0;
  int16_t _lastFrameH = 0;
  int16_t _pendingPriorX = 0;
  int16_t _pendingPriorY = 0;
  int16_t _pendingPriorW = 0;
  int16_t _pendingPriorH = 0;
  bool _dirty = false;
  bool _forceRedraw = false;
  bool _begun = false;
  bool _autoblinkOn = false;
  bool _idleWanderOn = false;
  bool _blinkActive = false;
  bool _lastFrameValid = false;
  bool _thinkTowardWest = true;
  bool _havePendingPrior = false;
  uint8_t _errorShakeStep = 0;
};
