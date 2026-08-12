#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <ESP32Servo.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <driver/gpio.h>
#include <esp32-hal-ledc.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <Wire.h>
#include <CST816_TouchLib.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <ctype.h>
#include <atomic>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "EyeEngine.h"
#include "display_transition.h"
#include "drive_calibration.h"
#include "drive_session.h"
#include "imu_qmi8658.h"
#include "mascot/garage_logo.h"
#include "mascot/mascot_a.h"
#include "movement_expression.h"
#include "piezo_tone.h"
#include "qrcode.h"
#include "serial_line_buffer.h"
#include "spin_calibration.h"
#include "turn_measure.h"
#include "bench_log.h"
#include "rf_map.h"
#include "motion_events.h"
#include "melody.h"
#include "melody_gen.h"
#include "sheet_transition.h"
#include "nvs.h"
#include <esp_random.h>

#include "override_html.h"
#include "serial_console.h"

namespace {

constexpr uint32_t kSerialBaud = 115200UL;   // native USB-CDC ignores baud, but for monitor
constexpr char kBuildId[] = __DATE__ " " __TIME__;
// LCD SPI clock.
//
// 80 MHz was inherited and is the ESP32-S3's ceiling, NOT the panel's.
//
// MEASURED AGAINST THE ACTUAL PART: datasheets/ST7789V2_LCD.pdf, Table 6
// ("4-line serial Interface Characteristics", p.44) gives
//     TSCYCW  serial clock cycle (write)  MIN 16 ns  -> 62.5 MHz ceiling
//     TSHW/TSLW  SCL high/low pulse width MIN  7 ns  each
// At 80 MHz the cycle is 12.5 ns and each half-cycle 6.25 ns, so BOTH the
// cycle time and the pulse widths are violated. It is an overclock of ~1.28x.
//
// That modest margin is exactly why the failure is INTERMITTENT rather than
// total: the part usually tolerates it, and fails only when temperature,
// supply ripple or silicon lot happen to line up badly. A corrupted byte in
// the init sequence (SLPOUT 0x11 / DISPON 0x29 / COLMOD 0x3A) leaves the
// panel dark or misconfigured, and there is NO read-back on this bus, so the
// firmware reports perfect health while nothing is visible. Every later pixel
// push then "succeeds" into a panel that never woke up. That is the observed
// signature: black or glitched after a reset, fine after a power cycle.
//
// (Note for anyone re-checking: the widely quoted "66 ns / 15 MHz" figure is
// TWC from Table 4, the 8080 PARALLEL write cycle on p.42. It is not the
// serial number and does not apply to this wiring.)
//
// 40 MHz = 25 ns cycle, comfortably inside spec. Waveshare's own BSP for this
// board uses 24 MHz. A full 240x280x2 frame costs 26.9 ms at 40 MHz against
// 13.4 ms at 80 MHz, but full pushes are rare — the eye engine normally
// repaints only its dirty region.
//
// Runtime-settable and persisted so the value can be A/B tested across many
// resets without a reflash between each trial. `lcdspi <hz>` sets and stores.
constexpr uint32_t kLcdSpiHzDefault = 40000000UL;
constexpr uint32_t kLcdSpiHzMin = 8000000UL;
constexpr uint32_t kLcdSpiHzMax = 80000000UL;
uint32_t g_lcdSpiHz = kLcdSpiHzDefault;
uint8_t g_lcdInitAttempts = 0;  // how many init passes the panel needed

// ---------------------------------------------------------------------------
// Crash breadcrumbs.
//
// A spontaneous restart while using the AP is the hardest class of bug here:
// it happens untethered, so there is no serial capture, and by the time the
// board is plugged back in the evidence is gone. esp_reset_reason() survives
// and says WHAT killed us (panic / watchdog / brownout), but not WHERE.
//
// RTC memory is not cleared by a software reset, a panic or a watchdog -- only
// by a true power cycle -- so a two-word breadcrumb written on the way through
// each interesting phase survives exactly the events we care about. Cost is
// two 32-bit stores, cheap enough for hot paths.
//
// A coredump partition would give a real backtrace and would be better, but
// Arduino-ESP32 ships a precompiled IDF with core dump disabled, so it is not
// available without leaving the Arduino framework.
enum : uint32_t {
  kCrumbMagic = 0xB1EAD00DU,
  kCrashRecordMagic = 0xC7A54100U,
};

enum CrumbPhase : uint32_t {
  kCrumbNone = 0,
  kCrumbSetupDone,
  kCrumbLoopIdle,
  kCrumbApStart,
  kCrumbApStaConnected,
  kCrumbHttpRoot,
  kCrumbHttpOther,
  kCrumbWsConnect,
  kCrumbWsData,
  kCrumbWsDisconnect,
  kCrumbWsTelemetry,
  kCrumbLcdPush,
  kCrumbLcdQr,
  kCrumbDriveApply,
  kCrumbGesture,
  kCrumbMelody,
  // Main-loop phases. The board was resetting with TASK_WDT and the last
  // crumb read "setup complete" -- true but useless, because nothing in the
  // loop wrote a crumb. These name the phase that hung.
  kCrumbLoopSerial,
  kCrumbLoopTouch,
  kCrumbLoopOverride,
  kCrumbLoopPower,
  kCrumbLoopBattery,
  kCrumbLoopDemo,
  kCrumbLoopPush,
  kCrumbLoopEyes,
  kCrumbLoopMelodyTick,
  kCrumbQuarantineSpin,
  kCrumbWsCleanup,
  kCrumbWsCloseAll,
};

struct CrashRecord {
  uint32_t magic;
  uint32_t reason;
  uint32_t phase;
  uint32_t detail;
  uint32_t seq;
  uint32_t freeHeap;
  uint32_t minHeap;
  uint32_t count;
};

RTC_NOINIT_ATTR uint32_t g_crumbMagic;RTC_NOINIT_ATTR uint32_t g_crumbPhase;
RTC_NOINIT_ATTR uint32_t g_crumbDetail;
RTC_NOINIT_ATTR uint32_t g_crumbSeq;

// ---------------------------------------------------------------------------
// Stall forensics. RTC memory, because the event we are hunting RESETS THE
// BOARD -- anything in normal RAM dies with it, and the serial line is exactly
// what we cannot trust (a full CDC FIFO was itself a stall cause once).
//
// The open question: a TASK_WDT fired with the breadcrumb at "loop: touch
// poll", meaning the loop task entered g_orTouch.control() and never reached
// the next crumb. But the watchdog only proves the loop went 5 s without being
// fed; it does not prove WHERE the time went. These counters answer that.
//
// Recording BOTH the touch-poll worst case and the whole-loop worst case is the
// point. If they track each other, the touch poll is the stall. If the loop max
// is far larger than the touch max, the time is going somewhere else entirely
// and the touch breadcrumb was just the last one written before the reset --
// which is precisely how the earlier "websocket frame" crumb misled us.
//
// Microseconds, because the interesting range spans a 50 ms I2C timeout, a
// 120 ms chip reset, and a 5 s watchdog.
RTC_NOINIT_ATTR uint32_t g_rtcStallMagic;
RTC_NOINIT_ATTR uint32_t g_rtcTouchMaxUs;      // worst g_orTouch.control()
RTC_NOINIT_ATTR uint32_t g_rtcTouchSlowCount;  // calls over kStallWarnUs
RTC_NOINIT_ATTR uint32_t g_rtcLoopMaxUs;       // worst whole loop() pass
RTC_NOINIT_ATTR uint32_t g_rtcLoopMaxPhase;    // crumb phase during that pass
// Power and motion context captured AT THE WORST TOUCH POLL.
//
// The leading hypothesis for the stall needs exactly this to be confirmed or
// killed. Both watchdog resets happened UNTETHERED, and untethered is the only
// state in which two things are true at once: the board is on battery, and the
// servos actually run (a tethered robot physically cannot complete a rotation,
// so movement is never exercised on USB). A servo current spike sagging the
// rail could glitch the CST816T into holding SDA low, which would present
// exactly as a hung I2C read inside the touch poll.
//
// Without this context the eventual single occurrence would tell us only what
// we already know -- that it hung. With it, one event settles the question:
// rail on + battery sagging at the moment of the stall is the hypothesis
// confirmed; rail off and a healthy battery kills it outright.
RTC_NOINIT_ATTR uint32_t g_rtcStallBatMv;      // battery mV at the worst poll
RTC_NOINIT_ATTR uint32_t g_rtcStallFlags;      // bit0 rail, bit1 gesture, bit2 charging
constexpr uint32_t kStallFlagRail    = 1U << 0;
constexpr uint32_t kStallFlagGesture = 1U << 1;
constexpr uint32_t kStallFlagCharge  = 1U << 2;
constexpr uint32_t kStallMagic  = 0x5A11EDU;
constexpr uint32_t kStallWarnUs = 20000U;      // 20 ms: well past a healthy poll

inline void stallStatsReset() {
  g_rtcStallMagic     = kStallMagic;
  g_rtcTouchMaxUs     = 0;
  g_rtcTouchSlowCount = 0;
  g_rtcLoopMaxUs      = 0;
  g_rtcLoopMaxPhase   = 0;
  g_rtcStallBatMv     = 0;
  g_rtcStallFlags     = 0;
}
inline void stallStatsInitIfCold() {
  if (g_rtcStallMagic != kStallMagic) {
    stallStatsReset();
  }
}

inline void breadcrumb(uint32_t phase, uint32_t detail = 0) {
  g_crumbMagic = kCrumbMagic;
  g_crumbPhase = phase;
  g_crumbDetail = detail;
  ++g_crumbSeq;
}

const char* crumbName(uint32_t phase) {  switch (phase) {
    case kCrumbSetupDone:      return "setup complete";
    case kCrumbLoopIdle:       return "loop idle";
    case kCrumbApStart:        return "AP starting";
    case kCrumbApStaConnected: return "AP client connected";
    case kCrumbHttpRoot:       return "serving / (web app)";
    case kCrumbHttpOther:      return "serving other HTTP";
    case kCrumbWsConnect:      return "websocket connect";
    case kCrumbWsData:         return "websocket frame";
    case kCrumbWsDisconnect:   return "websocket disconnect";
    case kCrumbWsTelemetry:    return "websocket telemetry send";
    case kCrumbLcdPush:        return "LCD push (1=lock 2=window 3=pixels 4=endWrite 5=done)";
    case kCrumbLcdQr:          return "LCD QR render";
    case kCrumbDriveApply:     return "drive output";
    case kCrumbGesture:        return "gesture";
    case kCrumbMelody:         return "melody";
    case kCrumbLoopSerial:     return "loop: serial drain";
    case kCrumbLoopTouch:      return "loop: touch poll";
    case kCrumbLoopOverride:   return "loop: overrideLoop (wifi/http/ws)";
    case kCrumbLoopPower:      return "loop: power button";
    case kCrumbLoopBattery:    return "loop: battery meter (1=adc 2=draw)";
    case kCrumbLoopDemo:       return "loop: demo step";
    case kCrumbLoopPush:       return "loop: LCD push";
    case kCrumbLoopEyes:       return "loop: eye update";
    case kCrumbLoopMelodyTick: return "loop: melody service";
    case kCrumbQuarantineSpin: return "ws ingress quarantine SPIN";
    case kCrumbWsCleanup:      return "ws cleanupClients";
    case kCrumbWsCloseAll:     return "ws closeAll";
    default:                   return "unknown";
  }
}

uint32_t lcdProbeRead(uint8_t command, uint8_t bitsToRead);
uint8_t lcdReadPowerMode();
bool lcdPanelLooksAlive(uint8_t rddpm);
constexpr int16_t kLcdRotate = 0;
constexpr int16_t kScreenW = LCD_WIDTH;       // 240
constexpr int16_t kScreenH = LCD_HEIGHT;      // 280
constexpr int16_t kOriginX = (kScreenW - MASCOT_A_W) / 2;     // 10
constexpr int16_t kOriginY = (kScreenH - MASCOT_A_H) / 2;     // 32 (was 12 on round)
constexpr uint8_t kBacklightChannel = 4;
constexpr uint32_t kBacklightFrequencyHz = 5000;
constexpr uint8_t kBacklightResolutionBits = 8;
constexpr uint8_t kBacklightMax = 255;
constexpr uint8_t kLogoTransitionSteps = 8;
constexpr uint32_t kLogoTransitionFrameMs = 15;
constexpr uint32_t kStartupLogoHoldMs = 100;
constexpr uint32_t kShutdownLogoHoldMs = 100;
constexpr uint32_t kBacklightFadeMs = 90;
constexpr size_t kScreenPixelCount =
    static_cast<size_t>(kScreenW) * static_cast<size_t>(kScreenH);

constexpr int16_t kLeftEyeCenterX  = kOriginX + MASCOT_A_EYE_L_X + (MASCOT_A_EYE_L_W / 2);
constexpr int16_t kLeftEyeCenterY  = kOriginY + MASCOT_A_EYE_L_Y + (MASCOT_A_EYE_L_H / 2);
constexpr int16_t kRightEyeCenterX = kOriginX + MASCOT_A_EYE_R_X + (MASCOT_A_EYE_R_W / 2);
constexpr int16_t kRightEyeCenterY = kOriginY + MASCOT_A_EYE_R_Y + (MASCOT_A_EYE_R_H / 2);
constexpr int16_t kEyeCenterSeparation = kRightEyeCenterX - kLeftEyeCenterX;

constexpr uint8_t kEyeBaseWMin = 4;
constexpr uint8_t kEyeBaseWMax = 32;
constexpr uint8_t kEyeBaseHMin = 8;
constexpr uint8_t kEyeBaseHMax = 56;
constexpr int8_t kEyeMotionXMax = 30;
constexpr int8_t kEyeMotionYMax = 20;
constexpr uint8_t kHeadShiftDivMax = 32;
constexpr int16_t kEyeDirtyMargin = 2;

// SURPRISED-mood overrides: large square eyes pushed outward, with an OUTER
// ring drawn around each eye (eyeColor) separated by a bg gap. Swap is
// handled in setEyesMood(); engine dims/centers are restored on mood change.
constexpr uint8_t kSurprisedEyeW = 36;
constexpr uint8_t kSurprisedEyeH = 36;
constexpr uint8_t kSurprisedEyeR = 10;
constexpr int16_t kSurprisedCenterOffset = 16;
constexpr int16_t kSurprisedRingExtent = 10;  // 5px gap + 5px ring outside each eye

// Backup buffer / dirty region must cover whichever is biggest of the user's
// dims limits OR SURPRISED's enlarged geometry (including the outer ring).
constexpr uint8_t kRenderMaxBaseW = (kSurprisedEyeW > kEyeBaseWMax) ? kSurprisedEyeW : kEyeBaseWMax;
constexpr uint8_t kRenderMaxBaseH = (kSurprisedEyeH > kEyeBaseHMax) ? kSurprisedEyeH : kEyeBaseHMax;
constexpr int16_t kEyeBackupMaxW = kEyeCenterSeparation + (2 * kSurprisedCenterOffset) +
                                   (kEyeMotionXMax * 2) +
                                   (kRenderMaxBaseW + ((kRenderMaxBaseW + 3) / 4)) +
                                   (2 * kSurprisedRingExtent) +
                                   (kEyeDirtyMargin * 2);
constexpr int16_t kEyeBackupMaxH = (kEyeMotionYMax * 2) + kRenderMaxBaseH +
                                   (2 * kSurprisedRingExtent) +
                                   (kEyeDirtyMargin * 2);

uint8_t g_eyeBaseW = 12;
uint8_t g_eyeBaseH = 36;
uint8_t g_eyeRadius = 5;
int8_t g_eyeMotionX = 14;
int8_t g_eyeMotionY = 10;

// Currently-active eye centers (default = mascot centers; offset outward when
// SURPRISED is active). recomputeEyeRegionBounds() uses these.
int16_t g_activeLeftCenterX = kLeftEyeCenterX;
int16_t g_activeRightCenterX = kRightEyeCenterX;

// SURPRISED state — when active, base W/H/R hold SURPRISED values and the
// user's last-set dims are stashed in g_savedEyeBase*.
bool g_surprisedActive = false;
uint8_t g_savedEyeBaseW = 12;
uint8_t g_savedEyeBaseH = 36;
uint8_t g_savedEyeRadius = 5;

constexpr uint8_t kFastEyeFps = 50;
constexpr uint8_t kIdleEyeFps = 15;
constexpr uint16_t kFastFrameMs = 20;
constexpr uint16_t kIdleFrameMs = 67;
constexpr uint32_t kDemoStepIntervalMs = 2000UL;

EyeEngineGeom g_eyeGeom = {
    kLeftEyeCenterX,
    kLeftEyeCenterY,
    kRightEyeCenterX,
    kRightEyeCenterY,
    g_eyeBaseW,
    g_eyeBaseH,
    g_eyeRadius,
    g_eyeMotionX,
    g_eyeMotionY,
};

int16_t g_eyeMaxW = 0;
int16_t g_eyeMaxH = 0;
int16_t g_eyeRegionX = 0;
int16_t g_eyeRegionY = 0;
int16_t g_eyeRegionRight = 0;
int16_t g_eyeRegionBottom = 0;
int16_t g_eyeRegionW = 0;
int16_t g_eyeRegionH = 0;

struct NamedMood {
  const char* name;
  EyeMood mood;
};

struct NamedLook {
  const char* name;
  EyePosition position;
};

struct NamedColor {
  const char* name;
  uint16_t color;
};

// READ-6/READ-9: three mood tables in this file must stay in sync.
//   1. kMoodCommands below            -- name -> id lookup for the serial
//                                        console and WS command dispatch.
//   2. kDemoMoods                     -- the subset the idle demo cycles.
//   3. moodToUpperName() around line 2100 -- id -> UPPER-CASE label used
//                                        on the wire (mood label overlay).
// If you add or rename a mood you MUST update all three (and also
// EyeEngine's own mood tables in lib/EyeEngine).
//
// READ-9: row order in kMoodCommands is not cosmetic. moodName() returns
// the FIRST row whose id matches; the mapping goes out on the wire and
// shows up in serial replies. Put the CANONICAL name first and aliases
// after (e.g. {"think", ...} before {"thinking", ...}). Reordering these
// pairs silently renames moods for external clients.
const NamedMood kMoodCommands[] = {
    {"default", MOOD_DEFAULT},
    {"bored", MOOD_BORED},
    {"annoyed", MOOD_ANNOYED},
    {"tired", MOOD_TIRED},
    {"angry", MOOD_ANGRY},
    {"happy", MOOD_HAPPY},
    {"sad", MOOD_SAD},
    {"think", MOOD_THINK},
    {"thinking", MOOD_THINK},
    {"error", MOOD_ERROR},
    {"surprised", MOOD_SURPRISED},
    {"shock", MOOD_SURPRISED},
    {"scared", MOOD_SCARED},
    {"terrified", MOOD_SCARED},
    {"wink", MOOD_WINK},
    {"sleeping", MOOD_SLEEPING},
    {"asleep", MOOD_SLEEPING},
    {"love", MOOD_LOVE},
    {"heart", MOOD_LOVE},
    {"confused", MOOD_CONFUSED},
    {"curious", MOOD_CURIOUS},
};

// READ-6: subset of kMoodCommands that the idle demo cycles through.
// Order matters -- processDemo() steps through this table linearly and
// the demo transcript is what the operator sees. If you add a mood here
// bump kDemoMoodCount / kDemoTotalSteps below, and confirm it also
// appears in kMoodCommands above.
const NamedMood kDemoMoods[] = {
    {"default", MOOD_DEFAULT},
    {"bored", MOOD_BORED},
    {"annoyed", MOOD_ANNOYED},
    {"tired", MOOD_TIRED},
    {"angry", MOOD_ANGRY},
    {"happy", MOOD_HAPPY},
    {"sad", MOOD_SAD},
    {"error", MOOD_ERROR},
    {"surprised", MOOD_SURPRISED},
    {"scared", MOOD_SCARED},
    {"wink", MOOD_WINK},
    {"confused", MOOD_CONFUSED},
    {"sleeping", MOOD_SLEEPING},
};

const NamedLook kLookCommands[] = {
    {"default", POS_DEFAULT},
    {"center", POS_DEFAULT},
    {"n", POS_N},
    {"ne", POS_NE},
    {"e", POS_E},
    {"se", POS_SE},
    {"s", POS_S},
    {"sw", POS_SW},
    {"w", POS_W},
    {"nw", POS_NW},
};

const NamedLook kDemoLooks[] = {
    {"default", POS_DEFAULT},
    {"n", POS_N},
    {"ne", POS_NE},
    {"e", POS_E},
    {"se", POS_SE},
    {"s", POS_S},
    {"sw", POS_SW},
    {"w", POS_W},
    {"nw", POS_NW},
};

const NamedColor kNamedColors[] = {
    {"green", 0x974D},
    {"cyan", 0x07FF},
    {"red", 0xF800},
    {"blue", 0x001F},
    {"yellow", 0xFFE0},
    {"white", 0xFFFF},
    {"magenta", 0xF81F},
    {"orange", 0xFD20},
};

constexpr uint8_t kDemoMoodCount = static_cast<uint8_t>(sizeof(kDemoMoods) / sizeof(kDemoMoods[0]));
constexpr uint8_t kDemoLookCount = static_cast<uint8_t>(sizeof(kDemoLooks) / sizeof(kDemoLooks[0]));
// Demo cycle trimmed to moods-only so SLEEPING (last mood) doesn't
// linger through the look/blink phase. Restore `+ kDemoLookCount + 1U` to re-enable
// look-direction and blink steps after the mood cycle.
constexpr uint8_t kDemoTotalSteps = kDemoMoodCount;

Arduino_DataBus* g_bus = nullptr;
Arduino_GFX* g_gfx = nullptr;
Arduino_Canvas* g_canvas = nullptr;
// Second offscreen canvas: the pairing card that slides down over the face.
// Allocation is fail-soft; when null, the drag preview is disabled and the
// chip's own SwipeDown gesture drives the transition unchanged (the fallback
// path that shipped before this compositor existed).
Arduino_Canvas* g_sheetCanvas = nullptr;
// Sheet drag controller. Owns no hardware and holds no framebuffer; fed screen-
// space Y and a millisecond clock by serviceDragTransition() below.
SheetTransition::Controller g_sheet;
// Deadline after which chip-detected SwipeDown gestures are allowed through
// again. Set every time the sheet is engaged (or has just committed), so the
// chip's own release-time gesture cannot fire the AP a second time on top of
// the drag-driven commit. Zero = not suppressing.
uint32_t g_sheetGestureSuppressUntilMs = 0;
// Cool-down after the sheet finishes an interaction. Sized to bridge two
// things: the ~150-400 ms settle animation (25% remaining per tick at the
// FACE-mode loop cadence) and the CST816T's own release-gesture reporting
// which lands in the following g_orTouch.control() drain. Anything longer
// would swallow a deliberate second swipe by an operator who cancelled and
// immediately tried again; anything shorter leaves a window where the chip
// wins the race and starts the AP twice.
constexpr uint32_t kSheetGestureSuppressMs = 800;
EyeEngine g_eyes;

int8_t g_headShiftX = 0;
int8_t g_headShiftY = 0;
int8_t g_lastShiftX = 0;
int8_t g_lastShiftY = 0;
uint8_t g_headShiftXDiv = 0;   // 0 = head-sympathy shift disabled; enable via 'headgain <x> <y>'
uint8_t g_headShiftYDiv = 0;
bool g_paused = false;
bool g_forceFullPush = false;
uint32_t g_lcdPushCount = 0;    // total canvas->panel pushes
// Set from any task to cancel a gesture in flight; consumed by the loop task.
// Atomic because resetDriveControl can be reached from the AsyncTCP callback
// task (protocol faults) as well as from loop().
std::atomic<bool> g_movementAbortPending{false};
uint32_t g_lcdLastPushMs = 0;   // when the last one happened
uint32_t g_loopIterations = 0;  // main loop iterations, to detect a stall
bool g_forceEyeRedraw = false;
bool g_piezoAvailable = false;
bool g_backlightPwmReady = false;
uint8_t g_backlightLevel = 0;
// One-shot delayed repaint after boot; 0 once it has fired.
uint32_t g_bootRepaintAtMs = 0;  // retired; see the note in loop()
bool g_eyeEngineReady = false;

uint32_t g_lastRedrawMs = 0;
uint16_t g_targetFrameMs = kFastFrameMs;

SerialLineBuffer<513> g_serialInput;

bool g_demoActive = false;
uint8_t g_demoStep = 0;
uint32_t g_demoNextStepMs = 0;

// --- Left/Right motors (continuous-rotation servos) -----------------------
// Verified on the assembled chassis 2026-08-03 by pulsing one pin at a time:
// the robot's LEFT wheel is on GPIO18 and its RIGHT wheel is on GPIO17. These
// must stay matched to the physical wiring or differential steering inverts.
// Native MG90D pulse calibration comes exclusively from the eligible
// DriveCalibration snapshot. The legacy draft lives in the calibration module
// and is never eligible for production output.
constexpr uint8_t  kServoLeftPin       = 18;    // physical LEFT motor
constexpr uint8_t  kServoRightPin      = 17;    // physical RIGHT motor
constexpr uint16_t kServoMinPulseUs    = 1000;
constexpr uint16_t kServoMaxPulseUs    = 2000;
constexpr uint8_t  kServoTimerWidthBits = 14;
constexpr uint32_t kServoRailDischargeMs = 100;
constexpr uint32_t kServoPwmCommitMs = 25;
constexpr int kServoMaxReadbackErrorUs = 2;
Servo   g_servoLeft;
Servo   g_servoRight;
bool    g_servoLeftAttached  = false;
bool    g_servoRightAttached = false;
int8_t  g_servoLeftChannel   = -1;
int8_t  g_servoRightChannel  = -1;
uint16_t g_servoLeftRequestedPulseUs  = 0;
uint16_t g_servoRightRequestedPulseUs = 0;
uint16_t g_servoLeftReadbackPulseUs   = 0;
uint16_t g_servoRightReadbackPulseUs  = 0;
bool    g_driveOutputValid   = false;
bool    g_driveOutputWasNeutral = false;
bool    g_driveOutputHadState = false;
SemaphoreHandle_t g_driveMutex = nullptr;
std::atomic<bool> g_driveRailOn{false};
uint32_t g_driveIdleSinceMs    = 0;
bool     g_neutralIdleLatchedOff = false;
volatile bool g_systemShuttingDown = false;
std::atomic<bool> g_driveOwnerLossPending{false};
portMUX_TYPE g_driveHardwareMux = portMUX_INITIALIZER_UNLOCKED;
esp_timer_handle_t g_driveHardwareTimer = nullptr;
std::atomic<bool> g_driveHardwareTimerReady{false};
std::atomic<bool> g_driveHardwareLeaseArmed{false};
std::atomic<bool> g_driveHardwareInhibit{true};
std::atomic<bool> g_driveHardwareTripPending{false};
std::atomic<uint32_t> g_driveHardwareDeadlineMs{0};
std::atomic<uint32_t> g_driveHardwareTripCount{0};

// =========================================================================
// Relative heading.
//
// There is no compass on this board, so this is heading relative to whenever
// it was last zeroed — good for "turn 90 degrees from here", useless for
// "face north".
//
// Drift is bounded by keeping the integration windows short and by re-zeroing
// the gyro bias whenever the robot is known to be stationary (a zero-velocity
// update). The robot is stationary most of the time, and the longest
// continuous rotation is a single turn of about 1.3 s, so even a sloppy
// 0.5 dps residual contributes well under a degree.
//
// Declared up here rather than beside the calibration state machine because
// the serial command handler reads it.
// =========================================================================
TurnMeasure::Axis g_turnYawAxis = TurnMeasure::Axis::Z;
float g_turnBias[3] = {0.0f, 0.0f, 0.0f};
// Resting gravity vector, in g. The yaw rate is the gyro vector projected onto
// this, which is exact for a tilted platform where a single axis is not.
float g_turnGravity[3] = {0.0f, 0.0f, 1.0f};
// Fraction of the true yaw rate a single-axis reading would have captured.
// Reported so the value of the projection is visible rather than asserted.
float g_turnAxisFraction = 1.0f;

TurnMeasure::Integrator g_headingIntegrator;
// Runs alongside the projected integral during a turn, integrating the raw
// single-axis rate the old code used. Reported at the end so the value of the
// gravity projection is a measurement on real turns rather than a claim.
TurnMeasure::Integrator g_turnSingleAxisIntegrator;
// Measures the ACTUAL rotation axis from the gyro during a turn, so an
// accelerometer zero-g offset cannot be mistaken for physical tilt.
TurnMeasure::AxisEstimator g_turnAxisEstimator;
// One per gyro axis, NOT one for the yaw axis alone.
//
// The gravity projection combines all three gyro axes, so all three biases
// reach the heading — weighted by the gravity unit vector. Correcting only the
// dominant axis leaves the other two leaking in at their gravity weight, which
// on a board tilted 0.27 g in Z is about a quarter of that axis's bias. That
// was measured as 3.39 deg of drift over 65 s, against 0.05 deg when only the
// single axis was integrated. Estimating all three restores the bound.
TurnMeasure::BiasEstimator g_headingZupt[3];
TurnMeasure::TurnController g_turnController;

bool g_headingReady = false;
uint32_t g_headingLastSampleMs = 0;
uint32_t g_headingZuptAppliedMs = 0;
uint32_t g_headingZuptCount = 0;
// Windows rejected by the spread gate: motion that would have been absorbed as
// bias. Reported so the gate's effect is visible rather than silent.
uint32_t g_headingZuptRejected = 0;
float g_headingLastZuptSpread = 0.0f;
float g_headingLastZuptDps = 0.0f;

// Idle sampling rate. Fast enough to track a robot being turned by hand and to
// gather ZUPT samples, slow enough not to saturate the shared I2C bus that the
// touch controller also lives on. A turn samples every loop instead.
constexpr uint32_t kHeadingIdleSampleMs = 10;
// Apply a fresh bias estimate at most this often while sitting still.
constexpr uint32_t kHeadingZuptPeriodMs = 2000;
// A stationary gyro still reads a little noise; anything above this means the
// robot is actually moving and must not pollute the bias estimate.
// rfscan state. The scan runs asynchronously so loop() keeps feeding the task
// watchdog; a blocking scan reboots the board.
bool g_rfScanActive = false;
float g_rfScanHeading = 0.0f;
uint32_t g_rfScanDeadlineMs = 0;
constexpr uint32_t kRfScanTimeoutMs = 15000;

// Survey mode: repeat the scan N times at one labelled position and accumulate
// averages. Averaging is not optional here — the predicted signal from moving
// 0.3 m is 0.63 dB (n=2.5, AP at 5 m) against a measured fixed-point noise
// floor of 1-2 dB, so a single scan cannot see it. Temporal noise falls as
// 1/sqrt(N); multipath, being spatially fixed, does not fall at all.
RfMap::Store g_rfMap;
int8_t g_rfSurveyPosition = -1;
uint16_t g_rfSurveyRemaining = 0;
uint16_t g_rfSurveyDone = 0;

constexpr float kHeadingStationaryDps = 8.0f;
// Peak-to-peak spread allowed within a zero-velocity window. A genuinely still
// robot varies only by sensor noise; anything wider means it moved during the
// window and the mean is not a bias. Set generously above the observed noise
// so it rejects motion, not noise — tighten only against measured data.
constexpr float kHeadingZuptMaxSpreadDps = 4.0f;

// Closed-loop turn command state.
enum class TurnCmdPhase : uint8_t {
  Idle,
  Turning,
  // Motors are already stopped, but the robot is still coasting. Integration
  // continues so the reported angle is where it ACTUALLY came to rest, not
  // where it was when the command ended. Without this the coast is invisible
  // and every turn looks more accurate than it is.
  Coasting,
};

TurnCmdPhase g_turnCmdPhase = TurnCmdPhase::Idle;
bool g_turnCmdActive = false;
bool g_turnCmdClockwise = true;
uint16_t g_turnCmdTimeoutMs = 0;
int8_t g_turnCmdSpeed = MovementExpression::kSpinSpeed;
uint32_t g_turnCmdCoastUntilMs = 0;
bool g_turnCmdReached = false;
float g_turnCmdTargetDeg = 0.0f;
float g_turnCmdAtStopDeg = 0.0f;
// Yaw rate at the instant the motors were cut, for learning the coast lead.
float g_turnCmdRateAtStop = 0.0f;
// How long to keep measuring after the motors stop.
constexpr uint32_t kTurnCoastMs = 400;

// Wheel command at which the chassis produces no rotation at all. Derived from
// the measured model rate = 0.91*speed - 24.2 dps, which crosses zero at about
// 27. Used to scale a turn's time estimate for speed, because the relationship
// is not proportional: halving the command more than halves the rate.
constexpr float kTurnDeadbandCommand = 27.0f;

// Standing-start allowance added to every turn estimate. A turn spends this
// long getting up to speed, during which it sweeps far less than the steady
// rate implies.
constexpr uint32_t kTurnRampMs = 400;

// Coast compensation. The chassis keeps rotating after the motors cut, and the
// turn is stopped early by the angle it is predicted to coast through.
//
// MEASURED, on the desk, untethered, across two supervised runs (see
// `docs/bench-run-001.md`, `-002.md`):
//   speed 100 -> ~275 dps -> 16.3-17.4 deg coast    alpha 2185 / 2304
//   speed  75 -> ~190 dps ->  8.1-8.8 deg           alpha 2196 / 2169
//   speed  55 -> ~130 dps ->  4.6-5.4 deg           alpha 1750 / 1727
//
// This is NOT linear in rate: the implied constant coast/rate ranges 0.0368 to
// 0.0616 s, a 1.7x spread. Coulomb friction, coast = w^2/(2*alpha), fits.
//
// ALPHA IS SET FROM THE CLOSED-LOOP A/B, NOT FROM THE BLOCK MEANS. Run 002 ran
// the compensation live at alpha=2100 and measured a mean error of -4.49 deg —
// it over-led and UNDERSHOT. Extrapolating that measurement says alpha ~2750
// would zero it, while the lead-off block means suggest ~2300. 2600 sits
// between them, and both estimates put the residual under 2 deg.
//
// The asymmetry decides the tie-break: HIGHER alpha means less predicted coast,
// so a later stop, so a small OVERSHOOT — which is recoverable. Lower alpha
// undershoots, and correcting an undershoot needs a nudge smaller than the
// servo deadband can produce. When in doubt, lead less.
//
// The earlier 7-9 deg figure this was first built against was roughly HALF the
// truth; it had been taken under conditions that restrained rotation.
constexpr float kCoastAlphaDefault = 2600.0f;   // deg/s^2
constexpr float kCoastAlphaMin = 500.0f;
constexpr float kCoastAlphaMax = 20000.0f;
float g_coastAlpha = kCoastAlphaDefault;
// Retained so the compensation can be disabled or forced linear on the bench.
constexpr float kCoastLeadDefaultS = 0.0f;
constexpr float kCoastLeadMinS = 0.0f;
constexpr float kCoastLeadMaxS = 0.120f;
float g_coastLeadSeconds = kCoastLeadDefaultS;
uint32_t g_coastLeadUpdates = 0;

// The on-screen test sequence.
//
// Right 90, left 90, left 45, right 90, left 45 — which sums to zero, so
// "did it come back to where it started" is the pass/fail. Small angles are
// deliberately included: coast after the command stops is a fixed number of
// degrees, so it hurts a 45 degree turn proportionally far more than a 360.
//
// Each entry is followed by a settle during which the gyro bias is re-learned
// from a standstill, so no turn inherits the previous one's error.
struct TestSeqStep {
  float degrees;
  bool clockwise;
  const char* label;
};

constexpr TestSeqStep kTestSequence[] = {
    { 90.0f, true,  "R90" },
    { 90.0f, false, "L90" },
    { 45.0f, false, "L45" },
    { 90.0f, true,  "R90" },
    { 45.0f, false, "L45" },
};
constexpr uint8_t kTestSequenceLength =
    static_cast<uint8_t>(sizeof(kTestSequence) / sizeof(kTestSequence[0]));

bool g_testSeqActive = false;
uint8_t g_testSeqIndex = 0;
uint32_t g_testSeqNextAtMs = 0;
bool g_testSeqPending = false;
// Signed sum of what the robot ACTUALLY did, including coast. Should end near
// zero because the commanded angles cancel.
float g_testSeqNetDegrees = 0.0f;
// Full tilt, deliberately. The point of the test is to see what the robot does
// at the speed it actually moves at — including coast overshoot, which is
// worst at full speed and hurts the 45 degree steps most. Testing at a gentler
// speed would hide the failure mode being looked for.
int8_t g_testSeqSpeed = MovementExpression::kCommandMax;
// =========================================================================
// Straight-line driving and the autonomous RSSI survey.
//
// WHY THIS EXISTS. Rotating in place produces exactly zero RSSI information —
// the antenna's range to every AP is unchanged. TRANSLATION does change range,
// so it is the only motion that can reveal anything, and a robot that surveys
// on its own is the only way to gather enough of it.
//
// WHAT IT CAN AND CANNOT KNOW. The gyro can hold a heading, so the robot can
// drive roughly straight and turn accurately between legs. It CANNOT measure
// distance: there are no wheel encoders, so distance is time x a calibrated
// speed, and wheel slip corrupts that silently and without any observable
// signature. Everything downstream inherits that.
//
// The default speed below is not a guess pulled from nowhere. Spinning in place
// at command 100 measures ~286 deg/s = 4.99 rad/s; for a differential drive
// pivoting about its centre, wheel speed v = w*b/2. At an assumed 8 cm
// wheelbase that is 20 cm/s. It still MUST be calibrated against a tape measure,
// because slip while pivoting is not the same as slip while rolling straight.
constexpr float kForwardCmPerSecDefault = 20.0f;
constexpr float kForwardCmPerSecMin = 3.0f;
constexpr float kForwardCmPerSecMax = 100.0f;
float g_forwardCmPerSec = kForwardCmPerSecDefault;
// How many times the inertial distance has refined the wheel-speed constant.
// Zero means the value is still the derived guess, which the report says out
// loud rather than presenting a guess as a measurement.
uint32_t g_forwardCalUpdates = 0;
int8_t g_straightSpeed = MovementExpression::kSpinSpeed;

// Heading-hold gain: command units per degree off course, and the hard bound
// that stops a correction becoming a pivot.
constexpr float kHeadingHoldGain = 1.6f;
constexpr int8_t kHeadingHoldMaxTrim = 22;

TurnMeasure::HeadingHold g_headingHold;
TurnMeasure::DisplacementEstimator g_displacement;
bool g_straightActive = false;
float g_straightHeadingRef = 0.0f;
uint32_t g_straightEndsAtMs = 0;
int16_t g_straightTargetCm = 0;

// The triangle survey. Equilateral, so each exterior turn is 120 degrees and
// three legs return the robot to its start — which makes the closing position
// a free measurement of how badly dead reckoning drifted.
enum class SurveyPhase : uint8_t { Idle, Sampling, Driving, Turning, Done };
SurveyPhase g_surveyPhase = SurveyPhase::Idle;
uint8_t g_surveyVertex = 0;
uint16_t g_surveySideCm = 100;
uint16_t g_surveyScans = 12;
constexpr uint8_t kSurveyVertices = 3;

// Gyro scale gains measured by the chip's own Calibration On Demand.
//
// These are VOLATILE on the device — lost on every power cycle — so they are
// persisted here and re-applied at boot. Scale factor is the one error the
// zero-velocity update structurally cannot see (3% of zero is still zero), so
// this is the only term that would otherwise need an external angle reference.
struct CodGains {
  uint16_t x;
  uint16_t y;
  uint16_t z;
  bool valid;
};
CodGains g_codGains = {0, 0, 0, false};
constexpr char kCodNamespace[] = "imucod";
constexpr char kCodKey[] = "gains";

bool loadCodGains() {
  nvs_handle_t handle = 0;
  if (nvs_open(kCodNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  uint16_t stored[3] = {0, 0, 0};
  size_t size = sizeof(stored);
  const esp_err_t error = nvs_get_blob(handle, kCodKey, stored, &size);
  nvs_close(handle);
  if (error != ESP_OK || size != sizeof(stored)) return false;
  // A zero gain would scale the gyro to nothing; treat it as absent rather
  // than applying it.
  if (stored[0] == 0 || stored[1] == 0 || stored[2] == 0) return false;
  g_codGains.x = stored[0];
  g_codGains.y = stored[1];
  g_codGains.z = stored[2];
  g_codGains.valid = true;
  return true;
}

bool saveCodGains(uint16_t x, uint16_t y, uint16_t z) {
  const uint16_t stored[3] = {x, y, z};
  nvs_handle_t handle = 0;
  if (nvs_open(kCodNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t error = nvs_set_blob(handle, kCodKey, stored, sizeof(stored));
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  if (error != ESP_OK) return false;
  g_codGains.x = x;
  g_codGains.y = y;
  g_codGains.z = z;
  g_codGains.valid = true;
  return true;
}

// Gyro scale check state.
//
// Scale is the last unmeasured error, and the only one needing an external
// reference. The check drives N full revolutions and asks how far past the
// starting mark the robot actually stopped: the error accumulates with every
// revolution while the human's ability to read the mark does not, so 3 turns
// makes even a 3% error a visible 32 degrees.
constexpr char kScaleNamespace[] = "gyroscale";
constexpr char kScaleKey[] = "corr";
constexpr char kScaleRevsKey[] = "revs";
constexpr char kScaleBoundKey[] = "bound";
uint8_t g_scaleCheckRevs = 0;   // non-zero once a check has been run
// Upper bound on the scale error, in percent, from the last check. Zero means
// never checked. This is NOT the same as a correction of 1.0: "never measured"
// and "measured and found correct" are different claims, and conflating them
// would let an unverified sensor look verified.
float g_scaleBoundPercent = 0.0f;

// How precisely a human can read the offset from a floor mark without a
// protractor. Deliberately conservative: it sets the threshold below which a
// computed correction is indistinguishable from 1.0, and applying a correction
// smaller than the noise that produced it is false precision.
constexpr float kMarkReadingUncertaintyDeg = 3.0f;

bool loadGyroScale() {
  nvs_handle_t handle = 0;
  if (nvs_open(kScaleNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  float stored = 1.0f;
  size_t size = sizeof(stored);
  const esp_err_t error = nvs_get_blob(handle, kScaleKey, &stored, &size);
  nvs_close(handle);
  if (error != ESP_OK || size != sizeof(stored)) return false;
  if (!(stored > 0.5f) || !(stored < 2.0f)) return false;
  ImuQmi8658::setGyroScaleCorrection(stored);
  return true;
}

bool saveGyroScale(float correction) {
  nvs_handle_t handle = 0;
  if (nvs_open(kScaleNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t error = nvs_set_blob(handle, kScaleKey, &correction,
                                 sizeof(correction));
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  return error == ESP_OK;
}

// The pending revolution count has to survive the unplug, because the check is
// necessarily untethered: the cable restrains rotation.
bool saveScaleCheckRevs(uint8_t revs) {
  nvs_handle_t handle = 0;
  if (nvs_open(kScaleNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t error = nvs_set_blob(handle, kScaleRevsKey, &revs, sizeof(revs));
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  g_scaleCheckRevs = revs;
  return error == ESP_OK;
}

bool loadScaleCheckRevs() {
  nvs_handle_t handle = 0;
  if (nvs_open(kScaleNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  uint8_t stored = 0;
  size_t size = sizeof(stored);
  const esp_err_t error = nvs_get_blob(handle, kScaleRevsKey, &stored, &size);
  nvs_close(handle);
  if (error != ESP_OK || size != sizeof(stored)) return false;
  g_scaleCheckRevs = stored;
  return true;
}

// The test slot on the gesture menu exists only while something is armed, so
// instrumentation is reachable when it is needed and invisible when it is not.
static bool testSlotArmed() {
  return g_scaleCheckRevs > 0 || BenchLog::isArmed();
}

bool saveScaleBound(float percent) {
  nvs_handle_t handle = 0;
  if (nvs_open(kScaleNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  esp_err_t error = nvs_set_blob(handle, kScaleBoundKey, &percent,
                                 sizeof(percent));
  if (error == ESP_OK) error = nvs_commit(handle);
  nvs_close(handle);
  g_scaleBoundPercent = percent;
  return error == ESP_OK;
}

bool loadScaleBound() {
  nvs_handle_t handle = 0;
  if (nvs_open(kScaleNamespace, NVS_READWRITE, &handle) != ESP_OK) return false;
  float stored = 0.0f;
  size_t size = sizeof(stored);
  const esp_err_t error = nvs_get_blob(handle, kScaleBoundKey, &stored, &size);
  nvs_close(handle);
  if (error != ESP_OK || size != sizeof(stored)) return false;
  g_scaleBoundPercent = stored;
  return true;
}

// Bump / shake / pick-up detection. Runs continuously from the same
// accelerometer samples everything else uses.
MotionEvents::Detector g_motionEvents;
bool g_motionEventsReady = false;
uint32_t g_motionLastSampleMs = 0;
// The accelerometer is only polled this often when nothing else needs it.
// Fast enough to catch a 50 ms impact, slow enough not to crowd the shared
// I2C bus that the touch controller and RTC also live on.
constexpr uint32_t kMotionSampleMs = 10;
// Announced on the LCD as well as the log, since the interesting cases
// (picked up, shaken) are exactly the ones where nobody is watching a
// terminal.
bool g_motionAnnounce = true;

// Expression voice playback. Pure logic in melody.{h,cpp}; this drives it into
// PiezoTone every loop, which is also what keeps the 750 ms tone watchdog fed
// on notes longer than that.
Melody::Player g_melody;

// Long enough for the chassis to stop coasting before bias is re-measured.
constexpr uint32_t kTestSeqSettleMs = 1200;

// =========================================================================
// The bench run: one supervised, untethered session that answers every open
// movement question at once.
//
// WHY IT IS SHAPED LIKE THIS.
// Movement cannot be tested by the agent at all. A tethered turn is invalid —
// the USB cable measurably restrains rotation — and the robot must not be
// driven while plugged in. Sitting on its stand is worse still: the wheels come
// out of the friction path and the board tilt roughly doubles, which silently
// corrupts both the coast figure and the gravity projection. So every number
// that matters has to be taken with the robot unplugged, on the desk, on its
// own wheels, with a human present.
//
// That makes supervised time the scarce resource. The answer is not to test
// more often, it is to test ONCE and extract everything: this script measures
// the coast constant, the speed/rate model, and the lead-compensation A/B from
// the SAME turns, rather than asking for three separate sessions.
//
// Turns are issued as clockwise/counter-clockwise PAIRS so the robot returns to
// where it started after each repeat and stays put on the desk.
struct BenchStep {
  float degrees;
  int8_t speed;
  bool leadOn;
  uint8_t pairs;
  const char* label;
};

// Three speeds with lead off give coast-versus-rate, which is what decides
// whether coast is linear or quadratic in speed — and the same turns give the
// untethered rate model, replacing the cable-contaminated one. The final block
// repeats the fastest case with lead ON, so the A/B is against data gathered
// minutes earlier on the same surface, battery and motor temperature.
constexpr BenchStep kBenchScript[] = {
    {180.0f, 100, false, 3, "coast@100 lead-off"},
    {180.0f,  75, false, 3, "coast@75 lead-off"},
    {180.0f,  55, false, 3, "coast@55 lead-off"},
    {180.0f, 100, true,  3, "lead-on A/B @100"},
};
constexpr uint8_t kBenchScriptLength =
    static_cast<uint8_t>(sizeof(kBenchScript) / sizeof(kBenchScript[0]));

// Stationary hold at the start, which captures the desk pose and the drift
// figure that the stand measurements could not give honestly.
constexpr uint32_t kBenchBaselineMs = 30000;

enum class BenchPhase : uint8_t { Idle, Baseline, Turning, Finishing };

BenchLog::Store g_benchLog;
BenchPhase g_benchPhase = BenchPhase::Idle;
uint8_t g_benchStep = 0;
uint8_t g_benchPair = 0;
bool g_benchClockwise = true;
uint32_t g_benchPhaseUntilMs = 0;
uint32_t g_benchNextAtMs = 0;
bool g_benchPending = false;
// Suppresses the coast-lead learner for the duration of a run: the A/B is only
// meaningful if the lead constant does not move underneath it.
bool g_benchActive = false;
// Forces compensation fully off for the lead-off blocks of a bench run.
bool g_benchLeadDisabled = false;

DriveCalibration::Store g_driveCalibration;
SpinCalibration::Store g_spinCalibration;
DriveSession g_driveSession;

enum class WsEventType : uint8_t {
  Connect,
  Disconnect,
  Control,
  Mood,
  Gesture,
  Sound,
  Trim,
  MalformedControl,
  ProtocolFailure,
  TransportFailure,
  Unknown,
};

struct WsEvent {
  WsEventType type;
  uint32_t client;
  uint32_t receivedAtMs;
  uint32_t transportGeneration;
  uintptr_t transportToken;
  uint8_t transportFlags;
  uint8_t seq;
  int16_t left;
  int16_t right;
  uint8_t values[4];
};

constexpr uint8_t kWsEventTransportOwner = 0x01U;
constexpr uint8_t kWsEventDriveSessionOwner = 0x02U;
constexpr uint8_t kWsEventTerminalFault = 0x04U;
constexpr UBaseType_t kWsEventQueueDepth = 24;
constexpr size_t kTelemetryFrameSize = 30;
QueueHandle_t g_wsEventQueue = nullptr;
std::atomic<bool> g_wsIngressBlocked{true};
std::atomic<uint32_t> g_wsIngressActive{0};
std::atomic<bool> g_wsQueueEmergency{false};
// Terminal protocol/queue faults poison the entire credential activation.
// Transport resets and disconnects do not clear this latch; only a credential
// lifecycle boundary may do so.
std::atomic<bool> g_activationTerminalFaulted{false};
std::atomic<uint32_t> g_wsQueueOverflowCount{0};
portMUX_TYPE g_wsTransportMux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> g_wsTransportGeneration{1};
std::atomic<uintptr_t> g_wsTransportOwnerToken{0};
std::atomic<uint32_t> g_wsTransportOwnerClient{0};
std::atomic<uint32_t> g_wsTransportOwnerGeneration{0};
std::atomic<uintptr_t> g_wsTransportFaultToken{0};
std::atomic<uint32_t> g_wsTransportFaultGeneration{0};
std::atomic<uint32_t> g_wsTransportOpenCount{0};
std::atomic<uint32_t> g_wsTransportLastTelemetryMs{0};
std::atomic<uint32_t> g_wsTelemetrySequence{0};
std::atomic<uint8_t> g_wsTelemetryFrame[kTelemetryFrameSize]{};
std::atomic<uintptr_t> g_driveSessionTransportToken{0};
std::atomic<uint32_t> g_driveSessionTransportGeneration{0};
// WPA2 requires 8..63 characters; 32 covers any SSID/password worth typing.
constexpr size_t kApSsidMaxLength = 32;
constexpr size_t kApPasswordMaxLength = 32;
constexpr size_t kAccessTokenHexLength = 16;

struct ActivationCredentials {
  char ssid[kApSsidMaxLength + 1];
  char password[kApPasswordMaxLength + 1];
  char accessToken[kAccessTokenHexLength + 1];
  bool valid;
};

portMUX_TYPE g_credentialMux = portMUX_INITIALIZER_UNLOCKED;
ActivationCredentials g_activationCredentials = {};
std::atomic<bool> g_apLifecycleActive{false};
// Arduino's AP reconfiguration emits AP_START -> AP_STOP -> AP_START. Event
// delivery can lag past softAP() returning, so the callback closes this
// window only after it sees the post-bounce AP_START (or a short timeout).
std::atomic<bool> g_apStartupInProgress{false};
std::atomic<bool> g_apStartupStopSeen{false};
std::atomic<uint32_t> g_apStartupDeadlineMs{0};
// Intentional AP stops are serialized: the callback is the sole consumer.
// A new activation cannot start until the matching AP_STOP has consumed this.
std::atomic<bool> g_apExpectedStopPending{false};
std::atomic<bool> g_apStopFaultPending{false};

static inline void latchDriveEventQueueEmergency() {
  g_activationTerminalFaulted.store(true, std::memory_order_release);
  g_wsQueueEmergency.store(true, std::memory_order_release);
}

class DriveIngressQuarantine {
 public:
  explicit DriveIngressQuarantine(const char* reason);
  ~DriveIngressQuarantine();

  DriveIngressQuarantine(const DriveIngressQuarantine&) = delete;
  DriveIngressQuarantine& operator=(const DriveIngressQuarantine&) = delete;

 private:
  const char* reason_;
};

constexpr uint16_t kAllCalibrationFieldsTouched = 0x03FFU;
uint16_t g_calibrationTouchedMask = 0;
DriveCalibration::Snapshot g_calibrationPending =
    DriveCalibration::legacyDraft();

int16_t  g_lastRawDriveL       = 0;
int16_t  g_lastRawDriveR       = 0;
int16_t  g_lastEffectiveDriveL = 0;
int16_t  g_lastEffectiveDriveR = 0;
uint32_t g_lastControlEventMs  = 0;
uint32_t g_lastDriveClient     = 0;
uint8_t  g_lastDriveSeq        = 0;
const char* g_lastDriveSource  = "none";

uint16_t* g_eyeRegionBackup = nullptr;

// [DEBUG-DELETE] BEGIN — demo mood label overlay (bottom-right red text).
// Used during mood-review demos so we can identify the mood on-camera.
// Remove this block + all matching `// [DEBUG-DELETE]` lines before ship.
static const char* g_demoMoodLabel = "DEFAULT";
// [DEBUG-DELETE] END

// Battery meter — permanent feature, see PROJECT-HISTORY §4.6 (D19).
// Reads VBAT via the 200k/100k divider on GPIO1 (BAT_ADC), averages 16
// samples for noise, runs an exponential moving average filter to mask
// the brief voltage sag from servo current spikes, and slew-limits the
// displayed percentage to ±1% per tick so it never visibly bounces.
// This mimics how phones smooth their battery indicator (sans the
// coulomb-counting fuel-gauge IC we don't have).
constexpr uint16_t kBatSampleCount       = 16;
constexpr uint8_t  kBatDividerRatio      = 3;     // (200k + 100k) / 100k
constexpr uint32_t kBatUpdateIntervalMs  = 1000;  // sample + EMA tick
constexpr uint32_t kBatPctUpdateMs       = 5000;  // % only ticks every 5s
constexpr float    kBatEmaAlpha          = 0.10f; // 10% new / 90% old → smooth
// Charging detection (no hardware VBUS sense on this board, so infer from
// VBAT trend). Pure raw-vs-raw delta: compare the latest ADC reading to
// the previous one. Both samples are taken at kBatUpdateIntervalMs.
//   raw_now > raw_prev + kBatSlopeUp   → voltage rose  → charging
//   raw_now < raw_prev + kBatSlopeDown → voltage fell  → not charging
//   otherwise                           → flat → keep last state
// Note: this is sensitive to single-sample noise — a servo-current sag
// between two adjacent reads will look like a discharge event. Widen the
// deadband if you see false unplug triggers.
constexpr float    kBatSlopeUp           = 0.010f; // +10mV raw delta → CHG
constexpr float    kBatSlopeDown         = -0.010f;// -10mV raw delta → !CHG
constexpr float    kBatBootChargeV       = 4.10f;  // seed CHG=true at boot if raw > this
constexpr int16_t  kBatLabelY            = 4;
constexpr int16_t  kBatLabelH            = 36;
uint32_t g_batLastUpdateMs = 0;
uint32_t g_batLastPctMs    = 0;
float    g_batVoltage      = 0.0f;    // fast EMA — what we display
float    g_batVoltageRaw   = 0.0f;    // latest unfiltered reading (for log)
float    g_batVoltageRawPrev = 0.0f;  // previous raw reading (for slope detect)
bool     g_batSeeded       = false;
bool     g_batCharging     = false;   // inferred from voltage trend
uint8_t  g_batPercent      = 0;       // displayed (slew-limited) %

bool mountLittleFs() {
  return LittleFS.begin(/*formatOnFail=*/true,
                        /*basePath=*/"/littlefs",
                        /*maxOpenFiles=*/10,
                        /*partitionLabel=*/"littlefs");
}

bool initDisplay() {
  // Load the persisted SPI clock BEFORE the bus is created — it is a
  // construction-time parameter, so changing it needs a reboot to take effect.
  {
    nvs_handle_t handle = 0;
    if (nvs_open("lcdcfg", NVS_READWRITE, &handle) == ESP_OK) {
      uint32_t stored = 0;
      if (nvs_get_u32(handle, "spihz", &stored) == ESP_OK &&
          stored >= kLcdSpiHzMin && stored <= kLcdSpiHzMax) {
        g_lcdSpiHz = stored;
      }
      nvs_close(handle);
    }
  }

  g_bus = new Arduino_ESP32SPI(LCD_DC, LCD_CS, LCD_SCK, LCD_MOSI);
  if (g_bus == nullptr) {
    return false;
  }

  // ST7789V2 on the Waveshare LCD-1.69:
  //  - panel is 240x280 mapped inside a 240x320 RAM, so row offset = 20
  //  - ips=true triggers color-inversion (matches BSP's panel_invert_color(true))
  g_gfx = new Arduino_ST7789(g_bus,
                             LCD_RST,
                             kLcdRotate,
                             /*ips=*/true,
                             /*width=*/kScreenW,
                             /*height=*/kScreenH,
                             /*col_offset1=*/0,
                             /*row_offset1=*/LCD_ROW_OFFSET,
                             /*col_offset2=*/0,
                             /*row_offset2=*/0);
  if (g_gfx == nullptr) {
    return false;
  }

  g_canvas = new Arduino_Canvas(kScreenW, kScreenH, g_gfx);
  if (g_canvas == nullptr) {
    return false;
  }

  if (!g_canvas->begin(g_lcdSpiHz)) {
    return false;
  }

  // Second offscreen canvas for the pairing card. Same 240x280x2 = 134,400 B
  // framebuffer, allocated in PSRAM (8 MB free at boot). Constructed WITHOUT
  // an output pointer so begin() only allocates the framebuffer and does not
  // re-run g_gfx->begin() -- the panel has already been initialised above and
  // running init a second time on a working panel invites transient corruption
  // on this ST7789V2 (see kLcdSpiHzDefault).
  //
  // Allocation is fail-soft. Null g_sheetCanvas means the drag-preview
  // compositor is disabled everywhere it is used (serviceDragTransition
  // returns immediately; the observer's chip-suppression is skipped so the
  // chip's own SwipeDown continues to drive the pairing flow instantly as it
  // did before this feature existed). This board's LCD path has a documented
  // history of mystery blackouts under a 5 s task watchdog, and a null
  // dereference inside the strip pusher is exactly the shape of failure that
  // presents as a spontaneous reset.
  g_sheetCanvas = new Arduino_Canvas(kScreenW, kScreenH, nullptr);
  if (g_sheetCanvas == nullptr) {
    Serial.println(F("lcd: sheet canvas alloc FAILED -- drag preview disabled"));
  } else if (!g_sheetCanvas->begin(0)) {
    Serial.println(F("lcd: sheet canvas begin FAILED -- drag preview disabled"));
    delete g_sheetCanvas;
    g_sheetCanvas = nullptr;
  } else {
    Serial.printf("lcd: sheet canvas ready (%u B in PSRAM)\n",
                  static_cast<unsigned>(kScreenPixelCount * 2U));
  }

  Serial.printf("lcd: SPI %lu Hz (%lu MHz)\n",
                static_cast<unsigned long>(g_lcdSpiHz),
                static_cast<unsigned long>(g_lcdSpiHz / 1000000UL));

  // VERIFY the panel rather than assume it. This is the whole point of the
  // read-back work: a corrupted init byte leaves the panel asleep or blanked,
  // and without this check nothing downstream can tell -- the backlight, the
  // rails and every pixel push all still report success into a dead panel.
  //
  // Retry rather than merely report, because the failure is a transient bit
  // error: re-running init is very likely to succeed where the last attempt
  // did not. Bounded, because a panel that never answers is a wiring fault and
  // looping forever would just hide it.
  constexpr uint8_t kMaxInitAttempts = 3;
  for (uint8_t attempt = 1; attempt <= kMaxInitAttempts; ++attempt) {
    const uint8_t rddpm = lcdReadPowerMode();
    if (lcdPanelLooksAlive(rddpm)) {
      if (attempt > 1) {
        Serial.printf("lcd: panel OK after %u attempts (RDDPM=0x%02X)\n",
                      static_cast<unsigned>(attempt),
                      static_cast<unsigned>(rddpm));
      } else {
        Serial.printf("lcd: panel verified RDDPM=0x%02X\n",
                      static_cast<unsigned>(rddpm));
      }
      g_lcdInitAttempts = attempt;
      return true;
    }
    Serial.printf("lcd: PANEL NOT READY RDDPM=0x%02X (SLPOUT=%u DISON=%u) "
                  "attempt %u/%u\n",
                  static_cast<unsigned>(rddpm),
                  static_cast<unsigned>((rddpm >> 4) & 1),
                  static_cast<unsigned>((rddpm >> 2) & 1),
                  static_cast<unsigned>(attempt),
                  static_cast<unsigned>(kMaxInitAttempts));
    if (attempt < kMaxInitAttempts) {
      // Re-run the panel's own init (hardware reset + SWRESET + sequence).
      // Repainting would be useless here: pixels cannot wake a sleeping panel.
      g_gfx->begin(static_cast<int32_t>(g_lcdSpiHz));
    }
  }

  g_lcdInitAttempts = kMaxInitAttempts;
  Serial.println(F("lcd: panel FAILED verification -- continuing anyway"));
  return true;
}

// ---------------------------------------------------------------------------
// Panel read-back probe.
//
// The whole reason the black-screen fault has been so hard to pin down is that
// nothing could see the panel: this board routes SDA only (no SDO), so the
// firmware had no way to tell an initialised panel from a dead one. Every
// status it could report -- backlight, PWM, SYS_EN -- was true and irrelevant.
//
// But the panel is not actually write-only. ST7789V2 datasheet 8.4 (p.55):
// 4-line serial interface I uses "SDA (serial input/output data)", and 8.4.1
// notes the driver "shifts SDA (output data) at the falling edge of SCL". So
// the same GPIO can be released and read.
//
// RDDPM (0x0Ah, datasheet 9.1.5 p.172) returns the two bits that matter:
//     bit4 SLPOUT  1 = sleep out   (did SLPOUT 0x11 take?)
//     bit2 DISON   1 = display on  (did DISPON 0x29 take?)
// plus BSTON bit7 and NORON bit3. A healthy panel reads 0x9C.
//
// Reads are far slower than writes -- TSCYCR min 150 ns (6.67 MHz) against
// TSCYCW's 16 ns -- so this is bit-banged well inside spec.
//
// SCK and MOSI belong to the SPI peripheral, so they are borrowed for the
// transaction and handed back by re-running the bus setup, which re-attaches
// the pins WITHOUT sending anything to the panel. CS and DC are already plain
// GPIOs owned by us.
uint32_t lcdProbeRead(uint8_t command, uint8_t bitsToRead) {
  pinMode(LCD_SCK, OUTPUT);
  pinMode(LCD_MOSI, OUTPUT);
  digitalWrite(LCD_SCK, LOW);
  delayMicroseconds(2);

  digitalWrite(LCD_CS, LOW);
  digitalWrite(LCD_DC, LOW);  // command phase
  for (int8_t bit = 7; bit >= 0; --bit) {
    digitalWrite(LCD_MOSI, (command >> bit) & 0x01);
    delayMicroseconds(1);
    digitalWrite(LCD_SCK, HIGH);  // panel samples input on the rising edge
    delayMicroseconds(1);
    digitalWrite(LCD_SCK, LOW);
  }

  digitalWrite(LCD_DC, HIGH);  // parameter phase
  pinMode(LCD_MOSI, INPUT);    // release SDA so the panel can drive it
  delayMicroseconds(2);

  uint32_t value = 0;
  for (uint8_t i = 0; i < bitsToRead; ++i) {
    digitalWrite(LCD_SCK, HIGH);
    delayMicroseconds(1);
    // Panel shifts on the falling edge, so the bit is stable while SCK is high.
    value = (value << 1) | (digitalRead(LCD_MOSI) ? 1U : 0U);
    digitalWrite(LCD_SCK, LOW);
    delayMicroseconds(1);
  }

  digitalWrite(LCD_CS, HIGH);

  // Hand the pins back to the SPI peripheral. This reconfigures the ESP32 side
  // only -- it issues no panel commands, so the display state is untouched.
  if (g_bus != nullptr) {
    g_bus->begin(static_cast<int32_t>(g_lcdSpiHz));
  }
  return value;
}

// Reads RDDPM (0x0A) and returns the status byte.
//
// Framing note, measured rather than assumed: the datasheet shows a "dummy
// read" parameter before the status byte, which on the parallel bus is a whole
// dummy BYTE. On this serial interface it is not -- a 24-bit read returns
// 0x9C0000, i.e. the status byte arrives FIRST and the rest is idle line. So
// eight clocks is the whole transaction.
uint8_t lcdReadPowerMode() {
  return static_cast<uint8_t>(lcdProbeRead(0x0A, 8));
}

// True when RDDPM reports a panel that is awake and displaying.
bool lcdPanelLooksAlive(uint8_t rddpm) {
  const bool sleepOut = (rddpm & 0x10) != 0;
  const bool displayOn = (rddpm & 0x04) != 0;
  return sleepOut && displayOn;
}

// ---------------------------------------------------------------------------
// Serial output gating.
//
// THE BLACK SCREEN. Captured TASK_WDT resets landed in "loop: demo step" and
// "loop: battery meter" -- two phases with nothing in common except that both
// call Serial.printf. Combined with the user's observation that the display
// recovers the instant a serial port is OPENED (with no command sent), that
// points at the USB CDC transmit path rather than the display.
//
// This board is ARDUINO_USB_MODE=1 + ARDUINO_USB_CDC_ON_BOOT=1, so `Serial`
// is the built-in USB-Serial-JTAG peripheral. With no host draining it, its
// small TX FIFO fills; the routine chatter here is roughly 400+ bytes/s
// (diag + lcdact + bat every second), so it fills in well under a second.
// A write that then waits for space stalls the loop task, nothing feeds the
// watchdog, and TASK_WDT resets the board ~5 s later. Opening the port drains
// the FIFO and the loop resumes -- which is exactly the "it came back when you
// connected" behaviour, and why every earlier "fix" appeared to work: each one
// required opening the port to send it.
//
// Serial.setTxTimeoutMs(0) is already set and is supposed to drop instead of
// block; it demonstrably is not enough on its own here.
//
// So: routine, periodic chatter is emitted only when a host is actually
// attached. HWCDC::operator bool() reports host presence. Errors, boot lines
// and command replies are left ungated -- they are rare, and a command reply
// only happens when a host is by definition attached.
// True only when a host is attached AND the USB CDC TX FIFO has room for a
// full diagnostic line.
//
// Gating on host-attachment alone was NOT enough, and the measurement that
// proved it is worth recording. With the port closed the board survived a
// 150 s idle window with no reset. It then reset while a host WAS attached --
// because an attached host is not necessarily a host that is *reading*. A
// script that opens the port, writes a command, sleeps, and only then reads
// leaves the FIFO to fill exactly as if nothing were attached, and the next
// write blocks the loop task until TASK_WDT fires ~5 s later.
//
// availableForWrite() is the only condition that actually matters: if the
// bytes will not fit, skip the line. Diagnostics are best-effort by nature --
// dropping one is strictly better than stalling the control loop, and a
// dropped line is invisible whereas a watchdog reset blanks the screen.
//
// 200 bytes covers the longest periodic line (the 1 Hz `diag:` record).
inline bool serialCanWrite(size_t bytes = 200) {
  return static_cast<bool>(Serial) &&
         static_cast<size_t>(Serial.availableForWrite()) >= bytes;
}

void setEyeLoopRate(bool animating) {
  const uint16_t desiredFrameMs = animating ? kFastFrameMs : kIdleFrameMs;
  if (desiredFrameMs == g_targetFrameMs) {
    return;
  }

  g_targetFrameMs = desiredFrameMs;
  g_eyes.setFramerate(animating ? kFastEyeFps : kIdleEyeFps);
}

void requestImmediateFrame() {
  const uint32_t now = millis();
  g_lastRedrawMs = (now >= g_targetFrameMs) ? (now - g_targetFrameMs) : 0U;
}

void recomputeEyeRegionBounds() {
  g_eyeMaxW = static_cast<int16_t>(g_eyeBaseW + ((g_eyeBaseW + 3U) / 4U));
  g_eyeMaxH = g_eyeBaseH;
  const int16_t surprisedPad = g_surprisedActive ? kSurprisedRingExtent : static_cast<int16_t>(0);
  g_eyeRegionX = static_cast<int16_t>(g_activeLeftCenterX - g_eyeMotionX - (g_eyeMaxW / 2) - kEyeDirtyMargin - surprisedPad);
  g_eyeRegionY = static_cast<int16_t>(kLeftEyeCenterY - g_eyeMotionY - (g_eyeMaxH / 2) - kEyeDirtyMargin - surprisedPad);
  g_eyeRegionRight = static_cast<int16_t>(g_activeRightCenterX + g_eyeMotionX + (g_eyeMaxW / 2) + kEyeDirtyMargin + surprisedPad);
  g_eyeRegionBottom = static_cast<int16_t>(kRightEyeCenterY + g_eyeMotionY + (g_eyeMaxH / 2) + kEyeDirtyMargin + surprisedPad);
  g_eyeRegionW = static_cast<int16_t>(g_eyeRegionRight - g_eyeRegionX);
  g_eyeRegionH = static_cast<int16_t>(g_eyeRegionBottom - g_eyeRegionY);
}

// Forward decl so setEyesMood can call drawMascotToCanvas to repaint the
// background after a SURPRISED transition shrinks the eye region.
void drawMascotToCanvas(int16_t originX, int16_t originY);

// [DEBUG-DELETE] forward decls for demo mood label overlay
static const char* moodToUpperName(EyeMood m);
static void drawDemoMoodLabel();
// [DEBUG-DELETE] END

// =========================================================================
// §16 — Override macro state machine (forward decls + early globals).
// Moved up here so dispatchCommand() can poke them.
// =========================================================================
enum OverrideState : uint8_t {
  ORS_IDLE      = 0,
  ORS_WIFI_BOOT = 1,
  ORS_RUNNING   = 2,
  ORS_GRACE     = 3,
};
volatile OverrideState g_orState           = ORS_IDLE;
volatile uint32_t      g_orStateEnterMs    = 0;
volatile bool          g_orPendingSwipeDown = false;
volatile bool          g_orPendingSwipeUp   = false;
// ---------------------------------------------------------------------------
// Drag-to-commit state.
//
// A swipe used to be an instant, all-or-nothing event: the chip decided a
// gesture had happened and the screen changed. There was no way to start the
// motion, see what it was going to do, and change your mind.
//
// The current-position ledger has moved into SheetTransition::Controller
// (g_sheet, declared above). Finger samples are pulled by serviceDragTransition
// via readTouchPointDirect on the loop task, and the controller owns the
// press-origin / velocity / settle target -- there is no longer any drag state
// written from inside the touch callback.
volatile uint32_t      g_orTouchEventCount  = 0;
// Streams raw gesture IDs and touch coordinates so the panel-to-screen axis
// mapping can be established by measurement rather than assumption. The chip
// reports in the panel's native orientation, which is 90 degrees off from the
// screen on this board — the swipe IDs had to be found the same way.
// Default ON while the touch panel is being diagnosed, so the beep works
// straight from boot with no serial session attached. Turn off with
// `touchdebug off` once the panel is trusted.
bool                   g_touchDebug         = true;
// Audible touch feedback. Every attempt to diagnose the touch panel over
// serial required Nick and the capture window to be synchronised, which they
// never reliably were — "zero events" could equally mean broken hardware or
// nobody touching it. A beep closes the feedback loop on the device so the
// panel can be tested at his own pace, with no serial session at all.
volatile uint32_t      g_orTouchPointCount  = 0;
volatile uint32_t      g_touchBeepUntilMs   = 0;
constexpr uint32_t     kTouchBeepMs         = 70;
constexpr uint16_t     kTouchBeepHz         = 2600;

// Ring buffer of recent touches. Every attempt to capture coordinates over a
// live serial window failed because it required Nick and the capture to be
// synchronised — "zero events" was always ambiguous. Recording them lets him
// tap whenever he likes (the beep confirms each one) and the log be read back
// afterwards, which removes the coupling entirely.
struct TouchLogEntry {
  uint32_t ms;
  int16_t x;
  int16_t y;
  int8_t gestureId;   // -1 for a plain touch point
  bool pressed;
};
constexpr uint8_t kTouchLogSize = 24;
TouchLogEntry g_touchLog[kTouchLogSize];
volatile uint8_t g_touchLogNext = 0;
volatile uint32_t g_touchLogTotal = 0;

static void recordTouch(int gestureId, int x, int y, bool pressed) {
  const uint8_t slot = g_touchLogNext;
  g_touchLog[slot].ms = millis();
  g_touchLog[slot].x = static_cast<int16_t>(x);
  g_touchLog[slot].y = static_cast<int16_t>(y);
  g_touchLog[slot].gestureId = static_cast<int8_t>(gestureId);
  g_touchLog[slot].pressed = pressed;
  g_touchLogNext = static_cast<uint8_t>((slot + 1) % kTouchLogSize);
  g_touchLogTotal++;
}

// =========================================================================
// On-screen gesture buttons.
//
// Three full-width buttons, one per expression, so a movement can be triggered
// with the robot untethered — no serial session, no phone, no WiFi. Reached by
// swiping up from the idle face (swipe-down is already the WiFi/QR flow), and
// swiping up again leaves.
//
// A FOURTH button appears only while a bench run or scale check is armed.
//
// That conditional slot is deliberate. Those measurements are only meaningful
// untethered — measured on hardware 2026-08-05, a tethered robot cannot
// complete a rotation at all, because the USB cable restrains it — so they need
// a way to start without a cable. But they are instrumentation, not features,
// and a permanent "360 test" button on a robot whose job is to emote is clutter
// that invites a mis-tap. Arming is already required and already persists
// across the unplug, so the button can simply follow the armed state: present
// when it is useful, absent the rest of the time.
//
// Targets are 50 px tall against a 44 px minimum, because this is operated by
// a fingertip on a 1.69" panel while the robot may be moving.
// =========================================================================
constexpr uint8_t kGestureExpressionCount = 3;
// Layout is still sized for four slots so the armed test button lands in a
// position that was already checked against the footer.
constexpr uint8_t kGestureButtonCount = 4;
constexpr int16_t kGestureBtnX = 10;
constexpr int16_t kGestureBtnW = kScreenW - (2 * kGestureBtnX);   // 220
constexpr int16_t kGestureBtnH = 50;
constexpr int16_t kGestureBtnTop = 40;
constexpr int16_t kGestureBtnGap = 6;
// Last button ends at 40 + 4*50 + 3*6 = 258, leaving the footer row clear.
// Checked rather than assumed: an earlier 72 px layout ran to y=273 and the
// footer at y=268 was drawn on top of the last button.
constexpr int16_t kGestureFooterY = 264;

// Index of the conditional test slot — not a movement, handled separately and
// only drawn while something is armed.
constexpr uint8_t kGestureTestIndex = 3;

const char* const kGestureButtonNames[kGestureButtonCount] = {
    "frustrated", "excited", "scared", "run test"};
// One accent per button so they are never told apart by position alone.
const uint16_t kGestureButtonColors[kGestureButtonCount] = {
    0xF9A6,  // warm red    — frustrated
    0x5FEB,  // bright cyan — excited
    0xB59F,  // violet      — scared
    0xFD20,  // amber       — armed measurement, not a mood
};

static inline int16_t gestureButtonY(uint8_t index) {
  return static_cast<int16_t>(kGestureBtnTop +
                              (index * (kGestureBtnH + kGestureBtnGap)));
}

// Returns the button under a touch, or -1. Kept pure so it is unit-testable
// and so the coordinate convention lives in exactly one place.
//
// `slots` is how many buttons are currently on screen, which varies: the test
// slot only exists while a measurement is armed. Passing it in rather than
// reading a global keeps this testable.
static int8_t gestureButtonAtWithSlots(int16_t x, int16_t y, uint8_t slots) {
  if (x < kGestureBtnX || x >= (kGestureBtnX + kGestureBtnW)) return -1;
  for (uint8_t i = 0; i < slots; ++i) {
    const int16_t top = gestureButtonY(i);
    if (y >= top && y < (top + kGestureBtnH)) {
      return static_cast<int8_t>(i);
    }
  }
  return -1;
}

// True while a bench run or scale check is armed, which is the only time the
// test slot exists on screen.
static bool testSlotArmed();

// Only the armed test slot is touchable now; the movement buttons are gone.
static int8_t gestureButtonAt(int16_t x, int16_t y) {
  if (!testSlotArmed()) return -1;
  if (x < kGestureBtnX || x >= (kGestureBtnX + kGestureBtnW)) return -1;
  const int16_t top = gestureButtonY(kGestureTestIndex);
  if (y >= top && y < (top + kGestureBtnH)) {
    return static_cast<int8_t>(kGestureTestIndex);
  }
  return -1;
}

// Set by the touch observer, consumed by the loop. The observer only stamps
// intent; starting a gesture takes the drive mutex and must not happen from a
// callback.
volatile int8_t g_gesturePendingButton = -1;
volatile int8_t g_gestureFlashButton = -1;
volatile uint32_t g_gestureFlashUntilMs = 0;
// Last touch in SCREEN space, drawn on the menu so a wrong mapping stays
// visible on the device instead of having to be inferred from a serial capture.
volatile int16_t g_lastTouchX = -1;
volatile int16_t g_lastTouchY = -1;

// The CST816T reports in the panel's native orientation, which is rotated 90
// degrees from the screen. This is not a guess: the swipe IDs above were
// confirmed on this exact hardware, and screen swipe-DOWN arrives as the
// chip's GESTURE_RIGHT — so increasing screen Y is increasing chip X, and the
// two axes are swapped.
//
//   screenY = chipX          (confirmed by the swipe mapping)
//   screenX = 240 - chipY    (the perpendicular axis)
//
// The X mirror is the one part not pinned down by the swipe evidence, and it
// deliberately does not matter for the button menu: the buttons span the full
// width, so only Y selects between them. If a future UI needs precise X, that
// is the thing to verify first.
static void mapTouchToScreen(int rawX, int rawY, int16_t& screenX,
                             int16_t& screenY) {
  screenY = static_cast<int16_t>(rawX);
  screenX = static_cast<int16_t>(kScreenW - rawY);
  if (screenX < 0) screenX = 0;
  if (screenX >= kScreenW) screenX = kScreenW - 1;
  if (screenY < 0) screenY = 0;
  if (screenY >= kScreenH) screenY = kScreenH - 1;
}

// Defined with the touch driver further down; declared here so the serial
// command handler above it can query and reconfigure the chip.
extern MDO::CST816Touch_HWMode g_orTouch;
constexpr uint32_t kOrGraceTimeoutMs = 30000;    // 30 s post-disconnect grace
constexpr uint32_t kOrWifiBootMaxMs  = 10000;    // safety: abort if AP fails to come up

// LcdMode also referenced by dispatchCommand's `state` command.
enum LcdMode : uint8_t {
  LCD_MODE_FACE         = 0,
  LCD_MODE_QR_WIFI      = 1,
  LCD_MODE_QR_URL       = 2,
  LCD_MODE_ESTABLISHING = 3,  // §16: "Establishing Server..." while AP boots
  LCD_MODE_GESTURE_MENU = 4,  // on-screen buttons to trigger movement gestures
};
volatile LcdMode  g_lcdMode        = LCD_MODE_FACE;
volatile uint32_t g_lcdNextPollMs  = 0;     // set 0 to force an immediate poll
static   LcdMode  g_lcdPendingMode = LCD_MODE_FACE;
static   uint32_t g_lcdPendingMs   = 0;
constexpr uint32_t kLcdPollIntervalMs = 400;

// WiFi/server globals referenced by dispatchCommand for the `wifi`/`state`
// commands. Constructed early so they're alive by the time setup() runs.
bool           g_overrideUp        = false;
extern const uint16_t kOverridePort;     // defined later in the WiFi section (= 80)
// Defined further down; needed by the websocket auth guard below.
static bool requestHasValidAccessToken(AsyncWebServerRequest* request);

AsyncWebServer g_orServer(80);           // MUST match kOverridePort; literal to dodge fwd-decl
AsyncWebSocket g_orWs("/ws");

// Intercepts /ws upgrades that carry no valid activation token, so they are
// answered with 403 at the HTTP layer instead of becoming a websocket client
// that then has to be closed from inside its own connect callback. See the
// registration site for the crash this prevents.
class WsAuthGuardHandler : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest* request) override {
    if (request->method() != HTTP_GET || !request->url().equals("/ws")) {
      return false;
    }
    // Only claim the request when the token is missing/wrong; a valid one
    // falls through to the real websocket handler registered after this.
    return !requestHasValidAccessToken(request);
  }

  void handleRequest(AsyncWebServerRequest* request) override {
    Serial.println(F("ws: upgrade refused at handshake (no valid token)"));
    request->send(403, "text/plain", "Forbidden");
  }
};

WsAuthGuardHandler g_orWsAuthGuard;
DNSServer      g_orDns;
uint32_t       g_orTelemetryNextMs = 0;

static void enterIdleDemo();
static void enterWifiBoot();
static void enterRunning();
static void enterGrace();
static void teardownToIdle();
static void startWifiServer();
static void stopWifiServer();
static void handleApLifecycleFault(const char* reason);
static void enterLcdQrWifiMode();
static bool setBoostEn(bool on);
static void powerDownDriveMotors(const char* reason);
static void requestWsTransportReset();
void beginWsIngressQuarantine();
static void resetDriveControl(const char* reason);
void stopMovementGesture(const char* reason);
// Cancels a gesture in flight from any context. Deliberately independent of
// DriveSession::state(), because a fault reset and an idle robot are the same
// state (SafeOff) and only one of them should cancel. See resetDriveControl.
void requestMovementAbort(const char* reason);
static void serviceGestureMenu(uint32_t now);
bool startMovementGesture(const char* name, uint16_t runMs = 0);
bool startSpinCalibration(uint16_t fullTurnMs, bool clockwise,
                          int8_t speed = MovementExpression::kSpinSpeed);
bool startTurnAutoMeasure(uint16_t fullTurnMs, bool clockwise,
                          int8_t speed = MovementExpression::kSpinSpeed);
bool startTurnCalibration();
bool startTurnTestSequence();
void serviceTurnTestSequence(uint32_t now);
void serviceHeading(uint32_t now);
void serviceRfScan(uint32_t now);
void serviceBench(uint32_t now);
void serviceStraightDrive(uint32_t now);
void serviceSurvey(uint32_t now);
void serviceMotionEvents(uint32_t now);
void serviceMelody(uint32_t now);
bool playMood(MelodyGen::Mood mood);
bool playMoodByName(const char* name);
uint32_t turnTimeoutMsFor(float degrees, int8_t speed);
bool startStraightDrive(int16_t centimetres, int8_t speed);
bool startTriangleSurvey(uint16_t sideCm, uint16_t scans);
void surveyFinish(const char* reason);
bool startBenchRun();
void benchRecordTurn(float coastDegrees, float rateAtStopDps,
                     float errorDegrees);
void benchAdvance(uint32_t now);
bool establishYawAxis();
float projectedYawRate(const ImuQmi8658::Sample& gyro);
bool startClosedLoopTurn(float degrees, bool clockwise,
                         int8_t speed = MovementExpression::kSpinSpeed);
// True whenever anything is driving the motors or owns the gyro, so a command
// that blocks for seconds can refuse rather than starve the control loop.
bool motionInProgress();
static void serviceTurnAutoMeasure(uint32_t now);
static void driveSignalPinsLow();static void serviceDriveControl(uint32_t now);
static bool refreshDriveHardwareLease();
static void resetDriveHardwareSafetyLatch();
static bool initBacklight();
static void setBacklightLevel(uint8_t level);
static void runStartupLogoAnimation();
static void runSleepLogoAnimation();
static void runWakeLogoAnimation();
[[noreturn]] static void runPowerOffSequence();
void stopDemo(bool restoreDefaults);
void applyDriveDecision(const DriveSession::Decision& decision,
                        const char* source, uint32_t client, uint8_t seq,
                        int rawLeft, int rawRight, uint32_t now);

// Forward decls for battery meter overlay
static float   batteryReadVoltage();
static uint8_t batteryVoltsToPercent(float v);
// PERF-2: battery meter push gating.
//
// Data source ticks at 1 Hz (kBatUpdateIntervalMs) but drawBatteryMeter()
// used to fire every eye-render frame whose dirty rect brushed the strip
// (SURPRISED/SCARED at ~50 Hz), and every g_forceFullPush. That is 50+
// LCD pushes/s for content that changes at 1 Hz.
//
// Gate: cache (rawMv, dspMv, pct, charging) and only re-render into
// canvas + push when either
//   (a) the content actually changed, OR
//   (b) the canvas strip has been clobbered (eye bled into it, or the
//       mascot background was just repainted), so canvas is stale.
//
// Callers set g_batStripCanvasFresh=false BEFORE calling drawBatteryMeter
// whenever they know the strip in canvas is now something other than the
// last battery text (eye pixels, mascot pixels, etc). drawBatteryMeter
// sets it TRUE on successful render. When the content matches AND the
// canvas is fresh, drawBatteryMeter is a no-op.
struct BatteryMeterSignature {
  uint16_t rawMv;
  uint16_t dspMv;
  uint8_t  pct;
  bool     charging;
  bool     valid;
};
static BatteryMeterSignature g_batMeterLastSig = {0, 0, 0, false, false};
static bool g_batStripCanvasFresh = false;

static void    drawBatteryMeter();
static void    updateBatteryMeter();
// True if [y, y+h) intersects the battery-meter strip at the top of the
// screen. Used to know when an eye-region push needs us to re-overlay
// the meter so expressive faces (SURPRISED/SCARED) don't paint over it.
static inline bool rectTouchesBatStrip(int16_t y, int16_t h) {
  return (y < static_cast<int16_t>(kBatLabelY + kBatLabelH)) &&
         (static_cast<int16_t>(y + h) > kBatLabelY);
}

// Wrap EyeEngine::setMood so MOOD_SURPRISED transparently swaps in larger
// square eyes pushed outward; switching to any other mood restores the
// user's previous dims and the mascot's default centers.
//
// CRITICAL: any mood change that shrinks the eye-region bounds (i.e.
// SURPRISED -> anything-else) must repaint the mascot bg over the OLD
// wider region on the canvas BEFORE the next frame, otherwise the
// previous mood's outer pixels (e.g. SURPRISED's outer ring) become
// permanent stale artifacts -- the new mood's dirty rect is smaller and
// will never overwrite them, and `wake` (which does g_forceFullPush)
// would only re-display whatever's on the canvas, making it worse.
static void setEyesMood(EyeMood m) {
  const bool wantSurprised = (m == MOOD_SURPRISED);
  const bool boundsChanging = (wantSurprised != g_surprisedActive);

  // Snapshot OLD bounds before recompute so we can union them with the
  // new bounds and clear any stale pixels in the difference area.
  const int16_t oldRegionX = g_eyeRegionX;
  const int16_t oldRegionY = g_eyeRegionY;
  const int16_t oldRegionRight = g_eyeRegionRight;
  const int16_t oldRegionBottom = g_eyeRegionBottom;

  if (wantSurprised && !g_surprisedActive) {
    g_savedEyeBaseW = g_eyeBaseW;
    g_savedEyeBaseH = g_eyeBaseH;
    g_savedEyeRadius = g_eyeRadius;
    g_eyeBaseW = kSurprisedEyeW;
    g_eyeBaseH = kSurprisedEyeH;
    g_eyeRadius = kSurprisedEyeR;
    g_eyeGeom.baseW = g_eyeBaseW;
    g_eyeGeom.baseH = g_eyeBaseH;
    g_eyeGeom.borderRadius = g_eyeRadius;
    g_activeLeftCenterX = static_cast<int16_t>(kLeftEyeCenterX - kSurprisedCenterOffset);
    g_activeRightCenterX = static_cast<int16_t>(kRightEyeCenterX + kSurprisedCenterOffset);
    g_eyes.setDims(g_eyeBaseW, g_eyeBaseH, g_eyeRadius);
    g_eyes.setCenters(g_activeLeftCenterX, kLeftEyeCenterY,
                      g_activeRightCenterX, kRightEyeCenterY);
    recomputeEyeRegionBounds();
    g_surprisedActive = true;
  } else if (!wantSurprised && g_surprisedActive) {
    g_eyeBaseW = g_savedEyeBaseW;
    g_eyeBaseH = g_savedEyeBaseH;
    g_eyeRadius = g_savedEyeRadius;
    g_eyeGeom.baseW = g_eyeBaseW;
    g_eyeGeom.baseH = g_eyeBaseH;
    g_eyeGeom.borderRadius = g_eyeRadius;
    g_activeLeftCenterX = kLeftEyeCenterX;
    g_activeRightCenterX = kRightEyeCenterX;
    g_eyes.setDims(g_eyeBaseW, g_eyeBaseH, g_eyeRadius);
    g_eyes.setCenters(g_activeLeftCenterX, kLeftEyeCenterY,
                      g_activeRightCenterX, kRightEyeCenterY);
    recomputeEyeRegionBounds();
    g_surprisedActive = false;
  }
  if (boundsChanging) {
    // Eye geometry is changing (SURPRISED <-> other). Snap the engine's
    // mood overlay state so it doesn't fade the previous mood's
    // decorations (e.g. SURPRISED's outer ring) at the NEW eye centers,
    // which would paint them in the wrong place and leave artifacts
    // outside the new dirty rect.
    g_eyes.snapMood(m);
  } else {
    g_eyes.setMood(m);
  }

  if (boundsChanging && g_canvas != nullptr) {
    // Union of old + new eye-region bounds covers the stale-pixel band
    // (i.e. where SURPRISED's outer ring used to live, now outside the
    // smaller default region). Repaint mascot bg there, then redraw the
    // mascot bitmap so the rest of the face stays intact. Force a full
    // LCD push next frame so the screen catches up.
    const int16_t unionX = (oldRegionX < g_eyeRegionX) ? oldRegionX : g_eyeRegionX;
    const int16_t unionY = (oldRegionY < g_eyeRegionY) ? oldRegionY : g_eyeRegionY;
    const int16_t unionRight = (oldRegionRight > g_eyeRegionRight) ? oldRegionRight : g_eyeRegionRight;
    const int16_t unionBottom = (oldRegionBottom > g_eyeRegionBottom) ? oldRegionBottom : g_eyeRegionBottom;
    g_canvas->fillRect(unionX, unionY,
                       static_cast<int16_t>(unionRight - unionX),
                       static_cast<int16_t>(unionBottom - unionY),
                       MASCOT_A_BG);
    drawMascotToCanvas(static_cast<int16_t>(kOriginX + g_lastShiftX),
                       static_cast<int16_t>(kOriginY + g_lastShiftY));
    g_forceFullPush = true;
    g_forceEyeRedraw = true;
  }

  // [DEBUG-DELETE] keep label in sync with the mood we just set, then
  // repaint it (the boundsChanging path already requests a full push, but
  // the non-boundsChanging path needs us to push the label rect ourself
  // since the eye-only dirty rect won't reach the bottom-right margin).
  // Also re-overlay the battery meter — the full-mascot repaint above
  // wipes the top strip on the canvas, and SURPRISED/SCARED eye regions
  // reach up into that strip, so without this the meter disappears for a
  // beat after every expressive-mood change.
  g_demoMoodLabel = moodToUpperName(m);
  drawDemoMoodLabel();
  if (g_batSeeded) {
    // PERF-2: the full-mascot repaint above clobbers the meter strip in
    // canvas -- invalidate so drawBatteryMeter() re-renders even when
    // the numeric content has not moved.
    g_batStripCanvasFresh = false;
    drawBatteryMeter();
  }
  // [DEBUG-DELETE] END
}

void drawMascotToCanvas(int16_t originX, int16_t originY) {
  if (g_canvas == nullptr) {
    return;
  }

  // PERF-2: the mascot bitmap is full-screen, so drawing it wipes the
  // battery strip in canvas. Anyone who then calls drawBatteryMeter()
  // must see that the canvas strip is stale, even when the numeric
  // content has not moved since the last render.
  g_batStripCanvasFresh = false;

  g_canvas->draw16bitRGBBitmap(originX,
                               originY,
                               const_cast<uint16_t*>(MASCOT_A_RGB565),
                               MASCOT_A_W,
                               MASCOT_A_H);
}

void pushCanvasRectToLcd(int16_t x, int16_t y, int16_t w, int16_t h);
void backupEyeRegionFromCanvas();
void drawMascotToCanvas(int16_t originX, int16_t originY);

void pushCanvasRectToLcd(int16_t x, int16_t y, int16_t w, int16_t h) {
  if ((g_canvas == nullptr) || (g_gfx == nullptr)) {
    return;
  }
  // Counted so the 1 Hz diag can report push activity. The black screen is
  // suspected to be "the loop stopped pushing", but until now nothing
  // distinguished that from "the pushes happen and the panel drops them" --
  // and every fix so far has involved SENDING a serial command, which is
  // itself a perturbation. These counters are readable PASSIVELY.
  ++g_lcdPushCount;
  g_lcdLastPushMs = millis();
  if (x < 0) {
    w += x;
    x = 0;
  }
  if (y < 0) {
    h += y;
    y = 0;
  }
  if (x + w > kScreenW) {
    w = kScreenW - x;
  }
  if (y + h > kScreenH) {
    h = kScreenH - y;
  }
  if ((w <= 0) || (h <= 0)) {
    return;
  }

  uint16_t* framebuffer = g_canvas->getFramebuffer();
  if (framebuffer == nullptr) {
    return;
  }

  // PERF: single SPI transaction + single writeAddrWindow for the entire
  // rect, then stream pixels. Replaces the previous row-by-row
  // draw16bitRGBBitmap loop which fired h x (startWrite + CASET+RASET+RAMWR +
  // endWrite) -- ~280 transactions for a full-screen push, now 1.
  // Step crumbs across the SPI transaction. A TASK_WDT reset was captured in
  // the "battery meter" phase, which ends in a push -- but that phase covers
  // an ADC read AND this transaction, and this function has three places that
  // can block indefinitely: startWrite() takes the SPI bus lock, and
  // writeAddrWindow/writePixels poll the peripheral's busy bit. A spin on a
  // peripheral that never reports done never feeds the watchdog, which is
  // exactly a 5 s TASK_WDT. These detail codes name the stage.
  Arduino_TFT* tft = static_cast<Arduino_TFT*>(g_gfx);
  breadcrumb(kCrumbLcdPush, 1);
  tft->startWrite();
  breadcrumb(kCrumbLcdPush, 2);
  tft->writeAddrWindow(static_cast<uint16_t>(x),
                       static_cast<uint16_t>(y),
                       static_cast<uint16_t>(w),
                       static_cast<uint16_t>(h));
  breadcrumb(kCrumbLcdPush, 3);
  if ((x == 0) && (w == kScreenW)) {
    // Full-width: rows are contiguous in the framebuffer -- single blast.
    tft->writePixels(&framebuffer[static_cast<int32_t>(y) * kScreenW],
                     static_cast<uint32_t>(w) * static_cast<uint32_t>(h));
  } else {
    // Partial-width: writePixels per row, but all inside the same SPI
    // transaction (no addr-window churn between rows).
    for (int16_t row = 0; row < h; ++row) {
      tft->writePixels(&framebuffer[(static_cast<int32_t>(y + row) * kScreenW) + x],
                       static_cast<uint32_t>(w));
    }
  }
  breadcrumb(kCrumbLcdPush, 4);
  tft->endWrite();
  breadcrumb(kCrumbLcdPush, 5);
}

static bool initBacklight() {
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, LOW);
  g_backlightPwmReady =
      ledcSetup(
          kBacklightChannel, kBacklightFrequencyHz,
          kBacklightResolutionBits) != 0U;
  if (g_backlightPwmReady) {
    ledcAttachPin(LCD_BL, kBacklightChannel);
    ledcWrite(kBacklightChannel, 0U);
  }
  g_backlightLevel = 0U;
  return g_backlightPwmReady;
}

static void setBacklightLevel(uint8_t level) {
  if (g_backlightPwmReady) {
    ledcWrite(kBacklightChannel, level);
  } else {
    digitalWrite(LCD_BL, level == 0U ? LOW : HIGH);
  }
  g_backlightLevel = level;
}

static void fadeBacklightTo(uint8_t target, uint32_t durationMs) {
  const uint8_t start = g_backlightLevel;
  if (start == target) {
    return;
  }
  if (!g_backlightPwmReady || durationMs == 0U) {
    setBacklightLevel(target);
    return;
  }
  for (uint8_t step = 1U; step <= kLogoTransitionSteps; ++step) {
    const uint8_t alpha =
        DisplayTransition::smoothstep8(step, kLogoTransitionSteps);
    const int32_t delta =
        static_cast<int32_t>(target) - static_cast<int32_t>(start);
    const int32_t level =
        static_cast<int32_t>(start) +
        (delta * static_cast<int32_t>(alpha) +
         (delta >= 0 ? 127 : -127)) /
            255;
    setBacklightLevel(static_cast<uint8_t>(level));
    delay(durationMs / kLogoTransitionSteps);
  }
  setBacklightLevel(target);
}

static uint8_t garageLogoAlpha(size_t index) {
  return GARAGE_LOGO_ALPHA[index];
}

static void drawGarageLogoToCanvas() {
  if (g_canvas == nullptr) {
    return;
  }
  uint16_t* framebuffer = g_canvas->getFramebuffer();
  if (framebuffer == nullptr) {
    return;
  }
  for (size_t index = 0; index < kScreenPixelCount; ++index) {
    framebuffer[index] = DisplayTransition::compositeWhiteOver(
        0x0000U, garageLogoAlpha(index));
  }
}

static uint16_t* allocateTransitionFrame() {
  return static_cast<uint16_t*>(heap_caps_malloc(
      kScreenPixelCount * sizeof(uint16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void renderFaceTargetToCanvas() {
  if (g_canvas == nullptr) {
    return;
  }
  g_canvas->fillScreen(MASCOT_A_BG);
  drawMascotToCanvas(
      static_cast<int16_t>(kOriginX + g_lastShiftX),
      static_cast<int16_t>(kOriginY + g_lastShiftY));
  if (g_eyeEngineReady) {
    g_eyes.forceRedraw();
    (void)g_eyes.update();
    g_eyes.clearDirty();
  }
}

static void blendGarageToTarget(const uint16_t* target) {
  uint16_t* framebuffer =
      g_canvas == nullptr ? nullptr : g_canvas->getFramebuffer();
  if (framebuffer == nullptr || target == nullptr) {
    return;
  }
  for (uint8_t step = 1U; step <= kLogoTransitionSteps; ++step) {
    const uint8_t alpha =
        DisplayTransition::smoothstep8(step, kLogoTransitionSteps);
    for (size_t index = 0; index < kScreenPixelCount; ++index) {
      framebuffer[index] = DisplayTransition::startupPixel(
          target[index], garageLogoAlpha(index), alpha);
    }
    pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
    delay(kLogoTransitionFrameMs);
  }
}

static void blendSourceToGarage(const uint16_t* source) {
  uint16_t* framebuffer =
      g_canvas == nullptr ? nullptr : g_canvas->getFramebuffer();
  if (framebuffer == nullptr || source == nullptr) {
    return;
  }
  for (uint8_t step = 1U; step <= kLogoTransitionSteps; ++step) {
    const uint8_t alpha =
        DisplayTransition::smoothstep8(step, kLogoTransitionSteps);
    for (size_t index = 0; index < kScreenPixelCount; ++index) {
      framebuffer[index] = DisplayTransition::shutdownPixel(
          source[index], garageLogoAlpha(index), alpha);
    }
    pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
    delay(kLogoTransitionFrameMs);
  }
}

static void runStartupLogoAnimation() {
  if (g_canvas == nullptr || g_gfx == nullptr) {
    setBacklightLevel(kBacklightMax);
    return;
  }

  renderFaceTargetToCanvas();
  uint16_t* faceTarget = allocateTransitionFrame();
  uint16_t* framebuffer = g_canvas->getFramebuffer();
  if (faceTarget != nullptr && framebuffer != nullptr) {
    memcpy(
        faceTarget, framebuffer,
        kScreenPixelCount * sizeof(uint16_t));
  }

  drawGarageLogoToCanvas();
  pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
  fadeBacklightTo(kBacklightMax, kBacklightFadeMs);
  delay(kStartupLogoHoldMs);

  if (faceTarget != nullptr) {
    blendGarageToTarget(faceTarget);
    free(faceTarget);
  } else {
    renderFaceTargetToCanvas();
    pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
  }

  g_lcdMode = LCD_MODE_FACE;
  g_forceFullPush = false;
  g_forceEyeRedraw = false;
  requestImmediateFrame();
}

static void runSleepLogoAnimation() {
  if (g_canvas == nullptr || g_gfx == nullptr) {
    setBacklightLevel(0U);
    return;
  }

  uint16_t* source = allocateTransitionFrame();
  uint16_t* framebuffer = g_canvas->getFramebuffer();
  if (source != nullptr && framebuffer != nullptr) {
    memcpy(
        source, framebuffer,
        kScreenPixelCount * sizeof(uint16_t));
    blendSourceToGarage(source);
    free(source);
  } else {
    drawGarageLogoToCanvas();
    pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
  }

  delay(kShutdownLogoHoldMs);
  fadeBacklightTo(0U, kBacklightFadeMs);
}

static void runWakeLogoAnimation() {
  setBacklightLevel(0U);
  runStartupLogoAnimation();
}

// [DEBUG-DELETE] BEGIN — demo mood label overlay helpers.
// Renders the current mood name in RED, all-caps, in the bottom-right
// corner. Used during mood-review demos only. Remove this block + all
// matching `// [DEBUG-DELETE]` lines before ship.
//
// READ-6: this table must stay in sync with kMoodCommands and kDemoMoods
// near the top of this file. Adding a new mood requires updating all
// three (plus EyeEngine's own tables in lib/EyeEngine).
static const char* moodToUpperName(EyeMood m) {
  switch (m) {
    case MOOD_DEFAULT:   return "DEFAULT";
    case MOOD_BORED:     return "BORED";
    case MOOD_ANNOYED:   return "ANNOYED";
    case MOOD_TIRED:     return "TIRED";
    case MOOD_ANGRY:     return "ANGRY";
    case MOOD_HAPPY:     return "HAPPY";
    case MOOD_SAD:       return "SAD";
    case MOOD_THINK:     return "THINK";
    case MOOD_ERROR:     return "ERROR";
    case MOOD_SURPRISED: return "SURPRISED";
    case MOOD_SCARED:    return "SCARED";
    case MOOD_WINK:      return "WINK";
    case MOOD_SLEEPING:  return "SLEEPING";
    case MOOD_LOVE:      return "LOVE";
    case MOOD_CONFUSED:  return "CONFUSED";
    case MOOD_CURIOUS:   return "CURIOUS";
  }
  // READ-11: an unmapped mood used to fall through silently -- if
  // EyeEngine ever emits a new EyeMood before this switch is extended
  // we would just start rendering "?" on the overlay with no clue why.
  // Log once per unmapped id (gated so a spurious high-rate call cannot
  // saturate the CDC FIFO); the label still renders as "?" so nothing
  // higher up crashes.
  if (serialCanWrite()) {
    Serial.printf("moodToUpperName: unmapped mood id=%d -> \"?\"\n",
                  static_cast<int>(m));
  }
  return "?";
}

static void drawDemoMoodLabel() {
  if ((g_canvas == nullptr) || (g_gfx == nullptr)) {
    return;
  }
  // Full-width strip in the black margin below the mascot. Clearing the
  // whole strip every update lets the centered text shift left/right as
  // mood names change length without leaving stale glyph tails.
  constexpr int16_t kLabelH = 18;
  const int16_t ly = static_cast<int16_t>(kScreenH - kLabelH);
  g_canvas->fillRect(0, ly, kScreenW, kLabelH, MASCOT_A_BG);
  g_canvas->setTextSize(2);
  g_canvas->setTextColor(0xF800);  // RED in RGB565
  g_canvas->setTextWrap(false);
  const int16_t textW = static_cast<int16_t>(strlen(g_demoMoodLabel) * 12);
  const int16_t cursorX = static_cast<int16_t>((kScreenW - textW) / 2);
  const int16_t cursorY = static_cast<int16_t>(kScreenH - 17);
  g_canvas->setCursor(cursorX, cursorY);
  g_canvas->print(g_demoMoodLabel);
  pushCanvasRectToLcd(0, ly, kScreenW, kLabelH);
}
// [DEBUG-DELETE] END

// Battery meter — permanent feature, see PROJECT-HISTORY §4.6 (D19).
static float batteryReadVoltage() {
  uint32_t sumMv = 0;
  for (uint16_t i = 0; i < kBatSampleCount; i++) {
    sumMv += analogReadMilliVolts(BAT_ADC);
  }
  const float adcVolts = static_cast<float>(sumMv / kBatSampleCount) / 1000.0f;
  return adcVolts * static_cast<float>(kBatDividerRatio);
}

static uint8_t batteryVoltsToPercent(float v) {
  // Piecewise-linear 1S LiPo curve calibrated for this rig:
  // 4.10V == 100% (charger plateau under load). Range 3.30-4.10V.
  if (v >= 4.10f) return 100;
  if (v >= 4.00f) return static_cast<uint8_t>(85  + (v - 4.00f) * 150.0f);
  if (v >= 3.90f) return static_cast<uint8_t>(70  + (v - 3.90f) * 150.0f);
  if (v >= 3.80f) return static_cast<uint8_t>(55  + (v - 3.80f) * 150.0f);
  if (v >= 3.70f) return static_cast<uint8_t>(40  + (v - 3.70f) * 150.0f);
  if (v >= 3.60f) return static_cast<uint8_t>(25  + (v - 3.60f) * 150.0f);
  if (v >= 3.50f) return static_cast<uint8_t>(12  + (v - 3.50f) * 130.0f);
  if (v >= 3.30f) return static_cast<uint8_t>((v - 3.30f) * 60.0f);
  return 0;
}

static void drawBatteryMeter() {
  if ((g_canvas == nullptr) || (g_gfx == nullptr)) {
    return;
  }

  // PERF-2 gate: build a small signature from the render inputs and
  // compare against the last successful render. If the inputs match AND
  // canvas is fresh (nobody clobbered our strip since last time), we can
  // return without touching canvas or LCD -- the previous push is still
  // valid on screen.
  const BatteryMeterSignature sig = {
      static_cast<uint16_t>(g_batVoltageRaw * 1000.0f + 0.5f),
      static_cast<uint16_t>(g_batVoltage    * 1000.0f + 0.5f),
      g_batPercent,
      g_batCharging,
      /*valid=*/true,
  };
  if (g_batMeterLastSig.valid && g_batStripCanvasFresh &&
      g_batMeterLastSig.rawMv    == sig.rawMv &&
      g_batMeterLastSig.dspMv    == sig.dspMv &&
      g_batMeterLastSig.pct      == sig.pct  &&
      g_batMeterLastSig.charging == sig.charging) {
    return;
  }

  // Top row: raw (unfiltered) voltage straight off the ADC.
  // Bottom row: filtered/displayed voltage + slew-limited percentage,
  // plus a CHG tag when we think USB power is connected.
  char rawBuf[24];
  char dspBuf[28];
  snprintf(rawBuf, sizeof(rawBuf), "act %.3fV",
           static_cast<double>(g_batVoltageRaw));
  snprintf(dspBuf, sizeof(dspBuf), "dsp %.3fV %u%%%s",
           static_cast<double>(g_batVoltage),
           static_cast<unsigned>(g_batPercent),
           g_batCharging ? " CHG" : "");

  // Color the bottom row by percent; flip to CYAN while charging so the
  // state is obvious at a glance even when % is already at 100.
  uint16_t dspColor = 0x07E0;                       // GREEN
  if (g_batPercent < 20)       dspColor = 0xF800;   // RED
  else if (g_batPercent < 50)  dspColor = 0xFFE0;   // YELLOW
  if (g_batCharging)           dspColor = 0x07FF;   // CYAN
  constexpr uint16_t rawColor = 0xFFFF;             // WHITE

  g_canvas->fillRect(0, kBatLabelY, kScreenW, kBatLabelH, MASCOT_A_BG);
  g_canvas->setTextSize(2);
  g_canvas->setTextWrap(false);

  // Row 1 (raw)
  const int16_t rawW = static_cast<int16_t>(strlen(rawBuf) * 12);
  g_canvas->setTextColor(rawColor);
  g_canvas->setCursor(static_cast<int16_t>((kScreenW - rawW) / 2),
                      static_cast<int16_t>(kBatLabelY + 2));
  g_canvas->print(rawBuf);

  // Row 2 (displayed/filtered)
  const int16_t dspW = static_cast<int16_t>(strlen(dspBuf) * 12);
  g_canvas->setTextColor(dspColor);
  g_canvas->setCursor(static_cast<int16_t>((kScreenW - dspW) / 2),
                      static_cast<int16_t>(kBatLabelY + 20));
  g_canvas->print(dspBuf);

  pushCanvasRectToLcd(0, kBatLabelY, kScreenW, kBatLabelH);

  g_batMeterLastSig     = sig;
  g_batStripCanvasFresh = true;
}

static void updateBatteryMeter() {
  const uint32_t now = millis();
  if ((g_batLastUpdateMs != 0) && (now - g_batLastUpdateMs < kBatUpdateIntervalMs)) {
    return;
  }
  g_batLastUpdateMs = now;

  breadcrumb(kCrumbLoopBattery, 1);
  g_batVoltageRaw = batteryReadVoltage();
  breadcrumb(kCrumbLoopBattery, 2);
  if (!g_batSeeded) {
    // First sample on boot: skip the filters and seed everything to the
    // current reading so the display doesn't slow-ramp from 0V/0%. Seed
    // the charging state from absolute voltage (slope detector has no
    // history yet, so we can't tell direction from a single sample).
    g_batVoltage        = g_batVoltageRaw;
    g_batVoltageRawPrev = g_batVoltageRaw;
    g_batPercent        = batteryVoltsToPercent(g_batVoltage);
    g_batCharging       = (g_batVoltageRaw > kBatBootChargeV);
    g_batLastPctMs      = now;
    g_batSeeded         = true;
  } else {
    // Fast EMA — masks short-lived dips (servo current spike sags VBAT
    // ~50mV momentarily, EMA smooths that to ~5mV).
    g_batVoltage = (kBatEmaAlpha * g_batVoltageRaw)
                 + ((1.0f - kBatEmaAlpha) * g_batVoltage);

    // Slew-limit the displayed percentage. Even if the underlying %
    // jumps several points, we walk toward it at ±1% per kBatPctUpdateMs.
    if ((now - g_batLastPctMs) >= kBatPctUpdateMs) {
      g_batLastPctMs = now;
      const uint8_t targetPct = batteryVoltsToPercent(g_batVoltage);
      if (targetPct > g_batPercent && g_batPercent < 100) {
        g_batPercent++;
      } else if (targetPct < g_batPercent && g_batPercent > 0) {
        g_batPercent--;
      }
    }
  }

  // Slope-based charging detector: compare the latest raw reading to the
  // previous raw reading. Direct dV between consecutive ADC samples — no
  // filtering, no baseline EMA. Servo-current sags between samples will
  // register here; widen the deadband if they cause false unplug events.
  const float slope = g_batVoltageRaw - g_batVoltageRawPrev;
  if (g_batSeeded) {
    if (slope > kBatSlopeUp) {
      g_batCharging = true;
    } else if (slope < kBatSlopeDown) {
      g_batCharging = false;
    }
    // else: flat tick → keep last state.
  }
  g_batVoltageRawPrev = g_batVoltageRaw;

  drawBatteryMeter();
  if (serialCanWrite()) {
    Serial.printf("bat raw=%.3fV filt=%.3fV slope=%+.3fV pct=%u%s\n",
                  static_cast<double>(g_batVoltageRaw),
                  static_cast<double>(g_batVoltage),
                  static_cast<double>(slope),
                  static_cast<unsigned>(g_batPercent),
                  g_batCharging ? " CHG" : "");
  }
}

// Lazily allocated in PSRAM, and only when head-sympathy shift is actually
// switched on.
//
// This was a static .bss array sized to the worst-case eye rectangle:
// 191 x 120 x 2 B = 45,840 bytes (confirmed from the linker map, not
// calculated) of INTERNAL SRAM -- about 44% of all RAM the firmware reported
// in use. Its only consumer is applyHeadSympathyShift(), which does nothing
// unless g_headShiftXDiv/YDiv are non-zero, and both default to 0. So the
// board was permanently holding 45 KB of fast internal RAM for a feature that
// is off until somebody types `headgain`.
//
// PSRAM is the right home: this is a bulk memcpy target touched at most once
// per frame, not cache-hot state, and the canvas framebuffer already lives
// there. Returns false if allocation fails, in which case the caller simply
// skips the backup rather than writing out of bounds.
bool ensureEyeRegionBackup() {
  if (g_eyeRegionBackup != nullptr) return true;
  constexpr size_t kBytes =
      static_cast<size_t>(kEyeBackupMaxW) * kEyeBackupMaxH * sizeof(uint16_t);
  g_eyeRegionBackup = static_cast<uint16_t*>(ps_malloc(kBytes));
  if (g_eyeRegionBackup == nullptr) {
    Serial.println(F("eye: backup buffer alloc FAILED; head shift disabled"));
    return false;
  }
  memset(g_eyeRegionBackup, 0, kBytes);
  Serial.printf("eye: backup buffer %u B allocated in PSRAM\n",
                static_cast<unsigned>(kBytes));
  return true;
}

void backupEyeRegionFromCanvas() {
  if (g_canvas == nullptr) {
    return;
  }

  uint16_t* framebuffer = g_canvas->getFramebuffer();
  if (framebuffer == nullptr) {
    return;
  }
  if (!ensureEyeRegionBackup()) {
    return;
  }

  for (int16_t row = 0; row < g_eyeRegionH; ++row) {
    memcpy(&g_eyeRegionBackup[row * g_eyeRegionW],
           &framebuffer[(static_cast<int32_t>(g_eyeRegionY + row) * kScreenW) + g_eyeRegionX],
           static_cast<size_t>(g_eyeRegionW) * sizeof(uint16_t));
  }
}

void restoreEyeRegionToCanvas() {
  if (g_canvas == nullptr || g_eyeRegionBackup == nullptr) {
    return;
  }

  g_canvas->draw16bitRGBBitmap(g_eyeRegionX, g_eyeRegionY, g_eyeRegionBackup, g_eyeRegionW, g_eyeRegionH);
}

// --- Motor drive infrastructure -------------------------------------------
uint32_t driveHardwareNowMs() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000LL);
}

void onDriveHardwareTimer(void*) {
  if (!g_driveHardwareLeaseArmed.load(std::memory_order_acquire)) {
    return;
  }
  const uint32_t now = driveHardwareNowMs();
  const uint32_t deadline =
      g_driveHardwareDeadlineMs.load(std::memory_order_acquire);
  if (static_cast<int32_t>(now - deadline) < 0) {
    return;
  }

  portENTER_CRITICAL(&g_driveHardwareMux);
  const bool expired =
      g_driveHardwareLeaseArmed.load(std::memory_order_relaxed) &&
      static_cast<int32_t>(
          now - g_driveHardwareDeadlineMs.load(std::memory_order_relaxed)) >=
          0;
  if (expired) {
    g_driveHardwareInhibit.store(true, std::memory_order_relaxed);
    g_driveHardwareLeaseArmed.store(false, std::memory_order_relaxed);
    g_driveHardwareTripPending.store(true, std::memory_order_release);
    g_driveHardwareTripCount.fetch_add(1, std::memory_order_relaxed);
    if (BOOST_EN_GPIO >= 0) {
      gpio_set_level(
          static_cast<gpio_num_t>(BOOST_EN_GPIO), 0);
    }
    g_driveRailOn.store(false, std::memory_order_release);
  }
  portEXIT_CRITICAL(&g_driveHardwareMux);
}

bool initDriveHardwareSafetyTimer() {
  g_driveHardwareTimerReady.store(false, std::memory_order_release);
  g_driveHardwareLeaseArmed.store(false, std::memory_order_release);
  g_driveHardwareInhibit.store(true, std::memory_order_release);
  g_driveHardwareTripPending.store(false, std::memory_order_release);

  esp_timer_create_args_t args = {};
  args.callback = &onDriveHardwareTimer;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "drive-safety";
  args.skip_unhandled_events = true;
  if (esp_timer_create(&args, &g_driveHardwareTimer) != ESP_OK) {
    return false;
  }
  if (esp_timer_start_periodic(g_driveHardwareTimer, 10000ULL) != ESP_OK) {
    esp_timer_delete(g_driveHardwareTimer);
    g_driveHardwareTimer = nullptr;
    return false;
  }

  portENTER_CRITICAL(&g_driveHardwareMux);
  g_driveHardwareTimerReady.store(true, std::memory_order_relaxed);
  g_driveHardwareInhibit.store(false, std::memory_order_relaxed);
  portEXIT_CRITICAL(&g_driveHardwareMux);
  return true;
}

static bool refreshDriveHardwareLease() {
  const uint32_t deadline =
      driveHardwareNowMs() + DriveSession::kControlTimeoutMs;
  bool refreshed = false;
  portENTER_CRITICAL(&g_driveHardwareMux);
  if (g_driveHardwareTimerReady.load(std::memory_order_relaxed) &&
      !g_driveHardwareInhibit.load(std::memory_order_relaxed) &&
      !g_systemShuttingDown) {
    g_driveHardwareDeadlineMs.store(deadline, std::memory_order_relaxed);
    g_driveHardwareLeaseArmed.store(true, std::memory_order_release);
    refreshed = true;
  }
  portEXIT_CRITICAL(&g_driveHardwareMux);
  return refreshed;
}

static void resetDriveHardwareSafetyLatch() {
  portENTER_CRITICAL(&g_driveHardwareMux);
  g_driveHardwareLeaseArmed.store(false, std::memory_order_relaxed);
  g_driveHardwareTripPending.store(false, std::memory_order_relaxed);
  g_driveHardwareInhibit.store(
      !g_driveHardwareTimerReady.load(std::memory_order_relaxed),
      std::memory_order_release);
  if (BOOST_EN_GPIO >= 0) {
    gpio_set_level(static_cast<gpio_num_t>(BOOST_EN_GPIO), 0);
  }
  g_driveRailOn.store(false, std::memory_order_release);
  portEXIT_CRITICAL(&g_driveHardwareMux);
}

void initServoTimers() {
  // Pre-allocate a PWM timer slot so the first attach() call is fast.
  // Servo library lazily allocates otherwise.
  ESP32PWM::allocateTimer(0);
  if (!initDriveHardwareSafetyTimer()) {
    setBoostEn(false);
    driveSignalPinsLow();
    Serial.println(
        F("FATAL: independent drive safety timer failed; motors disabled"));
  } else {
    Serial.println(F("drive safety: independent 300ms EN cutoff ready"));
  }
  g_driveMutex = xSemaphoreCreateMutex();
  if (g_driveMutex == nullptr) {
    setBoostEn(false);
    driveSignalPinsLow();
    Serial.println(F("FATAL: drive mutex allocation failed; motors disabled"));
  }
  g_wsEventQueue = xQueueCreate(kWsEventQueueDepth, sizeof(WsEvent));
  if (g_wsEventQueue == nullptr) {
    latchDriveEventQueueEmergency();
    setBoostEn(false);
    driveSignalPinsLow();
    Serial.println(F("FATAL: drive event queue allocation failed; motors disabled"));
  } else {
    g_wsQueueEmergency.store(false, std::memory_order_release);
    g_wsIngressBlocked.store(false, std::memory_order_release);
  }
}

bool servoChannelConfigured(Servo& servo, int8_t channel) {
  return channel >= 0 && servo.attached() &&
         servo.readTimerWidth() == kServoTimerWidthBits;
}

bool servoPwmHardwareMatches(Servo& servo, int8_t channel) {
  return servoChannelConfigured(servo, channel) &&
         ledcReadFreq(static_cast<uint8_t>(channel)) == 50U &&
         ledcRead(static_cast<uint8_t>(channel)) ==
             static_cast<uint32_t>(servo.readTicks());
}

bool servoReadbackMatchesRequest(int requestedUs, int readbackUs) {
  return readbackUs > 0 &&
         abs(readbackUs - requestedUs) <= kServoMaxReadbackErrorUs;
}

void ensureServoAttached(Servo& s, bool& attachedFlag, int8_t& channel,
                         uint8_t pin, const char* label) {
  attachedFlag =
      servoChannelConfigured(s, channel);
  if (!attachedFlag) {
    if (s.attached()) {
      s.release();
      s.detach();
    }
    channel = -1;
    const int attachedChannel =
        s.attach(pin, kServoMinPulseUs, kServoMaxPulseUs);
    if (s.attached()) {
      // ESP32Servo 1.2.1 resets timer width to 10 bits in attach().
      // Reconfigure only while the 5V rail is off, then verify it stuck.
      s.setPeriodHertz(50);
      s.setTimerWidth(kServoTimerWidthBits);
    }
    if (attachedChannel >= 0 && attachedChannel <= INT8_MAX) {
      channel = static_cast<int8_t>(attachedChannel);
    }
    attachedFlag = servoChannelConfigured(s, channel);
    if (!attachedFlag && s.attached()) {
      s.release();
      s.detach();
      channel = -1;
    }
    Serial.printf(
        "servo %s attach GPIO%u channel=%d 1000..2000us timerBits=%d %s\n",
                  label, static_cast<unsigned>(pin),
                  static_cast<int>(channel),
                  s.readTimerWidth(),
                  attachedFlag ? "OK" : "FAILED");
  }
}

void detachOneServo(Servo& s, bool& attachedFlag, int8_t& channel,
                    const char* label) {
  attachedFlag = s.attached();
  if (attachedFlag) {
    s.release();
    s.detach();
    attachedFlag = s.attached();
    Serial.printf("servo %s duty released; detached=%u\n",
                  label, attachedFlag ? 0U : 1U);
  }
  channel = -1;
}

void detachAllServos() {
  detachOneServo(
      g_servoLeft, g_servoLeftAttached, g_servoLeftChannel, "L");
  detachOneServo(
      g_servoRight, g_servoRightAttached, g_servoRightChannel, "R");
}

// --- Power button long-press → power off ----------------------------------
// SYS_OUT (GPIO40) reads LOW while the user is holding the side power button.
// A 3-second hold runs the visual shutdown, then drives SYS_EN LOW. Battery
// power disappears; USB bypasses the latch, so USB enters a soft-off wait
// and restarts on the next deliberate power-button press.
constexpr uint32_t kPowerOffHoldMs = 3000;

// Ignore the button for this long after boot. Covers reset and USB
// re-enumeration transients, when GPIO40's state is not yet trustworthy.
constexpr uint32_t kPowerButtonGraceMs = 1500;

// SYS_OUT is configured as a plain INPUT with no internal pull-up, so a
// floating or stuck-low pin would otherwise read as a deliberate 3-second
// hold and silently power the board down. Two independent guards:
//   1. The line must be observed HIGH at least once before any press counts,
//      so a pin that is low from boot can never trigger a shutdown.
//   2. Nothing counts at all during the boot grace period.
// Neither changes the electrical configuration, which is deliberate: the
// external pull-up situation has not been confirmed against the schematic, so
// the idle level is logged at boot instead of assumed.
bool g_powerButtonArmed = false;
bool g_powerOffTriggered = false;
uint32_t g_powerButtonLowSinceMs = 0;

[[noreturn]] static void runPowerOffSequence() {
  Serial.println(F("power: shutdown sequence"));
  g_systemShuttingDown = true;
  stopDemo(false);
  beginWsIngressQuarantine();
  PiezoTone::stop();
  const DriveSession::Decision decision = g_driveSession.shutdown();
  (void)decision;
  g_driveSessionTransportToken.store(0, std::memory_order_release);
  g_driveSessionTransportGeneration.store(0, std::memory_order_release);
  powerDownDriveMotors("system shutdown");
  stopWifiServer();
  g_paused = true;
  g_forceFullPush = false;
  g_forceEyeRedraw = false;

  runSleepLogoAnimation();
  Serial.println(F("power: Garage logo complete; releasing battery latch"));
  Serial.flush();
  digitalWrite(SYS_EN, LOW);
  disableLoopWDT();

  // Battery power ends here. Reaching this delay means USB is still present.
  delay(150);
  while (digitalRead(SYS_OUT) == LOW) {
    delay(10);
  }
  delay(50);
  Serial.println(F("power: USB soft-off; press power to restart"));
  Serial.flush();
  while (digitalRead(SYS_OUT) != LOW) {
    delay(20);
  }
  delay(50);
  ESP.restart();
  for (;;) {
    delay(100);
  }
}

void checkPowerButton() {
  const uint32_t now = millis();
  const bool low = (digitalRead(SYS_OUT) == LOW);

  // Boot transients are not presses.
  if (now < kPowerButtonGraceMs) {
    return;
  }

  if (!low) {
    g_powerButtonArmed = true;
    g_powerButtonLowSinceMs = 0;
    return;
  }

  // Never seen released: treat as a stuck or floating pin, not a hold.
  if (!g_powerButtonArmed) {
    return;
  }

  if (g_powerButtonLowSinceMs == 0) {
    g_powerButtonLowSinceMs = now;
    return;
  }

  const uint32_t heldMs = now - g_powerButtonLowSinceMs;
  if (heldMs >= kPowerOffHoldMs) {
    // Logged with the measured duration so a spurious trigger is visible in
    // the capture rather than silent.
    Serial.printf("power: button held %lu ms -> shutdown\n",
                  static_cast<unsigned long>(heldMs));
    g_powerOffTriggered = true;
    runPowerOffSequence();
  }
}

void stopDemo(bool restoreDefaults) {
  const bool wasDemoActive = g_demoActive;
  g_demoActive = false;
  g_demoStep = 0;
  g_demoNextStepMs = 0;
  if (wasDemoActive) {
    g_eyes.setIdleWander(true);
  }
  if (restoreDefaults) {
    setEyesMood(MOOD_DEFAULT);
    g_eyes.setPosition(POS_DEFAULT);
  }
}

void startDemo() {
  g_demoActive = true;
  g_demoStep = 0;
  g_demoNextStepMs = millis();
  g_eyes.setIdleWander(false);
  setEyesMood(MOOD_DEFAULT);
  g_eyes.setPosition(POS_DEFAULT);
  setEyeLoopRate(true);
  requestImmediateFrame();
}

void finishDemo() {
  stopDemo(true);
  Serial.println(F("demo complete"));
  setEyeLoopRate(true);
  requestImmediateFrame();
}

char* normalizeLine(char* line) {
  while ((*line != '\0') && isspace(static_cast<unsigned char>(*line))) {
    ++line;
  }

  char* end = line + strlen(line);
  while ((end > line) && isspace(static_cast<unsigned char>(*(end - 1)))) {
    --end;
  }
  *end = '\0';

  for (char* cursor = line; *cursor != '\0'; ++cursor) {
    *cursor = static_cast<char>(tolower(static_cast<unsigned char>(*cursor)));
  }

  return line;
}

bool parseMood(const char* name, EyeMood& outMood) {
  for (size_t index = 0; index < (sizeof(kMoodCommands) / sizeof(kMoodCommands[0])); ++index) {
    if (strcmp(name, kMoodCommands[index].name) == 0) {
      outMood = kMoodCommands[index].mood;
      return true;
    }
  }
  return false;
}

bool parseLook(const char* name, EyePosition& outPosition) {
  for (size_t index = 0; index < (sizeof(kLookCommands) / sizeof(kLookCommands[0])); ++index) {
    if (strcmp(name, kLookCommands[index].name) == 0) {
      outPosition = kLookCommands[index].position;
      return true;
    }
  }
  return false;
}

const char* moodName(EyeMood mood) {
  for (size_t index = 0; index < (sizeof(kMoodCommands) / sizeof(kMoodCommands[0])); ++index) {
    if (kMoodCommands[index].mood == mood) {
      return kMoodCommands[index].name;
    }
  }
  return "default";
}

const char* lookName(EyePosition position) {
  if (position == POS_DEFAULT) {
    return "center";
  }

  for (size_t index = 0; index < (sizeof(kLookCommands) / sizeof(kLookCommands[0])); ++index) {
    if ((kLookCommands[index].position == position) &&
        (strcmp(kLookCommands[index].name, "default") != 0) &&
        (strcmp(kLookCommands[index].name, "center") != 0)) {
      return kLookCommands[index].name;
    }
  }
  return "center";
}

bool parseLongArguments(char* text, long* values, size_t count) {
  if ((text == nullptr) || (count == 0U)) {
    return false;
  }

  char* cursor = text;
  for (size_t index = 0; index < count; ++index) {
    if (*cursor == '\0') {
      return false;
    }

    char* end = nullptr;
    values[index] = strtol(cursor, &end, 10);
    if (end == cursor) {
      return false;
    }

    while ((*end != '\0') && isspace(static_cast<unsigned char>(*end))) {
      ++end;
    }

    cursor = end;
  }

  return *cursor == '\0';
}

bool parseColorArgument(const char* text, uint16_t& outColor) {
  for (size_t index = 0; index < (sizeof(kNamedColors) / sizeof(kNamedColors[0])); ++index) {
    if (strcmp(text, kNamedColors[index].name) == 0) {
      outColor = kNamedColors[index].color;
      return true;
    }
  }

  const size_t length = strlen(text);
  char* end = nullptr;
  const long value = strtol(text, &end, 16);
  if ((end == text) || (*end != '\0')) {
    return false;
  }

  if (length == 4U) {
    if ((value < 0L) || (value > 0xFFFFL)) {
      return false;
    }
    outColor = static_cast<uint16_t>(value);
    return true;
  }

  if (length != 6U) {
    return false;
  }

  const uint8_t red = static_cast<uint8_t>((value >> 16) & 0xFFL);
  const uint8_t green = static_cast<uint8_t>((value >> 8) & 0xFFL);
  const uint8_t blue = static_cast<uint8_t>(value & 0xFFL);
  outColor = static_cast<uint16_t>(((red & 0xF8U) << 8) |
                                   ((green & 0xFCU) << 3) |
                                   (blue >> 3));
  return true;
}

const char* driveSessionStateName(DriveSession::State state) {
  switch (state) {
    case DriveSession::State::SafeOff:     return "SAFE_OFF";
    case DriveSession::State::WaitNeutral: return "WAIT_NEUTRAL";
    case DriveSession::State::ArmedOff:    return "ARMED_OFF";
    case DriveSession::State::Active:      return "ACTIVE";
    case DriveSession::State::NeutralHold: return "NEUTRAL_HOLD";
    case DriveSession::State::Manual:      return "MANUAL";
    case DriveSession::State::Shutdown:    return "SHUTDOWN";
  }
  return "UNKNOWN";
}

const char* driveReasonName(DriveSession::Reason reason) {
  switch (reason) {
    case DriveSession::Reason::None:                    return "none";
    case DriveSession::Reason::Connected:               return "connected";
    case DriveSession::Reason::OwnerBusy:               return "owner_busy";
    case DriveSession::Reason::Disconnected:            return "disconnected";
    case DriveSession::Reason::NonOwner:                return "non_owner";
    case DriveSession::Reason::MalformedCommand:        return "malformed";
    case DriveSession::Reason::StaleSequence:           return "stale_seq";
    case DriveSession::Reason::CalibrationIneligible:   return "cal_ineligible";
    case DriveSession::Reason::NeutralProgress:         return "neutral_progress";
    case DriveSession::Reason::NeutralGapRestart:       return "neutral_gap";
    case DriveSession::Reason::NeutralReady:            return "neutral_ready";
    case DriveSession::Reason::NeutralRequired:         return "neutral_required";
    case DriveSession::Reason::ArmedOffNeutral:         return "armed_off_neutral";
    case DriveSession::Reason::DriveAccepted:           return "drive";
    case DriveSession::Reason::NeutralHoldEntered:      return "neutral_enter";
    case DriveSession::Reason::NeutralHeld:             return "neutral_held";
    case DriveSession::Reason::ControlTimeout:          return "timeout";
    case DriveSession::Reason::ManualEntered:           return "manual_enter";
    case DriveSession::Reason::AlreadyManual:           return "manual_already";
    case DriveSession::Reason::ManualExited:            return "manual_exit";
    case DriveSession::Reason::ManualNotActive:         return "manual_inactive";
    case DriveSession::Reason::ManualMode:              return "manual_mode";
    case DriveSession::Reason::ShutdownEntered:         return "shutdown";
    case DriveSession::Reason::ShutdownLocked:          return "shutdown_locked";
    case DriveSession::Reason::Reset:                   return "reset";
  }
  return "unknown";
}

const char* calibrationValidationError(
    const DriveCalibration::MotorCalibration& motor) {
  const DriveCalibration::Result result =
      DriveCalibration::validateMotor(motor);
  return result ? nullptr : result.error;
}

const char* calibrationValidationError(
    const DriveCalibration::Snapshot& snapshot) {
  const DriveCalibration::Result result =
      DriveCalibration::validateSnapshot(snapshot);
  return result ? nullptr : result.error;
}

void printCalibrationSnapshot() {
  const DriveCalibration::Snapshot applied = g_driveCalibration.snapshot();
  const DriveCalibration::Snapshot& snapshot = g_calibrationPending;
  const char* source =
      snapshot.source == DriveCalibration::Source::User ? "user" : "legacy";
  const char* validation = calibrationValidationError(snapshot);
  Serial.printf(
      "cal state=%s source=%s gen=%lu flags=0x%02X dirty=%u nvs=%u "
      "retained=%u retainedGen=%lu valid=%u touched=0x%03X "
      "left={stopUs:%u,fwdStartUs:%u,revStartUs:%u,fwdMaxUs:%u,revMaxUs:%u} "
      "right={stopUs:%u,fwdStartUs:%u,revStartUs:%u,fwdMaxUs:%u,revMaxUs:%u}%s%s\n",
      g_driveCalibration.eligible() ? "ELIGIBLE" : "LOCKED",
      source,
      static_cast<unsigned long>(applied.generation),
      static_cast<unsigned>(applied.flags),
      snapshot.dirty ? 1U : 0U,
      applied.loadedFromNvs ? 1U : 0U,
      g_driveCalibration.hasRetainedBaseline() ? 1U : 0U,
      static_cast<unsigned long>(
          g_driveCalibration.retainedGeneration()),
      validation == nullptr ? 1U : 0U,
      static_cast<unsigned>(g_calibrationTouchedMask),
      static_cast<unsigned>(snapshot.left.stopUs),
      static_cast<unsigned>(snapshot.left.fwdStartUs),
      static_cast<unsigned>(snapshot.left.revStartUs),
      static_cast<unsigned>(snapshot.left.fwdMaxUs),
      static_cast<unsigned>(snapshot.left.revMaxUs),
      static_cast<unsigned>(snapshot.right.stopUs),
      static_cast<unsigned>(snapshot.right.fwdStartUs),
      static_cast<unsigned>(snapshot.right.revStartUs),
      static_cast<unsigned>(snapshot.right.fwdMaxUs),
      static_cast<unsigned>(snapshot.right.revMaxUs),
      validation == nullptr ? "" : " error=",
      validation == nullptr ? "" : validation);
}

bool parseCalibrationPulseUs(const char* text, uint16_t& valueUs) {
  if (text == nullptr || *text == '\0') {
    return false;
  }

  const char* cursor = text;
  uint32_t value = 0;
  while (isdigit(static_cast<unsigned char>(*cursor))) {
    const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
    if (value > 200U || (value == 200U && digit > 0U)) {
      return false;
    }
    value = value * 10U + digit;
    ++cursor;
  }
  if (cursor == text || *cursor != '\0' ||
      value < kServoMinPulseUs || value > kServoMaxPulseUs) {
    return false;
  }
  valueUs = static_cast<uint16_t>(value);
  return true;
}

bool parseCalibrationSide(const char* text, DriveCalibration::Side& side) {
  if (strcmp(text, "left") == 0) {
    side = DriveCalibration::Side::Left;
    return true;
  }
  if (strcmp(text, "right") == 0) {
    side = DriveCalibration::Side::Right;
    return true;
  }
  return false;
}

bool parseCalibrationField(const char* text, DriveCalibration::Field& field) {
  if (strcmp(text, "stop") == 0) {
    field = DriveCalibration::Field::Stop;
  } else if (strcmp(text, "fwdstart") == 0) {
    field = DriveCalibration::Field::ForwardStart;
  } else if (strcmp(text, "revstart") == 0) {
    field = DriveCalibration::Field::ReverseStart;
  } else if (strcmp(text, "fwdmax") == 0) {
    field = DriveCalibration::Field::ForwardMax;
  } else if (strcmp(text, "revmax") == 0) {
    field = DriveCalibration::Field::ReverseMax;
  } else {
    return false;
  }
  return true;
}

void printMotorState() {
  const DriveCalibration::Snapshot snapshot = g_driveCalibration.snapshot();
  const UBaseType_t queued =
      g_wsEventQueue == nullptr ? 0 : uxQueueMessagesWaiting(g_wsEventQueue);
  Serial.printf(
      "{\"build\":\"%s\",\"cal\":{\"state\":\"%s\",\"gen\":%lu,\"flags\":%u,"
      "\"source\":\"%s\",\"dirty\":%s,\"nvs\":%s,"
      "\"retained\":%s,\"retainedGen\":%lu,\"touched\":%u,"
      "\"left\":{\"stopUs\":%u,\"fwdStartUs\":%u,\"revStartUs\":%u,"
      "\"fwdMaxUs\":%u,\"revMaxUs\":%u},"
      "\"right\":{\"stopUs\":%u,\"fwdStartUs\":%u,\"revStartUs\":%u,"
      "\"fwdMaxUs\":%u,\"revMaxUs\":%u}},"
      "\"session\":{\"state\":\"%s\",\"owner\":%lu,\"hasOwner\":%s,\"gen\":%lu},"
      "\"queue\":{\"depth\":%u,\"capacity\":%u,\"emergency\":%s,\"overflows\":%lu},"
      "\"safety\":{\"timer\":%s,\"armed\":%s,\"inhibit\":%s,"
      "\"tripPending\":%s,\"trips\":%lu},"
      "\"output\":{\"rail\":%s,\"leftAttached\":%s,\"rightAttached\":%s,"
      "\"valid\":%s,\"raw\":[%d,%d],\"effective\":[%d,%d],"
      "\"requestedPulseUs\":[%u,%u],\"nominalReadbackPulseUs\":[%u,%u],"
      "\"source\":\"%s\",\"client\":%lu,\"seq\":%u}}\n",
      kBuildId,
      g_driveCalibration.eligible() ? "ELIGIBLE" : "LOCKED",
      static_cast<unsigned long>(snapshot.generation),
      static_cast<unsigned>(snapshot.flags),
      snapshot.source == DriveCalibration::Source::User ? "user" : "legacy",
      snapshot.dirty ? "true" : "false",
      snapshot.loadedFromNvs ? "true" : "false",
      g_driveCalibration.hasRetainedBaseline() ? "true" : "false",
      static_cast<unsigned long>(
          g_driveCalibration.retainedGeneration()),
      static_cast<unsigned>(g_calibrationTouchedMask),
      static_cast<unsigned>(snapshot.left.stopUs),
      static_cast<unsigned>(snapshot.left.fwdStartUs),
      static_cast<unsigned>(snapshot.left.revStartUs),
      static_cast<unsigned>(snapshot.left.fwdMaxUs),
      static_cast<unsigned>(snapshot.left.revMaxUs),
      static_cast<unsigned>(snapshot.right.stopUs),
      static_cast<unsigned>(snapshot.right.fwdStartUs),
      static_cast<unsigned>(snapshot.right.revStartUs),
      static_cast<unsigned>(snapshot.right.fwdMaxUs),
      static_cast<unsigned>(snapshot.right.revMaxUs),
      driveSessionStateName(g_driveSession.state()),
      static_cast<unsigned long>(g_driveSession.owner()),
      g_driveSession.hasOwner() ? "true" : "false",
      static_cast<unsigned long>(g_driveSession.generation()),
      static_cast<unsigned>(queued),
      static_cast<unsigned>(kWsEventQueueDepth),
      g_wsQueueEmergency.load(std::memory_order_acquire) ? "true" : "false",
      static_cast<unsigned long>(
          g_wsQueueOverflowCount.load(std::memory_order_relaxed)),
      g_driveHardwareTimerReady.load(std::memory_order_acquire)
          ? "true" : "false",
      g_driveHardwareLeaseArmed.load(std::memory_order_acquire)
          ? "true" : "false",
      g_driveHardwareInhibit.load(std::memory_order_acquire)
          ? "true" : "false",
      g_driveHardwareTripPending.load(std::memory_order_acquire)
          ? "true" : "false",
      static_cast<unsigned long>(
          g_driveHardwareTripCount.load(std::memory_order_relaxed)),
      g_driveRailOn ? "true" : "false",
      g_servoLeft.attached() ? "true" : "false",
      g_servoRight.attached() ? "true" : "false",
      g_driveOutputValid ? "true" : "false",
      static_cast<int>(g_lastRawDriveL),
      static_cast<int>(g_lastRawDriveR),
      static_cast<int>(g_lastEffectiveDriveL),
      static_cast<int>(g_lastEffectiveDriveR),
      static_cast<unsigned>(g_servoLeftRequestedPulseUs),
      static_cast<unsigned>(g_servoRightRequestedPulseUs),
      static_cast<unsigned>(g_servoLeftReadbackPulseUs),
      static_cast<unsigned>(g_servoRightReadbackPulseUs),
      g_lastDriveSource,
      static_cast<unsigned long>(g_lastDriveClient),
      static_cast<unsigned>(g_lastDriveSeq));
  Serial.println(F("OK"));
}

void dispatchCalibrationCommand(char* argument) {
  if (argument == nullptr || *argument == '\0') {
    Serial.println(F("ERR cal needs show|set|validate|save|reset|erase"));
    return;
  }

  char* context = nullptr;
  char* operation = strtok_r(argument, " \t", &context);
  if (operation == nullptr) {
    Serial.println(F("ERR cal needs show|set|validate|save|reset|erase"));
    return;
  }

  if (strcmp(operation, "show") == 0) {
    if (strtok_r(nullptr, " \t", &context) != nullptr) {
      Serial.println(F("ERR cal show takes no arguments"));
      return;
    }
    printCalibrationSnapshot();
    Serial.println(F("OK"));
    return;
  }

  if (strcmp(operation, "validate") == 0) {
    if (strtok_r(nullptr, " \t", &context) != nullptr) {
      Serial.println(F("ERR cal validate takes no arguments"));
      return;
    }
    const char* error = calibrationValidationError(g_calibrationPending);
    if (error != nullptr) {
      Serial.printf("ERR calibration invalid: %s\n", error);
      return;
    }
    Serial.printf("cal valid eligible=%u\n",
                  g_driveCalibration.eligible() ? 1U : 0U);
    Serial.println(F("OK"));
    return;
  }

  if (strcmp(operation, "set") == 0) {
    DriveIngressQuarantine quarantine("cal set");
    char* sideText = strtok_r(nullptr, " \t", &context);
    char* fieldText = strtok_r(nullptr, " \t", &context);
    char* valueText = strtok_r(nullptr, " \t", &context);
    if (sideText == nullptr || fieldText == nullptr || valueText == nullptr ||
        strtok_r(nullptr, " \t", &context) != nullptr) {
      Serial.println(F("ERR cal set <left|right> <field> <value>"));
      return;
    }
    DriveCalibration::Side side = DriveCalibration::Side::Left;
    DriveCalibration::Field field = DriveCalibration::Field::Stop;
    if (!parseCalibrationSide(sideText, side) ||
        !parseCalibrationField(fieldText, field)) {
      Serial.println(F("ERR invalid calibration side or field"));
      return;
    }
    uint16_t valueUs = 0;
    if (!parseCalibrationPulseUs(valueText, valueUs)) {
      Serial.println(F("ERR calibration pulse must be an integer 1000..2000us"));
      return;
    }
    const DriveCalibration::Snapshot applied = g_driveCalibration.snapshot();
    if (DriveCalibration::hasFlag(
            applied.flags, DriveCalibration::Flag::Locked)) {
      Serial.println(F("ERR calibration locked; use cal reset confirm first"));
      return;
    }

    DriveCalibration::Snapshot proposed = g_calibrationPending;
    DriveCalibration::MotorCalibration& motor =
        side == DriveCalibration::Side::Left ? proposed.left : proposed.right;
    switch (field) {
      case DriveCalibration::Field::Stop:
        motor.stopUs = valueUs;
        break;
      case DriveCalibration::Field::ForwardStart:
        motor.fwdStartUs = valueUs;
        break;
      case DriveCalibration::Field::ReverseStart:
        motor.revStartUs = valueUs;
        break;
      case DriveCalibration::Field::ForwardMax:
        motor.fwdMaxUs = valueUs;
        break;
      case DriveCalibration::Field::ReverseMax:
        motor.revMaxUs = valueUs;
        break;
    }
    proposed.source = DriveCalibration::Source::User;
    proposed.flags = 0;
    proposed.generation = 0;
    proposed.dirty = true;
    proposed.loadedFromNvs = false;

    const uint8_t fieldIndex =
        static_cast<uint8_t>(field) +
        (side == DriveCalibration::Side::Right ? 5U : 0U);
    const uint16_t previousMask = g_calibrationTouchedMask;
    const DriveCalibration::Snapshot previousPending = g_calibrationPending;
    g_calibrationPending = proposed;
    g_calibrationTouchedMask |=
        static_cast<uint16_t>(1U << fieldIndex);

    const char* motorError = calibrationValidationError(motor);
    if (motorError == nullptr) {
      const DriveCalibration::Result result =
          g_driveCalibration.setMotor(side, motor);
      if (!result) {
        g_calibrationPending = previousPending;
        g_calibrationTouchedMask = previousMask;
        Serial.printf("ERR cal set: %s\n", result.error);
        return;
      }
    } else {
      Serial.printf("cal staged; motor incomplete: %s\n", motorError);
    }
    printCalibrationSnapshot();
    Serial.println(F("OK"));
    return;
  }

  if (strcmp(operation, "save") == 0) {
    char* confirm = strtok_r(nullptr, " \t", &context);
    if (confirm == nullptr || strcmp(confirm, "confirm") != 0 ||
        strtok_r(nullptr, " \t", &context) != nullptr) {
      Serial.println(F("ERR cal save confirm"));
      return;
    }
    DriveIngressQuarantine quarantine("cal save");
    if (g_calibrationPending.source != DriveCalibration::Source::User ||
        g_calibrationTouchedMask != kAllCalibrationFieldsTouched) {
      Serial.printf(
          "ERR cal save requires explicit cal set for all 10 fields (touched=0x%03X)\n",
          static_cast<unsigned>(g_calibrationTouchedMask));
      return;
    }
    const char* validation =
        calibrationValidationError(g_calibrationPending);
    if (validation != nullptr) {
      Serial.printf("ERR cal save invalid: %s\n", validation);
      return;
    }
    DriveCalibration::Result result = g_driveCalibration.setMotor(
        DriveCalibration::Side::Left, g_calibrationPending.left);
    if (!result) {
      Serial.printf("ERR cal save left: %s\n", result.error);
      return;
    }
    result = g_driveCalibration.setMotor(
        DriveCalibration::Side::Right, g_calibrationPending.right);
    if (!result) {
      Serial.printf("ERR cal save right: %s\n", result.error);
      return;
    }
    result = g_driveCalibration.saveConfirmed();
    if (!result) {
      Serial.printf("ERR cal save: %s\n", result.error);
      return;
    }
    g_calibrationPending = g_driveCalibration.snapshot();
    printCalibrationSnapshot();
    Serial.println(F("OK"));
    return;
  }

  if (strcmp(operation, "reset") == 0) {
    char* confirm = strtok_r(nullptr, " \t", &context);
    if (confirm == nullptr || strcmp(confirm, "confirm") != 0 ||
        strtok_r(nullptr, " \t", &context) != nullptr) {
      Serial.println(F("ERR cal reset confirm"));
      return;
    }
    DriveIngressQuarantine quarantine("cal reset");
    g_driveCalibration.resetDraft();
    g_calibrationPending = g_driveCalibration.snapshot();
    g_calibrationTouchedMask = 0;
    printCalibrationSnapshot();
    Serial.println(
        F("WARN draft reset only; retained NVS calibration reloads after reboot"));
    Serial.println(
        F("WARN motor replacement requires successful cal erase confirm"));
    Serial.println(F("OK"));
    return;
  }

  if (strcmp(operation, "erase") == 0) {
    char* confirm = strtok_r(nullptr, " \t", &context);
    if (confirm == nullptr || strcmp(confirm, "confirm") != 0 ||
        strtok_r(nullptr, " \t", &context) != nullptr) {
      Serial.println(F("ERR cal erase confirm"));
      return;
    }
    DriveIngressQuarantine quarantine("cal erase");
    const DriveCalibration::Result result =
        g_driveCalibration.eraseAndReset();
    g_calibrationPending = g_driveCalibration.snapshot();
    g_calibrationTouchedMask = 0;
    if (!result) {
      Serial.printf("ERR cal erase: %s\n", result.error);
      Serial.println(
          F("WARN erase was not verified; reboot may restore a surviving record"));
      return;
    }
    printCalibrationSnapshot();
    Serial.println(F("cal erase: all schema-3 and legacy records absent"));
    Serial.println(F("OK"));
    return;
  }

  Serial.println(F("ERR cal needs show|set|validate|save|reset|erase"));
}

// Built from the movement table rather than a literal, so adding a pattern
// cannot leave the usage text lying about what exists — which is exactly how
// the previous list came to advertise three movements when six were live.
void printGestureUsage() {
  Serial.print(F("gesture <"));
  for (uint8_t i = 0; i < MovementExpression::patternCount(); ++i) {
    if (i > 0) Serial.print('|');
    Serial.print(MovementExpression::patternNameAt(i));
  }
  Serial.println(F("|stop>  (frustrated accepts an optional runMs)"));
}

void printHelp();  // forward decl: definition after the SerialCommand table below

// Serial command handlers.  Each function is the verbatim body of one
// arm of the pre-refactor dispatchCommand() if/return chain.  They live
// here (not in serial_console.cpp) because they reach the ~100 globals
// that this translation unit's anonymous namespace already owns.

void cmdHelp(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR help takes no arguments"));
    return;
  }
  printHelp();
  Serial.println(F("OK"));
  return;
}

void cmdPing(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR ping takes no arguments"));
    return;
  }
  Serial.println(F("pong"));
  Serial.println(F("OK"));
  return;
}

void cmdMotorstate(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR motorstate takes no arguments"));
    return;
  }
  printMotorState();
  return;
}

void cmdCal(char* argument) {
  dispatchCalibrationCommand(argument);
  return;
}

void cmdStatus(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR status takes no arguments"));
    return;
  }

  uint8_t width = 0;
  uint8_t height = 0;
  uint8_t radius = 0;
  int8_t motionX = 0;
  int8_t motionY = 0;
  uint16_t blinkBase = 0;
  uint16_t blinkJitter = 0;
  g_eyes.getDims(width, height, radius);
  g_eyes.getMotionRange(motionX, motionY);
  g_eyes.getAutoblinkInterval(blinkBase, blinkJitter);
  Serial.printf("{\"mood\":\"%s\",\"look\":\"%s\",\"dims\":{\"w\":%u,\"h\":%u,\"r\":%u},\"motion\":{\"x\":%d,\"y\":%d},\"color\":\"%04X\",\"wander\":%s,\"autoblink\":{\"on\":%s,\"base\":%u,\"jitter\":%u},\"headgain\":{\"x\":%u,\"y\":%u},\"paused\":%s,\"fps\":%u}\n",
                moodName(g_eyes.getMood()),
                lookName(g_eyes.getPosition()),
                static_cast<unsigned>(width),
                static_cast<unsigned>(height),
                static_cast<unsigned>(radius),
                static_cast<int>(motionX),
                static_cast<int>(motionY),
                static_cast<unsigned>(g_eyes.getMainColor()),
                g_eyes.isIdleWander() ? "true" : "false",
                g_eyes.isAutoblinker() ? "true" : "false",
                static_cast<unsigned>(blinkBase),
                static_cast<unsigned>(blinkJitter),
                static_cast<unsigned>(g_headShiftXDiv),
                static_cast<unsigned>(g_headShiftYDiv),
                g_paused ? "true" : "false",
                static_cast<unsigned>(g_eyes.getFramerate()));
  Serial.println(F("OK"));
  return;
}

  // WiFi/AP diagnostic + start/stop control
void cmdWifi(char* argument) {
  if (argument != nullptr && *argument != '\0') {
    if (strcmp(argument, "on") == 0) {
      if (g_orState != ORS_IDLE) {
        Serial.printf("ERR wifi: state=%d, not IDLE\n", (int)g_orState);
        return;
      }
      enterWifiBoot();
      Serial.println(F("OK"));
      return;
    }
    if (strcmp(argument, "off") == 0) {
      if (g_orState == ORS_IDLE) {
        Serial.println(F("wifi: already off"));
        Serial.println(F("OK"));
        return;
      }
      teardownToIdle();
      Serial.println(F("OK"));
      return;
    }
    Serial.println(F("ERR wifi takes no arg, 'on', or 'off'"));
    return;
  }
  wifi_mode_t m = WiFi.getMode();
  uint8_t staMac[6] = {0}, apMac[6] = {0};
  esp_read_mac(staMac, ESP_MAC_WIFI_STA);
  esp_read_mac(apMac,  ESP_MAC_WIFI_SOFTAP);
  int8_t tx = 0;
  esp_wifi_get_max_tx_power(&tx);
  Serial.printf("wifi mode=%d  AP_IP=%s  AP_MAC=%s  stations=%d  efuseSTA=%02X:%02X:%02X:%02X:%02X:%02X  efuseAP=%02X:%02X:%02X:%02X:%02X:%02X  tx=%d  psram=%u  override_state=%d up=%d\n",
      (int)m,
      WiFi.softAPIP().toString().c_str(),
      WiFi.softAPmacAddress().c_str(),
      WiFi.softAPgetStationNum(),
      staMac[0], staMac[1], staMac[2], staMac[3], staMac[4], staMac[5],
      apMac[0],  apMac[1],  apMac[2],  apMac[3],  apMac[4],  apMac[5],
      (int)tx,
      (unsigned)ESP.getPsramSize(),
      (int)g_orState,
      g_overrideUp ? 1 : 0);
  Serial.println(F("OK"));
  return;
}

  // §16: simulate a swipe gesture for testing without touching the screen.
void cmdSwipe(char* argument) {
  if (argument == nullptr || *argument == '\0') {
    Serial.println(F("ERR swipe needs 'down' or 'up'"));
    return;
  }
  if (strcmp(argument, "down") == 0) {
    g_orPendingSwipeDown = true;
    Serial.println(F("OK (queued swipe-down)"));
    return;
  }
  if (strcmp(argument, "up") == 0) {
    g_orPendingSwipeUp = true;
    Serial.println(F("OK (queued swipe-up)"));
    return;
  }
  Serial.println(F("ERR swipe takes 'down' or 'up'"));
  return;
}

  // §16: override state inspection.
void cmdState(char* argument) {
  const char* sname = "?";
  switch (g_orState) {
    case ORS_IDLE:      sname = "IDLE_DEMO";     break;
    case ORS_WIFI_BOOT: sname = "WIFI_BOOT";     break;
    case ORS_RUNNING:   sname = "RUNNING";       break;
    case ORS_GRACE:     sname = "GRACE";         break;
  }
  const char* lcdname = "?";
  switch (g_lcdMode) {
    case LCD_MODE_FACE:         lcdname = "FACE";         break;
    case LCD_MODE_QR_WIFI:      lcdname = "QR_WIFI";      break;
    case LCD_MODE_QR_URL:       lcdname = "QR_URL";       break;
    case LCD_MODE_ESTABLISHING: lcdname = "ESTABLISHING"; break;
    case LCD_MODE_GESTURE_MENU: lcdname = "GESTURE_MENU"; break;
  }
  Serial.printf("state=%s lcd=%s wifiUp=%d wsClients=%u apStations=%u uptimeInState=%lu ms touchEvents=%u\n",
                sname, lcdname,
                g_overrideUp ? 1 : 0,
                static_cast<unsigned>(
                    g_wsTransportOpenCount.load(std::memory_order_acquire)),
                static_cast<unsigned>(WiFi.softAPgetStationNum()),
                static_cast<unsigned long>(millis() - g_orStateEnterMs),
                static_cast<unsigned>(g_orTouchEventCount));
  Serial.println(F("OK"));
  return;
}

void cmdMood(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.println(F("ERR mood needs a name"));
    return;
  }
  EyeMood mood = MOOD_DEFAULT;
  if (!parseMood(argument, mood)) {
    Serial.println(F("ERR unknown mood"));
    return;
  }
  stopDemo(false);
  setEyesMood(mood);
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdLook(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.println(F("ERR look needs a direction"));
    return;
  }
  EyePosition position = POS_DEFAULT;
  if (!parseLook(argument, position)) {
    Serial.println(F("ERR unknown direction"));
    return;
  }
  stopDemo(false);
  g_eyes.setPosition(position);
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdBlink(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR blink takes no arguments"));
    return;
  }
  stopDemo(false);
  g_eyes.blink();
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdWander(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.println(F("ERR wander needs on or off"));
    return;
  }
  if ((strcmp(argument, "on") != 0) && (strcmp(argument, "off") != 0)) {
    Serial.println(F("ERR wander must be on or off"));
    return;
  }

  uint16_t idleBase = 0;
  uint16_t idleJitter = 0;
  g_eyes.getIdleWanderInterval(idleBase, idleJitter);
  stopDemo(false);
  g_eyes.setIdleWander(strcmp(argument, "on") == 0, idleBase, idleJitter);
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdDims(char* argument) {
  long values[3] = {0, 0, 0};
  if (!parseLongArguments(argument, values, 3U)) {
    Serial.println(F("ERR dims needs w h r"));
    return;
  }
  if ((values[0] < kEyeBaseWMin) || (values[0] > kEyeBaseWMax)) {
    Serial.println(F("ERR dims w must be 4..32"));
    return;
  }
  if ((values[1] < kEyeBaseHMin) || (values[1] > kEyeBaseHMax)) {
    Serial.println(F("ERR dims h must be 8..56"));
    return;
  }

  const long maxRadius = (((values[0] < values[1]) ? values[0] : values[1]) / 2L);
  if ((values[2] < 0L) || (values[2] > maxRadius)) {
    Serial.println(F("ERR dims r must fit eye size"));
    return;
  }

  stopDemo(false);
  g_eyeBaseW = static_cast<uint8_t>(values[0]);
  g_eyeBaseH = static_cast<uint8_t>(values[1]);
  g_eyeRadius = static_cast<uint8_t>(values[2]);
  g_eyeGeom.baseW = g_eyeBaseW;
  g_eyeGeom.baseH = g_eyeBaseH;
  g_eyeGeom.borderRadius = g_eyeRadius;
  g_eyes.setDims(g_eyeBaseW, g_eyeBaseH, g_eyeRadius);
  recomputeEyeRegionBounds();
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdMotion(char* argument) {
  long values[2] = {0, 0};
  if (!parseLongArguments(argument, values, 2U)) {
    Serial.println(F("ERR motion needs x y"));
    return;
  }
  if ((values[0] < 0L) || (values[0] > kEyeMotionXMax)) {
    Serial.println(F("ERR motion x must be 0..30"));
    return;
  }
  if ((values[1] < 0L) || (values[1] > kEyeMotionYMax)) {
    Serial.println(F("ERR motion y must be 0..20"));
    return;
  }

  stopDemo(false);
  g_eyeMotionX = static_cast<int8_t>(values[0]);
  g_eyeMotionY = static_cast<int8_t>(values[1]);
  g_eyeGeom.motionRangeX = g_eyeMotionX;
  g_eyeGeom.motionRangeY = g_eyeMotionY;
  g_eyes.setMotionRange(g_eyeMotionX, g_eyeMotionY);
  recomputeEyeRegionBounds();
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdBlinkrate(char* argument) {
  long values[2] = {0, 0};
  if (!parseLongArguments(argument, values, 2U)) {
    Serial.println(F("ERR blinkrate needs sec jitter"));
    return;
  }
  if ((values[0] < 1L) || (values[0] > 60L)) {
    Serial.println(F("ERR blinkrate sec must be 1..60"));
    return;
  }
  if ((values[1] < 0L) || (values[1] > values[0])) {
    Serial.println(F("ERR blinkrate jitter must be 0..sec"));
    return;
  }

  stopDemo(false);
  g_eyes.setAutoblinker(true,
                        static_cast<uint16_t>(values[0] * 1000L),
                        static_cast<uint16_t>(values[1] * 1000L));
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdColor(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.println(F("ERR color needs a value"));
    return;
  }

  uint16_t color = 0;
  if (!parseColorArgument(argument, color)) {
    Serial.println(F("ERR invalid color"));
    return;
  }

  stopDemo(false);
  g_eyes.setMainColor(color);
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdHeadgain(char* argument) {
  long values[2] = {0, 0};
  if (!parseLongArguments(argument, values, 2U)) {
    Serial.println(F("ERR headgain needs xDiv yDiv"));
    return;
  }
  if ((values[0] < 0L) || (values[0] > kHeadShiftDivMax)) {
    Serial.println(F("ERR headgain xDiv must be 0..32"));
    return;
  }
  if ((values[1] < 0L) || (values[1] > kHeadShiftDivMax)) {
    Serial.println(F("ERR headgain yDiv must be 0..32"));
    return;
  }

  stopDemo(false);
  g_headShiftXDiv = static_cast<uint8_t>(values[0]);
  g_headShiftYDiv = static_cast<uint8_t>(values[1]);
  if ((g_headShiftXDiv == 0U) || (g_headShiftYDiv == 0U)) {
    g_forceEyeRedraw = true;
    g_forceFullPush = true;
  }
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdTone(char* argument) {
  long frequencyHz = 0;
  if (!parseLongArguments(argument, &frequencyHz, 1U)) {
    Serial.println(F("ERR tone needs exactly one integer argument"));
    return;
  }
  if ((frequencyHz != 0L) &&
      ((frequencyHz < 200L) || (frequencyHz > 6000L))) {
    Serial.println(F("ERR tone frequency must be 0 or 200..6000"));
    return;
  }
  if (!g_piezoAvailable) {
    Serial.println(F("ERR tone unavailable"));
    return;
  }
  if (!PiezoTone::command(static_cast<uint16_t>(frequencyHz), millis())) {
    Serial.println(F("ERR tone command failed"));
    return;
  }
  Serial.println(F("OK"));
  return;
}

void cmdCrash(char* argument) {
  if ((argument != nullptr) && strcmp(argument, "clear") == 0) {
    nvs_handle_t handle = 0;
    if (nvs_open("crashlog", NVS_READWRITE, &handle) == ESP_OK) {
      nvs_erase_all(handle);
      nvs_commit(handle);
      nvs_close(handle);
    }
    Serial.println(F("crash: cleared"));
    return;
  }
  nvs_handle_t handle = 0;
  // Stall timings live in RTC memory, not NVS, so they are worth printing even
  // when no crash record exists -- they describe the CURRENT uptime and are the
  // main readout while hunting the touch-poll stall.
  Serial.printf(
      "stall: touch max %lu us, slow(>%lu us) %lu | loop max %lu us in \"%s\"\n",
      static_cast<unsigned long>(g_rtcTouchMaxUs),
      static_cast<unsigned long>(kStallWarnUs),
      static_cast<unsigned long>(g_rtcTouchSlowCount),
      static_cast<unsigned long>(g_rtcLoopMaxUs),
      crumbName(g_rtcLoopMaxPhase));
  Serial.printf(
      "stall: at worst poll -- bat %lu mV, rail %s, gesture %s, %s\n",
      static_cast<unsigned long>(g_rtcStallBatMv),
      (g_rtcStallFlags & kStallFlagRail) ? "ON" : "off",
      (g_rtcStallFlags & kStallFlagGesture) ? "RUNNING" : "idle",
      (g_rtcStallFlags & kStallFlagCharge) ? "charging" : "on battery");
  if (nvs_open("crashlog", NVS_READONLY, &handle) != ESP_OK) {
    Serial.println(F("crash: none recorded"));
    return;
  }
  CrashRecord record{};
  size_t size = sizeof(record);
  const esp_err_t err = nvs_get_blob(handle, "rec", &record, &size);
  nvs_close(handle);
  if (err != ESP_OK || size != sizeof(record) ||
      record.magic != kCrashRecordMagic) {
    Serial.println(F("crash: none recorded"));
    return;
  }
  const char* reasonName = "?";
  switch (static_cast<esp_reset_reason_t>(record.reason)) {
    case ESP_RST_PANIC:    reasonName = "PANIC"; break;
    case ESP_RST_INT_WDT:  reasonName = "INT_WDT (interrupts off too long)"; break;
    case ESP_RST_TASK_WDT: reasonName = "TASK_WDT"; break;
    case ESP_RST_WDT:      reasonName = "WDT"; break;
    case ESP_RST_BROWNOUT: reasonName = "BROWNOUT"; break;
    case ESP_RST_SW:       reasonName = "SW (deliberate restart)"; break;
    case ESP_RST_EXT:      reasonName = "EXT (reset pin)"; break;
    default:               reasonName = "UNKNOWN/other"; break;
  }
  Serial.printf("crash: #%lu  reason=%s(%lu)\n",
                static_cast<unsigned long>(record.count), reasonName,
                static_cast<unsigned long>(record.reason));
  Serial.printf("crash: where=%s  phase=%lu detail=0x%lX (client=%lu step=%lu)\n",
                crumbName(record.phase),
                static_cast<unsigned long>(record.phase),
                static_cast<unsigned long>(record.detail),
                static_cast<unsigned long>(record.detail & 0xFF),
                static_cast<unsigned long>((record.detail >> 8) & 0xFF));
  Serial.printf("crash: seq=%lu heapAfter=%lu minHeap=%lu\n",
                static_cast<unsigned long>(record.seq),
                static_cast<unsigned long>(record.freeHeap),
                static_cast<unsigned long>(record.minHeap));
  return;
}

void cmdLcdpush(char* argument) {
  // Deliberately does ONLY a full canvas->panel push: no SPI re-init, no
  // panel re-init, no canvas redraw. `lcdprobe` happens to fix the black
  // screen, but it does two things at once (re-inits the SPI peripheral via
  // g_bus->begin() AND forces a full push), so it cannot tell us which one
  // matters. This isolates the push half.
  //
  // If THIS fixes a black screen, the canvas was correct and the panel had
  // simply stopped receiving the incremental eye-region pushes.
  // If this does NOT fix it but `lcdprobe` does, the SPI peripheral itself
  // was wedged and re-initialising the bus is the real repair.
  const uint32_t startedUs = micros();
  pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
  const uint32_t tookUs = micros() - startedUs;
  if (g_batSeeded) {
    // PERF-2: after an explicit lcdpush command, force a fresh meter
    // render so the operator sees the current voltage no matter what.
    g_batStripCanvasFresh = false;
    drawBatteryMeter();
  }
  Serial.printf("lcdpush: full push %lux%lu took %lu us (SPI %lu MHz)\n",
                static_cast<unsigned long>(kScreenW),
                static_cast<unsigned long>(kScreenH),
                static_cast<unsigned long>(tookUs),
                static_cast<unsigned long>(g_lcdSpiHz / 1000000UL));
  return;
}

void cmdLcdbus(char* argument) {
  // The other half: re-init the SPI peripheral only, no push.
  if (g_bus == nullptr) {
    Serial.println(F("ERR no bus"));
    return;
  }
  g_bus->begin(static_cast<int32_t>(g_lcdSpiHz));
  Serial.println(F("lcdbus: SPI peripheral re-initialised (no push)"));
  return;
}

void cmdLcdprobe(char* argument) {
  // Reads 24 bits rather than 8 and prints them raw. The datasheet shows a
  // "dummy read" before the status byte, but does not pin down whether that
  // is one dummy CLOCK or a whole dummy BYTE on the serial interface. Rather
  // than guess the framing, dump the window and locate 0x9C in it.
  const uint32_t rddpm = lcdProbeRead(0x0A, 24);
  const uint32_t rddid = lcdProbeRead(0x04, 32);
  Serial.printf("lcdprobe: RDDPM(0x0A) raw=0x%06lX  RDDID(0x04) raw=0x%08lX\n",
                static_cast<unsigned long>(rddpm & 0xFFFFFF),
                static_cast<unsigned long>(rddid));
  Serial.print(F("lcdprobe: RDDPM bits "));
  for (int8_t bit = 23; bit >= 0; --bit) {
    Serial.print((rddpm >> bit) & 1U);
    if (bit % 8 == 0) Serial.print(' ');
  }
  Serial.println();
  // Try each byte alignment; a live panel shows BSTON|SLPOUT|NORON|DISON.
  for (uint8_t shift = 0; shift <= 16; shift += 8) {
    const uint8_t candidate = static_cast<uint8_t>((rddpm >> shift) & 0xFF);
    Serial.printf("lcdprobe:   byte<<%u = 0x%02X  BSTON=%u SLPOUT=%u "
                  "NORON=%u DISON=%u%s\n",
                  static_cast<unsigned>(shift),
                  static_cast<unsigned>(candidate),
                  static_cast<unsigned>((candidate >> 7) & 1),
                  static_cast<unsigned>((candidate >> 4) & 1),
                  static_cast<unsigned>((candidate >> 3) & 1),
                  static_cast<unsigned>((candidate >> 2) & 1),
                  lcdPanelLooksAlive(candidate) ? "  <-- ALIVE" : "");
  }
  if ((rddpm & 0xFFFFFF) == 0 || (rddpm & 0xFFFFFF) == 0xFFFFFF) {
    // All-zero or all-one means the line never changed: either the panel is
    // not driving SDA back, or read-back is not wired on this module.
    Serial.println(F("lcdprobe: line static -- no read-back on this board"));
  }
  // The probe borrowed the SPI pins; repaint so nothing is left half-drawn.
  g_forceFullPush = true;
  g_forceEyeRedraw = true;
  g_eyes.forceRedraw();
  return;
}

void cmdLcdspi(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.printf("lcdspi: %lu Hz (%lu MHz) active; default %lu MHz\n",
                  static_cast<unsigned long>(g_lcdSpiHz),
                  static_cast<unsigned long>(g_lcdSpiHz / 1000000UL),
                  static_cast<unsigned long>(kLcdSpiHzDefault / 1000000UL));
    Serial.println(F("usage: lcdspi <hz>  (8000000..80000000; reboot to apply)"));
    return;
  }
  const long requested = strtol(argument, nullptr, 10);
  if (requested < static_cast<long>(kLcdSpiHzMin) ||
      requested > static_cast<long>(kLcdSpiHzMax)) {
    Serial.printf("ERR lcdspi must be %lu..%lu Hz\n",
                  static_cast<unsigned long>(kLcdSpiHzMin),
                  static_cast<unsigned long>(kLcdSpiHzMax));
    return;
  }
  nvs_handle_t handle = 0;
  if (nvs_open("lcdcfg", NVS_READWRITE, &handle) != ESP_OK) {
    Serial.println(F("ERR lcdcfg open failed"));
    return;
  }
  esp_err_t err = nvs_set_u32(handle, "spihz",
                              static_cast<uint32_t>(requested));
  if (err == ESP_OK) err = nvs_commit(handle);
  nvs_close(handle);
  if (err != ESP_OK) {
    Serial.println(F("ERR lcdcfg write failed"));
    return;
  }
  // The clock is fixed when the bus is constructed, so this cannot take
  // effect until the next boot. Say so rather than implying a live change.
  Serial.printf("lcdspi: stored %ld Hz; reboot to apply\n", requested);
  return;
}

void cmdSleep(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR sleep takes no arguments"));
    return;
  }
  stopDemo(false);
  PiezoTone::stop();
  g_paused = true;
  resetDriveControl("serial sleep");
  runSleepLogoAnimation();
  g_forceFullPush = false;
  g_forceEyeRedraw = false;
  Serial.println(F("OK"));
  return;
}

void cmdWake(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR wake takes no arguments"));
    return;
  }
  stopDemo(false);
  if (g_paused) {
    runWakeLogoAnimation();
  }
  g_paused = false;
  g_lcdMode = LCD_MODE_FACE;
  g_forceFullPush = false;
  g_forceEyeRedraw = false;
  setEyeLoopRate(true);
  requestImmediateFrame();
  Serial.println(F("OK"));
  return;
}

void cmdDemo(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR demo takes no arguments"));
    return;
  }
  startDemo();
  Serial.println(F("OK"));
  return;
}

void cmdServooff(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR servooff takes no arguments"));
    return;
  }
  stopMovementGesture("serial servooff");
  resetDriveControl("serial servooff");
  Serial.println(F("OK"));
  return;
}

void cmdTouchinfo(char* argument) {
  MDO::TouchScreenController::device_type_t deviceType;
  unsigned char firmware = 0;
  const bool haveType = g_orTouch.getDeviceType(deviceType);
  const bool haveFw = g_orTouch.getFirmwareVersion(firmware);
  Serial.printf("touch: type=%d(%d) fw=%d(%d) gestures=%u points=%u debug=%d\n",
                haveType ? static_cast<int>(deviceType) : -1, haveType ? 1 : 0,
                haveFw ? static_cast<int>(firmware) : -1, haveFw ? 1 : 0,
                static_cast<unsigned>(g_orTouchEventCount),
                static_cast<unsigned>(g_orTouchPointCount),
                g_touchDebug ? 1 : 0);
  return;
}

void cmdTouchlog(char* argument) {
  if ((argument != nullptr) && (strcmp(argument, "clear") == 0)) {
    g_touchLogNext = 0;
    g_touchLogTotal = 0;
    Serial.println(F("OK"));
    return;
  }
  const uint32_t total = g_touchLogTotal;
  const uint8_t count =
      total < kTouchLogSize ? static_cast<uint8_t>(total) : kTouchLogSize;
  Serial.printf("touchlog total=%lu shown=%u now=%lu\n",
                static_cast<unsigned long>(total),
                static_cast<unsigned>(count),
                static_cast<unsigned long>(millis()));
  // Oldest first, so a tap sequence reads in the order it was performed.
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t slot = static_cast<uint8_t>(
        (g_touchLogNext + kTouchLogSize - count + i) % kTouchLogSize);
    const TouchLogEntry& e = g_touchLog[slot];
    Serial.printf("  #%u ms=%lu gid=%d xy=(%d,%d) pressed=%d\n",
                  static_cast<unsigned>(i), static_cast<unsigned long>(e.ms),
                  static_cast<int>(e.gestureId), static_cast<int>(e.x),
                  static_cast<int>(e.y), e.pressed ? 1 : 0);
  }
  return;
}

void cmdTouchreset(char* argument) {
  // Recovery path: re-applies our configuration if the chip ever stops
  // reporting. Deliberately does NOT touch auto-sleep — see the note in
  // setup(); disabling it kills event reporting entirely.
  const bool reset = g_orTouch.resetChip(true);
  Serial.printf("touch: reset=%d\n", reset ? 1 : 0);
  return;
}

void cmdTouchdebug(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.printf("touchdebug=%s\n", g_touchDebug ? "on" : "off");
    return;
  }
  if (strcmp(argument, "on") == 0) {
    g_touchDebug = true;
  } else if (strcmp(argument, "off") == 0) {
    g_touchDebug = false;
  } else {
    Serial.println(F("ERR touchdebug <on|off>"));
    return;
  }
  Serial.println(F("OK"));
  return;
}

void cmdGesture(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    printGestureUsage();
    return;
  }
  if (strcmp(argument, "stop") == 0) {
    stopMovementGesture("serial request");
    Serial.println(F("OK"));
    return;
  }

  // Optional second token: the frustrated swing length, so it can be tuned
  // on the bench without a reflash.
  char nameBuffer[24];
  uint16_t runMs = 0;
  const char* space = strchr(argument, ' ');
  if (space != nullptr) {
    const size_t nameLength = static_cast<size_t>(space - argument);
    if (nameLength == 0 || nameLength >= sizeof(nameBuffer)) {
      printGestureUsage();
      return;
    }
    memcpy(nameBuffer, argument, nameLength);
    nameBuffer[nameLength] = '\0';

    const char* value = space;
    while (*value == ' ') ++value;
    if (*value == '\0') {
      printGestureUsage();
      return;
    }
    char* end = nullptr;
    const long parsed = strtol(value, &end, 10);
    if (end == value || (end != nullptr && *end != '\0') || parsed <= 0 ||
        parsed > 65535) {
      Serial.println(F("ERR run length must be a positive integer"));
      return;
    }
    runMs = static_cast<uint16_t>(parsed);
    argument = nameBuffer;
  }

  if (!startMovementGesture(argument, runMs)) {
    printGestureUsage();
    return;
  }
  Serial.println(F("OK"));
  return;
}

void cmdTest(char* argument) {
  // Same sequence the on-screen button runs, exposed on serial so it can be
  // watched: 360 one way, rezero, 360 back.
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR test takes no arguments"));
    return;
  }
  if (!startTurnTestSequence()) {
    Serial.println(F("ERR could not start test"));
    return;
  }
  return;
}

void cmdHeading(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    if (strcmp(argument, "zero") == 0) {
      g_headingIntegrator.reset();
      Serial.println(F("heading: zeroed"));
      Serial.println(F("OK"));
      return;
    }
    Serial.println(F("ERR heading [zero]"));
    return;
  }
  // Relative to the last zero — there is no compass, so this is not a
  // bearing. Bias and ZUPT count are reported because they are what bound
  // the drift, and drift is the only thing that makes this untrustworthy.
  Serial.printf(
      "heading: %.2f deg (axis %s, proj gain %.4f, bias %.3f dps, zupt %lu "
      "applied / %lu rejected, last %.3f spread %.3f, samples %lu)\n",
      static_cast<double>(g_headingIntegrator.totalDegrees()),
      TurnMeasure::axisName(g_turnYawAxis),
      static_cast<double>(g_turnAxisFraction > 0.0f
                              ? 1.0f / g_turnAxisFraction
                              : 0.0f),
      static_cast<double>(g_turnBias[static_cast<int>(g_turnYawAxis)]),
      static_cast<unsigned long>(g_headingZuptCount),
      static_cast<unsigned long>(g_headingZuptRejected),
      static_cast<double>(g_headingLastZuptDps),
      static_cast<double>(g_headingLastZuptSpread),
      static_cast<unsigned long>(g_headingIntegrator.sampleCount()));
  Serial.println(F("OK"));
  return;
}

void cmdTurn(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.println(F("ERR turn <degrees> [cw|ccw]"));
    return;
  }
  char* end = nullptr;
  const double parsed = strtod(argument, &end);
  if (end == argument) {
    Serial.println(F("ERR turn <degrees> [cw|ccw]"));
    return;
  }
  bool clockwise = true;
  int8_t speed = MovementExpression::kSpinSpeed;
  while (*end == ' ') ++end;
  if (*end != '\0') {
    // Optional direction, then optional speed, in either order after the
    // angle: "turn 90 ccw 60" and "turn 90 60" both work.
    char* cursor = end;
    while (*cursor != '\0') {
      while (*cursor == ' ') ++cursor;
      if (*cursor == '\0') break;
      if (strncmp(cursor, "ccw", 3) == 0) {
        clockwise = false;
        cursor += 3;
        continue;
      }
      if (strncmp(cursor, "cw", 2) == 0) {
        clockwise = true;
        cursor += 2;
        continue;
      }
      char* numEnd = nullptr;
      const long parsedSpeed = strtol(cursor, &numEnd, 10);
      if (numEnd == cursor) {
        Serial.println(F("ERR turn <degrees> [cw|ccw] [speed]"));
        return;
      }
      if (parsedSpeed < MovementExpression::kSpinMinSpeed ||
          parsedSpeed > MovementExpression::kCommandMax) {
        Serial.printf("ERR speed %d..%d\n",
                      static_cast<int>(MovementExpression::kSpinMinSpeed),
                      static_cast<int>(MovementExpression::kCommandMax));
        return;
      }
      speed = static_cast<int8_t>(parsedSpeed);
      cursor = numEnd;
    }
  }
  if (!startClosedLoopTurn(static_cast<float>(parsed), clockwise, speed)) {
    return;
  }
  return;
}

void cmdLead(char* argument) {
  // The coast lead is learned automatically from every turn, but being able
  // to read it, pin it, or disable it without a reflash makes it possible to
  // A/B the compensation on the bench in seconds.
  if ((argument == nullptr) || (*argument == '\0')) {
    // Report BOTH terms. The linear one is zero by default now that the
    // model is quadratic, and printing only that made the compensation look
    // disabled when it was in fact doing all the work.
    const float rate = 285.0f;
    const float linear = g_coastLeadSeconds * rate;
    const float quadratic =
        g_coastAlpha > 0.0f ? (rate * rate) / (2.0f * g_coastAlpha) : 0.0f;
    Serial.printf(
        "lead: alpha %.0f deg/s2 + linear %.4f s -> %.1f deg predicted "
        "coast at %.0f dps (%.1f quadratic + %.1f linear)\n",
        static_cast<double>(g_coastAlpha),
        static_cast<double>(g_coastLeadSeconds),
        static_cast<double>(quadratic + linear), static_cast<double>(rate),
        static_cast<double>(quadratic), static_cast<double>(linear));
    Serial.printf("lead: %lu learned updates\n",
                  static_cast<unsigned long>(g_coastLeadUpdates));
    return;
  }
  if (strcmp(argument, "reset") == 0) {
    g_coastLeadSeconds = kCoastLeadDefaultS;
    g_coastLeadUpdates = 0;
    Serial.printf("lead: reset to %.4f s\n",
                  static_cast<double>(g_coastLeadSeconds));
    return;
  }
  if (strcmp(argument, "off") == 0) {
    g_coastLeadSeconds = 0.0f;
    g_coastLeadUpdates = 0;
    Serial.println(F("lead: off (turns stop on the measured angle, as before)"));
    return;
  }
  char* end = nullptr;
  const double seconds = strtod(argument, &end);
  if (end == argument || seconds < kCoastLeadMinS || seconds > kCoastLeadMaxS) {
    Serial.printf("ERR lead [<%g..%g seconds>|off|reset]\n",
                  static_cast<double>(kCoastLeadMinS),
                  static_cast<double>(kCoastLeadMaxS));
    return;
  }
  g_coastLeadSeconds = static_cast<float>(seconds);
  g_coastLeadUpdates = 0;   // a manual value is not a learned one
  Serial.printf("lead: %.4f s set manually\n",
                static_cast<double>(g_coastLeadSeconds));
  return;
}

void cmdBench(char* argument) {
  // The bench run is the answer to a hard constraint: movement cannot be
  // tested from here. A tethered turn is invalid (the cable restrains
  // rotation) and the robot must not be driven while plugged in, so every
  // real measurement costs supervised, untethered human time. This makes that
  // time count by extracting the coast constant, the speed/rate model and the
  // lead-compensation A/B from a single ~2 minute run.
  if ((argument == nullptr) || (*argument == '\0') ||
      strcmp(argument, "status") == 0) {
    Serial.printf("bench: armed=%d stored=%u entries%s running=%d\n",
                  BenchLog::isArmed() ? 1 : 0,
                  static_cast<unsigned>(g_benchLog.count()),
                  g_benchLog.overflowed() ? " (TRUNCATED)" : "",
                  g_benchPhase != BenchPhase::Idle ? 1 : 0);
    return;
  }
  if (strcmp(argument, "arm") == 0) {
    if (!BenchLog::setArmed(true)) {
      Serial.println(F("ERR could not arm"));
      return;
    }
    Serial.println(F("bench: ARMED. Now:"));
    Serial.println(F("  1. unplug USB (the robot stays on battery)"));
    Serial.println(F("  2. put it on the desk, on its wheels, clear space"));
    Serial.println(F("  3. press the TEST button, do not touch it for ~2 min"));
    Serial.println(F("  4. plug back in and run 'bench report'"));
    return;
  }
  if (strcmp(argument, "disarm") == 0) {
    BenchLog::setArmed(false);
    Serial.println(F("bench: disarmed"));
    return;
  }
  if (strcmp(argument, "clear") == 0) {
    g_benchLog.erase();
    Serial.println(F("bench: results erased"));
    return;
  }
  if (strcmp(argument, "report") == 0) {
    if (g_benchLog.count() == 0 && !g_benchLog.load()) {
      Serial.println(F("bench: no stored run"));
      return;
    }
    Serial.printf("bench report: %u entries%s\n",
                  static_cast<unsigned>(g_benchLog.count()),
                  g_benchLog.overflowed() ? " (TRUNCATED)" : "");
    // Serial TX is normally non-blocking (setTxTimeoutMs(0)) so a missing
    // host can never stall the control loop. That means it DROPS bytes when
    // the USB CDC buffer fills — which silently corrupted two lines and
    // swallowed the entire summary on run 002. flush() does not help,
    // because with a zero timeout it does not wait either.
    //
    // For this one command, apply real backpressure. The timeout is kept
    // short so an absent host costs at most ~20 ms per write rather than
    // hanging, and it is restored immediately afterwards. This is the only
    // copy of data that costs a supervised session to retake.
    Serial.setTxTimeoutMs(20);
    for (uint16_t i = 0; i < g_benchLog.count(); ++i) {
      const BenchLog::Entry& entry = g_benchLog.at(i);
      Serial.printf(
          "bench %s param=%d cond=%s value=%.4f\n",
          BenchLog::metricName(static_cast<BenchLog::Metric>(entry.metric)),
          static_cast<int>(entry.param),
          BenchLog::conditionName(entry.condition),
          static_cast<double>(entry.value));
      // Yield periodically so the buffer drains even at 20 ms of backpressure.
      if ((i % 4) == 3) {
        Serial.flush();
        delay(4);
      }
    }
    Serial.flush();
    delay(10);
    // The comparisons the run exists to make, computed on-device so they
    // cannot be got wrong by hand. Split BY SPEED: pooling speeds averages a
    // 16 deg coast together with a 4 deg one and understates the effect.
    static const int16_t kReportSpeeds[] = {100, 75, 55};
    Serial.println(F("bench summary (per speed):"));
    for (int16_t speed : kReportSpeeds) {
      float coastMean = 0.0f, rateMean = 0.0f;
      uint16_t nCoast = 0, nRate = 0;
      const bool haveCoast = g_benchLog.summariseParam(
          BenchLog::Metric::CoastDegrees, BenchLog::kCondLeadOff, speed,
          &coastMean, nullptr, &nCoast);
      const bool haveRate = g_benchLog.summariseParam(
          BenchLog::Metric::SteadyRateDps, BenchLog::kCondLeadOff, speed,
          &rateMean, nullptr, &nRate);
      if (!haveCoast || !haveRate || rateMean <= 1.0f || coastMean <= 0.01f) {
        continue;
      }
      // alpha from the Coulomb model, which is what the controller uses. A
      // linear model would need coast/rate instead, and the fact that that
      // number is NOT constant across speeds is the whole reason the model
      // is quadratic. Both are printed so the choice stays checkable.
      const float alpha = (rateMean * rateMean) / (2.0f * coastMean);
      Serial.printf(
          "  speed %3d: rate %6.1f dps, coast %5.2f deg (n=%u) -> "
          "alpha %5.0f, linear-k %.4f s\n",
          static_cast<int>(speed), static_cast<double>(rateMean),
          static_cast<double>(coastMean), static_cast<unsigned>(nCoast),
          static_cast<double>(alpha),
          static_cast<double>(coastMean / rateMean));
      Serial.flush();
      delay(4);
    }
    for (int16_t speed : kReportSpeeds) {
      float off = 0.0f, on = 0.0f;
      uint16_t nOff = 0, nOn = 0;
      const bool haveOff = g_benchLog.summariseParam(
          BenchLog::Metric::TurnErrorDegrees, BenchLog::kCondLeadOff, speed,
          &off, nullptr, &nOff);
      const bool haveOn = g_benchLog.summariseParam(
          BenchLog::Metric::TurnErrorDegrees, BenchLog::kCondLeadOn, speed,
          &on, nullptr, &nOn);
      if (!haveOff || !haveOn) continue;
      // Compared on MAGNITUDE. Once compensation over-corrects the error goes
      // negative, and a naive ratio would report an over-correction as a
      // greater-than-100% improvement. The sign is printed too, because
      // which way it misses is the thing that decides the next alpha.
      const float offMag = off < 0 ? -off : off;
      const float onMag = on < 0 ? -on : on;
      Serial.printf(
          "  A/B speed %3d: %+.2f deg lead-off (n=%u) -> %+.2f deg lead-on "
          "(n=%u), |err| %.1f%% smaller%s\n",
          static_cast<int>(speed), static_cast<double>(off),
          static_cast<unsigned>(nOff), static_cast<double>(on),
          static_cast<unsigned>(nOn),
          static_cast<double>(offMag > 0.01f
                                  ? (1.0f - onMag / offMag) * 100.0f
                                  : 0.0f),
          on < -0.5f ? " (OVER-corrected: raise alpha)"
                     : (on > 0.5f ? " (under-corrected: lower alpha)" : ""));
      Serial.flush();
      delay(4);
    }
    Serial.printf("bench: compiled alpha %.0f deg/s2, live alpha %.0f\n",
                  static_cast<double>(kCoastAlphaDefault),
                  static_cast<double>(g_coastAlpha));
    Serial.flush();
    // Back to non-blocking, so a disconnected host can never stall the loop.
    Serial.setTxTimeoutMs(0);
    Serial.println(F("OK"));
    return;
  }
  Serial.println(F("ERR bench [status|arm|disarm|report|clear]"));
  return;
}

void cmdSound(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    // The generator now owns the sound vocabulary — three moods, each
    // composed fresh on every trigger.
    Serial.println(F("sound: happy, sad, confused"));
    Serial.printf("sound: playing=%d %s\n", g_melody.active() ? 1 : 0,
                  g_melody.active() ? g_melody.name() : "");
    Serial.println(F("usage: sound <happy|sad|confused> | sound stop"));
    return;
  }
  if (strcmp(argument, "stop") == 0) {
    g_melody.stop();
    PiezoTone::stop();
    Serial.println(F("sound: stopped"));
    return;
  }
  if (!g_piezoAvailable) {
    Serial.println(F("ERR piezo unavailable"));
    return;
  }
  if (!playMoodByName(argument)) {
    Serial.printf("ERR unknown mood \"%s\" (expected happy|sad|confused)\n",
                  argument);
    return;
  }
  Serial.printf("sound: %s\n", argument);
  return;
}

  // Named 'sense', not 'motion': 'motion' is already the eye-engine's gaze-range
  // command and silently shadowed this accelerometer diagnostic for months. The
  // output labels below deliberately still print "motion:" because external
  // scripts grep for them. See PROJECT-HISTORY Appendix B.
void cmdSense(char* argument) {
  // Live view of the detector, so a threshold that misfires can be SEEN
  // rather than guessed at. Pick the robot up while watching this and the
  // tilt number should move immediately.
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.printf(
        "motion: tilt %.1f deg, |a|-1 %+.3f g, linear %.3f g, lifted=%d\n",
        static_cast<double>(g_motionEvents.lastTiltDegrees()),
        static_cast<double>(g_motionEvents.lastMagnitudeDeviationG()),
        static_cast<double>(g_motionEvents.lastLinearG()),
        g_motionEvents.lifted() ? 1 : 0);
    Serial.printf("motion: bumps=%u shakes=%u lifts=%u\n",
                  static_cast<unsigned>(g_motionEvents.bumpCount()),
                  static_cast<unsigned>(g_motionEvents.shakeCount()),
                  static_cast<unsigned>(g_motionEvents.liftCount()));
    Serial.printf(
        "motion: thresholds bump %.2f g, shake %u alternations/s, "
        "lift %.0f deg or %.2f g held %lums\n",
        static_cast<double>(MotionEvents::kBumpG),
        static_cast<unsigned>(MotionEvents::kShakeAlternations),
        static_cast<double>(MotionEvents::kLiftTiltDegrees),
        static_cast<double>(MotionEvents::kLiftMagnitudeG),
        static_cast<unsigned long>(MotionEvents::kLiftHoldUs / 1000));
    return;
  }
  if (strcmp(argument, "zero") == 0) {
    if (!establishYawAxis()) {
      Serial.println(F("ERR could not read gravity"));
      return;
    }
    g_motionEvents.begin(g_turnGravity[0], g_turnGravity[1],
                         g_turnGravity[2]);
    Serial.println(F("sense: reference re-taken at the current pose"));
    return;
  }
  Serial.println(F("ERR sense [zero]"));
  return;
}

void cmdScale(char* argument) {
  // The last unmeasured error, and the only one that needs an external
  // reference: a scale error produces zero output at zero rate, so no
  // amount of sitting still can reveal it.
  if ((argument == nullptr) || (*argument == '\0') ||
      strcmp(argument, "status") == 0) {
    const float correction = ImuQmi8658::gyroScaleCorrection();
    Serial.printf("scale: correction %.4f\n",
                  static_cast<double>(correction));
    if (g_scaleBoundPercent > 0.0f) {
      Serial.printf(
          "scale: VERIFIED against a floor mark - error <%.3f%% "
          "(<%.2f deg per 360)%s\n",
          static_cast<double>(g_scaleBoundPercent),
          static_cast<double>(g_scaleBoundPercent / 100.0f * 360.0f),
          correction == 1.0f ? ", factory scale kept" : "");
    } else {
      Serial.println(F("scale: NEVER CHECKED - factory scale, unverified"));
    }
    if (g_scaleCheckRevs > 0) {
      Serial.printf(
          "scale: a %u-revolution check is PENDING - measure the offset "
          "from the mark, then run 'scale actual <deg>'\n",
          static_cast<unsigned>(g_scaleCheckRevs));
    }
    Serial.println(F("usage: scale check [revs] | scale actual <deg> | scale reset"));
    return;
  }
  if (strcmp(argument, "reset") == 0) {
    ImuQmi8658::setGyroScaleCorrection(1.0f);
    saveGyroScale(1.0f);
    saveScaleCheckRevs(0);
    saveScaleBound(0.0f);
    Serial.println(F("scale: reset to factory (1.0000), verification cleared"));
    return;
  }
  if (strncmp(argument, "check", 5) == 0) {
    long revs = 3;
    if (argument[5] != '\0') revs = strtol(argument + 5, nullptr, 10);
    if (revs < 1 || revs > 5) {
      Serial.println(F("ERR revs 1..5"));
      return;
    }
    if (g_turnCmdActive || g_straightActive || motionInProgress()) {
      Serial.println(F("ERR a movement is already running"));
      return;
    }
    // Validate NOW, while tethered. Discovering that the turn does not fit
    // at the moment the button is pressed would waste the whole trip.
    const uint32_t timeout = turnTimeoutMsFor(
        static_cast<float>(revs) * 360.0f, MovementExpression::kCommandMax);
    if (timeout == 0) {
      Serial.printf(
          "ERR %ld revolutions will not fit in the %u ms pattern limit at "
          "the current %u ms/turn calibration; try fewer\n",
          revs, MovementExpression::kSpinPatternMaxMs,
          g_spinCalibration.fullTurnMs());
      return;
    }
    // Remember it across the unplug: the turn MUST be untethered, because
    // the cable measurably restrains rotation.
    saveScaleCheckRevs(static_cast<uint8_t>(revs));
    Serial.printf("scale check: %ld revolutions armed (%lums).\n", revs,
                  static_cast<unsigned long>(timeout));
    Serial.println(F("  1. mark the floor at the robot's front"));
    Serial.println(F("  2. unplug USB"));
    Serial.println(F("  3. press TEST - it will spin, then stop"));
    Serial.println(F("  4. measure how far PAST the mark it stopped"));
    Serial.println(F("  5. plug in, run 'scale actual <deg>' (negative = short)"));
    return;
  }
  if (strncmp(argument, "actual", 6) == 0) {
    if (g_scaleCheckRevs == 0) {
      Serial.println(F("ERR run 'scale check' first"));
      return;
    }
    char* end = nullptr;
    const double offset = strtod(argument + 6, &end);
    if (end == argument + 6) {
      Serial.println(F("ERR scale actual <degrees past the mark>"));
      return;
    }
    const float commanded =
        static_cast<float>(g_scaleCheckRevs) * 360.0f;
    const float trueRotation = commanded + static_cast<float>(offset);
    if (!(trueRotation > 0.0f)) {
      Serial.println(F("ERR that offset implies it barely moved"));
      return;
    }
    // The gyro said `commanded`; the floor says `trueRotation`. The ratio is
    // exactly how much the gyro under-reports.
    const float correction = trueRotation / commanded;
    if (!(correction > 0.5f) || !(correction < 2.0f)) {
      Serial.printf(
          "ERR implied correction %.3f is outside 0.5..2.0 - re-check the "
          "measurement rather than applying it\n",
          static_cast<double>(correction));
      return;
    }

    // How well the offset could be read at all. Applying a correction
    // smaller than this would be false precision: it would encode the
    // reading error as if it were a sensor property.
    const float resolution = kMarkReadingUncertaintyDeg / commanded;
    const float implied = correction > 1.0f ? correction - 1.0f
                                            : 1.0f - correction;
    const float boundPercent =
        (implied > resolution ? implied : resolution) * 100.0f;

    Serial.printf(
        "scale: gyro said %.0f deg, floor says %.0f (offset %+.1f)\n",
        static_cast<double>(commanded), static_cast<double>(trueRotation),
        static_cast<double>(offset));
    Serial.printf(
        "scale: implied error %.3f%%, reading resolution %.3f%% over %u revs\n",
        static_cast<double>(implied * 100.0f),
        static_cast<double>(resolution * 100.0f),
        static_cast<unsigned>(g_scaleCheckRevs));

    if (implied <= resolution) {
      // Indistinguishable from no error. Record that it was CHECKED and
      // bounded, which is a stronger and more useful statement than an
      // invented correction — and leave the factory scale alone.
      saveScaleBound(boundPercent);
      saveScaleCheckRevs(0);
      Serial.printf(
          "scale: VERIFIED - error is below what the check can resolve.\n"
          "scale: factory scale kept; error bounded to <%.3f%% "
          "(<%.2f deg per 360)\n",
          static_cast<double>(boundPercent),
          static_cast<double>(boundPercent / 100.0f * 360.0f));
      Serial.println(F("OK"));
      return;
    }

    const float previous = ImuQmi8658::gyroScaleCorrection();
    ImuQmi8658::setGyroScaleCorrection(correction);
    saveGyroScale(correction);
    saveScaleBound(boundPercent);
    saveScaleCheckRevs(0);
    Serial.printf("scale: correction %.4f applied (was %.4f)\n",
                  static_cast<double>(correction),
                  static_cast<double>(previous));

    // alpha is measured THROUGH the gyro: alpha = w^2/(2*coast), and both w
    // and coast scale together, so alpha scales linearly. Adjusting it here
    // keeps the coast compensation valid instead of silently invalidating
    // three bench runs of tuning.
    const float ratio = correction / previous;
    g_coastAlpha *= ratio;
    Serial.printf("scale: coast alpha auto-adjusted x%.4f -> %.0f deg/s2\n",
                  static_cast<double>(ratio),
                  static_cast<double>(g_coastAlpha));
    Serial.println(F("scale: re-run 'spin auto' and a bench run to confirm"));
    if (establishYawAxis()) {
      Serial.println(F("scale: yaw axis re-established"));
    }
    Serial.println(F("OK"));
    return;
  }
  Serial.println(F("ERR scale [check [revs]|actual <deg>|reset|status]"));
  return;
}

void cmdCod(char* argument) {
  // Calibration On Demand: the chip measures and corrects its own gyro SCALE
  // factor. Worth doing because scale is the one error the zero-velocity
  // update cannot see — 3% of zero is still zero — so it would otherwise
  // need an external angle reference (a protractor and a human).
  if ((argument == nullptr) || (*argument == '\0') ||
      strcmp(argument, "status") == 0) {
    Serial.printf("cod: stored=%d gains=(%u,%u,%u)\n",
                  g_codGains.valid ? 1 : 0,
                  static_cast<unsigned>(g_codGains.x),
                  static_cast<unsigned>(g_codGains.y),
                  static_cast<unsigned>(g_codGains.z));
    Serial.printf("cod: gyro LPF %s mode %u\n",
                  ImuQmi8658::gyroLowPassEnabled() ? "ON" : "off",
                  static_cast<unsigned>(ImuQmi8658::gyroLowPassMode()));
    Serial.println(F("usage: cod run | cod clear"));
    return;
  }
  if (strcmp(argument, "clear") == 0) {
    nvs_handle_t handle = 0;
    if (nvs_open(kCodNamespace, NVS_READWRITE, &handle) == ESP_OK) {
      nvs_erase_key(handle, kCodKey);
      nvs_commit(handle);
      nvs_close(handle);
    }
    g_codGains.valid = false;
    Serial.println(F("cod: cleared (power-cycle to drop the applied gains)"));
    return;
  }
  if (strcmp(argument, "run") != 0) {
    Serial.println(F("ERR cod [run|clear|status]"));
    return;
  }
  if (g_turnCmdActive || g_straightActive || motionInProgress()) {
    Serial.println(F("ERR cod needs the robot stopped"));
    return;
  }
  Serial.println(F("cod: running, ~2s. DO NOT TOUCH THE ROBOT."));
  Serial.flush();
  ImuQmi8658::CodResult result{};
  const bool ok = ImuQmi8658::runCod(result);
  Serial.printf("cod: status=0x%02X gains=(%u,%u,%u)\n",
                static_cast<unsigned>(result.status),
                static_cast<unsigned>(result.gainX),
                static_cast<unsigned>(result.gainY),
                static_cast<unsigned>(result.gainZ));
  if (!ok) {
    const char* reason = ImuQmi8658::codFailureReason(result.status);
    Serial.printf("cod: FAILED - %s\n",
                  reason != nullptr ? reason : "no response from the device");
    return;
  }
  if (result.gainX == 0 || result.gainY == 0 || result.gainZ == 0) {
    Serial.println(F("cod: refusing a zero gain (would scale the gyro away)"));
    return;
  }
  if (!saveCodGains(result.gainX, result.gainY, result.gainZ)) {
    Serial.println(F("cod: measured, but FAILED TO SAVE"));
    return;
  }
  Serial.println(F("cod: saved, re-applied automatically at every boot"));
  // Re-establish the yaw axis and bias: the scale just changed underneath
  // every previously measured constant.
  if (establishYawAxis()) {
    Serial.println(F("cod: yaw axis re-established"));
  }
  Serial.println(F("OK"));
  return;
}

void cmdLpf(char* argument) {
  // Gyro low-pass. Runtime-switchable ON PURPOSE: it does NOT reduce
  // integrated-angle error from white noise (integration is already an
  // average), but it does prevent servo vibration above Nyquist from
  // aliasing down into the signal band. It also costs ~38 ms of group delay.
  // Both effects are measurable with a bench run, so make it easy to A/B.
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.printf("lpf: %s mode %u (%s)\n",
                  ImuQmi8658::gyroLowPassEnabled() ? "ON" : "off",
                  static_cast<unsigned>(ImuQmi8658::gyroLowPassMode()),
                  ImuQmi8658::gyroLowPassEnabled()
                      ? "~26 Hz bandwidth, ~38 ms group delay"
                      : "full Nyquist band");
    Serial.println(F("usage: lpf on [mode 0..3] | lpf off"));
    return;
  }
  if (strcmp(argument, "off") == 0) {
    if (!ImuQmi8658::setGyroLowPass(false, 0)) {
      Serial.println(F("ERR could not write CTRL5"));
      return;
    }
    Serial.println(F("lpf: off"));
    return;
  }
  if (strncmp(argument, "on", 2) == 0) {
    long mode = 0;
    if (argument[2] != '\0') mode = strtol(argument + 2, nullptr, 10);
    if (mode < 0 || mode > 3) {
      Serial.println(F("ERR mode 0..3"));
      return;
    }
    if (!ImuQmi8658::setGyroLowPass(true, static_cast<uint8_t>(mode))) {
      Serial.println(F("ERR could not write CTRL5"));
      return;
    }
    Serial.printf("lpf: on, mode %ld\n", mode);
    return;
  }
  Serial.println(F("ERR lpf [on [mode]|off]"));
  return;
}

void cmdStraight(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.printf("straight: %.1f cm/s at full speed (%s)\n",
                  static_cast<double>(g_forwardCmPerSec),
                  g_forwardCalUpdates == 0
                      ? "DERIVED from spin rate, never measured"
                      : "self-calibrated from inertial distance");
    Serial.printf("straight: %lu self-calibration updates so far\n",
                  static_cast<unsigned long>(g_forwardCalUpdates));
    Serial.println(F("usage: straight <cm> [speed] | straight cal <actual cm>"));
    return;
  }
  if (strncmp(argument, "cal ", 4) == 0) {
    // Optional override. The robot self-calibrates from inertial distance
    // after every leg, so this exists only for the case where a tape measure
    // disagrees with the IMU and you trust the tape.
    const long actual = strtol(argument + 4, nullptr, 10);
    if (actual < 5 || actual > 400 || g_straightTargetCm <= 0) {
      Serial.println(F("ERR straight cal <actual cm>, after a straight run"));
      return;
    }
    const float ratio = static_cast<float>(actual) /
                        static_cast<float>(g_straightTargetCm);
    float updated = g_forwardCmPerSec * ratio;
    if (updated < kForwardCmPerSecMin) updated = kForwardCmPerSecMin;
    if (updated > kForwardCmPerSecMax) updated = kForwardCmPerSecMax;
    Serial.printf(
        "straight cal: commanded %d cm, actually went %ld -> %.1f cm/s "
        "(was %.1f)\n",
        static_cast<int>(g_straightTargetCm), actual,
        static_cast<double>(updated), static_cast<double>(g_forwardCmPerSec));
    g_forwardCmPerSec = updated;
    ++g_forwardCalUpdates;
    return;
  }
  char* end = nullptr;
  const long cm = strtol(argument, &end, 10);
  int8_t speed = MovementExpression::kSpinSpeed;
  if (end != nullptr && *end != '\0') {
    const long parsedSpeed = strtol(end, nullptr, 10);
    if (parsedSpeed != 0) speed = static_cast<int8_t>(parsedSpeed);
  }
  startStraightDrive(static_cast<int16_t>(cm), speed);
  return;
}

void cmdSurvey(char* argument) {
  // Drives an equilateral triangle, sampling RSSI at each vertex and once
  // more back at the start. Translation is the only motion that changes range
  // to an AP, so it is the only motion that can produce RSSI information.
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.printf("survey: phase=%d side=%ucm scans=%u\n",
                  static_cast<int>(g_surveyPhase),
                  static_cast<unsigned>(g_surveySideCm),
                  static_cast<unsigned>(g_surveyScans));
    Serial.println(F("usage: survey start [sideCm] [scans]  (NEEDS FLOOR)"));
    return;
  }
  if (strcmp(argument, "stop") == 0) {
    if (g_surveyPhase == SurveyPhase::Idle) {
      Serial.println(F("survey: not running"));
      return;
    }
    surveyFinish("stopped");
    return;
  }
  if (strncmp(argument, "start", 5) == 0) {
    uint16_t side = 100;
    uint16_t scans = 12;
    const char* rest = argument + 5;
    char* end = nullptr;
    const long parsedSide = strtol(rest, &end, 10);
    if (end != rest && parsedSide > 0) {
      side = static_cast<uint16_t>(parsedSide);
      const long parsedScans = strtol(end, nullptr, 10);
      if (parsedScans > 0 && parsedScans <= 200) {
        scans = static_cast<uint16_t>(parsedScans);
      }
    }
    startTriangleSurvey(side, scans);
    return;
  }
  Serial.println(F("ERR survey [start [sideCm] [scans]|stop]"));
  return;
}

void cmdRfsurvey(char* argument) {
  // Samples RSSI at ONE labelled position, averaged over N scans.
  //
  // This tests the load-bearing assumption behind using WiFi for absolute
  // position: does moving the robot change RSSI by more than the scatter at
  // a fixed point? Predicted signal is 0.63 dB per 0.3 m and 1.98 dB per 1 m
  // (n=2.5, AP at 5 m); measured fixed-point noise is 1-2 dB. So the answer
  // is not obvious, averaging is mandatory, and the comparison must be made
  // against the spread rather than against zero.
  //
  // NOTE this is translation, not rotation. Rotating in place changes range
  // to nothing and produces exactly zero information; moving genuinely does
  // change range. The two ideas fail or succeed for completely different
  // reasons and must not be conflated.
  if (g_turnCmdActive || motionInProgress()) {
    Serial.println(F("ERR rfsurvey needs the robot stopped"));
    return;
  }
  if (g_overrideUp) {
    Serial.println(F("ERR rfsurvey cannot run while the override AP is up"));
    return;
  }
  if (g_rfScanActive || g_rfSurveyPosition >= 0) {
    Serial.println(F("ERR a scan is already running"));
    return;
  }
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.println(F("ERR rfsurvey <label> [scans]  |  rfsurvey report|clear"));
    return;
  }
  if (strcmp(argument, "clear") == 0) {
    g_rfMap.erase();
    Serial.println(F("rfsurvey: survey erased"));
    return;
  }
  if (strcmp(argument, "report") == 0) {
    if (g_rfMap.positionCount() == 0 && !g_rfMap.load()) {
      Serial.println(F("rfsurvey: no stored survey"));
      return;
    }
    Serial.setTxTimeoutMs(20);
    Serial.printf("rfsurvey report: %u positions, %u APs\n",
                  static_cast<unsigned>(g_rfMap.positionCount()),
                  static_cast<unsigned>(g_rfMap.apCount()));
    for (uint8_t a = 0; a < g_rfMap.apCount(); ++a) {
      const uint8_t* b = g_rfMap.bssidAt(a);
      Serial.printf("ap%u %02X:%02X:%02X:%02X:%02X:%02X\n",
                    static_cast<unsigned>(a), b[0], b[1], b[2], b[3], b[4],
                    b[5]);
      for (uint8_t p = 0; p < g_rfMap.positionCount(); ++p) {
        const RfMap::Reading& r = g_rfMap.readingAt(p, a);
        if (r.samples == 0) {
          Serial.printf("   %-12s  (not seen)\n", g_rfMap.labelAt(p));
          continue;
        }
        Serial.printf("   %-12s  %7.2f dBm  spread %5.2f  n=%u\n",
                      g_rfMap.labelAt(p),
                      static_cast<double>(r.meanTenthDbm) / 10.0,
                      static_cast<double>(r.spreadTenthDbm) / 10.0,
                      static_cast<unsigned>(r.samples));
      }
      // The verdict, per AP, for every position pair: is the difference
      // between positions bigger than the scatter within them?
      for (uint8_t p = 0; p + 1 < g_rfMap.positionCount(); ++p) {
        for (uint8_t q = p + 1; q < g_rfMap.positionCount(); ++q) {
          float delta = 0.0f, spread = 0.0f;
          uint16_t n = 0;
          if (!g_rfMap.compare(a, p, q, &delta, &spread, &n)) continue;
          const float mag = delta < 0 ? -delta : delta;
          Serial.printf("   %s->%s  delta %+.2f dB vs spread %.2f  -> %s\n",
                        g_rfMap.labelAt(p), g_rfMap.labelAt(q),
                        static_cast<double>(delta),
                        static_cast<double>(spread),
                        mag > spread ? "SIGNAL" : "below noise");
        }
      }
      Serial.flush();
      delay(6);
    }
    Serial.flush();
    Serial.setTxTimeoutMs(0);
    Serial.println(F("OK"));
    return;
  }

  // rfsurvey <label> [scans]
  char label[RfMap::kLabelLen] = {0};
  unsigned scans = 16;
  const char* space = strchr(argument, ' ');
  if (space == nullptr) {
    strncpy(label, argument, sizeof(label) - 1);
  } else {
    const size_t len = static_cast<size_t>(space - argument);
    strncpy(label, argument, len < sizeof(label) - 1 ? len
                                                     : sizeof(label) - 1);
    const long parsed = strtol(space + 1, nullptr, 10);
    if (parsed < 1 || parsed > 200) {
      Serial.println(F("ERR scans 1..200"));
      return;
    }
    scans = static_cast<unsigned>(parsed);
  }
  if (g_rfMap.positionCount() == 0) g_rfMap.load();
  const int8_t position = g_rfMap.beginPosition(label);
  if (position < 0) {
    Serial.println(F("ERR survey is full (8 positions)"));
    return;
  }
  const int started = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
  if (started == WIFI_SCAN_FAILED) {
    Serial.println(F("ERR rfsurvey could not start"));
    return;
  }
  WiFi.mode(WIFI_STA);
  g_rfScanActive = true;
  g_rfScanDeadlineMs = millis() + kRfScanTimeoutMs;
  g_rfSurveyPosition = position;
  g_rfSurveyRemaining = static_cast<uint16_t>(scans);
  g_rfSurveyDone = 0;
  Serial.printf("rfsurvey: \"%s\" x%u scans (~%us), hold still...\n", label,
                scans, static_cast<unsigned>((scans * 22) / 10));
  return;
}

void cmdRfscan(char* argument) {
  // The 8-heading stop-and-scan pre-check.
  //
  // The research verdict on WiFi as a compass is a clear no (see
  // IMU-RESEARCH.md section 4): rotating an antenna about its own phase
  // centre produces no range change, so there is no synthetic aperture, and
  // the published attempt closest to this idea scored F1 0.39 on 90-degree
  // classification. This command is deliberately NOT the spin-and-log tool
  // that verdict rules out. It is the cheap experiment that closes the
  // question with our own data instead of only a citation: stop at a
  // heading, scan, turn, repeat, and see whether RSSI varies with heading
  // repeatably. If it does not, the matter is settled empirically.
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR rfscan takes no arguments"));
    return;
  }
  // A scan blocks for seconds. Doing that mid-turn would starve the turn
  // controller of samples while the motors are running - the one thing the
  // closed loop must never be denied.
  if (g_turnCmdActive || motionInProgress()) {
    Serial.println(F("ERR rfscan needs the robot stopped"));
    return;
  }
  if (g_overrideUp) {
    // Scanning needs STA mode; switching would drop the joystick client.
    Serial.println(F("ERR rfscan cannot run while the override AP is up"));
    return;
  }
  if (g_rfScanActive) {
    Serial.println(F("ERR rfscan already running"));
    return;
  }

  const float headingNow = g_headingIntegrator.totalDegrees();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  // Async, NOT blocking. A synchronous scan parks the loop task for several
  // seconds, which starves the task watchdog and reboots the board - measured
  // as "task_wdt: Aborting" followed by RTC_SW_CPU_RST. The scan is collected
  // by serviceRfScan() once loop() sees it finish.
  const int started = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
  if (started == WIFI_SCAN_FAILED) {
    Serial.println(F("ERR rfscan could not start"));
    return;
  }
  g_rfScanActive = true;
  g_rfScanHeading = headingNow;
  g_rfScanDeadlineMs = millis() + kRfScanTimeoutMs;
  Serial.printf("rfscan: scanning at heading %.2f deg...\n",
                static_cast<double>(headingNow));
  return;
}

void cmdPower(char* argument) {
  if ((argument != nullptr) && (*argument != '\0')) {
    Serial.println(F("ERR power takes no arguments"));
    return;
  }
  // Reports the things that cannot be seen over a cable: whether the battery
  // latch is actually asserted, and whether the backlight is actually up.
  // A black screen with a healthy log is either backlight=0 or sysEn=0, and
  // this distinguishes them without needing someone to look at the robot.
  Serial.printf(
      "power: sysEn=%d sysOut=%d armed=%d triggered=%d backlight=%u pwm=%d "
      "paused=%d shuttingDown=%d lcd=%d uptime=%lums\n",
      digitalRead(SYS_EN), digitalRead(SYS_OUT),
      g_powerButtonArmed ? 1 : 0, g_powerOffTriggered ? 1 : 0,
      static_cast<unsigned>(g_backlightLevel),
      g_backlightPwmReady ? 1 : 0, g_paused ? 1 : 0,
      g_systemShuttingDown ? 1 : 0, static_cast<int>(g_lcdMode),
      static_cast<unsigned long>(millis()));
  Serial.println(F("OK"));
  return;
}

void cmdSpin(char* argument) {
  if ((argument == nullptr) || (*argument == '\0')) {
    Serial.println(F("ERR spin <test [ms] [cw|ccw]|auto|set <ms>|show|save confirm|erase confirm>"));
    return;
  }

  // Split off the first token; the rest is the optional remainder.
  char verb[16];
  const char* rest = strchr(argument, ' ');
  size_t verbLength =
      (rest == nullptr) ? strlen(argument)
                        : static_cast<size_t>(rest - argument);
  if (verbLength == 0 || verbLength >= sizeof(verb)) {
    Serial.println(F("ERR spin <auto|test|set|show|save|erase>"));
    return;
  }
  memcpy(verb, argument, verbLength);
  verb[verbLength] = '\0';
  if (rest != nullptr) {
    while (*rest == ' ') ++rest;
    if (*rest == '\0') rest = nullptr;
  }

  if (strcmp(verb, "calib") == 0) {
    // Same sequence the on-screen button runs: probe slow, probe fast, fit,
    // predict, verify, save. Exposed on serial so it can be watched.
    if (rest != nullptr) {
      Serial.println(F("ERR spin calib takes no arguments"));
      return;
    }
    if (!startTurnCalibration()) {
      Serial.println(F("ERR could not start calibration"));
      return;
    }
    return;
  }

  if (strcmp(verb, "show") == 0) {
    if (rest != nullptr) {
      Serial.println(F("ERR spin show takes no arguments"));
      return;
    }
    Serial.printf(
        "spin: effective=%u saved=%u default=%u dirty=%d stored=%d\n",
        g_spinCalibration.fullTurnMs(), g_spinCalibration.savedMs(),
        MovementExpression::kSpinDefaultMs,
        g_spinCalibration.dirty() ? 1 : 0,
        g_spinCalibration.loadedFromNvs() ? 1 : 0);
    Serial.println(F("OK"));
    return;
  }

  const bool isTest = strcmp(verb, "test") == 0;
  const bool isAuto = strcmp(verb, "auto") == 0;
  if (isTest || isAuto) {
    // `spin test` -> observe -> `spin set <better>` -> `spin test`, or
    // `spin auto` which measures with the gyro and stages the result itself.
    uint16_t ms = g_spinCalibration.fullTurnMs();
    bool clockwise = true;
    if (rest != nullptr) {
      char* end = nullptr;
      const long parsed = strtol(rest, &end, 10);
      if (end != rest) {
        if (parsed < 0 || parsed > 65535) {
          Serial.println(F("ERR turn must be a positive integer"));
          return;
        }
        ms = static_cast<uint16_t>(parsed);
        while (*end == ' ') ++end;
        rest = end;
      }
      if (rest != nullptr && *rest != '\0') {
        if (strcmp(rest, "ccw") == 0) {
          clockwise = false;
        } else if (strcmp(rest, "cw") == 0) {
          clockwise = true;
        } else {
          Serial.printf("ERR spin %s [ms] [cw|ccw]\n", verb);
          return;
        }
      }
    }
    if (isAuto) {
      if (!startTurnAutoMeasure(ms, clockwise)) {
        return;
      }
      // The result is reported asynchronously once the spin and the coast
      // after it have both been measured.
      return;
    }
    if (!startSpinCalibration(ms, clockwise)) {
      return;
    }
    Serial.println(F("OK"));
    return;
  }

  if (strcmp(verb, "set") == 0) {
    if (rest == nullptr) {
      Serial.println(F("ERR spin set <200..2000>"));
      return;
    }
    char* end = nullptr;
    const long parsed = strtol(rest, &end, 10);
    if (end == rest || (end != nullptr && *end != '\0') || parsed < 0 ||
        parsed > 65535) {
      Serial.println(F("ERR spin set <200..2000>"));
      return;
    }
    if (!g_spinCalibration.stage(static_cast<uint16_t>(parsed))) {
      Serial.printf("ERR turn must be %u..%u ms in steps of %u\n",
                    MovementExpression::kSpinMinMs,
                    MovementExpression::kSpinMaxMs,
                    MovementExpression::kStepQuantumMs);
      return;
    }
    Serial.printf("spin: staged %u ms (not saved)\n",
                  g_spinCalibration.fullTurnMs());
    Serial.println(F("OK"));
    return;
  }

  if (strcmp(verb, "save") == 0) {
    if (rest == nullptr || strcmp(rest, "confirm") != 0) {
      Serial.println(F("ERR spin save confirm"));
      return;
    }
    if (!g_spinCalibration.save()) {
      Serial.println(F("ERR spin save failed; nothing persisted"));
      return;
    }
    Serial.printf("spin: saved %u ms\n", g_spinCalibration.savedMs());
    Serial.println(F("OK"));
    return;
  }

  if (strcmp(verb, "erase") == 0) {
    if (rest == nullptr || strcmp(rest, "confirm") != 0) {
      Serial.println(F("ERR spin erase confirm"));
      return;
    }
    if (!g_spinCalibration.erase()) {
      Serial.println(F("ERR spin erase failed"));
      return;
    }
    Serial.printf("spin: erased; using default %u ms\n",
                  g_spinCalibration.fullTurnMs());
    Serial.println(F("OK"));
    return;
  }

  Serial.println(F("ERR spin <auto|test|set|show|save|erase>"));
  return;
}

// Dispatch/help table.  Rows are in help-print order; dispatch match is by
// strcmp on `name`, so order is irrelevant for correctness.
const SerialCommand kSerialCommands[] = {
  { "help", cmdHelp, "help", nullptr, nullptr },
  { "ping", cmdPing, "ping", nullptr, nullptr },
  { "status", cmdStatus, "status", nullptr, nullptr },
  { "mood", cmdMood, "mood <default|bored|annoyed|tired|angry|happy|sad|think|error|\n      surprised|scared|wink|sleeping|love|confused|curious>", nullptr, nullptr },
  { "look", cmdLook, "look <default|center|n|ne|e|se|s|sw|w|nw>", nullptr, nullptr },
  { "blink", cmdBlink, "blink", nullptr, nullptr },
  { "wander", cmdWander, "wander <on|off>", nullptr, nullptr },
  { "dims", cmdDims, "dims <w> <h> <r>", nullptr, nullptr },
  { "motion", cmdMotion, "motion <x> <y>", nullptr, nullptr },
  { "blinkrate", cmdBlinkrate, "blinkrate <sec> <jitter>", nullptr, nullptr },
  { "color", cmdColor, "color <RRGGBB|green|cyan|red|blue|yellow|white|magenta|orange>", nullptr, nullptr },
  { "headgain", cmdHeadgain, "headgain <xDiv> <yDiv>", nullptr, nullptr },
  { "tone", cmdTone, "tone <hz>", nullptr, nullptr },
  { "sleep", cmdSleep, "sleep", nullptr, nullptr },
  { "wake", cmdWake, "wake", nullptr, nullptr },
  { "demo", cmdDemo, "demo", nullptr, nullptr },
  { "motorstate", cmdMotorstate, "motorstate", nullptr, nullptr },
  { "cal", cmdCal, "cal show\ncal set <left|right> <stop|fwdstart|revstart|fwdmax|revmax> <1000..2000>\ncal validate\ncal save confirm\ncal reset confirm  (draft only; NVS reloads after reboot)\ncal erase confirm  (erase persisted calibration; drive remains locked)", nullptr, nullptr },
  { "servooff", cmdServooff, "servooff         (safe drive powerdown only)", nullptr, nullptr },
  { "gesture", cmdGesture, nullptr, printGestureUsage, nullptr },
  { "test", cmdTest, "test                     360 fast cw, rezero, 360 slow ccw", nullptr, nullptr },
  { "heading", cmdHeading, "heading [zero]          relative heading + drift state", nullptr, nullptr },
  { "turn", cmdTurn, "turn <deg> [cw|ccw] [speed]  closed-loop, gyro-stopped", nullptr, nullptr },
  { "power", cmdPower, "power                    battery latch + backlight state", nullptr, nullptr },
  { "spin", cmdSpin, "spin calib               probe+model+verify, saves itself\nspin auto [ms] [cw|ccw]  measure the turn with the gyro\nspin test [ms] [cw|ccw]  run one measurement turn\nspin set <200..2000>     stage the measured full turn\nspin show                staged vs saved vs default\nspin save confirm\nspin erase confirm", nullptr, nullptr },
  { "sound", cmdSound, "sound <happy|sad|confused>   compose + play (fresh each time)\nsound stop", nullptr, "-- expression --" },
  { "wifi", cmdWifi, "wifi                     AP mode, IP, MAC, stations, tx power", nullptr, "-- network / UI --" },
  { "state", cmdState, "state                    override state machine + LCD mode", nullptr, nullptr },
  { "swipe", cmdSwipe, "swipe <down|up>          queue a wake / teardown gesture", nullptr, nullptr },
  { "lcdspi", cmdLcdspi, "lcdspi [hz]              show/set SPI clock (8M..80M, reboot to apply)", nullptr, "-- display diagnostics --" },
  { "lcdprobe", cmdLcdprobe, "lcdprobe                 read panel RDDPM: is it awake and on?", nullptr, nullptr },
  { "lcdpush", cmdLcdpush, "lcdpush                  full canvas->panel push only", nullptr, nullptr },
  { "lcdbus", cmdLcdbus, "lcdbus                   re-init the SPI peripheral only", nullptr, nullptr },
  { "crash", cmdCrash, "crash [clear]            last fault: reason + phase + heap", nullptr, nullptr },
  { "straight", cmdStraight, "straight <cm> [speed]    gyro-corrected straight run", nullptr, "-- motion / IMU --" },
  { "sense", cmdSense, "sense                    bump/shake/lifted detector state", nullptr, nullptr },
  { "lead", cmdLead, "lead [alpha]             turn coast-compensation constant", nullptr, nullptr },
  { "scale", cmdScale, "scale [actual <deg>]     gyro scale check / correction", nullptr, nullptr },
  { "cod", cmdCod, "cod [run|status]         on-demand gyro calibration", nullptr, nullptr },
  { "lpf", cmdLpf, "lpf [0..3]               gyro low-pass filter mode", nullptr, nullptr },
  { "bench", cmdBench, "bench <arm|report|clear> unattended measurement run", nullptr, nullptr },
  { "survey", cmdSurvey, "survey ...               dead-reckoning position survey", nullptr, "-- survey / RF --" },
  { "rfscan", cmdRfscan, "rfscan / rfsurvey        RSSI scan + per-position store", nullptr, nullptr },
  { "rfsurvey", cmdRfsurvey, nullptr, nullptr, nullptr },
  { "touchinfo", cmdTouchinfo, "touchinfo / touchlog / touchdebug / touchreset", nullptr, "-- touch --" },
  { "touchlog", cmdTouchlog, nullptr, nullptr, nullptr },
  { "touchdebug", cmdTouchdebug, nullptr, nullptr, nullptr },
  { "touchreset", cmdTouchreset, nullptr, nullptr, nullptr },
};

constexpr size_t kSerialCommandCount =
    sizeof(kSerialCommands) / sizeof(kSerialCommands[0]);

void printHelp() {
  serialConsolePrintHelp(kSerialCommands, kSerialCommandCount);
}

void dispatchCommand(char* line) {
  serialConsoleDispatch(line, kSerialCommands, kSerialCommandCount);
}

void drainSerial() {
  constexpr uint16_t kMaxSerialBytesPerLoop = 64;
  uint16_t processed = 0;
  while (Serial.available() > 0 && processed < kMaxSerialBytesPerLoop) {
    const int nextByte = Serial.read();
    ++processed;
    if (nextByte < 0) {
      continue;
    }

    const SerialLineEvent event =
        g_serialInput.push(static_cast<uint8_t>(nextByte));
    switch (event) {
      case SerialLineEvent::InvalidByte:
        Serial.println(F("ERR invalid byte in command"));
        break;
      case SerialLineEvent::Overflow:
        Serial.println(F("ERR line too long"));
        break;
      case SerialLineEvent::LineReady: {
        char* line = normalizeLine(g_serialInput.line());
        if (*line != '\0') {
          dispatchCommand(line);
        }
        g_serialInput.consume();
        return;
      }
      case SerialLineEvent::None:
        break;
    }
  }
}

void processDemo() {
  if (!g_demoActive) {
    return;
  }

  const uint32_t now = millis();
  if (now < g_demoNextStepMs) {
    return;
  }

  if (g_demoStep >= kDemoTotalSteps) {
    // Loop forever — demo mode cycles continuously until the user sends
    // any other command (drainSerial calls stopDemo on every non-demo
    // command) or `demo off` / `stop` is sent.
    g_demoStep = 0;
  }

  if (g_demoStep < kDemoMoodCount) {
    const NamedMood& moodStep = kDemoMoods[g_demoStep];
    setEyesMood(moodStep.mood);
    if (serialCanWrite()) {
      Serial.printf("demo step %u/%u mood %s\n",
                    static_cast<unsigned>(g_demoStep + 1U),
                    static_cast<unsigned>(kDemoTotalSteps),
                    moodStep.name);
    }
  }
#if 0
  // READ-7: the look and blink phases below are provably unreachable
  // while kDemoTotalSteps == kDemoMoodCount (see the constexpr on line
  // ~418). Proof:
  //   1. Line above clamps g_demoStep to 0 when it hits kDemoTotalSteps.
  //   2. kDemoTotalSteps == kDemoMoodCount, so after the clamp
  //      g_demoStep < kDemoMoodCount always holds when we reach the if.
  //   3. Therefore the first `if` above is always taken, and the two
  //      branches below never execute.
  //   4. g_demoStep is only written by processDemo() (this function --
  //      the ++ below and the =0 above) and by startDemo/stopDemo
  //      which reset to 0. No other writer can push it into the
  //      look/blink range.
  // Kept as source-only documentation so the wiring is obvious when we
  // want to restore look/blink cycling: change the constexpr on line
  // ~418 back to `kDemoMoodCount + kDemoLookCount + 1U` AND flip this
  // `#if 0` to `#if 1` (or remove the guard). Do not delete -- the two
  // must move together to avoid a table drift bug.
  else if (g_demoStep < static_cast<uint8_t>(kDemoMoodCount + kDemoLookCount)) {
    const uint8_t lookIndex = static_cast<uint8_t>(g_demoStep - kDemoMoodCount);
    const NamedLook& lookStep = kDemoLooks[lookIndex];
    g_eyes.setPosition(lookStep.position);
    if (serialCanWrite()) {
      Serial.printf("demo step %u/%u look %s\n",
                    static_cast<unsigned>(g_demoStep + 1U),
                    static_cast<unsigned>(kDemoTotalSteps),
                    lookStep.name);
    }
  } else {
    g_eyes.blink();
    if (serialCanWrite()) {
      Serial.printf("demo step %u/%u blink\n",
                    static_cast<unsigned>(g_demoStep + 1U),
                    static_cast<unsigned>(kDemoTotalSteps));
    }
  }
#endif

  ++g_demoStep;
  g_demoNextStepMs = now + kDemoStepIntervalMs;
  setEyeLoopRate(true);
  requestImmediateFrame();
}

bool applyHeadSympathyShift() {
  const int16_t gazeX = g_eyes.gazeOffsetX();
  const int16_t gazeY = g_eyes.gazeOffsetY();
  const int8_t newShiftX = (g_headShiftXDiv == 0U)
                               ? 0
                               : static_cast<int8_t>(gazeX / g_headShiftXDiv);
  const int8_t newShiftY = (g_headShiftYDiv == 0U)
                               ? 0
                               : static_cast<int8_t>(gazeY / g_headShiftYDiv);

  g_headShiftX = newShiftX;
  g_headShiftY = newShiftY;

  if ((newShiftX == g_lastShiftX) && (newShiftY == g_lastShiftY)) {
    return false;
  }

  const int16_t oldOriginX = static_cast<int16_t>(kOriginX + g_lastShiftX);
  const int16_t oldOriginY = static_cast<int16_t>(kOriginY + g_lastShiftY);
  const int16_t newOriginX = static_cast<int16_t>(kOriginX + newShiftX);
  const int16_t newOriginY = static_cast<int16_t>(kOriginY + newShiftY);

  const int16_t unionX = (oldOriginX < newOriginX) ? oldOriginX : newOriginX;
  const int16_t unionY = (oldOriginY < newOriginY) ? oldOriginY : newOriginY;
  const int16_t unionRight = ((oldOriginX + MASCOT_A_W) > (newOriginX + MASCOT_A_W))
                                 ? static_cast<int16_t>(oldOriginX + MASCOT_A_W)
                                 : static_cast<int16_t>(newOriginX + MASCOT_A_W);
  const int16_t unionBottom = ((oldOriginY + MASCOT_A_H) > (newOriginY + MASCOT_A_H))
                                  ? static_cast<int16_t>(oldOriginY + MASCOT_A_H)
                                  : static_cast<int16_t>(newOriginY + MASCOT_A_H);
  const int16_t unionW = static_cast<int16_t>(unionRight - unionX);
  const int16_t unionH = static_cast<int16_t>(unionBottom - unionY);

  backupEyeRegionFromCanvas();
  g_canvas->fillRect(unionX, unionY, unionW, unionH, MASCOT_A_BG);
  drawMascotToCanvas(newOriginX, newOriginY);
  restoreEyeRegionToCanvas();
  pushCanvasRectToLcd(unionX, unionY, unionW, unionH);

  g_lastShiftX = newShiftX;
  g_lastShiftY = newShiftY;
  g_forceEyeRedraw = true;
  return true;
}

// =========================================================================
// Override phone UI — SSID/WPA per D6, HTTP + WS control plane per D7–D8, telemetry protocol per §4.2. Permanent.
//
// Local-only HTTP server in WiFi-AP mode (SSID: "override"). Phone or
// laptop joins the AP and loads http://192.168.4.1 — exposes:
//   * live status (mood, charging, raw V, dsp V, percent)
//   * mood/face picker
//   * tank-steering (two −100..+100 throttle sliders that snap to 0)
//   * read-only native MG90D pulse calibration
//
// DriveSession owns connection, sequence, five-neutral-frame arming, and the
// 300 ms control timeout. Only an eligible persisted calibration can reach
// the canonical output function below.
// =========================================================================
constexpr const char*  kOverrideSsid         = "Hanger OVERRIDE";
constexpr const char*  kOverridePassword     = "TheGarageAtl";
// Fixed, user-chosen AP name and WPA2 password. Only the URL/WebSocket access
// token is regenerated per activation.
static_assert(sizeof("Hanger OVERRIDE") <= kApSsidMaxLength + 1,
              "AP SSID does not fit the credential buffer");
static_assert(sizeof("TheGarageAtl") >= 9,
              "WPA2 requires at least 8 password characters");
static_assert(sizeof("TheGarageAtl") <= kApPasswordMaxLength + 1,
              "AP password does not fit the credential buffer");
constexpr uint16_t     kOverridePort         = 80;       // we own port 80 (no more WiFiManager)
constexpr uint16_t     kOverrideDnsPort      = 53;       // captive-portal DNS catch-all
constexpr uint8_t      kOverrideApChannel    = 6;        // pinned (2.4 GHz mid-band, low congestion)
// Raised 4 -> 10 (SEC-1). 10 is the ESP-IDF softAP cap
// (CONFIG_ESP_WIFI_SOFTAP_MAX_STA_NUM), so this is the ceiling, not a guess.
//
// It does NOT close the 802.11-layer slot-exhaustion hole: anyone with the WPA2
// password (which is printed on the pairing screen) can still occupy every slot
// and lock the operator out. It raises the cost 2.5x and, more usefully, gives
// headroom against the mundane case -- a phone that walked out of range without
// disassociating, or a household device re-associating, squatting a slot while
// the operator is refused.
//
// The elaborate fix (track idle stations and esp_wifi_deauth_sta them) was
// considered and rejected: it needs MAC<->IP correlation to tell a real
// operator mid-captive-portal from a squatter, and an over-eager timeout would
// deauth the operator while they are scanning the QR. Three "clever" fixes had
// to be reverted this week; smallest-change wins.
//
// Physical access remains the real trump card: swipe up, swipe down, new
// credentials, new QR.
constexpr uint8_t      kOverrideApMaxClients = 10;
constexpr int8_t       kOverrideApTxPowerQdb = 44;       // 11.0 dBm = ~12 mW (44 quarter-dB units)
                                                          // Lower than max — phones handshake more reliably
constexpr uint32_t     kMotorIdlePowerOffMs  = 1000;
constexpr uint32_t     kOverrideTelemetryMs  = 200;      // push telemetry frame at 5 Hz
// How long an admitted websocket owner may go silent before the transport is
// released. The web app streams control frames continuously, so real silence
// this long means the client is gone -- typically a phone that left WiFi
// without closing the socket, which produces no WS_EVT_DISCONNECT. Generous
// enough to survive a brief radio stall, short enough that the QR screen comes
// back while the user is still standing there wondering why it will not.
constexpr uint32_t     kWsOwnerSilenceTimeoutMs = 8000;
// Longest the main task will wait for in-flight websocket callbacks to drain
// before giving up. Comfortably longer than any real callback, and well under
// the 5 s task watchdog it exists to avoid tripping.
constexpr uint32_t     kIngressDrainTimeoutMs = 750;
// Expo curve for RC-style stick feel: gentle near center, steep near ends.
// out = sign(in) * (|in|/100) * (kExpo*(|in|/100)^2 + (1-kExpo)) * 100
// kExpo=0 → fully linear; kExpo=1 → pure cubic. 0.5 is the typical RC default.
constexpr float        kExpo                 = 0.5f;

// Binary wire-protocol opcodes (see plan §4 Phase 1).
constexpr uint8_t      kOpControl   = 0x01;   // C→S  (4 B)
constexpr uint8_t      kOpTelemetry = 0x12;   // S→C  (30 B; ten uint16 microsecond fields)
constexpr uint8_t      kOpTrim      = 0x03;   // legacy C→S; now read-only/rejected
constexpr uint8_t      kOpMood      = 0x04;   // C→S  (3 B)
// Movement expressions from the web app. Same shape as kOpMood — one id plus a
// sequence byte — because the web app is now the ONLY way to trigger a
// movement: the on-screen button menu was removed so instrumentation and test
// affordances stay off the robot's face.
constexpr uint8_t      kOpGesture   = 0x05;   // C→S  (3 B)
// Expression voices. Separate from kOpGesture because a sound is useful on its
// own — for reacting, acknowledging, or auditioning a voice against a face —
// and because sound needs no drive rail, so it stays available when movement
// is locked out.
constexpr uint8_t      kOpSound     = 0x06;   // C→S  (3 B)
// Round 4e: per-second WS receive counters for the 1-Hz diagnostic line.
volatile uint32_t g_diagCtrlRx = 0;
volatile uint32_t g_diagMoodRx = 0;
// g_overrideUp, g_orServer, g_orWs, g_orDns, g_orTelemetryNextMs declared
// earlier (in the §16 forward-decl block near line ~400).

// =========================================================================
// §16 — CST816T touch driver (swipe gestures)
// =========================================================================
// The Waveshare ESP32-S3-Touch-LCD-1.69 ships with a CST816T cap-touch
// controller on I2C @ 0x15 (SDA=GPIO11, SCL=GPIO10, INT=GPIO14, RST=GPIO13).
// We use mjdonders/CST816_TouchLib in hardware-gesture mode — the chip
// itself detects swipes/taps/longpress and we just poll its gesture register
// from the main loop. ~0 CPU cost when idle.
//
// The chip's "down/up/left/right" labels are wired to the panel's native
// orientation; our Arduino_GFX setRotation may put screen-up in a different
// direction than chip-up. We log raw gesture IDs on the first few events so
// orientation can be verified at runtime, and translate to screen-relative
// "SwipeDown / SwipeUp" via kTouchSwipeDownId / kTouchSwipeUpId below.
//
// Mapping for THIS hardware (Waveshare ESP32-S3-Touch-LCD-1.69, our case):
// Confirmed on-device 2026-06-10 by streaming raw IDs while user swiped:
//   screen swipe-down → chip reports id=1 (GESTURE_RIGHT)
//   screen swipe-up   → chip reports id=2 (GESTURE_LEFT)  [opposite axis]
// The chip's "down/up" labels are 90° off from this panel's orientation;
// the chip's "right/left" axis aligns with the screen's vertical axis.
constexpr uint8_t kTouchSwipeDownId = 1;  // chip GESTURE_RIGHT  → screen DOWN
constexpr uint8_t kTouchSwipeUpId   = 2;  // chip GESTURE_LEFT   → screen UP

class OverrideTouchObserver : public MDO::TouchScreenObserver {
public:
  void gestureNotification(int iGestureId, int x, int y, bool bCurrentlyPressed) override;
  void touchNotification(int x, int y, bool bCurrentlyPressed) override;
};
OverrideTouchObserver  g_orTouchObs;
MDO::CST816Touch_HWMode g_orTouch;

// ---------------------------------------------------------------------------
// Direct finger position, read straight off the CST816T.
//
// The library in hardware-gesture mode only ever reports a COMPLETED gesture --
// verified on hardware, where dragging a finger produced three touchlog entries
// (all gesture IDs, pressed=0) and zero touch points. Its coordinate accessors
// (parseTouchControllerEvent, read, readRegister) are all private, so there is
// no way to ask it politely.
//
// But the chip itself knows exactly where the finger is, and says so in plain
// registers. This reads them directly:
//
//   0x01 gesture     0x02 finger count (0 none / 1 touching)
//   0x03 X[11:8]     0x04 X[7:0]        (bits 7:6 of XH are the event code)
//   0x05 Y[11:8]     0x06 Y[7:0]
//
// Six bytes, ~150 us at 400 kHz. Deliberately NOT a driver-mode change: the
// library keeps doing gesture detection exactly as before, with its swipe IDs
// still calibrated for this panel's 90-degree axis offset. This only adds a
// read alongside it.
//
// Called AFTER g_orTouch.control() so the library always sees the registers
// first and its own event handling is unaffected. Reads on this chip are
// non-destructive -- the values persist until the next touch event -- so both
// can read the same registers in the same pass.
//
// Gated to drag-capable modes by the caller, because this bus has an
// unexplained stall under investigation and there is no reason to add traffic
// on screens that cannot use the result.
constexpr uint8_t kCstRegGesture = 0x01;

bool readTouchPointDirect(int16_t& rawX, int16_t& rawY, bool& pressed) {
  Wire.beginTransmission(TP_ADDR);
  Wire.write(kCstRegGesture);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(static_cast<uint8_t>(TP_ADDR), static_cast<uint8_t>(6)) != 6) {
    return false;
  }
  const uint8_t gesture = static_cast<uint8_t>(Wire.read());  // 0x01
  const uint8_t fingers = static_cast<uint8_t>(Wire.read());  // 0x02
  const uint8_t xh      = static_cast<uint8_t>(Wire.read());  // 0x03
  const uint8_t xl      = static_cast<uint8_t>(Wire.read());  // 0x04
  const uint8_t yh      = static_cast<uint8_t>(Wire.read());  // 0x05
  const uint8_t yl      = static_cast<uint8_t>(Wire.read());  // 0x06
  (void)gesture;
  pressed = (fingers != 0U);
  rawX = static_cast<int16_t>((static_cast<uint16_t>(xh & 0x0FU) << 8) | xl);
  rawY = static_cast<int16_t>((static_cast<uint16_t>(yh & 0x0FU) << 8) | yl);
  return true;
}
// g_orTouchEventCount declared earlier with other §16 state vars.

// =========================================================================
// ROUND 4 — LCD mode + QR screen
// =========================================================================
// When the bot is idle (no WS client connected), the LCD shows a "scan-me"
// screen with two QR codes:
//   1. Dynamic WIFI payload for this activation         — iOS Camera prompts to join
//   2. http://192.168.4.1/?k=<activation token>         — opens the authorized UI
// Once a client connects (WS_EVT_CONNECT), the LCD reverts to the face. On
// disconnect, after a debounce, we go back to QR.
//
// CRITICAL: AsyncWebSocket callbacks fire on the AsyncTCP task, NOT loop().
// They own transport pointers, parse/copy bounded events, and may send only
// the prebuilt atomic telemetry snapshot. All production work stays in loop().
//
// Round 4c: 3-state machine driven by softAPgetStationNum() plus the atomic
// callback-owned transport state:
//   no AP clients          → LCD_MODE_QR_WIFI   (scan to join the current SSID)
//   AP clients, no WS      → LCD_MODE_QR_URL    (scan to open browser)
//   any WS client          → LCD_MODE_FACE      (mascot)
// LcdMode enum + g_lcdMode/g_lcdNextPollMs/g_lcdPendingMode/g_lcdPendingMs/
// kLcdPollIntervalMs are all declared earlier (in the §16 forward-decl
// block near line ~400) so dispatchCommand can reference them.

// QR rendering constants — round-4c: two QR screens (WIFI + URL).
// Wi-Fi QR is version 3 (29 modules) at scale 7 → 203 px square
//   (240-px canvas, 18-px margin ≈ 2.6 modules quiet zone — safe).
// URL QR is version 3 (29 modules) at scale 7 → 203 px square.
constexpr uint16_t kQrWhite          = 0xFFFF;
constexpr uint16_t kQrBlack          = 0x0000;
constexpr uint8_t  kQrWifiPixelScale = 6;
constexpr uint8_t  kQrUrlPixelScale  = 7;
constexpr uint8_t  kQrWifiVersion    = 4;       // 33×33 modules — 67-byte cap; static payload is 45 bytes
constexpr uint8_t  kQrUrlVersion     = 3;       // 29×29 modules — fits the 38-byte tokenized URL
constexpr const char* kAccessUrlPrefix = "http://192.168.4.1/?k=";

static void secureZero(void* data, size_t length) {
  volatile uint8_t* bytes = static_cast<volatile uint8_t*>(data);
  while (length-- > 0U) {
    *bytes++ = 0U;
  }
}

static void generateActivationCredentials() {
  ActivationCredentials credentials = {};
  const uint32_t tokenHigh = esp_random();
  const uint32_t tokenLow = esp_random();

  snprintf(credentials.ssid, sizeof(credentials.ssid), "%s", kOverrideSsid);
  snprintf(credentials.password, sizeof(credentials.password), "%s",
           kOverridePassword);
  snprintf(credentials.accessToken, sizeof(credentials.accessToken),
           "%08lX%08lX",
           static_cast<unsigned long>(tokenHigh),
           static_cast<unsigned long>(tokenLow));
  credentials.valid = true;

  portENTER_CRITICAL(&g_credentialMux);
  secureZero(&g_activationCredentials, sizeof(g_activationCredentials));
  memcpy(&g_activationCredentials, &credentials, sizeof(credentials));
  portEXIT_CRITICAL(&g_credentialMux);
  // Fresh credentials define a fresh activation.
  g_activationTerminalFaulted.store(false, std::memory_order_release);
  secureZero(&credentials, sizeof(credentials));
}

static void invalidateActivationCredentials() {
  portENTER_CRITICAL(&g_credentialMux);
  secureZero(&g_activationCredentials, sizeof(g_activationCredentials));
  portEXIT_CRITICAL(&g_credentialMux);
  // With no valid activation, there is no activation-wide fault to retain.
  g_activationTerminalFaulted.store(false, std::memory_order_release);
}

static bool copyActivationCredentials(ActivationCredentials& out) {
  bool valid = false;
  portENTER_CRITICAL(&g_credentialMux);
  valid = g_activationCredentials.valid;
  if (valid) {
    memcpy(&out, &g_activationCredentials, sizeof(out));
  }
  portEXIT_CRITICAL(&g_credentialMux);
  if (!valid) {
    secureZero(&out, sizeof(out));
  }
  return valid;
}

static bool copyAccessToken(char* out, size_t outSize) {
  if (out == nullptr || outSize < kAccessTokenHexLength + 1U) {
    return false;
  }
  bool valid = false;
  portENTER_CRITICAL(&g_credentialMux);
  valid = g_activationCredentials.valid;
  if (valid) {
    memcpy(out, g_activationCredentials.accessToken,
           sizeof(g_activationCredentials.accessToken));
  }
  portEXIT_CRITICAL(&g_credentialMux);
  if (!valid) {
    secureZero(out, outSize);
  }
  return valid;
}

static bool requestHasValidAccessToken(AsyncWebServerRequest* request) {
  if (request == nullptr) {
    return false;
  }

  AsyncWebParameter* tokenParameter = nullptr;
  size_t tokenCount = 0;
  for (size_t i = 0; i < request->params(); ++i) {
    AsyncWebParameter* parameter = request->getParam(i);
    if (parameter != nullptr && !parameter->isPost() && !parameter->isFile() &&
        parameter->name() == "k") {
      tokenParameter = parameter;
      ++tokenCount;
    }
  }
  if (tokenCount != 1U || tokenParameter == nullptr) {
    return false;
  }

  char expected[kAccessTokenHexLength + 1];
  if (!copyAccessToken(expected, sizeof(expected))) {
    return false;
  }
  const String& supplied = tokenParameter->value();
  uint8_t difference = 0;
  const bool correctLength = supplied.length() == kAccessTokenHexLength;
  // Always run the full compare, even when the length is wrong.
  //
  // Skipping the loop on a length mismatch returns measurably faster, which
  // lets an observer distinguish "wrong length" from "wrong content" by
  // response timing and converge on the token length in fewer probes. Reading
  // past the end of a short string is avoided by indexing a fixed-length local
  // copy rather than the String itself; the mismatch is then folded into
  // `difference` so the result is unchanged.
  char probe[kAccessTokenHexLength];
  for (size_t i = 0; i < kAccessTokenHexLength; ++i) {
    probe[i] = (i < supplied.length()) ? supplied[i] : '\0';
  }
  for (size_t i = 0; i < kAccessTokenHexLength; ++i) {
    difference |= static_cast<uint8_t>(probe[i] ^ expected[i]);
  }
  secureZero(probe, sizeof(probe));
  secureZero(expected, sizeof(expected));
  return correctLength && difference == 0U;
}

static bool buildAccessUrl(char* out, size_t outSize) {
  char token[kAccessTokenHexLength + 1];
  if (!copyAccessToken(token, sizeof(token))) {
    return false;
  }
  const int written =
      snprintf(out, outSize, "%s%s", kAccessUrlPrefix, token);
  secureZero(token, sizeof(token));
  return written > 0 && static_cast<size_t>(written) < outSize;
}

bool takeDriveMutex(TickType_t waitTicks) {
  return (g_driveMutex != nullptr) &&
         (xSemaphoreTake(g_driveMutex, waitTicks) == pdTRUE);
}

void giveDriveMutex() {
  if (g_driveMutex != nullptr) {
    xSemaphoreGive(g_driveMutex);
  }
}

static void driveSignalPinsLow() {
  digitalWrite(kServoLeftPin, LOW);
  digitalWrite(kServoRightPin, LOW);
  pinMode(kServoLeftPin, OUTPUT);
  pinMode(kServoRightPin, OUTPUT);
}

// Caller must hold g_driveMutex.
void powerDownDriveMotorsLocked(const char* reason) {
  const bool wasActive =
      g_driveRailOn || g_servoLeft.attached() || g_servoRight.attached() ||
      g_driveOutputValid;
  // Remove 5V before changing the PWM routing. This prevents a powered servo
  // from seeing a stale LEDC duty or a GPIO-matrix transition.
  setBoostEn(false);
  detachAllServos();
  driveSignalPinsLow();
  g_driveIdleSinceMs = 0;
  g_neutralIdleLatchedOff = false;
  g_driveOutputValid = false;
  g_driveOutputHadState = false;
  g_lastEffectiveDriveL = 0;
  g_lastEffectiveDriveR = 0;
  g_servoLeftRequestedPulseUs = 0;
  g_servoRightRequestedPulseUs = 0;
  g_servoLeftReadbackPulseUs = 0;
  g_servoRightReadbackPulseUs = 0;
  if (wasActive && reason != nullptr) {
    Serial.printf("drive: power off (%s)\n", reason);
  }
}

static void powerDownDriveMotors(const char* reason) {
  if (!takeDriveMutex(portMAX_DELAY)) return;
  powerDownDriveMotorsLocked(reason);
  giveDriveMutex();
}

void lockWsTransportState() {
  portENTER_CRITICAL(&g_wsTransportMux);
}

void unlockWsTransportState() {
  portEXIT_CRITICAL(&g_wsTransportMux);
}

static void requestWsTransportReset() {
  lockWsTransportState();
  g_wsTransportGeneration.fetch_add(1, std::memory_order_acq_rel);
  g_wsTransportOwnerToken.store(0, std::memory_order_release);
  g_wsTransportOwnerClient.store(0, std::memory_order_relaxed);
  g_wsTransportOwnerGeneration.store(0, std::memory_order_relaxed);
  g_wsTransportFaultToken.store(0, std::memory_order_release);
  g_wsTransportFaultGeneration.store(0, std::memory_order_relaxed);
  g_wsTransportOpenCount.store(0U, std::memory_order_release);
  g_wsTransportLastTelemetryMs.store(0, std::memory_order_relaxed);
  // The activation-wide terminal latch intentionally survives transport
  // resets; only credential generation/invalidation clears it.
  unlockWsTransportState();
}

bool wsEventTransportFaulted(const WsEvent& event) {
  bool faulted = false;
  lockWsTransportState();
  faulted =
      event.transportToken != 0U &&
      g_wsTransportFaultToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_wsTransportFaultGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration;
  unlockWsTransportState();
  return faulted;
}

bool faultAndRevokeWsTransport(WsEvent& event) {
  bool faulted = false;
  lockWsTransportState();
  // This function is reached only for a terminal protocol fault. Latch it
  // under the same lock used by admission so a later transport cannot slip
  // into the faulted activation.
  g_activationTerminalFaulted.store(true, std::memory_order_release);
  const bool currentOwner =
      event.transportToken != 0U &&
      event.transportGeneration ==
          g_wsTransportGeneration.load(std::memory_order_relaxed) &&
      g_wsTransportOwnerToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_wsTransportOwnerClient.load(std::memory_order_relaxed) ==
          event.client &&
      g_wsTransportOwnerGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration;
  if (currentOwner) {
    if (g_driveSessionTransportToken.load(std::memory_order_relaxed) ==
            event.transportToken &&
        g_driveSessionTransportGeneration.load(std::memory_order_relaxed) ==
            event.transportGeneration) {
      event.transportFlags |= kWsEventDriveSessionOwner;
    }
    g_wsTransportFaultGeneration.store(
        event.transportGeneration, std::memory_order_relaxed);
    g_wsTransportFaultToken.store(
        event.transportToken, std::memory_order_release);
    g_wsTransportOwnerToken.store(0, std::memory_order_release);
    g_wsTransportOwnerClient.store(0, std::memory_order_relaxed);
    g_wsTransportOwnerGeneration.store(0, std::memory_order_relaxed);
    g_wsTransportLastTelemetryMs.store(0, std::memory_order_relaxed);
    g_wsTransportOpenCount.store(0U, std::memory_order_release);
    faulted = true;
  }
  unlockWsTransportState();
  return faulted;
}

bool wsTransportOwnerConnected() {
  const uintptr_t token =
      g_wsTransportOwnerToken.load(std::memory_order_acquire);
  if (token == 0U) {
    return false;
  }
  const uint32_t generation =
      g_wsTransportGeneration.load(std::memory_order_acquire);
  return g_wsTransportOwnerGeneration.load(std::memory_order_acquire) ==
         generation;
}

bool wsEventMatchesCurrentTransport(const WsEvent& event) {
  bool matches = false;
  lockWsTransportState();
  const uint32_t generation =
      g_wsTransportGeneration.load(std::memory_order_relaxed);
  matches =
      event.transportToken != 0U &&
      event.transportGeneration == generation &&
      g_wsTransportOwnerToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_wsTransportOwnerClient.load(std::memory_order_relaxed) ==
          event.client &&
      g_wsTransportOwnerGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration;
  unlockWsTransportState();
  return matches;
}

bool finalizeDriveSessionTransportAdmission(const WsEvent& event) {
  bool finalized = false;
  lockWsTransportState();
  const uint32_t generation =
      g_wsTransportGeneration.load(std::memory_order_relaxed);
  const bool activationFaulted =
      g_activationTerminalFaulted.load(std::memory_order_relaxed);
  const bool faulted =
      g_wsTransportFaultToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_wsTransportFaultGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration;
  finalized =
      event.transportToken != 0U &&
      event.transportGeneration == generation &&
      !activationFaulted &&
      !faulted &&
      g_wsTransportOwnerToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_wsTransportOwnerClient.load(std::memory_order_relaxed) ==
          event.client &&
      g_wsTransportOwnerGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration &&
      g_wsTransportOpenCount.load(std::memory_order_relaxed) == 1U &&
      g_driveSessionTransportToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_driveSessionTransportGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration;
  if (finalized) {
    g_driveOwnerLossPending.store(false, std::memory_order_release);
  }
  unlockWsTransportState();
  return finalized;
}

bool publishProvisionalDriveSessionTransport(
    const WsEvent& event, DriveSession::Decision& decision) {
  bool transportCurrent = false;
  lockWsTransportState();
  const uint32_t generation =
      g_wsTransportGeneration.load(std::memory_order_relaxed);
  const bool activationFaulted =
      g_activationTerminalFaulted.load(std::memory_order_relaxed);
  const bool faulted =
      g_wsTransportFaultToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_wsTransportFaultGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration;
  const bool currentOwner =
      event.transportToken != 0U &&
      event.transportGeneration == generation &&
      !activationFaulted &&
      !faulted &&
      g_wsTransportOwnerToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_wsTransportOwnerClient.load(std::memory_order_relaxed) ==
          event.client &&
      g_wsTransportOwnerGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration;
  if (currentOwner) {
    decision = g_driveSession.connect(event.client, event.receivedAtMs);
    transportCurrent = true;
    if (decision.reason == DriveSession::Reason::Connected) {
      g_driveSessionTransportGeneration.store(
          event.transportGeneration, std::memory_order_relaxed);
      g_driveSessionTransportToken.store(
          event.transportToken, std::memory_order_release);
    }
  }
  unlockWsTransportState();
  return transportCurrent;
}

void clearDriveSessionTransportIfMatches(const WsEvent& event) {
  lockWsTransportState();
  if (g_driveSessionTransportToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_driveSessionTransportGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration) {
    g_driveSessionTransportToken.store(0, std::memory_order_release);
    g_driveSessionTransportGeneration.store(0, std::memory_order_relaxed);
  }
  unlockWsTransportState();
}

bool driveSessionOwnerMatchesEvent(const WsEvent& event) {
  return g_driveSession.hasOwner() &&
         g_driveSession.owner() == event.client &&
         g_driveSessionTransportToken.load(std::memory_order_acquire) ==
             event.transportToken &&
         g_driveSessionTransportGeneration.load(std::memory_order_acquire) ==
             event.transportGeneration;
}

void rollbackProvisionalDriveAdmission(const WsEvent& event, uint32_t now) {
  if (!driveSessionOwnerMatchesEvent(event)) {
    clearDriveSessionTransportIfMatches(event);
    return;
  }
  const DriveSession::Decision rollback =
      g_driveSession.disconnect(event.client);
  clearDriveSessionTransportIfMatches(event);
  applyDriveDecision(rollback, "ws-admission-rollback", event.client,
                     0, 0, 0, now);
}

// Records that a client's authority was revoked. DOES NOT TOUCH THE CLIENT.
//
// This previously did `g_orWs.client(id)` followed by `close()`, from the LOOP
// task, at seven call sites -- the exact cross-task pattern that corrupted the
// heap (RELY-4/RELY-5). close() reaches AsyncWebSocketClient::_queueControl,
// which does an unsynchronised `_controlQueue.add()` (AsyncWebSocket.cpp:561),
// racing AsyncTCP's own _onData on the same unlocked LinkedList. The observed
// result was a fault inside the allocator's free-list unlink:
//
//     _onData -> _queueControl -> operator new -> tlsf_malloc
//             -> block_locate_free -> remove_free_block   StoreProhibited
//
// Dropping the close costs nothing, for two independent reasons.
//
// 1. Authority is already gone. Every caller revokes through our own atomics
//    BEFORE reaching here: the client is stale, faulted or a non-owner to begin
//    with, or rejectWsTransportAdmission cleared the owner token first. A
//    revoked client cannot drive, cannot be re-admitted, and its frames are
//    ignored on the non-owner path.
//
// 2. The socket still gets closed -- from the correct task. Both telemetry send
//    paths in orOnWsEvent re-check ownership and close on mismatch
//    (main.cpp:8982, 9192). Those run on AsyncTCP, driven by the client's own
//    next frame, so a live phone is dropped within one control tick. A phone
//    that sends nothing is already gone and needs no close. The web app's 4 s
//    telemetry-staleness watchdog is the backstop for anything else.
//
// Rate-limited and FIFO-gated: a client wedged in a revoked state can generate
// these at control-frame rate, and an unbounded Serial.printf on the loop task
// is its own hazard (RELY-6).
void revokeWsEventClient(const WsEvent& event, const char* reason) {
  static uint32_t s_lastRevokeLogMs = 0;
  const uint32_t nowMs = millis();
  if (static_cast<int32_t>(nowMs - s_lastRevokeLogMs) < 1000) {
    return;
  }
  s_lastRevokeLogMs = nowMs;
  if (!serialCanWrite()) {
    return;
  }
  Serial.printf("ws: client #%lu revoked (%s)\n",
                static_cast<unsigned long>(event.client), reason);
}

// ---------------------------------------------------------------------------
// THE ASYNCWEBSOCKET OBJECT BELONGS TO THE ASYNCTCP TASK. DO NOT TOUCH IT
// FROM loop().
//
// AsyncWebSocketClient::_queueControl does an unsynchronised
// `_controlQueue.add()` (AsyncWebSocket.cpp:561, StringArray.h:76). close()
// goes through that same path. So calling close() -- or closeAll(), or
// cleanupClients() -- from the loop task races AsyncTCP's own _onData on the
// identical LinkedList, and two concurrent operator new calls corrupt the heap.
//
// This is not theoretical. A deferred-close queue was added here to move
// closes OFF the AsyncTCP callback, on the theory that closing from inside a
// callback was the cause of an earlier crash. It made things strictly worse
// and produced, on the first phone connect:
//
//   Guru Meditation Error: Core 1 panic'ed (StoreProhibited)
//   ... _async_service_task -> _s_recv -> _recv -> _onData
//       -> _queueControl -> LinkedList::add -> operator new
//
// The library is single-task by design. Closes therefore happen on the
// AsyncTCP task, which is where its callbacks already run. The loop task
// touches only OUR atomics.
void rejectWsTransportAdmission(const WsEvent& event, const char* reason) {
  lockWsTransportState();
  const bool currentOwner =
      event.transportToken != 0U &&
      event.transportGeneration ==
          g_wsTransportGeneration.load(std::memory_order_relaxed) &&
      g_wsTransportOwnerToken.load(std::memory_order_relaxed) ==
          event.transportToken &&
      g_wsTransportOwnerClient.load(std::memory_order_relaxed) ==
          event.client &&
      g_wsTransportOwnerGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration;
  if (currentOwner) {
    g_wsTransportOwnerToken.store(0, std::memory_order_release);
    g_wsTransportOwnerClient.store(0, std::memory_order_relaxed);
    g_wsTransportOwnerGeneration.store(0, std::memory_order_relaxed);
    g_wsTransportLastTelemetryMs.store(0, std::memory_order_relaxed);
    g_wsTransportOpenCount.store(0U, std::memory_order_release);
  }
  unlockWsTransportState();
  revokeWsEventClient(event, reason);
  g_lcdNextPollMs = 0;
}

static void resetDriveSessionAndPower(const char* reason) {
  const DriveSession::Decision decision = g_driveSession.reset();
  g_driveSessionTransportToken.store(0, std::memory_order_release);
  g_driveSessionTransportGeneration.store(0, std::memory_order_release);
  powerDownDriveMotors(reason);
  resetDriveHardwareSafetyLatch();
  Serial.printf("drive session: %s reason=%s gen=%lu\n",
                driveSessionStateName(decision.state),
                reason == nullptr ? "reset" : reason,
                static_cast<unsigned long>(g_driveSession.generation()));
}

bool beginWsIngress() {
  if (g_wsIngressBlocked.load(std::memory_order_acquire)) {
    return false;
  }
  g_wsIngressActive.fetch_add(1, std::memory_order_acq_rel);
  if (g_wsIngressBlocked.load(std::memory_order_acquire)) {
    g_wsIngressActive.fetch_sub(1, std::memory_order_acq_rel);
    return false;
  }
  return true;
}

void endWsIngress() {
  g_wsIngressActive.fetch_sub(1, std::memory_order_acq_rel);
}

class WsIngressGuard {
 public:
  WsIngressGuard() : active_(beginWsIngress()) {}
  ~WsIngressGuard() {
    if (active_) {
      endWsIngress();
    }
  }

  explicit operator bool() const { return active_; }

 private:
  bool active_;
};

void purgeWsEventQueueBlocked() {
  if (g_wsEventQueue != nullptr) {
    xQueueReset(g_wsEventQueue);
  }
}

void beginWsIngressQuarantine() {
  g_wsIngressBlocked.store(true, std::memory_order_release);
  // BOUNDED. This used to spin forever waiting for in-flight AsyncTCP
  // callbacks to drain. If one of those callbacks is itself blocked -- or was
  // entered from the main task, so it can never complete while the main task
  // sits here -- the wait never ends, the loop task stops feeding the
  // watchdog, and TASK_WDT resets the board about five seconds later. That is
  // a deadlock, not a delay, so waiting longer cannot help; proceeding is
  // strictly better than resetting.
  breadcrumb(kCrumbQuarantineSpin);
  const uint32_t deadline = millis() + kIngressDrainTimeoutMs;
  while (g_wsIngressActive.load(std::memory_order_acquire) != 0U) {
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      Serial.printf("ws: ingress drain timed out with %lu active; proceeding\n",
                    static_cast<unsigned long>(
                        g_wsIngressActive.load(std::memory_order_acquire)));
      break;
    }
    delay(1);
  }
  requestWsTransportReset();
  purgeWsEventQueueBlocked();
}

void endWsIngressQuarantine() {
  requestWsTransportReset();
  purgeWsEventQueueBlocked();
  if (g_wsEventQueue == nullptr) {
    latchDriveEventQueueEmergency();
    return;
  }
  g_wsQueueEmergency.store(false, std::memory_order_release);
  g_wsIngressBlocked.store(false, std::memory_order_release);
}

void requestMovementAbort(const char* reason) {
  // Unconditional set. The sequencer may start between this call and the
  // loop's next tick, and a stale-but-set flag costs at most one cancelled
  // gesture, whereas a missed one leaves the wheels turning with no operator.
  // The consumer (serviceMovementGesture) logs the actual cancellation, so
  // nothing is lost by not inspecting sequencer state here — which also keeps
  // this callable from the AsyncTCP task without touching loop-owned objects.
  (void)reason;
  g_movementAbortPending.store(true, std::memory_order_release);
}

static void resetDriveControl(const char* reason) {
  // Cancel any gesture in flight. This CANNOT be inferred from
  // DriveSession::state(): a fault reset lands the session in SafeOff, which is
  // the same state as "idle, nobody connected" -- the state the gesture path
  // must be allowed to run in. Without an explicit signal, a transport fault
  // (MalformedControl / ProtocolFailure / TransportFailure / queue overflow)
  // powers the rail down and then the very next loop tick re-energises it,
  // because serviceMovementGesture writes drive output and each write also
  // refreshes the 300 ms hardware lease. The independent EN cutoff therefore
  // never fires, and the only remaining bound is the 8 s pattern cap.
  //
  // Net effect without this: operator loses the connection mid-`excited` and
  // the robot keeps spinning at full speed for up to 8 s, unattended.
  requestMovementAbort(reason);
  beginWsIngressQuarantine();
  resetDriveSessionAndPower(reason);
  endWsIngressQuarantine();
}

DriveIngressQuarantine::DriveIngressQuarantine(const char* reason)
    : reason_(reason) {
  beginWsIngressQuarantine();
  resetDriveSessionAndPower(reason_);
}

DriveIngressQuarantine::~DriveIngressQuarantine() {
  resetDriveSessionAndPower(reason_);
  endWsIngressQuarantine();
}

// Caller must hold g_driveMutex. If either PWM channel is missing, rebuild
// both while the 5V rail is off so no library/default/stale duty reaches a
// powered motor.
bool ensureDriveChannelsReadyLocked() {
  g_servoLeftAttached =
      servoChannelConfigured(g_servoLeft, g_servoLeftChannel);
  g_servoRightAttached =
      servoChannelConfigured(g_servoRight, g_servoRightChannel);
  if (g_servoLeftAttached && g_servoRightAttached) return true;

  setBoostEn(false);
  detachAllServos();
  driveSignalPinsLow();
  ensureServoAttached(
      g_servoRight, g_servoRightAttached, g_servoRightChannel,
      kServoRightPin, "R");
  ensureServoAttached(
      g_servoLeft, g_servoLeftAttached, g_servoLeftChannel,
      kServoLeftPin, "L");
  g_servoLeftAttached =
      servoChannelConfigured(g_servoLeft, g_servoLeftChannel);
  g_servoRightAttached =
      servoChannelConfigured(g_servoRight, g_servoRightChannel);
  return g_servoLeftAttached && g_servoRightAttached;
}

// RC-transmitter-style expo curve. Applied here so every control source
// (WebSocket today, future UDP / ESP-NOW gamepad) inherits the same feel.
int applyExpo(int t) {
  if (t == 0) return 0;
  const int sign = (t > 0) ? 1 : -1;
  int mag = (t > 0) ? t : -t;
  if (mag > 100) mag = 100;
  const float x = (float)mag / 100.0f;
  const float y = x * (kExpo * x * x + (1.0f - kExpo));
  int out = (int)(y * 100.0f + 0.5f);
  if (out > 100) out = 100;
  return sign * out;
}

bool mapThrottleToPulseUs(
    int throttle, const DriveCalibration::MotorCalibration& motor,
    uint16_t& pulseUs) {
  if (throttle < DriveSession::kCommandMin ||
      throttle > DriveSession::kCommandMax) {
    return false;
  }
  if (throttle == 0) {
    pulseUs = motor.stopUs;
    return pulseUs >= kServoMinPulseUs && pulseUs <= kServoMaxPulseUs;
  }

  const bool forward = throttle > 0;
  const uint16_t startUs =
      forward ? motor.fwdStartUs : motor.revStartUs;
  const uint16_t maximumUs =
      forward ? motor.fwdMaxUs : motor.revMaxUs;
  const uint16_t magnitude =
      static_cast<uint16_t>(throttle > 0 ? throttle : -throttle);
  const int32_t delta =
      static_cast<int32_t>(maximumUs) - static_cast<int32_t>(startUs);
  const int32_t scaled =
      delta * static_cast<int32_t>(magnitude - 1U);
  const int32_t adjustment =
      scaled >= 0 ? (scaled + 49) / 99 : -(((-scaled) + 49) / 99);
  const int32_t mapped = static_cast<int32_t>(startUs) + adjustment;
  if (mapped < kServoMinPulseUs || mapped > kServoMaxPulseUs) {
    return false;
  }
  pulseUs = static_cast<uint16_t>(mapped);
  return true;
}

// This is the only production location that writes servo PWM pulses.
// Caller must hold g_driveMutex.
bool writeDriveOutputLocked(const char* source, uint32_t client, uint8_t seq,
                            int rawLeft, int rawRight, uint32_t now) {
  const auto ingressEmergencyLatched = []() {
    return g_wsQueueEmergency.load(std::memory_order_acquire) ||
           g_wsIngressBlocked.load(std::memory_order_acquire) ||
           g_driveHardwareInhibit.load(std::memory_order_acquire) ||
           g_driveHardwareTripPending.load(std::memory_order_acquire);
  };

  if (ingressEmergencyLatched()) {
    powerDownDriveMotorsLocked("drive ingress emergency before output");
    return false;
  }

  g_lastRawDriveL = rawLeft;
  g_lastRawDriveR = rawRight;
  g_lastDriveSource = source == nullptr ? "unknown" : source;
  g_lastDriveClient = client;
  g_lastDriveSeq = seq;

  if (g_systemShuttingDown || g_paused ||
      !g_driveCalibration.eligible() ||
      rawLeft < DriveSession::kCommandMin ||
      rawLeft > DriveSession::kCommandMax ||
      rawRight < DriveSession::kCommandMin ||
      rawRight > DriveSession::kCommandMax) {
    powerDownDriveMotorsLocked("canonical output rejected");
    return false;
  }

  const DriveCalibration::Snapshot calibration = g_driveCalibration.snapshot();
  if (calibrationValidationError(calibration) != nullptr) {
    powerDownDriveMotorsLocked("canonical calibration invalid");
    return false;
  }

  const int effectiveLeft = applyExpo(rawLeft);
  const int effectiveRight = applyExpo(rawRight);
  uint16_t leftPulseUs = 0;
  uint16_t rightPulseUs = 0;
  if (!mapThrottleToPulseUs(
          effectiveLeft, calibration.left, leftPulseUs) ||
      !mapThrottleToPulseUs(
          effectiveRight, calibration.right, rightPulseUs) ||
      leftPulseUs == 0U || rightPulseUs == 0U) {
    powerDownDriveMotorsLocked("canonical mapping out of bounds");
    return false;
  }

  const bool neutral = rawLeft == 0 && rawRight == 0;
  g_lastEffectiveDriveL = effectiveLeft;
  g_lastEffectiveDriveR = effectiveRight;
  if (neutral && g_neutralIdleLatchedOff && !g_driveRailOn) {
    return true;
  }

  const bool pulsesChanged =
      !g_driveOutputValid ||
      leftPulseUs != g_servoLeftRequestedPulseUs ||
      rightPulseUs != g_servoRightRequestedPulseUs;
  const bool neutralTransition =
      !g_driveOutputHadState || neutral != g_driveOutputWasNeutral;
  const bool channelsReady =
      servoChannelConfigured(g_servoLeft, g_servoLeftChannel) &&
      servoChannelConfigured(g_servoRight, g_servoRightChannel);
  if (!pulsesChanged && !neutralTransition && g_driveOutputValid &&
      g_driveRailOn && channelsReady) {
    if (neutral) {
      if (g_driveIdleSinceMs == 0U) g_driveIdleSinceMs = now;
    } else {
      g_driveIdleSinceMs = 0;
      g_neutralIdleLatchedOff = false;
    }
    if (ingressEmergencyLatched()) {
      powerDownDriveMotorsLocked(
          "drive ingress emergency before output reuse");
      return false;
    }
    if (!refreshDriveHardwareLease()) {
      powerDownDriveMotorsLocked("drive hardware lease refresh failed");
      return false;
    }
    return true;
  }

  const bool coldStart =
      !g_driveRailOn || !channelsReady || !g_driveOutputValid;
  if (coldStart) {
    const bool railWasOn =
        g_driveRailOn.load(std::memory_order_acquire);
    setBoostEn(false);
    if (railWasOn) {
      delay(kServoRailDischargeMs);
    }
    if (!ensureDriveChannelsReadyLocked()) {
      powerDownDriveMotorsLocked("servo attach verification failed");
      return false;
    }
  }
  if (ingressEmergencyLatched()) {
    powerDownDriveMotorsLocked("drive ingress emergency before PWM write");
    return false;
  }
  g_servoRight.writeMicroseconds(rightPulseUs);
  g_servoLeft.writeMicroseconds(leftPulseUs);
  if (ingressEmergencyLatched()) {
    powerDownDriveMotorsLocked("drive ingress emergency after PWM pair");
    return false;
  }
  if (coldStart) {
    delay(kServoPwmCommitMs);
  }
  if (ingressEmergencyLatched()) {
    powerDownDriveMotorsLocked("drive emergency during PWM commit");
    return false;
  }
  if (!servoChannelConfigured(g_servoRight, g_servoRightChannel) ||
      !servoChannelConfigured(g_servoLeft, g_servoLeftChannel)) {
    powerDownDriveMotorsLocked("servo detached during canonical write");
    return false;
  }
  const int rightReadbackPulseUs = g_servoRight.readMicroseconds();
  const int leftReadbackPulseUs = g_servoLeft.readMicroseconds();
  if (!servoReadbackMatchesRequest(
          rightPulseUs, rightReadbackPulseUs) ||
      !servoReadbackMatchesRequest(
          leftPulseUs, leftReadbackPulseUs)) {
    powerDownDriveMotorsLocked("servo pulse readback failed");
    return false;
  }
  if (coldStart &&
      (!servoPwmHardwareMatches(
           g_servoRight, g_servoRightChannel) ||
       !servoPwmHardwareMatches(
           g_servoLeft, g_servoLeftChannel))) {
    powerDownDriveMotorsLocked("servo PWM hardware verification failed");
    return false;
  }
  if (!refreshDriveHardwareLease()) {
    powerDownDriveMotorsLocked("drive hardware lease refresh failed");
    return false;
  }
  if (coldStart && !setBoostEn(true)) {
    powerDownDriveMotorsLocked("drive rail enable inhibited");
    return false;
  }

  g_servoRightAttached = true;
  g_servoLeftAttached = true;
  g_servoRightRequestedPulseUs = rightPulseUs;
  g_servoLeftRequestedPulseUs = leftPulseUs;
  g_servoRightReadbackPulseUs =
      static_cast<uint16_t>(rightReadbackPulseUs);
  g_servoLeftReadbackPulseUs =
      static_cast<uint16_t>(leftReadbackPulseUs);
  g_driveOutputValid = true;
  g_driveOutputHadState = true;
  g_driveOutputWasNeutral = neutral;
  if (neutral) {
    if (g_driveIdleSinceMs == 0U) g_driveIdleSinceMs = now;
  } else {
    g_driveIdleSinceMs = 0;
    g_neutralIdleLatchedOff = false;
  }
  if (ingressEmergencyLatched()) {
    powerDownDriveMotorsLocked("drive emergency after output apply");
    return false;
  }

  if (pulsesChanged || neutralTransition) {
    Serial.printf(
        "drive output: requestedUs=(%u,%u) readbackUs=(%u,%u) neutral=%u coldStart=%u calGen=%lu "
        "source=%s client=%lu seq=%u rail=%u raw=(%d,%d) effective=(%d,%d)\n",
        static_cast<unsigned>(leftPulseUs),
        static_cast<unsigned>(rightPulseUs),
        static_cast<unsigned>(g_servoLeftReadbackPulseUs),
        static_cast<unsigned>(g_servoRightReadbackPulseUs),
        neutral ? 1U : 0U,
        coldStart ? 1U : 0U,
        static_cast<unsigned long>(calibration.generation),
        g_lastDriveSource,
        static_cast<unsigned long>(client),
        static_cast<unsigned>(seq),
        g_driveRailOn ? 1U : 0U,
        rawLeft, rawRight, effectiveLeft, effectiveRight);
  }
  return true;
}

void applyDriveDecision(const DriveSession::Decision& decision,
                        const char* source, uint32_t client, uint8_t seq,
                        int rawLeft, int rawRight, uint32_t now) {
  if (decision.stateChanged) {
    Serial.printf("drive session: state=%s reason=%s owner=%lu gen=%lu\n",
                  driveSessionStateName(decision.state),
                  driveReasonName(decision.reason),
                  static_cast<unsigned long>(g_driveSession.owner()),
                  static_cast<unsigned long>(g_driveSession.generation()));
  }

  if (decision.action == DriveSession::Action::None) {
    return;
  }
  if (decision.action == DriveSession::Action::PowerDown) {
    powerDownDriveMotors(driveReasonName(decision.reason));
    return;
  }

  if (!takeDriveMutex(portMAX_DELAY)) {
    resetDriveControl("drive mutex unavailable");
    return;
  }
  const int outputLeft =
      decision.action == DriveSession::Action::Neutral ? 0 : decision.l;
  const int outputRight =
      decision.action == DriveSession::Action::Neutral ? 0 : decision.r;
  const bool success = writeDriveOutputLocked(
      source, client, seq, outputLeft, outputRight, now);
  giveDriveMutex();
  if (!success) {
    resetDriveControl("canonical drive output failed");
  }
}

// =========================================================================
// Emotional movement gestures.
//
// A locally sequenced movement source that feeds the SAME canonical drive
// output the WebSocket joystick uses, so it inherits every safety check
// already in place: emergency latches, pause, calibration eligibility, range
// clamping, the independent 300 ms hardware lease and the 1 s neutral idle
// power-down. Nothing here talks to the servos directly.
//
// Patterns are net-zero by construction (see movement_expression.h), so a
// gesture leaves the robot where it found it.
// =========================================================================
MovementExpression::Sequencer g_movementSequencer;
uint32_t g_movementLastTickMs = 0;
constexpr uint32_t kMovementTickMs = MovementExpression::kStepQuantumMs;

// Set when a measurement spin is stopped by anything other than reaching its
// own end, so a partial integral can never become a calibration. Declared here
// because stopMovementGesture() below is what raises it.
bool g_turnAutoAborted = false;

// On-device calibration driver, so the whole converge-and-save loop can run
// with the robot unplugged. The screen is the only feedback in that state.
bool g_calibActive = false;
uint8_t g_calibIteration = 0;
bool g_calibClockwise = true;
bool g_calibPendingNext = false;
uint32_t g_calibNextAtMs = 0;
bool g_calibStatusDirty = false;
char g_calibStatus[40] = {0};
constexpr uint8_t kCalibMaxIterations = 6;
// Long enough for the chassis to stop rocking before the next bias sample.
constexpr uint32_t kCalibGapMs = 900;

void setCalibStatus(const char* text) {
  snprintf(g_calibStatus, sizeof(g_calibStatus), "%s", text);
  g_calibStatusDirty = true;
  Serial.printf("calib: %s\n", g_calibStatus);
}

void stopMovementGesture(const char* reason) {
  if (!g_movementSequencer.active()) return;
  const MovementExpression::Pattern* running = g_movementSequencer.pattern();
  const char* label = (running != nullptr &&
                       strcmp(running->name, "spin") == 0)
                          ? "spin"
                          : "gesture";
  // Reaching the end of a pattern does not come through here, so anything that
  // does is an interruption as far as a measurement is concerned.
  if (running != nullptr && strcmp(running->name, "spin") == 0) {
    g_turnAutoAborted = true;
  }
  g_movementSequencer.stop();
  // Power down explicitly. Writing neutral alone leaves the rail live, and
  // with nothing refreshing the lease the independent 300 ms cutoff fires and
  // logs an emergency — the safety net doing its job, but for a gesture that
  // ended normally. That would pollute the trip counter used to detect real
  // faults.
  powerDownDriveMotors(reason == nullptr ? "gesture stop" : reason);
  if (reason != nullptr) {
    Serial.printf("%s: stopped (%s)\n", label, reason);
  }
}

bool startMovementGesture(const char* name, uint16_t runMs) {
  const MovementExpression::Pattern* pattern =
      MovementExpression::patternForName(name);
  if (pattern == nullptr) {
    return false;
  }

  // Frustrated is built at runtime so its swing length can be tuned live.
  // The storage is static because the sequencer holds a pointer to it for the
  // life of the gesture.
  static MovementExpression::Step
      s_frustratedSteps[MovementExpression::kFrustratedStepCap];
  static MovementExpression::Pattern s_frustratedPattern;
  // Excited is built at runtime for a different reason: its bookend turns must
  // track the measured full turn, so `spin save confirm` takes effect without
  // a reflash. The static table is only the uncalibrated fallback.
  static MovementExpression::Step
      s_excitedSteps[MovementExpression::kExcitedStepCap];
  static MovementExpression::Pattern s_excitedPattern;

  if (strcmp(pattern->name, "frustrated") == 0) {
    const uint16_t run =
        runMs == 0 ? MovementExpression::kFrustratedDefaultRunMs : runMs;
    if (!MovementExpression::buildFrustrated(
            run, s_frustratedSteps,
            static_cast<uint8_t>(MovementExpression::kFrustratedStepCap),
            s_frustratedPattern)) {
      Serial.printf("ERR run length must be %u..%u ms in steps of %u\n",
                    MovementExpression::kFrustratedMinRunMs,
                    MovementExpression::kFrustratedMaxRunMs,
                    MovementExpression::kStepQuantumMs);
      return false;
    }
    pattern = &s_frustratedPattern;
  } else if (runMs != 0) {
    Serial.println(F("ERR only frustrated takes a run length"));
    return false;
  } else if (strcmp(pattern->name, "excited") == 0) {
    const uint16_t turn = g_spinCalibration.fullTurnMs();
    if (!MovementExpression::buildExcited(
            turn, s_excitedSteps,
            static_cast<uint8_t>(MovementExpression::kExcitedStepCap),
            s_excitedPattern)) {
      // Only reachable if the stored turn is out of range, which the store
      // already rejects on read — fall back rather than refusing to move.
      Serial.printf("WARN excited build failed at %u ms; using default table\n",
                    turn);
    } else {
      pattern = &s_excitedPattern;
    }
  }

  // Refuse rather than half-perform: an uncalibrated or paused robot would
  // have every frame rejected downstream and just sit there twitching.
  if (!g_driveCalibration.eligible()) {
    Serial.println(F("ERR gesture needs an eligible calibration"));
    return false;
  }
  if (g_paused || g_systemShuttingDown) {
    Serial.println(F("ERR gesture unavailable while paused"));
    return false;
  }
  const uint32_t now = millis();
  // Consume any stale abort request before starting.
  //
  // requestMovementAbort() sets the flag unconditionally, including when no
  // gesture is running -- e.g. every AP teardown calls resetDriveControl().
  // Without this clear the flag survives until the NEXT gesture starts and
  // cancels it on its first tick, so gestures appear broken after any drive
  // reset. Caught on hardware: `swipe up` then `gesture frustrated` produced
  // "gesture: stopped (drive control reset)" instead of running.
  //
  // Clearing here is safe because it happens before the sequencer starts: an
  // abort raised from this point on is genuinely about THIS gesture.
  g_movementAbortPending.store(false, std::memory_order_release);
  if (!g_movementSequencer.start(pattern, now)) {
    Serial.println(F("ERR gesture pattern failed validation"));
    return false;
  }
  g_movementLastTickMs = now;
  // Movement only. A movement button moves; it does not play a melody.
  //
  // This used to also fire playMood() with an "accompanying mood" mapped from
  // the pattern name, on the reasoning that "an expression is movement AND
  // sound". That was never asked for. The web app has three separate button
  // groups -- Faces, Sounds, Moves -- and each one does exactly its own thing.
  // Sound is triggered by OP_SOUND from the Sounds sheet.
  Serial.printf("gesture: %s %lums\n", pattern->name,
                static_cast<unsigned long>(
                    MovementExpression::totalDurationMs(*pattern)));
  return true;
}

// Runs one measurement turn. Deliberately NOT net-zero: it rotates one way so
// the result can be observed. It still pivots in place, and it goes through the
// same sequencer and canonical output as a gesture, so it inherits the drive
// mutex, the 300 ms lease, the emergency latch and the clean powerdown.
bool startSpinCalibration(uint16_t fullTurnMs, bool clockwise, int8_t speed) {
  static MovementExpression::Step
      s_spinSteps[MovementExpression::kSpinStepCap];
  static MovementExpression::Pattern s_spinPattern;

  if (!MovementExpression::buildSpin(
          fullTurnMs, clockwise, speed, s_spinSteps,
          static_cast<uint8_t>(MovementExpression::kSpinStepCap),
          s_spinPattern)) {
    Serial.printf("ERR turn %u..%u ms in steps of %u, speed %d..%d\n",
                  MovementExpression::kSpinMinMs,
                  MovementExpression::kSpinMaxMs,
                  MovementExpression::kStepQuantumMs,
                  static_cast<int>(MovementExpression::kSpinMinSpeed),
                  static_cast<int>(MovementExpression::kCommandMax));
    return false;
  }

  if (!g_driveCalibration.eligible()) {
    Serial.println(F("ERR spin needs an eligible calibration"));
    return false;
  }
  if (g_paused || g_systemShuttingDown) {
    Serial.println(F("ERR spin unavailable while paused"));
    return false;
  }
  if (g_driveSession.state() != DriveSession::State::SafeOff) {
    Serial.println(F("ERR spin unavailable while driving"));
    return false;
  }

  const uint32_t now = millis();
  if (!g_movementSequencer.start(&s_spinPattern, now,
                                 MovementExpression::Balance::Unbalanced)) {
    Serial.println(F("ERR spin pattern failed validation"));
    return false;
  }
  g_movementLastTickMs = now;
  Serial.printf("spin: %u ms %s at %d\n", fullTurnMs,
                clockwise ? "cw" : "ccw", static_cast<int>(speed));
  return true;
}

// =========================================================================
// Gyro-measured turn calibration.
//
// The board carries a QMI8658C — a 6-axis IMU, so there is no compass, but a
// gyroscope is the better instrument for this anyway: it measures angular RATE
// directly, where a magnetometer sitting inches from two DC motors and a LiPo
// under load would be reading their fields as much as the Earth's.
//
// This replaces "run a spin, squint at it, guess a better number" with a
// measurement. The arithmetic all lives in turn_measure.{h,cpp} and is
// unit-tested on the host; this is only the sequencing.
// =========================================================================
enum class TurnAutoPhase : uint8_t {
  Idle,
  Spinning,
  Settling,
};

// Calibration runs as a characterisation, not a search.
//
// PROBE the chassis at two wheel commands and measure the STEADY yaw rate at
// each — excluding the acceleration ramp, which is what makes a rate a
// property of the chassis rather than of the particular duration used. Two
// points give rate(speed) = slope*speed + intercept (the intercept matters:
// continuous-rotation servos have a deadband, so the line does not pass
// through the origin). From that, the duration for a full turn is computed
// directly rather than converged on. VERIFY runs the prediction once and
// applies a single scale correction, which is enough for a ballpark.
enum class CalibStage : uint8_t {
  Off,
  ProbeSlow,
  ProbeFast,
  Verify,
};

constexpr int8_t kCalibSlowSpeed = 55;
constexpr int8_t kCalibFastSpeed = MovementExpression::kCommandMax;
// Long enough that the steady window still holds plenty of samples after the
// ramp is discarded.
constexpr uint16_t kCalibProbeMs = 1400;
// Discarded from the front of each probe: the standing-start ramp.
constexpr uint32_t kCalibRampUs = 400000;
// Assumed ramp length when converting a rate into a duration.
constexpr uint16_t kCalibRampMs = 400;

TurnAutoPhase g_turnAutoPhase = TurnAutoPhase::Idle;
// All three axes are integrated, not just the one gravity picked. A wrong axis
// choice and a genuinely slow chassis both look like "a small number", and the
// only way to tell them apart is to see what the other two axes did.
TurnMeasure::Integrator g_turnIntegrators[3];
TurnMeasure::WindowedRate g_turnSteadyRate;
// g_turnYawAxis and g_turnBias are declared with the heading state above,
// because the serial command handler reads them too.

CalibStage g_calibStage = CalibStage::Off;
TurnMeasure::RateSample g_calibProbes[2];
uint8_t g_calibProbeCount = 0;
TurnMeasure::RateModel g_calibModel{};
int8_t g_turnAutoSpeed = MovementExpression::kSpinSpeed;
uint16_t g_turnAutoTestedMs = 0;
uint32_t g_turnAutoSettleUntilMs = 0;

// The rotation continues briefly after the command stops, and where the robot
// finally comes to rest is what a person judges as "did it turn all the way".
// So the coast is measured rather than cut off at the last commanded step.
constexpr uint32_t kTurnSettleMs = 250;

bool readAllAxisRates(float dps[3], bool& saturated) {
  ImuQmi8658::Sample sample{};
  if (!ImuQmi8658::readGyroRaw(sample)) return false;
  const int16_t raw[3] = {sample.x, sample.y, sample.z};
  saturated = false;
  for (int i = 0; i < 3; ++i) {
    if (TurnMeasure::isSaturated(raw[i])) saturated = true;
    dps[i] = ImuQmi8658::gyroDps(raw[i]) - g_turnBias[i];
  }
  return true;
}

// Same read, but also returns the gravity-projected yaw rate — the value any
// decision should be made on. The per-axis values remain for diagnostics.
bool readAxisRatesAndYaw(float dps[3], float& yawDps, bool& saturated) {
  ImuQmi8658::Sample sample{};
  if (!ImuQmi8658::readGyroRaw(sample)) return false;
  const int16_t raw[3] = {sample.x, sample.y, sample.z};
  saturated = false;
  for (int i = 0; i < 3; ++i) {
    if (TurnMeasure::isSaturated(raw[i])) saturated = true;
    dps[i] = ImuQmi8658::gyroDps(raw[i]) - g_turnBias[i];
  }
  yawDps = projectedYawRate(sample);
  return true;
}

// Blocking, but only ever while the robot is deliberately stationary and the
// motor rail is off, so there is nothing to starve.
bool measureYawAxisAndBias() {
  ImuQmi8658::Sample accel{};
  if (!ImuQmi8658::readAccelRaw(accel)) {
    Serial.println(F("ERR imu accelerometer read failed"));
    return false;
  }
  const float gx = ImuQmi8658::accelG(accel.x);
  const float gy = ImuQmi8658::accelG(accel.y);
  const float gz = ImuQmi8658::accelG(accel.z);
  g_turnGravity[0] = gx;
  g_turnGravity[1] = gy;
  g_turnGravity[2] = gz;
  g_turnYawAxis = TurnMeasure::yawAxisFromGravity(gx, gy, gz);
  g_turnAxisFraction =
      TurnMeasure::singleAxisYawFraction(g_turnYawAxis, gx, gy, gz);

  TurnMeasure::BiasEstimator bias[3];
  const uint32_t until = millis() + 400U;
  while (static_cast<int32_t>(millis() - until) < 0) {
    ImuQmi8658::Sample gyro{};
    if (ImuQmi8658::readGyroRaw(gyro)) {
      bias[0].addSample(ImuQmi8658::gyroDps(gyro.x));
      bias[1].addSample(ImuQmi8658::gyroDps(gyro.y));
      bias[2].addSample(ImuQmi8658::gyroDps(gyro.z));
    }
    delay(2);
  }
  if (!bias[0].ready()) {
    Serial.println(F("ERR imu gyro produced too few samples for bias"));
    return false;
  }

  // A bias estimate is only meaningful if the robot was actually still. Taken
  // while it is coasting after a turn, the coast gets baked in as "zero" and
  // silently corrupts every later measurement. Reject rather than absorb it.
  const float candidate = bias[static_cast<int>(g_turnYawAxis)].bias();
  if (candidate > kHeadingStationaryDps || candidate < -kHeadingStationaryDps) {
    Serial.printf(
        "ERR robot not still for bias (%.2f dps on %s); let it settle\n",
        static_cast<double>(candidate), TurnMeasure::axisName(g_turnYawAxis));
    return false;
  }

  for (int i = 0; i < 3; ++i) g_turnBias[i] = bias[i].bias();

  // Printing the resting gravity vector makes the axis choice checkable rather
  // than something to take on faith.
  Serial.printf(
      "spin auto: rest g=(%.2f,%.2f,%.2f) -> yaw axis %s; bias=(%.2f,%.2f,"
      "%.2f) dps over %lu samples\n",
      static_cast<double>(gx), static_cast<double>(gy),
      static_cast<double>(gz), TurnMeasure::axisName(g_turnYawAxis),
      static_cast<double>(g_turnBias[0]), static_cast<double>(g_turnBias[1]),
      static_cast<double>(g_turnBias[2]),
      static_cast<unsigned long>(bias[0].sampleCount()));
  return true;
}

// Handles one completed probe: record its steady rate, then either run the
// second probe or fit the model and predict.
void advanceCalibrationStage(float steadyDps) {
  if (g_calibProbeCount < 2) {
    g_calibProbes[g_calibProbeCount].speed =
        (g_calibStage == CalibStage::ProbeSlow) ? kCalibSlowSpeed
                                                : kCalibFastSpeed;
    g_calibProbes[g_calibProbeCount].dps = steadyDps;
    ++g_calibProbeCount;
  }

  char message[40];
  if (g_calibStage == CalibStage::ProbeSlow) {
    snprintf(message, sizeof(message), "slow: %d dps  probing fast",
             static_cast<int>(steadyDps < 0 ? -steadyDps : steadyDps));
    setCalibStatus(message);
    g_calibStage = CalibStage::ProbeFast;
    g_calibClockwise = !g_calibClockwise;
    g_calibPendingNext = true;
    g_calibNextAtMs = millis() + kCalibGapMs;
    return;
  }

  // Both probes in: fit and predict.
  g_calibModel = TurnMeasure::fitRateModel(g_calibProbes, g_calibProbeCount);
  if (!g_calibModel.valid) {
    g_calibActive = false;
    g_calibStage = CalibStage::Off;
    // The signature of a restrained or slipping robot: more wheel speed did
    // not produce more rotation, so there is nothing to extrapolate from.
    setCalibStatus("FAILED: speed didnt change rate");
    return;
  }

  const float fastRate = TurnMeasure::predictRateDps(g_calibModel,
                                                     kCalibFastSpeed);
  const uint16_t predicted = TurnMeasure::predictDurationMs(
      g_calibModel, kCalibFastSpeed, 360.0f, kCalibRampMs);
  Serial.printf(
      "calib: model rate=%.2f*speed%+.1f dps; at %d -> %.0f dps; 360 needs "
      "%u ms\n",
      static_cast<double>(g_calibModel.slope),
      static_cast<double>(g_calibModel.intercept),
      static_cast<int>(kCalibFastSpeed), static_cast<double>(fastRate),
      predicted);

  if (predicted == 0 ||
      !MovementExpression::isValidFullTurnMs(predicted)) {
    g_calibActive = false;
    g_calibStage = CalibStage::Off;
    // Report the number rather than clamping to the cap: knowing the chassis
    // needs 2900 ms is actionable, "2000" pretending to have converged is not.
    const float reachable =
        fastRate * (static_cast<float>(MovementExpression::kSpinMaxMs) /
                    1000.0f);
    snprintf(message, sizeof(message), "360 needs %ums; max %udeg", predicted,
             static_cast<unsigned>(reachable));
    setCalibStatus(message);
    return;
  }

  if (!g_spinCalibration.stage(predicted)) {
    g_calibActive = false;
    g_calibStage = CalibStage::Off;
    setCalibStatus("FAILED: prediction rejected");
    return;
  }

  snprintf(message, sizeof(message), "predict %u ms, verifying", predicted);
  setCalibStatus(message);
  g_calibStage = CalibStage::Verify;
  g_calibClockwise = !g_calibClockwise;
  g_calibPendingNext = true;
  g_calibNextAtMs = millis() + kCalibGapMs;
}

void finishTurnAutoMeasure() {
  g_turnAutoPhase = TurnAutoPhase::Idle;

  const int yawIndex = static_cast<int>(g_turnYawAxis);
  TurnMeasure::Integrator& yaw = g_turnIntegrators[yawIndex];
  const float turnedDegrees = yaw.totalDegrees();

  // All three axes, so the axis choice is verifiable from the output instead
  // of being an assumption buried in the firmware.
  Serial.printf(
      "spin auto: rotation X=%.1f Y=%.1f Z=%.1f deg (yaw=%s)\n",
      static_cast<double>(g_turnIntegrators[0].totalDegrees()),
      static_cast<double>(g_turnIntegrators[1].totalDegrees()),
      static_cast<double>(g_turnIntegrators[2].totalDegrees()),
      TurnMeasure::axisName(g_turnYawAxis));
  Serial.printf(
      "spin auto: turned %.1f deg in %u ms (%lu samples, mean %.1f ms, worst "
      "gap %.1f ms)\n",
      static_cast<double>(turnedDegrees), g_turnAutoTestedMs,
      static_cast<unsigned long>(yaw.sampleCount()),
      static_cast<double>(yaw.meanIntervalUs() / 1000.0f),
      static_cast<double>(yaw.worstGapUs() / 1000.0f));

  // If some other axis rotated more than the one gravity picked, then either
  // the robot is not pivoting about vertical or the pick is wrong. Either way
  // the number is not a turn measurement and must not be presented as one.
  int dominant = 0;
  for (int i = 1; i < 3; ++i) {
    const float best = g_turnIntegrators[dominant].totalDegrees();
    const float here = g_turnIntegrators[i].totalDegrees();
    if ((here < 0 ? -here : here) > (best < 0 ? -best : best)) dominant = i;
  }
  if (dominant != yawIndex) {
    Serial.printf(
        "ERR %s rotated more than the chosen yaw axis %s; is the robot "
        "pivoting flat?\n",
        TurnMeasure::axisName(static_cast<TurnMeasure::Axis>(dominant)),
        TurnMeasure::axisName(g_turnYawAxis));
    if (g_calibActive) {
      g_calibActive = false;
      setCalibStatus("FAILED: not pivoting flat");
    }
    return;
  }

  if (g_turnAutoAborted) {
    Serial.println(F("ERR spin was interrupted; measurement rejected"));
    if (g_calibActive) {
      g_calibActive = false;
      setCalibStatus("cancelled");
    }
    return;
  }
  if (!TurnMeasure::spanCoversCommandedSpin(yaw.spanUs(),
                                            g_turnAutoTestedMs)) {
    // Belt and braces: an emergency path can return before the abort flag is
    // ever set, and a truncated integral is indistinguishable from a good one
    // by value alone.
    Serial.println(F("ERR measurement span too short; rejected"));
    if (g_calibActive) {
      g_calibActive = false;
      setCalibStatus("FAILED: spin cut short");
    }
    return;
  }
  if (yaw.saturated()) {
    // Clipping under-reads while still looking like data, so this must never
    // be quietly turned into a calibration.
    Serial.println(F("ERR gyro saturated; measurement rejected"));
    if (g_calibActive) {
      g_calibActive = false;
      setCalibStatus("FAILED: gyro saturated");
    }
    return;
  }
  if (!yaw.usable()) {
    Serial.println(F("ERR too few gyro samples; measurement rejected"));
    if (g_calibActive) {
      g_calibActive = false;
      char message[40];
      // The count itself, not just "too few": unplugged there is no serial to
      // fall back on, and the number is what distinguishes a stalled main loop
      // from a dead sensor.
      snprintf(message, sizeof(message), "FAILED: only %lu samples",
               static_cast<unsigned long>(yaw.sampleCount()));
      setCalibStatus(message);
    }
    return;
  }

  // A probe is not trying to reach 360 — it is measuring the steady rate the
  // chassis holds at a given wheel command. Hand it to the model and stop.
  if (g_calibActive && (g_calibStage == CalibStage::ProbeSlow ||
                        g_calibStage == CalibStage::ProbeFast)) {
    if (!g_turnSteadyRate.ready()) {
      g_calibActive = false;
      g_calibStage = CalibStage::Off;
      char message[40];
      snprintf(message, sizeof(message), "FAILED: %lu steady samples",
               static_cast<unsigned long>(g_turnSteadyRate.sampleCount()));
      setCalibStatus(message);
      return;
    }
    const float steady = g_turnSteadyRate.meanDps();
    Serial.printf("calib: probe at %d -> steady %.1f dps (%lu samples)\n",
                  static_cast<int>(g_turnAutoSpeed),
                  static_cast<double>(steady),
                  static_cast<unsigned long>(g_turnSteadyRate.sampleCount()));
    advanceCalibrationStage(steady);
    return;
  }

  const TurnMeasure::Correction correction =
      TurnMeasure::correctTurn(g_turnAutoTestedMs, turnedDegrees);
  if (correction.ms == 0) {
    Serial.println(F("ERR measurement cannot justify a correction"));
    if (g_calibActive) {
      g_calibActive = false;
      g_calibStage = CalibStage::Off;
      char message[40];
      snprintf(message, sizeof(message), "FAILED: only %d deg",
               static_cast<int>(turnedDegrees));
      setCalibStatus(message);
    }
    return;
  }
  if (correction.clamped) {
    // Pinning at a range limit returns the same value every time, which reads
    // as convergence while actually meaning the chassis never got close.
    const double needed =
        static_cast<double>(g_turnAutoTestedMs) * 360.0 /
        static_cast<double>(turnedDegrees < 0 ? -turnedDegrees
                                              : turnedDegrees);
    Serial.printf(
        "ERR a full turn needs ~%.0f ms, outside the %u..%u ms range; nothing "
        "staged. Chassis is not completing turns (tether? slipping?)\n",
        needed, MovementExpression::kSpinMinMs,
        MovementExpression::kSpinMaxMs);
    if (g_calibActive) {
      g_calibActive = false;
      g_calibStage = CalibStage::Off;
      char message[40];
      snprintf(message, sizeof(message), "FAILED: needs ~%dms, too slow",
               static_cast<int>(needed));
      setCalibStatus(message);
    }
    return;
  }

  const uint16_t corrected = correction.ms;
  if (!g_spinCalibration.stage(corrected)) {
    Serial.println(F("ERR corrected value rejected by the store"));
    if (g_calibActive) {
      g_calibActive = false;
      g_calibStage = CalibStage::Off;
      setCalibStatus("FAILED: value rejected");
    }
    return;
  }

  // The verify pass applies one scale correction to the prediction and stops.
  // A ballpark is the goal, so a second guessing loop would add spins without
  // adding accuracy the gesture can use.
  if (g_calibActive && g_calibStage == CalibStage::Verify) {
    g_calibActive = false;
    g_calibStage = CalibStage::Off;
    char message[40];
    if (g_spinCalibration.save()) {
      snprintf(message, sizeof(message), "SAVED  360 = %u ms", corrected);
    } else {
      snprintf(message, sizeof(message), "measured %u ms, SAVE FAILED",
               corrected);
    }
    setCalibStatus(message);
    Serial.println(F("OK"));
    return;
  }

  if (corrected == g_turnAutoTestedMs) {
    Serial.printf("spin auto: converged at %u ms\n", corrected);
    Serial.println(F("spin auto: `spin save confirm` to persist"));
    Serial.println(F("OK"));
    return;
  }

  Serial.printf(
      "spin auto: staged %u ms (was %u); run `spin auto` again to confirm\n",
      corrected, g_turnAutoTestedMs);
  Serial.println(F("OK"));
}

bool startTurnAutoMeasure(uint16_t fullTurnMs, bool clockwise, int8_t speed) {
  if (!ImuQmi8658::available()) {
    Serial.println(F("ERR imu unavailable; use `spin test` and judge by eye"));
    return false;
  }
  if (g_turnAutoPhase != TurnAutoPhase::Idle ||
      g_movementSequencer.active()) {
    Serial.println(F("ERR a movement is already running"));
    return false;
  }
  if (!measureYawAxisAndBias()) {
    return false;
  }

  g_turnIntegrators[0].reset();
  g_turnIntegrators[1].reset();
  g_turnIntegrators[2].reset();
  // Steady window: skip the standing-start ramp, stop at the end of the
  // commanded spin so the coast never pulls the rate down.
  g_turnSteadyRate.reset(kCalibRampUs,
                         static_cast<uint32_t>(fullTurnMs) * 1000U);
  g_turnAutoTestedMs = fullTurnMs;
  g_turnAutoSpeed = speed;
  g_turnAutoAborted = false;
  if (!startSpinCalibration(fullTurnMs, clockwise, speed)) {
    return false;
  }
  g_turnAutoPhase = TurnAutoPhase::Spinning;
  return true;
}

static void serviceTurnAutoMeasure(uint32_t now) {
  // Chain the next calibration stage once the gap has elapsed.
  if (g_calibPendingNext && g_turnAutoPhase == TurnAutoPhase::Idle &&
      static_cast<int32_t>(now - g_calibNextAtMs) >= 0) {
    g_calibPendingNext = false;
    const bool probing = (g_calibStage == CalibStage::ProbeSlow ||
                          g_calibStage == CalibStage::ProbeFast);
    const uint16_t ms = probing ? kCalibProbeMs
                                : g_spinCalibration.fullTurnMs();
    const int8_t speed = (g_calibStage == CalibStage::ProbeSlow)
                             ? kCalibSlowSpeed
                             : kCalibFastSpeed;
    if (!startTurnAutoMeasure(ms, g_calibClockwise, speed)) {
      g_calibActive = false;
      g_calibStage = CalibStage::Off;
      setCalibStatus("FAILED: could not start");
    }
  }

  if (g_turnAutoPhase == TurnAutoPhase::Idle) return;

  if (g_turnAutoPhase == TurnAutoPhase::Spinning &&
      !g_movementSequencer.active()) {
    g_turnAutoPhase = TurnAutoPhase::Settling;
    g_turnAutoSettleUntilMs = now + kTurnSettleMs;
  }

  float dps[3] = {0.0f, 0.0f, 0.0f};
  float yawDps = 0.0f;
  bool saturated = false;
  if (readAxisRatesAndYaw(dps, yawDps, saturated)) {
    const uint32_t stamp = micros();
    for (int i = 0; i < 3; ++i) {
      if (saturated) g_turnIntegrators[i].markSaturated();
      g_turnIntegrators[i].addSample(dps[i], stamp);
    }
    // The steady-rate probe drives the chassis model, so it uses the projected
    // yaw rate rather than a single axis.
    g_turnSteadyRate.addSample(yawDps, stamp);
  }

  if (g_turnAutoPhase == TurnAutoPhase::Settling &&
      static_cast<int32_t>(now - g_turnAutoSettleUntilMs) >= 0) {
    finishTurnAutoMeasure();
  }
}

bool startTurnCalibration() {
  if (g_calibActive) {
    return false;
  }
  if (!ImuQmi8658::available()) {
    setCalibStatus("FAILED: no imu");
    return false;
  }
  g_calibActive = true;
  g_calibIteration = 0;
  g_calibClockwise = true;
  g_calibPendingNext = false;
  g_calibStage = CalibStage::ProbeSlow;
  g_calibProbeCount = 0;
  g_calibModel = TurnMeasure::RateModel{};
  setCalibStatus("probing slow ...");
  if (!startTurnAutoMeasure(kCalibProbeMs, g_calibClockwise,
                            kCalibSlowSpeed)) {
    g_calibActive = false;
    g_calibStage = CalibStage::Off;
    setCalibStatus("FAILED: could not start");
    return false;
  }
  return true;
}

// =========================================================================
// Relative heading service + closed-loop turn.
// =========================================================================
// Motors off, but keep integrating: the robot is still coasting and where it
// comes to rest is what actually matters.
void beginTurnCoast(bool reached, uint32_t now) {
  g_turnCmdReached = reached;
  g_turnCmdAtStopDeg = g_turnController.turnedDegrees();
  // Rate at the moment of cutting power, for learning the coast constant.
  const float rateAtStop = g_turnController.smoothedRateDps();
  g_turnCmdRateAtStop = rateAtStop < 0 ? -rateAtStop : rateAtStop;
  if (g_movementSequencer.active()) {
    g_movementSequencer.stop();
  }
  if (takeDriveMutex(portMAX_DELAY)) {
    writeDriveOutputLocked("turn-end", 0, 0, 0, 0, now);
    giveDriveMutex();
  }
  powerDownDriveMotors("turn complete");
  g_turnCmdPhase = TurnCmdPhase::Coasting;
  g_turnCmdCoastUntilMs = now + kTurnCoastMs;
}

void finishClosedLoopTurn(uint32_t now) {
  const float turned = g_turnController.turnedDegrees();
  const float target = g_turnCmdTargetDeg;
  const float coast = turned - g_turnCmdAtStopDeg;
  const float magnitude = turned < 0 ? -turned : turned;
  // Judge success on where it actually ENDED UP, not on whether the powered
  // phase alone got there. A turn that timed out mid-ramp but coasted past its
  // target has still performed the turn, and calling that a stall was both
  // wrong and confusing ("STALLED at 97" for a 90 degree target).
  const bool reached = g_turnCmdReached || (magnitude >= target);
  g_turnController.stop();
  g_turnCmdActive = false;
  g_turnCmdPhase = TurnCmdPhase::Idle;

  Serial.printf(
      "turn: %s target %.0f, stopped at %.1f, coasted %+.1f, final %.1f deg\n",
      reached ? (g_turnCmdReached ? "reached" : "reached on coast")
              : "TIMED OUT",
      static_cast<double>(target),
      static_cast<double>(g_turnCmdAtStopDeg < 0 ? -g_turnCmdAtStopDeg
                                                 : g_turnCmdAtStopDeg),
      static_cast<double>(coast < 0 ? -coast : coast),
      static_cast<double>(magnitude));

  // Evidence for the gravity projection, on a real turn: the same samples
  // integrated single-axis would have read this much less, so the robot would
  // have physically over-rotated by the difference before the loop closed.
  const float singleAxis = g_turnSingleAxisIntegrator.totalDegrees();
  const float singleMag = singleAxis < 0 ? -singleAxis : singleAxis;
  Serial.printf(
      "turn: projected %.2f deg vs single-axis %.2f deg (%.2f deg recovered, "
      "gain %.4f)\n",
      static_cast<double>(magnitude), static_cast<double>(singleMag),
      static_cast<double>(magnitude - singleMag),
      static_cast<double>(singleMag > 0.01f ? magnitude / singleMag : 0.0f));

  // Settles whether the apparent tilt is physical or accelerometer offset.
  // The gyro measures the rotation axis directly; gravity only claims to. If
  // these disagree, the accelerometer's zero-g offset (spec +/-100 mg on this
  // part, the same size as the tilt being measured) is the likely culprit, and
  // projecting onto gravity would be applying an error rather than removing
  // one.
  float mx = 0.0f, my = 0.0f, mz = 0.0f;
  if (g_turnAxisEstimator.axis(mx, my, mz)) {
    const float disagreement = g_turnAxisEstimator.angleToDegrees(
        g_turnGravity[0], g_turnGravity[1], g_turnGravity[2]);
    Serial.printf(
        "turn: spin axis measured (%.3f,%.3f,%.3f) vs gravity (%.2f,%.2f,%.2f)"
        " -> %.2f deg apart (%lu samples)\n",
        static_cast<double>(mx), static_cast<double>(my),
        static_cast<double>(mz), static_cast<double>(g_turnGravity[0]),
        static_cast<double>(g_turnGravity[1]),
        static_cast<double>(g_turnGravity[2]),
        static_cast<double>(disagreement),
        static_cast<unsigned long>(g_turnAxisEstimator.sampleCount()));
  }

  // Learn the coast constant from what actually happened. coast = rate * t, so
  // t = coast / rate. Blended, bounded, and only from turns that ran fast
  // enough for the ratio to mean anything.
  // Learn the coast constant from what actually happened. The model is
  // coast = w^2/(2*alpha), so alpha = w^2/(2*coast). Blended, bounded, and only
  // from turns fast enough for the ratio to mean anything.
  const float coastMag = coast < 0 ? -coast : coast;
  if (!g_benchActive && g_turnCmdRateAtStop > 40.0f && coastMag > 0.05f) {
    const float observed = (g_turnCmdRateAtStop * g_turnCmdRateAtStop) /
                           (2.0f * coastMag);
    float blended = (0.7f * g_coastAlpha) + (0.3f * observed);
    if (blended < kCoastAlphaMin) blended = kCoastAlphaMin;
    if (blended > kCoastAlphaMax) blended = kCoastAlphaMax;
    g_coastAlpha = blended;
    ++g_coastLeadUpdates;
    Serial.printf(
        "turn: coast %.2f deg at %.0f dps -> alpha %.0f observed, alpha now "
        "%.0f deg/s2 (%lu updates)\n",
        static_cast<double>(coastMag),
        static_cast<double>(g_turnCmdRateAtStop),
        static_cast<double>(observed), static_cast<double>(g_coastAlpha),
        static_cast<unsigned long>(g_coastLeadUpdates));
  }

  // A bench run captures every turn, and owns the sequencing itself.
  if (g_benchPhase == BenchPhase::Turning) {
    const float signedError = magnitude - target;
    benchRecordTurn(coastMag, g_turnCmdRateAtStop, signedError);
    if (!reached) {
      Serial.printf("bench: turn stalled at %.0f of %.0f deg\n",
                    static_cast<double>(magnitude),
                    static_cast<double>(target));
    }
    benchAdvance(now);
    return;
  }

  if (!g_testSeqActive) return;

  char message[40];
  if (!reached) {
    g_testSeqActive = false;
    snprintf(message, sizeof(message), "STALLED %d of %d deg",
             static_cast<int>(magnitude), static_cast<int>(target));
    setCalibStatus(message);
    return;
  }

  g_testSeqNetDegrees += turned;
  ++g_testSeqIndex;

  if (g_testSeqIndex >= kTestSequenceLength) {
    // The commanded angles sum to zero, so the accumulated measurement is the
    // error: how far from its starting orientation the robot ended up.
    g_testSeqActive = false;
    snprintf(message, sizeof(message), "done  off by %+d deg",
             static_cast<int>(g_testSeqNetDegrees));
    setCalibStatus(message);
    Serial.printf("test: complete, net %+.1f deg from start\n",
                  static_cast<double>(g_testSeqNetDegrees));
    return;
  }

  g_testSeqNextAtMs = now + kTestSeqSettleMs;
  g_testSeqPending = true;
  snprintf(message, sizeof(message), "%u/%u %s done, rezero ...",
           static_cast<unsigned>(g_testSeqIndex),
           static_cast<unsigned>(kTestSequenceLength),
           kTestSequence[g_testSeqIndex - 1].label);
  setCalibStatus(message);
}

// Establishes which gyro axis is yaw from the resting gravity vector. Must run
// before any heading is integrated: the axis is otherwise left at its default,
// and a stationary robot looks perfectly healthy on the WRONG axis because
// every axis reads about zero. The error only appears once it rotates.
bool establishYawAxis() {
  ImuQmi8658::Sample accel{};
  if (!ImuQmi8658::readAccelRaw(accel)) return false;
  const float gx = ImuQmi8658::accelG(accel.x);
  const float gy = ImuQmi8658::accelG(accel.y);
  const float gz = ImuQmi8658::accelG(accel.z);
  g_turnGravity[0] = gx;
  g_turnGravity[1] = gy;
  g_turnGravity[2] = gz;
  g_turnYawAxis = TurnMeasure::yawAxisFromGravity(gx, gy, gz);
  g_turnAxisFraction =
      TurnMeasure::singleAxisYawFraction(g_turnYawAxis, gx, gy, gz);
  Serial.printf(
      "imu: rest g=(%.2f,%.2f,%.2f) -> yaw axis %s, single-axis would lose "
      "%.2f deg per 360\n",
      static_cast<double>(gx), static_cast<double>(gy),
      static_cast<double>(gz), TurnMeasure::axisName(g_turnYawAxis),
      static_cast<double>(360.0f * (1.0f - g_turnAxisFraction)));
  return true;
}

// Bias-corrected yaw rate from a raw gyro sample, projected onto gravity.
// This is the single place the projection happens, so the single-axis
// shortcut cannot creep back in through one call site.
float projectedYawRate(const ImuQmi8658::Sample& gyro) {
  return TurnMeasure::yawRateFromVector(
      ImuQmi8658::gyroDps(gyro.x) - g_turnBias[0],
      ImuQmi8658::gyroDps(gyro.y) - g_turnBias[1],
      ImuQmi8658::gyroDps(gyro.z) - g_turnBias[2],
      g_turnGravity[0], g_turnGravity[1], g_turnGravity[2]);
}

void serviceHeading(uint32_t now) {
  if (!ImuQmi8658::available()) return;

  // A calibration measurement owns the gyro and does its own sampling.
  if (g_turnAutoPhase != TurnAutoPhase::Idle) return;

  const bool turning = g_turnCmdActive;
  if (!turning &&
      static_cast<uint32_t>(now - g_headingLastSampleMs) <
          kHeadingIdleSampleMs) {
    return;
  }
  g_headingLastSampleMs = now;

  ImuQmi8658::Sample gyro{};
  if (!ImuQmi8658::readGyroRaw(gyro)) return;

  // A straight leg needs acceleration too, so the distance can be measured
  // independently of "time x assumed speed" — which physically cannot see
  // wheel slip.
  if (g_straightActive) {
    ImuQmi8658::Sample accel{};
    if (ImuQmi8658::readAccelRaw(accel)) {
      g_displacement.addSample(ImuQmi8658::accelG(accel.x),
                               ImuQmi8658::accelG(accel.y),
                               ImuQmi8658::accelG(accel.z), micros());
    }
  }

  // Projected onto gravity, not a single axis: exact on a tilted platform.
  const float dps = projectedYawRate(gyro);
  // Raw single-axis value is still needed for the zero-velocity update, which
  // learns the per-axis bias that the projection then subtracts.
  const float rawAxis = TurnMeasure::componentFor(
      g_turnYawAxis, static_cast<float>(gyro.x), static_cast<float>(gyro.y),
      static_cast<float>(gyro.z));
  const float rawDps = ImuQmi8658::gyroDps(static_cast<int16_t>(rawAxis));
  const uint32_t stamp = micros();

  g_headingIntegrator.addSample(dps, stamp);
  // Same samples, single-axis, for the side-by-side comparison.
  g_turnSingleAxisIntegrator.addSample(
      rawDps - g_turnBias[static_cast<int>(g_turnYawAxis)], stamp);
  g_headingReady = true;

  if (turning) {
    g_turnController.addSample(dps, stamp);
    // Same sample, raw and bias-corrected, so the rotation axis is measured
    // from the gyro itself rather than assumed from gravity.
    g_turnAxisEstimator.addSample(
        ImuQmi8658::gyroDps(gyro.x) - g_turnBias[0],
        ImuQmi8658::gyroDps(gyro.y) - g_turnBias[1],
        ImuQmi8658::gyroDps(gyro.z) - g_turnBias[2]);
    if (g_turnCmdPhase == TurnCmdPhase::Coasting) {
      // Motors are already off; keep integrating until the robot settles.
      if (static_cast<int32_t>(now - g_turnCmdCoastUntilMs) >= 0) {
        finishClosedLoopTurn(now);
      }
      return;
    }
    if (g_turnController.reachedTarget()) {
      beginTurnCoast(/*reached=*/true, now);
      return;
    }
    if (g_turnController.timedOut(now)) {
      // The bound that makes closing the loop safe: a wedged or disconnected
      // gyro can never keep the motors running.
      beginTurnCoast(/*reached=*/false, now);
      return;
    }
    if (!g_movementSequencer.active()) {
      beginTurnCoast(/*reached=*/false, now);
    }
    return;
  }

  // Zero-velocity update. The robot is stationary far more than it moves, so
  // this is where the gyro bias gets re-learned for free. Anything above the
  // stationary threshold means it is actually moving (or being picked up) and
  // must not pollute the estimate.
  const bool settled = !g_driveRailOn && !g_movementSequencer.active() &&
                       (dps < kHeadingStationaryDps) &&
                       (dps > -kHeadingStationaryDps);
  if (!settled) {
    for (int i = 0; i < 3; ++i) g_headingZupt[i].reset();
    return;
  }

  // Every axis, because the projection reads every axis.
  g_headingZupt[0].addSample(ImuQmi8658::gyroDps(gyro.x));
  g_headingZupt[1].addSample(ImuQmi8658::gyroDps(gyro.y));
  g_headingZupt[2].addSample(ImuQmi8658::gyroDps(gyro.z));

  const int yaw = static_cast<int>(g_turnYawAxis);
  if (g_headingZupt[yaw].ready() &&
      static_cast<uint32_t>(now - g_headingZuptAppliedMs) >=
          kHeadingZuptPeriodMs) {
    // Spread gate. The magnitude gate above is deliberately loose (8 dps) so
    // that a bias which has DRIFTED still qualifies for re-estimation — that
    // is the case a tighter magnitude threshold would lock out, exactly when
    // it is most needed. What must be rejected instead is a window whose rate
    // is VARYING: a nudge, a pick-up, or a coast that has not finished. Those
    // show up as spread, not as mean.
    //
    // Judged on the worst axis: the robot was either still or it was not, and
    // motion about any axis disqualifies the whole window.
    float spread = 0.0f;
    for (int i = 0; i < 3; ++i) {
      const float s = g_headingZupt[i].spreadDps();
      if (s > spread) spread = s;
    }
    if (spread > kHeadingZuptMaxSpreadDps) {
      for (int i = 0; i < 3; ++i) g_headingZupt[i].reset();
      ++g_headingZuptRejected;
      return;
    }
    for (int i = 0; i < 3; ++i) {
      g_turnBias[i] = g_headingZupt[i].bias();
      g_headingZupt[i].reset();
    }
    g_headingLastZuptDps = g_turnBias[yaw];
    g_headingLastZuptSpread = spread;
    g_headingZuptAppliedMs = now;
    ++g_headingZuptCount;
  }
}

bool motionInProgress() {
  return g_turnAutoPhase != TurnAutoPhase::Idle ||
         g_movementSequencer.active() || g_driveRailOn;
}

// Collects an async WiFi scan without ever blocking the loop.
void serviceRfScan(uint32_t now) {
  if (!g_rfScanActive) return;

  const int status = WiFi.scanComplete();
  if (status == WIFI_SCAN_RUNNING) {
    if (static_cast<int32_t>(now - g_rfScanDeadlineMs) >= 0) {
      WiFi.scanDelete();
      g_rfScanActive = false;
      Serial.println(F("rfscan: timed out"));
    }
    return;
  }
  g_rfScanActive = false;
  if (status < 0) {
    WiFi.scanDelete();
    Serial.println(F("rfscan: failed"));
    g_rfSurveyRemaining = 0;
    return;
  }

  // Survey mode: fold this scan into the position's running averages instead
  // of printing every AP for every repeat.
  if (g_rfSurveyPosition >= 0) {
    uint8_t* bssid = nullptr;
    for (int i = 0; i < status; ++i) {
      bssid = WiFi.BSSID(i);
      if (bssid == nullptr) continue;
      g_rfMap.addSample(g_rfSurveyPosition, bssid,
                        static_cast<int16_t>(WiFi.RSSI(i)));
    }
    WiFi.scanDelete();
    ++g_rfSurveyDone;
    if (g_rfSurveyRemaining > 0) --g_rfSurveyRemaining;
    if (g_rfSurveyRemaining > 0) {
      // Chain the next repeat.
      const int started =
          WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
      if (started != WIFI_SCAN_FAILED) {
        g_rfScanActive = true;
        g_rfScanDeadlineMs = now + kRfScanTimeoutMs;
        return;
      }
      Serial.println(F("rfsurvey: scan failed, stopping early"));
      g_rfSurveyRemaining = 0;
    }
    const bool saved = g_rfMap.save();
    Serial.printf(
        "rfsurvey: \"%s\" done, %u scans, %u APs tracked, saved=%d\n",
        g_rfMap.labelAt(static_cast<uint8_t>(g_rfSurveyPosition)),
        static_cast<unsigned>(g_rfSurveyDone),
        static_cast<unsigned>(g_rfMap.apCount()), saved ? 1 : 0);
    g_rfSurveyPosition = -1;
    Serial.println(F("OK"));
    return;
  }

  // One line per AP, each prefixed with the heading it was taken at, so a
  // series of scans can be grouped by BSSID and compared across headings.
  for (int i = 0; i < status; ++i) {
    Serial.printf("rfscan %.2f %s %d %d \"%s\"\n",
                  static_cast<double>(g_rfScanHeading),
                  WiFi.BSSIDstr(i).c_str(), WiFi.RSSI(i), WiFi.channel(i),
                  WiFi.SSID(i).c_str());
  }
  WiFi.scanDelete();
  // Deliberately NOT WiFi.mode(WIFI_OFF): see stopWifiServer(), where turning
  // the radio fully off is documented to corrupt the netif chain on this core
  // and break later softAP bring-ups. Idle STA is harmless and the joystick
  // sets its own mode when it starts.
  Serial.printf("rfscan: heading %.2f deg, %d networks\n",
                static_cast<double>(g_rfScanHeading), status);
  Serial.println(F("OK"));
}

// =========================================================================
// Bench run
// =========================================================================
bool benchStartNextTurn() {
  const BenchStep& step = kBenchScript[g_benchStep];
  // Lead is forced per block rather than read from the learner, so the A/B
  // compares the two conditions and nothing else.
  g_benchLeadDisabled = !step.leadOn;
  g_coastLeadSeconds = 0.0f;
  if (!startClosedLoopTurn(step.degrees, g_benchClockwise, step.speed)) {
    Serial.println(F("bench: turn refused, aborting"));
    return false;
  }
  return true;
}

void benchFinish(const char* reason) {
  g_benchPhase = BenchPhase::Idle;
  g_benchActive = false;
  g_benchPending = false;
  g_benchLeadDisabled = false;
  g_coastLeadSeconds = kCoastLeadDefaultS;
  const bool saved = g_benchLog.save();
  Serial.printf("bench: %s - %u entries, saved=%d%s\n", reason,
                static_cast<unsigned>(g_benchLog.count()), saved ? 1 : 0,
                g_benchLog.overflowed() ? " (TRUNCATED)" : "");
  Serial.println(F("bench: reconnect and run 'bench report'"));
}

// Called once per completed turn, from finishClosedLoopTurn.
void benchRecordTurn(float coastDegrees, float rateAtStopDps,
                     float errorDegrees) {
  if (g_benchPhase != BenchPhase::Turning) return;
  const BenchStep& step = kBenchScript[g_benchStep];
  const uint8_t condition =
      step.leadOn ? BenchLog::kCondLeadOn : BenchLog::kCondLeadOff;
  const int16_t speed = static_cast<int16_t>(step.speed);

  g_benchLog.add(BenchLog::Metric::CoastDegrees, speed, coastDegrees,
                 condition);
  g_benchLog.add(BenchLog::Metric::SteadyRateDps, speed, rateAtStopDps,
                 condition);
  g_benchLog.add(BenchLog::Metric::TurnErrorDegrees, speed, errorDegrees,
                 condition);
  // NOTE: the implied coast constant is deliberately NOT logged. It is exactly
  // coast/rate, i.e. derivable from the two entries above, and logging it cost
  // 25% of the log for nothing — which is what truncated the first run and lost
  // the last two turns of the A/B block.
}

// Advances to the next turn in the script, after each turn has settled.
void benchAdvance(uint32_t now) {
  // Each pair is clockwise then counter-clockwise, so the robot ends a pair
  // back where it began and does not walk across the desk.
  if (g_benchClockwise) {
    g_benchClockwise = false;
  } else {
    g_benchClockwise = true;
    ++g_benchPair;
  }
  if (g_benchPair >= kBenchScript[g_benchStep].pairs) {
    g_benchPair = 0;
    ++g_benchStep;
    if (g_benchStep >= kBenchScriptLength) {
      benchFinish("complete");
      return;
    }
    Serial.printf("bench: step %u/%u %s\n",
                  static_cast<unsigned>(g_benchStep + 1),
                  static_cast<unsigned>(kBenchScriptLength),
                  kBenchScript[g_benchStep].label);
  }
  g_benchPending = true;
  g_benchNextAtMs = now + kTestSeqSettleMs;
}

void serviceBench(uint32_t now) {
  if (g_benchPhase == BenchPhase::Idle) return;

  if (g_benchPhase == BenchPhase::Baseline) {
    if (static_cast<int32_t>(now - g_benchPhaseUntilMs) < 0) return;
    // The drift the robot actually has, in the pose it actually works in.
    g_benchLog.add(BenchLog::Metric::DriftDegrees,
                   static_cast<int16_t>(kBenchBaselineMs / 1000),
                   g_headingIntegrator.totalDegrees());
    g_benchLog.add(BenchLog::Metric::BiasDps, 1,
                   g_turnBias[static_cast<int>(g_turnYawAxis)]);
    g_benchLog.add(BenchLog::Metric::NoiseSpreadDps, 1,
                   g_headingLastZuptSpread);
    g_benchLog.save();   // checkpoint: an abort from here still yields the pose

    g_benchPhase = BenchPhase::Turning;
    g_benchStep = 0;
    g_benchPair = 0;
    g_benchClockwise = true;
    Serial.printf("bench: step 1/%u %s\n",
                  static_cast<unsigned>(kBenchScriptLength),
                  kBenchScript[0].label);
    if (!benchStartNextTurn()) benchFinish("aborted");
    return;
  }

  if (g_benchPhase == BenchPhase::Turning && g_benchPending) {
    if (static_cast<int32_t>(now - g_benchNextAtMs) < 0) return;
    g_benchPending = false;
    if (!benchStartNextTurn()) benchFinish("aborted");
  }
}

bool startBenchRun() {
  if (g_benchPhase != BenchPhase::Idle) return false;
  if (g_testSeqActive || g_turnCmdActive || motionInProgress()) return false;
  if (!ImuQmi8658::available()) {
    Serial.println(F("bench: ERR imu unavailable"));
    return false;
  }
  if (!establishYawAxis()) {
    Serial.println(F("bench: ERR could not read the pose"));
    return false;
  }

  g_benchLog.reset();
  g_benchActive = true;

  // Capture the pose FIRST. Every later number has to be read against it, and
  // it is precisely the thing that silently differed between stand and desk.
  g_benchLog.add(BenchLog::Metric::GravityX, 0, g_turnGravity[0]);
  g_benchLog.add(BenchLog::Metric::GravityY, 0, g_turnGravity[1]);
  g_benchLog.add(BenchLog::Metric::GravityZ, 0, g_turnGravity[2]);
  g_benchLog.add(BenchLog::Metric::ProjectionGain, 0,
                 g_turnAxisFraction > 0.0f ? 1.0f / g_turnAxisFraction : 0.0f);

  g_headingIntegrator.reset();
  g_benchPhase = BenchPhase::Baseline;
  g_benchPhaseUntilMs = millis() + kBenchBaselineMs;
  Serial.printf(
      "bench: started. Baseline hold %lus, then %u turn blocks. "
      "Do not touch the robot.\n",
      static_cast<unsigned long>(kBenchBaselineMs / 1000),
      static_cast<unsigned>(kBenchScriptLength));
  return true;
}

// =========================================================================
// Straight-line driving
// =========================================================================
// The timeout a closed-loop turn of this size would get, or 0 if it does not
// fit. Factored out so a request can be validated WHILE TETHERED rather than
// failing at the moment the button is pressed — an untethered test that
// refuses to start costs a whole supervised trip.
uint32_t turnTimeoutMsFor(float degrees, int8_t speed) {
  const float span = static_cast<float>(MovementExpression::kCommandMax) -
                     kTurnDeadbandCommand;
  const float usable = static_cast<float>(speed) - kTurnDeadbandCommand;
  if (!(usable > 0.0f)) return 0;
  const float speedFactor = span / usable;

  const uint32_t steadyMs = static_cast<uint32_t>(
      (static_cast<double>(g_spinCalibration.fullTurnMs()) *
       static_cast<double>(degrees) / 360.0) *
      static_cast<double>(speedFactor));
  const uint32_t estimateMs = steadyMs + kTurnRampMs;

  uint32_t timeoutMs = (estimateMs * 8U) / 5U;   // 1.6x headroom
  if (timeoutMs < MovementExpression::kSpinMinMs) {
    timeoutMs = MovementExpression::kSpinMinMs;
  }
  timeoutMs -= (timeoutMs % MovementExpression::kStepQuantumMs);
  if (timeoutMs > MovementExpression::kSpinPatternMaxMs) return 0;
  return timeoutMs;
}

// Feeds the accelerometer to the bump/shake/pick-up detector.
//
// Deliberately separate from serviceHeading(): that one owns the GYRO and is
// gated on turns and calibrations, while this must keep running whatever else
// is happening — being picked up mid-turn is precisely when you want to know.
void serviceMotionEvents(uint32_t now) {
  if (!ImuQmi8658::available()) return;
  if (static_cast<uint32_t>(now - g_motionLastSampleMs) < kMotionSampleMs) {
    return;
  }
  g_motionLastSampleMs = now;

  ImuQmi8658::Sample accel{};
  if (!ImuQmi8658::readAccelRaw(accel)) return;

  if (!g_motionEventsReady) {
    // Reference gravity comes from the same vector the yaw axis was
    // established with, so "tilted" means tilted relative to how this board
    // actually sits — it is not axis-aligned, and assuming flat would make a
    // healthy robot look permanently tipped.
    g_motionEvents.begin(g_turnGravity[0], g_turnGravity[1], g_turnGravity[2]);
    g_motionEventsReady = true;
    return;
  }

  const bool driving = g_driveRailOn || g_movementSequencer.active() ||
                       g_turnCmdActive || g_straightActive;

  const MotionEvents::Event event = g_motionEvents.addSample(
      ImuQmi8658::accelG(accel.x), ImuQmi8658::accelG(accel.y),
      ImuQmi8658::accelG(accel.z), micros(), driving);

  if (event == MotionEvents::Event::None) return;

  Serial.printf("motion: %s (tilt %.1f deg, |a|-1 %+.2f g, linear %.2f g)\n",
                MotionEvents::eventName(event),
                static_cast<double>(g_motionEvents.lastTiltDegrees()),
                static_cast<double>(g_motionEvents.lastMagnitudeDeviationG()),
                static_cast<double>(g_motionEvents.lastLinearG()));

  switch (event) {
    case MotionEvents::Event::Bump:
      // A collision while driving means the commanded motion is no longer
      // describing reality — whatever distance estimate is running is now
      // wrong, and continuing to drive into an obstacle stalls the servos,
      // which the vendor warns damages them.
      if (g_straightActive) {
        Serial.println(F("motion: bump during a straight run - stopping"));
        stopMovementGesture("bump detected");
        g_straightActive = false;
      }
      break;

    case MotionEvents::Event::Lifted:
      // Everything positional is now meaningless: the robot may be pointed
      // anywhere by the time it comes back down. Stop driving rather than
      // spin the wheels in mid-air.
      if (driving) {
        Serial.println(F("motion: picked up while moving - stopping"));
        stopMovementGesture("picked up");
        g_straightActive = false;
      }
      break;

    case MotionEvents::Event::Placed:
      // The gravity reference and yaw axis are stale after a carry. Re-derive
      // them rather than integrating from a reference that no longer
      // describes which way is down.
      if (establishYawAxis()) {
        g_motionEvents.begin(g_turnGravity[0], g_turnGravity[1],
                             g_turnGravity[2]);
        g_headingIntegrator.reset();
        Serial.println(F("motion: placed - yaw axis and heading re-zeroed"));
      }
      break;

    default:
      break;
  }
}

// Drives the melody player into the piezo.
//
// Re-commanding the same frequency every tick is deliberate: PiezoTone stops
// any tone that has not been refreshed within 750 ms, so a note longer than
// that would otherwise die mid-way. Re-issuing costs one LEDC write and makes
// long notes work without touching the safety watchdog.
void serviceMelody(uint32_t now) {
  if (!g_piezoAvailable) return;
  const bool wasActive = g_melody.active();
  const uint16_t hz = g_melody.service(now);
  if (hz != 0) {
    PiezoTone::command(hz, now);
  } else if (wasActive) {
    // Only stop on the transition, so this never fights anything else that may
    // legitimately own the piezo while no melody is playing.
    PiezoTone::stop();
  }
}

bool playMood(MelodyGen::Mood mood) {
  // Storage lifetime: Melody::Player holds the const Note* WITHOUT copying,
  // so the generated notes must live in storage that outlasts playback.
  // A single static Phrase (owned here, updated in place on each trigger)
  // is the smallest object that satisfies that — see the header comment in
  // melody_gen.h. Retries against a fixed-capacity history let sequential
  // triggers of the same mood produce fresh phrases without needing an RNG
  // sequence long enough on its own.
  static MelodyGen::Phrase s_phrase;
  static MelodyGen::History s_history;

  const uint64_t seed = (static_cast<uint64_t>(esp_random()) << 32) |
                        static_cast<uint64_t>(esp_random());
  if (!MelodyGen::generateFresh(mood, seed, s_phrase, s_history)) return false;
  if (!g_melody.start(s_phrase.notes, s_phrase.noteCount, millis())) return false;
  g_melody.setName(MelodyGen::moodName(mood));
  return true;
}

bool playMoodByName(const char* name) {
  MelodyGen::Mood mood;
  if (!MelodyGen::moodForName(name, mood)) return false;
  return playMood(mood);
}

bool startStraightDrive(int16_t centimetres, int8_t speed) {
  if (!ImuQmi8658::available()) {
    Serial.println(F("ERR imu unavailable"));
    return false;
  }
  if (g_turnCmdActive || g_straightActive || motionInProgress() ||
      g_movementSequencer.active()) {
    Serial.println(F("ERR a movement is already running"));
    return false;
  }
  if (centimetres < 5 || centimetres > 300) {
    Serial.println(F("ERR straight 5..300 cm"));
    return false;
  }
  if (speed < MovementExpression::kSpinMinSpeed ||
      speed > MovementExpression::kCommandMax) {
    Serial.printf("ERR speed %d..%d\n",
                  static_cast<int>(MovementExpression::kSpinMinSpeed),
                  static_cast<int>(MovementExpression::kCommandMax));
    return false;
  }

  // Speed scales with command the same way rotation does: a continuous-rotation
  // servo has a deadband, so the usable span starts above it.
  const float span = static_cast<float>(MovementExpression::kCommandMax) -
                     kTurnDeadbandCommand;
  const float usable = static_cast<float>(speed) - kTurnDeadbandCommand;
  if (!(usable > 0.0f)) {
    Serial.printf("ERR speed %d is inside the servo deadband (~%d)\n",
                  static_cast<int>(speed),
                  static_cast<int>(kTurnDeadbandCommand));
    return false;
  }
  const float cmPerSec = g_forwardCmPerSec * (usable / span);
  uint32_t durationMs = static_cast<uint32_t>(
      (static_cast<float>(centimetres) / cmPerSec) * 1000.0f);
  durationMs -= (durationMs % MovementExpression::kStepQuantumMs);
  if (durationMs < MovementExpression::kStraightMinMs) {
    durationMs = MovementExpression::kStraightMinMs;
  }
  if (durationMs > MovementExpression::kStraightMaxMs) {
    Serial.printf(
        "ERR %d cm at speed %d needs ~%lums, over the %u ms limit; "
        "use a higher speed or a shorter leg\n",
        static_cast<int>(centimetres), static_cast<int>(speed),
        static_cast<unsigned long>(durationMs),
        MovementExpression::kStraightMaxMs);
    return false;
  }

  static MovementExpression::Step
      s_straightSteps[MovementExpression::kStraightStepCap];
  static MovementExpression::Pattern s_straightPattern;
  if (!MovementExpression::buildStraight(
          static_cast<uint16_t>(durationMs), /*forward=*/true, speed,
          s_straightSteps,
          static_cast<uint8_t>(MovementExpression::kStraightStepCap),
          s_straightPattern)) {
    Serial.println(F("ERR straight pattern failed to build"));
    return false;
  }
  if (!g_driveCalibration.eligible()) {
    Serial.println(F("ERR straight needs an eligible calibration"));
    return false;
  }
  if (g_paused || g_systemShuttingDown ||
      g_driveSession.state() != DriveSession::State::SafeOff) {
    Serial.println(F("ERR straight unavailable right now"));
    return false;
  }

  // Freshest possible zero, so the heading reference the run holds against is
  // measured immediately before it starts moving.
  if (!measureYawAxisAndBias()) return false;

  const uint32_t now = millis();
  if (!g_movementSequencer.start(&s_straightPattern, now,
                                 MovementExpression::Balance::Unbalanced)) {
    Serial.println(F("ERR straight pattern failed validation"));
    return false;
  }
  g_movementLastTickMs = now;
  g_headingHold.begin(kHeadingHoldGain, kHeadingHoldMaxTrim);
  // g_turnGravity is the accelerometer reading taken at standstill moments ago
  // by measureYawAxisAndBias(). It carries gravity AND sensor bias, and
  // subtracting it removes both — they are not separable from one stationary
  // reading, and for a leg driven at roughly constant tilt they enter the same
  // way.
  g_displacement.begin(g_turnGravity[0], g_turnGravity[1], g_turnGravity[2]);
  g_straightHeadingRef = g_headingIntegrator.totalDegrees();
  g_straightActive = true;
  g_straightTargetCm = centimetres;
  g_straightSpeed = speed;
  g_straightEndsAtMs = now + durationMs;
  Serial.printf(
      "straight: %d cm at speed %d (~%lums at %.1f cm/s) - distance is "
      "ESTIMATED, not measured\n",
      static_cast<int>(centimetres), static_cast<int>(speed),
      static_cast<unsigned long>(durationMs), static_cast<double>(cmPerSec));
  return true;
}

void serviceStraightDrive(uint32_t now) {
  if (!g_straightActive) return;
  if (g_movementSequencer.active() &&
      static_cast<int32_t>(now - g_straightEndsAtMs) < 0) {
    return;
  }
  g_straightActive = false;
  const float drift = g_headingIntegrator.totalDegrees() - g_straightHeadingRef;
  const float imuCm = g_displacement.correctedDistanceMetres() * 100.0f;
  const float rawCm = g_displacement.rawDistanceMetres() * 100.0f;
  const float residual = g_displacement.residualSpeedMetresPerSec();
  const float commandedCm = static_cast<float>(g_straightTargetCm);

  Serial.printf("straight: done, heading drifted %+.2f deg\n",
                static_cast<double>(drift));
  // TWO independent distance estimates, reported side by side and never
  // averaged. Time x speed cannot see wheel slip; the accelerometer can.
  // Their DISAGREEMENT is the slip measurement, and hiding it behind a blend
  // would throw away the only slip signal this chassis has.
  Serial.printf(
      "straight: commanded %.0f cm | imu %.1f cm (raw %.1f, residual "
      "%.3f m/s, %lu samples over %.2fs)\n",
      static_cast<double>(commandedCm), static_cast<double>(imuCm),
      static_cast<double>(rawCm), static_cast<double>(residual),
      static_cast<unsigned long>(g_displacement.sampleCount()),
      static_cast<double>(g_displacement.elapsedSeconds()));
  if (commandedCm > 1.0f) {
    const float ratio = imuCm / commandedCm;
    const char* verdict =
        residual > 0.05f
            ? "imu estimate is LOW CONFIDENCE (large leftover velocity)"
            : (ratio < 0.7f ? "IMU SAYS IT WENT MUCH LESS FAR - likely wheel slip"
                            : (ratio > 1.4f
                                   ? "imu says further than commanded - check "
                                     "the speed calibration"
                                   : "estimates agree"));
    Serial.printf("straight: imu/commanded = %.2f -> %s\n",
                  static_cast<double>(ratio), verdict);
  }

  // SELF-CALIBRATION. The inertial distance does not depend on the wheel-speed
  // constant, so it can be used to LEARN that constant: speed = distance /
  // time. This is not circular, and it is what removes the tape measure from
  // the loop entirely — a calibration that needs a human is a calibration that
  // does not happen.
  //
  // Gated hard, because a bad update is worse than no update:
  //   - a large leftover velocity means the inertial estimate is untrustworthy
  //   - a very short leg has too little signal above the noise floor
  //   - a wildly different answer is more likely slip or a bad reading than a
  //     genuine 3x speed change, so it is rejected rather than absorbed
  const float elapsed = g_displacement.elapsedSeconds();
  if (residual < 0.03f && elapsed > 1.0f && imuCm > 15.0f) {
    const float observed = imuCm / elapsed;   // cm/s at the speed just used
    const float span = static_cast<float>(MovementExpression::kCommandMax) -
                       kTurnDeadbandCommand;
    const float usable =
        static_cast<float>(g_straightSpeed) - kTurnDeadbandCommand;
    if (usable > 0.0f) {
      const float atFullSpeed = observed * (span / usable);
      const float ratioToCurrent = atFullSpeed / g_forwardCmPerSec;
      if (ratioToCurrent > 0.4f && ratioToCurrent < 2.5f) {
        float blended = (0.6f * g_forwardCmPerSec) + (0.4f * atFullSpeed);
        if (blended < kForwardCmPerSecMin) blended = kForwardCmPerSecMin;
        if (blended > kForwardCmPerSecMax) blended = kForwardCmPerSecMax;
        Serial.printf(
            "straight: self-cal %.1f cm/s observed -> %.1f cm/s (was %.1f, "
            "%lu updates)\n",
            static_cast<double>(atFullSpeed), static_cast<double>(blended),
            static_cast<double>(g_forwardCmPerSec),
            static_cast<unsigned long>(g_forwardCalUpdates + 1));
        g_forwardCmPerSec = blended;
        ++g_forwardCalUpdates;
      } else {
        Serial.printf(
            "straight: self-cal REJECTED (%.1f cm/s implied, %.1fx current) - "
            "likely slip\n",
            static_cast<double>(atFullSpeed),
            static_cast<double>(ratioToCurrent));
      }
    }
  }
}

// =========================================================================
// Autonomous triangle survey
// =========================================================================
bool surveyStartSampling(uint32_t now);

void surveyFinish(const char* reason) {
  g_surveyPhase = SurveyPhase::Idle;
  g_straightActive = false;
  const bool saved = g_rfMap.save();
  Serial.printf("survey: %s - %u positions, %u APs, saved=%d\n", reason,
                static_cast<unsigned>(g_rfMap.positionCount()),
                static_cast<unsigned>(g_rfMap.apCount()), saved ? 1 : 0);
  Serial.println(F("survey: run 'rfsurvey report' for the comparison"));
}

bool surveyStartSampling(uint32_t now) {
  (void)now;
  char label[RfMap::kLabelLen];
  // The fourth sample is taken back at the start, so the survey closes on
  // itself. Comparing it with vertex 0 is a free measurement of how far dead
  // reckoning drifted over the whole triangle — the only check available on a
  // chassis with no encoders.
  if (g_surveyVertex >= kSurveyVertices) {
    snprintf(label, sizeof(label), "close");
  } else {
    snprintf(label, sizeof(label), "v%u",
             static_cast<unsigned>(g_surveyVertex));
  }
  if (g_rfMap.positionCount() == 0) g_rfMap.load();
  const int8_t position = g_rfMap.beginPosition(label);
  if (position < 0) {
    surveyFinish("survey full");
    return false;
  }
  const int started = WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/true);
  if (started == WIFI_SCAN_FAILED) {
    surveyFinish("scan failed");
    return false;
  }
  WiFi.mode(WIFI_STA);
  g_rfScanActive = true;
  g_rfScanDeadlineMs = millis() + kRfScanTimeoutMs;
  g_rfSurveyPosition = position;
  g_rfSurveyRemaining = g_surveyScans;
  g_rfSurveyDone = 0;
  g_surveyPhase = SurveyPhase::Sampling;
  Serial.printf("survey: sampling \"%s\" x%u\n", label,
                static_cast<unsigned>(g_surveyScans));
  return true;
}

void serviceSurvey(uint32_t now) {
  if (g_surveyPhase == SurveyPhase::Idle) return;

  switch (g_surveyPhase) {
    case SurveyPhase::Sampling: {
      // Sampling finishes when the rfsurvey machinery releases the position.
      if (g_rfScanActive || g_rfSurveyPosition >= 0) return;
      if (g_surveyVertex >= kSurveyVertices) {
        surveyFinish("complete");
        return;
      }
      if (!startStraightDrive(static_cast<int16_t>(g_surveySideCm),
                              MovementExpression::kSpinSpeed)) {
        surveyFinish("drive refused");
        return;
      }
      g_surveyPhase = SurveyPhase::Driving;
      return;
    }
    case SurveyPhase::Driving: {
      if (g_straightActive) return;
      ++g_surveyVertex;
      if (g_surveyVertex >= kSurveyVertices) {
        // Third leg done: the robot should be back where it started. Sample
        // once more so the closure error is recorded rather than assumed.
        surveyStartSampling(now);
        return;
      }
      // Exterior angle of an equilateral triangle.
      if (!startClosedLoopTurn(120.0f, /*clockwise=*/true,
                               MovementExpression::kSpinSpeed)) {
        surveyFinish("turn refused");
        return;
      }
      g_surveyPhase = SurveyPhase::Turning;
      return;
    }
    case SurveyPhase::Turning: {
      if (g_turnCmdActive) return;
      surveyStartSampling(now);
      return;
    }
    default:
      return;
  }
}

bool startTriangleSurvey(uint16_t sideCm, uint16_t scans) {
  if (g_surveyPhase != SurveyPhase::Idle) return false;
  if (g_turnCmdActive || g_straightActive || motionInProgress()) {
    Serial.println(F("ERR a movement is already running"));
    return false;
  }
  if (g_overrideUp) {
    Serial.println(F("ERR survey cannot run while the override AP is up"));
    return false;
  }
  if (sideCm < 30 || sideCm > 300) {
    Serial.println(F("ERR side 30..300 cm"));
    return false;
  }
  g_surveySideCm = sideCm;
  g_surveyScans = scans;
  g_surveyVertex = 0;
  g_rfMap.reset();
  Serial.printf(
      "survey: equilateral triangle, %u cm sides, %u scans per vertex.\n",
      static_cast<unsigned>(sideCm), static_cast<unsigned>(scans));
  Serial.println(F("survey: NEEDS CLEAR FLOOR - it will drive off a desk."));
  return surveyStartSampling(millis());
}

bool startClosedLoopTurn(float degrees, bool clockwise, int8_t speed) {  if (!ImuQmi8658::available()) {
    Serial.println(F("ERR imu unavailable"));
    return false;
  }
  if (g_turnCmdActive || g_turnAutoPhase != TurnAutoPhase::Idle ||
      g_movementSequencer.active()) {
    Serial.println(F("ERR a movement is already running"));
    return false;
  }
  if (!(degrees > 0.0f) || degrees > TurnMeasure::kMaxUsableDegrees) {
    Serial.printf("ERR turn 1..%d degrees\n",
                  static_cast<int>(TurnMeasure::kMaxUsableDegrees));
    return false;
  }
  if (speed < MovementExpression::kSpinMinSpeed ||
      speed > MovementExpression::kCommandMax) {
    Serial.printf("ERR speed %d..%d\n",
                  static_cast<int>(MovementExpression::kSpinMinSpeed),
                  static_cast<int>(MovementExpression::kCommandMax));
    return false;
  }

  // Re-measure the axis and bias immediately before turning, so the integral
  // starts from the freshest possible zero.
  if (!measureYawAxisAndBias()) {
    return false;
  }

  // Scale the estimate for speed. NOT linearly: a continuous-rotation servo
  // has a deadband, so halving the command more than halves the rate. The
  // measured model on this chassis was rate = 0.91*speed - 24.2 dps, i.e. zero
  // rate at about command 27, so the useful span is (speed - 27) of 73.
  const float span = static_cast<float>(MovementExpression::kCommandMax) -
                     kTurnDeadbandCommand;
  const float usable = static_cast<float>(speed) - kTurnDeadbandCommand;
  if (!(usable > 0.0f)) {
    Serial.printf("ERR speed %d is inside the servo deadband (~%d)\n",
                  static_cast<int>(speed),
                  static_cast<int>(kTurnDeadbandCommand));
    return false;
  }
  const float speedFactor = span / usable;   // 1.0 at full speed, >1 slower

  // The ramp allowance is NOT optional, and forgetting it is what made small
  // turns stall: scaling the full-turn duration alone assumes steady-state
  // rate for the whole turn, but every turn starts from rest. For a 360 the
  // standing start is a small fraction of the total; for a 90 it dominates.
  // Observed: a 90 degree turn estimated at 316 ms timed out mid-ramp and then
  // coasted to 97 degrees — past its own target.
  //
  // Shared with the arm-time validation, so a request that is accepted while
  // tethered cannot then be refused at the moment the button is pressed.
  const uint32_t timeoutMs = turnTimeoutMsFor(degrees, speed);
  if (timeoutMs == 0) {
    // Refuse rather than silently clamp: a clamped timeout would stop a turn
    // that was progressing perfectly well and report it as a stall.
    Serial.printf(
        "ERR %.0f deg at speed %d does not fit the %u ms pattern limit; "
        "use a higher speed or a smaller angle\n",
        static_cast<double>(degrees), static_cast<int>(speed),
        MovementExpression::kSpinPatternMaxMs);
    return false;
  }

  g_headingIntegrator.reset();
  g_turnSingleAxisIntegrator.reset();
  g_turnAxisEstimator.reset();
  g_turnCmdClockwise = clockwise;
  g_turnCmdSpeed = speed;
  g_turnCmdTargetDeg = degrees;
  g_turnCmdAtStopDeg = 0.0f;
  g_turnCmdRateAtStop = 0.0f;
  g_turnCmdReached = false;
  g_turnCmdPhase = TurnCmdPhase::Turning;
  g_turnCmdTimeoutMs = static_cast<uint16_t>(timeoutMs);
  if (!g_turnController.begin(degrees, timeoutMs, millis())) {
    Serial.println(F("ERR turn parameters rejected"));
    return false;
  }

  // Stop early by the predicted coast, so the angle the chassis comes to REST
  // at is the target rather than the angle at which power was cut. Bound it:
  // on a small turn the coast can exceed the whole target, and an unbounded
  // lead would satisfy reachedTarget() on the first fast sample and cut power
  // mid-ramp. Never lead away more than 40% of the turn.
  const float expectedDps =
      (g_spinCalibration.fullTurnMs() > 0)
          ? (360000.0f / static_cast<float>(g_spinCalibration.fullTurnMs()) /
             speedFactor)
          : 0.0f;
  float leadS = g_coastLeadSeconds;
  if (expectedDps > 1.0f) {
    const float maxLead = (0.4f * degrees) / expectedDps;
    if (leadS > maxLead) leadS = maxLead;
  }
  g_turnController.setLeadSeconds(leadS);
  // Quadratic term. Bounded the same way: on a small turn the predicted coast
  // can exceed the whole target, and an unbounded lead would satisfy
  // reachedTarget() on the first fast sample and cut power mid-ramp.
  float alpha = g_benchLeadDisabled ? 0.0f : g_coastAlpha;
  if (alpha > 0.0f && expectedDps > 1.0f) {
    const float predicted = (expectedDps * expectedDps) / (2.0f * alpha);
    const float maxCoast = 0.4f * degrees;
    if (predicted > maxCoast && maxCoast > 0.0f) {
      // Raise alpha (weaker lead) just enough to respect the bound.
      alpha = (expectedDps * expectedDps) / (2.0f * maxCoast);
    }
  }
  g_turnController.setCoastAlpha(alpha);

  // Drive through the ordinary spin pattern so the turn inherits the mutex,
  // the 300 ms lease, the emergency latch and the clean powerdown. The pattern
  // duration IS the timeout; the controller stops it early on the angle.
  if (!startSpinCalibration(static_cast<uint16_t>(timeoutMs), clockwise,
                            speed)) {
    g_turnController.stop();
    return false;
  }

  g_turnCmdActive = true;
  Serial.printf("turn: %.0f deg %s at %d (timeout %lums)\n",
                static_cast<double>(degrees), clockwise ? "cw" : "ccw",
                static_cast<int>(speed),
                static_cast<unsigned long>(timeoutMs));
  return true;
}

bool startTurnTestSequence() {
  if (g_testSeqActive || g_turnCmdActive) return false;
  if (!ImuQmi8658::available()) {
    setCalibStatus("FAILED: no imu");
    return false;
  }

  // If a scale check was armed while tethered, THIS press runs it. Scale is
  // the one error needing an external reference, and the turn must be
  // untethered because the cable restrains rotation.
  if (g_scaleCheckRevs > 0 && !g_benchActive) {
    const float degrees = static_cast<float>(g_scaleCheckRevs) * 360.0f;
    if (startClosedLoopTurn(degrees, /*clockwise=*/true,
                            MovementExpression::kCommandMax)) {
      char msg[40];
      snprintf(msg, sizeof(msg), "scale check %ux360",
               static_cast<unsigned>(g_scaleCheckRevs));
      setCalibStatus(msg);
      return true;
    }
    setCalibStatus("scale check FAILED");
    return false;
  }

  // If a bench run was armed while tethered, THIS press is what runs it.
  //
  // The agent cannot drive the motors: a tethered turn is invalid because the
  // cable restrains rotation, and the robot must not be driven while plugged
  // in. So the long measurement run is armed over serial, the robot is
  // unplugged, and the human starts it here. Consumed on use, so the next
  // press does the ordinary short sequence.
  if (BenchLog::isArmed()) {
    BenchLog::setArmed(false);
    if (startBenchRun()) {
      setCalibStatus("bench run: ~2 min");
      return true;
    }
    setCalibStatus("bench FAILED to start");
    return false;
  }

  g_testSeqActive = true;
  g_testSeqIndex = 0;
  g_testSeqPending = false;
  g_testSeqNetDegrees = 0.0f;

  char message[40];
  snprintf(message, sizeof(message), "1/%u %s ...",
           static_cast<unsigned>(kTestSequenceLength),
           kTestSequence[0].label);
  setCalibStatus(message);
  Serial.println(F("test: R90, L90, L45, R90, L45 (sums to zero)"));
  if (!startClosedLoopTurn(kTestSequence[0].degrees,
                           kTestSequence[0].clockwise, g_testSeqSpeed)) {
    g_testSeqActive = false;
    setCalibStatus("FAILED: could not start");
    return false;
  }
  return true;
}

void serviceTurnTestSequence(uint32_t now) {
  if (!g_testSeqPending) return;
  if (static_cast<int32_t>(now - g_testSeqNextAtMs) < 0) return;
  g_testSeqPending = false;
  if (!g_testSeqActive || g_testSeqIndex >= kTestSequenceLength) return;

  const TestSeqStep& step = kTestSequence[g_testSeqIndex];
  char message[40];
  snprintf(message, sizeof(message), "%u/%u %s ...",
           static_cast<unsigned>(g_testSeqIndex + 1),
           static_cast<unsigned>(kTestSequenceLength), step.label);
  setCalibStatus(message);

  // startClosedLoopTurn re-measures the gyro bias from a standstill, so this
  // IS the recalibration between every pair of turns.
  if (!startClosedLoopTurn(step.degrees, step.clockwise, g_testSeqSpeed)) {
    g_testSeqActive = false;
    setCalibStatus("FAILED: turn refused");
  }
}

void serviceMovementGesture(uint32_t now) {
  if (!g_movementSequencer.active()) return;
  // A fault or an explicit reset cancels the gesture. Checked BEFORE the
  // state-based guard below, because DriveSession::state() cannot express the
  // difference between "idle, nobody connected" and "just force-reset by a
  // transport fault" — both are SafeOff, and only the second must abort.
  if (g_movementAbortPending.exchange(false, std::memory_order_acq_rel)) {
    stopMovementGesture("drive control reset");
    return;
  }
  // Abort only when the operator is actually DRIVING, not merely connected.
  //
  // This guard used to be `state() != SafeOff`, which was correct when the
  // movement buttons lived on the LCD and were unreachable with a web client
  // connected. Now the web app is the only trigger, so a gesture ALWAYS runs
  // alongside a live control session sitting at neutral (ArmedOff) — and this
  // cancelled every gesture on its first tick. The sound had already been
  // started at gesture start, so the robot chirped and never moved.
  //
  // The WS Control handler already suppresses the joystick's output while a
  // gesture runs, so an idle session cannot fight for the wheels. What must
  // still win over an expression is a real stick input: those states abort.
  const DriveSession::State sessionState = g_driveSession.state();
  const bool operatorIsDriving = sessionState == DriveSession::State::Active ||
                                 sessionState == DriveSession::State::Manual ||
                                 sessionState == DriveSession::State::Shutdown;
  if (operatorIsDriving) {
    stopMovementGesture("drive session became active");
    return;
  }
  if (g_paused || g_systemShuttingDown ||
      !g_driveCalibration.eligible()) {
    stopMovementGesture("drive no longer permitted");
    return;
  }

  if (static_cast<uint32_t>(now - g_movementLastTickMs) < kMovementTickMs) {
    return;
  }
  g_movementLastTickMs = now;

  int left = 0;
  int right = 0;
  // Captured before sampling: reaching the end stops the sequencer, which
  // clears the pattern pointer.
  const MovementExpression::Pattern* running = g_movementSequencer.pattern();
  const bool measuring =
      running != nullptr && strcmp(running->name, "spin") == 0;
  if (!g_movementSequencer.sample(now, left, right)) {
    // Finished. Write neutral so the wheels stop immediately, then power the
    // rail down explicitly rather than letting the lease lapse into an
    // emergency cutoff.
    if (takeDriveMutex(portMAX_DELAY)) {
      writeDriveOutputLocked("gesture-end", 0, 0, 0, 0, now);
      giveDriveMutex();
    }
    powerDownDriveMotors(measuring ? "spin complete" : "gesture complete");
    // The operator loop is "type `spin test`, watch, read the log", so a
    // measurement must not report itself as a gesture at the exact moment the
    // user is correlating command with behaviour.
    Serial.println(measuring ? F("spin: complete") : F("gesture: complete"));
    return;
  }

  if (!takeDriveMutex(portMAX_DELAY)) {
    stopMovementGesture("drive mutex unavailable");
    return;
  }
  // Live heading trim for a straight run. The pattern commands both wheels
  // equally; two hobby servos never match, so without this the robot curves by
  // an amount nobody can predict. The gyro is the only feedback available —
  // note it corrects HEADING, not lateral position, and says nothing at all
  // about distance travelled.
  if (g_straightActive && g_headingReady) {
    const float error = g_headingIntegrator.totalDegrees() - g_straightHeadingRef;
    const int8_t trim = g_headingHold.trimFor(error);
    int correctedLeft = static_cast<int>(left) + trim;
    int correctedRight = static_cast<int>(right) - trim;
    if (correctedLeft > MovementExpression::kCommandMax) {
      correctedLeft = MovementExpression::kCommandMax;
    }
    if (correctedLeft < -MovementExpression::kCommandMax) {
      correctedLeft = -MovementExpression::kCommandMax;
    }
    if (correctedRight > MovementExpression::kCommandMax) {
      correctedRight = MovementExpression::kCommandMax;
    }
    if (correctedRight < -MovementExpression::kCommandMax) {
      correctedRight = -MovementExpression::kCommandMax;
    }
    left = static_cast<int8_t>(correctedLeft);
    right = static_cast<int8_t>(correctedRight);
  }
  const bool ok =
      writeDriveOutputLocked("gesture", 0, 0, left, right, now);
  giveDriveMutex();
  if (!ok) {
    stopMovementGesture("canonical output rejected");
  }
}

void serviceDriveIdlePowerdown(uint32_t now) {
  if (!takeDriveMutex(0)) return;
  if (!g_driveRailOn || g_driveIdleSinceMs == 0U ||
      g_lastEffectiveDriveL != 0 || g_lastEffectiveDriveR != 0 ||
      static_cast<uint32_t>(now - g_driveIdleSinceMs) <
          kMotorIdlePowerOffMs) {
    giveDriveMutex();
    return;
  }

  powerDownDriveMotorsLocked("1s neutral idle");
  g_neutralIdleLatchedOff = true;
  giveDriveMutex();
}

// Read an integer field from a flat JSON-ish body. No deps, no nesting.
// Looks for "<key>":<digits> with optional leading sign and whitespace.
bool jsonFindInt(const char* body, const char* key, int& outVal) {
  if (body == nullptr) return false;
  char needle[24];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char* p = strstr(body, needle);
  if (p == nullptr) return false;
  p += strlen(needle);
  while ((*p == ' ') || (*p == ':') || (*p == '\t')) p++;
  char* endPtr = nullptr;
  long v = strtol(p, &endPtr, 10);
  if (endPtr == p) return false;
  outVal = (int)v;
  return true;
}

bool jsonFindStr(const char* body, const char* key, char* outBuf, size_t outLen) {
  if (body == nullptr || outBuf == nullptr || outLen == 0) return false;
  char needle[24];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char* p = strstr(body, needle);
  if (p == nullptr) return false;
  p += strlen(needle);
  while ((*p == ' ') || (*p == ':') || (*p == '\t')) p++;
  if (*p != '"') return false;
  p++;
  const char* end = strchr(p, '"');
  if (end == nullptr) return false;
  size_t n = (size_t)(end - p);
  if (n >= outLen) n = outLen - 1;
  memcpy(outBuf, p, n);
  outBuf[n] = '\0';
  return true;
}

// Firmware-side helpers --------------------------------------------------
const char* moodNameSafe() {
  return moodName(g_eyes.getMood());
}

bool isValidMoodId(uint8_t id) {
  // EyeMood enum skips 4. Anything in 0..16 except 4 is real.
  return (id <= 16) && (id != 4);
}

// Build telemetry in-place.
//
// Wire format (30 bytes = kTelemetryFrameSize):
//   [0]      kOpTelemetry (opcode)
//   [1..2]   rawMv       (big-endian, mV before display filtering)
//   [3..4]   dspMv       (big-endian, mV after display filtering)
//   [5]      batPercent  (0..100)
//   [6]      current mood id (EyeMood enum, skips 4)
//   [7]      charging flag (0/1)
//   [8]      drive calibration eligible flag (0/1)
//   [9]      drive session state (DriveSession::State enum)
//   [10..29] DEAD PAYLOAD -- see below.
//
// Bytes 10..29 hold ten big-endian uint16 microsecond calibration values
// (left {stop, fwdStart, revStart, fwdMax, revMax} then the same five for
// right). They ARE NOT DECODED by any client -- the phone-side calibration
// UI that consumed them was retired long ago, and `grep CAL_KEYS src lib
// docs` returns nothing. `src/override_html.cpp:273` asserts the wire is
// exactly 30 bytes and reads only bytes 0, 3, 5, 6, 7, 8, 9. That check
// -- and `test/test_motor_us_contract.py` around lines 161..193, which
// asserts each of these 10 values is written in this exact big-endian
// layout -- pins the frame size to 30 today.
//
// Shrinking the frame (option (a) in the task list) is deliberately NOT
// done here because it requires coordinated changes in files outside
// this agent's ownership:
//   * src/override_html.cpp    -- byteLength check must drop 30 -> 10.
//   * test/test_motor_us_contract.py -- the "10 uint16 values" pin must
//                                       be removed and the frame-size
//                                       assertion updated to 10.
// Until those two land together with a matching kTelemetryFrameSize
// reduction here, leave the wire format alone: shrinking one side
// unilaterally hangs a mismatched-length assertion on every telemetry
// tick and breaks the host test suite. ~100 B/s of trash over softAP is
// not free but it is not on any hot budget either.
//
// TODO(cross-file): when the three-file change lands, drop the writes to
// bytes 10..29, cut kTelemetryFrameSize to 10, and update the two files
// above. Keep this comment as a tombstone until then.
void orBuildTelemetry(uint8_t* out) {
  const DriveCalibration::Snapshot calibration = g_driveCalibration.snapshot();

  const uint16_t rawMv = static_cast<uint16_t>(g_batVoltageRaw * 1000.0f + 0.5f);
  const uint16_t dspMv = static_cast<uint16_t>(g_batVoltage    * 1000.0f + 0.5f);
  out[0]  = kOpTelemetry;
  out[1]  = static_cast<uint8_t>(rawMv >> 8);
  out[2]  = static_cast<uint8_t>(rawMv & 0xFF);
  out[3]  = static_cast<uint8_t>(dspMv >> 8);
  out[4]  = static_cast<uint8_t>(dspMv & 0xFF);
  out[5]  = g_batPercent;
  out[6]  = static_cast<uint8_t>(g_eyes.getMood());
  out[7]  = g_batCharging ? 1 : 0;
  out[8]  = g_driveCalibration.eligible() ? 1U : 0U;
  out[9]  = static_cast<uint8_t>(g_driveSession.state());
  const uint16_t values[] = {
      calibration.left.stopUs,
      calibration.left.fwdStartUs,
      calibration.left.revStartUs,
      calibration.left.fwdMaxUs,
      calibration.left.revMaxUs,
      calibration.right.stopUs,
      calibration.right.fwdStartUs,
      calibration.right.revStartUs,
      calibration.right.fwdMaxUs,
      calibration.right.revMaxUs,
  };
  for (size_t i = 0; i < 10U; ++i) {
    out[10U + i * 2U] = static_cast<uint8_t>(values[i] >> 8);
    out[11U + i * 2U] = static_cast<uint8_t>(values[i] & 0xFFU);
  }
}

void publishWsTelemetrySnapshot() {
  uint8_t frame[kTelemetryFrameSize];
  orBuildTelemetry(frame);
  const uint32_t oddSequence =
      g_wsTelemetrySequence.fetch_add(1, std::memory_order_acq_rel) + 1U;
  for (size_t i = 0; i < kTelemetryFrameSize; ++i) {
    g_wsTelemetryFrame[i].store(frame[i], std::memory_order_relaxed);
  }
  g_wsTelemetrySequence.store(oddSequence + 1U, std::memory_order_release);
}

bool copyWsTelemetrySnapshot(uint8_t* out) {
  constexpr uint8_t kMaxSnapshotAttempts = 4;
  for (uint8_t attempt = 0; attempt < kMaxSnapshotAttempts; ++attempt) {
    const uint32_t before =
        g_wsTelemetrySequence.load(std::memory_order_acquire);
    if ((before & 1U) != 0U) {
      continue;
    }
    for (size_t i = 0; i < kTelemetryFrameSize; ++i) {
      out[i] = g_wsTelemetryFrame[i].load(std::memory_order_relaxed);
    }
    const uint32_t after =
        g_wsTelemetrySequence.load(std::memory_order_acquire);
    if (before == after && (after & 1U) == 0U) {
      return true;
    }
  }
  return false;
}

bool enqueueWsEventWhileGuarded(const WsEvent& event) {
  if (g_wsEventQueue == nullptr ||
      xQueueSend(g_wsEventQueue, &event, 0) != pdTRUE) {
    g_wsQueueOverflowCount.fetch_add(1, std::memory_order_relaxed);
    latchDriveEventQueueEmergency();
    if ((event.transportFlags & kWsEventDriveSessionOwner) != 0U) {
      g_driveOwnerLossPending.store(true, std::memory_order_release);
    }
    return false;
  }
  return true;
}

bool enqueueWsAdmissionEventWhileGuarded(const WsEvent& event) {
  if (g_wsEventQueue == nullptr ||
      xQueueSend(g_wsEventQueue, &event, 0) != pdTRUE) {
    g_wsQueueOverflowCount.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

bool enqueueWsEventForCapturedOwnerWhileGuarded(const WsEvent& event) {
  if ((event.transportFlags & kWsEventDriveSessionOwner) != 0U) {
    return enqueueWsEventWhileGuarded(event);
  }
  return enqueueWsAdmissionEventWhileGuarded(event);
}

bool enqueueWsEvent(const WsEvent& event) {
  WsIngressGuard ingress;
  if (!ingress) {
    return false;
  }
  return enqueueWsEventWhileGuarded(event);
}

void terminateWsTransportForProtocolFault(
    AsyncWebSocketClient* client, WsEvent& event) {
  event.transportFlags |= kWsEventTerminalFault;
  if (faultAndRevokeWsTransport(event)) {
    if ((event.transportFlags & kWsEventDriveSessionOwner) != 0U) {
      g_driveOwnerLossPending.store(true, std::memory_order_release);
    }
    enqueueWsEventForCapturedOwnerWhileGuarded(event);
  }
  client->close(1002, "Protocol fault");
}

// AsyncTCP context: own transport identity/lifetime, send only the published
// atomic snapshot, and parse/copy/enqueue bounded records. Production state
// is exclusively consumed by the Arduino task.
void orOnWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                 AwsEventType type, void* arg, uint8_t* data, size_t len) {
  (void)server;
  if (client == nullptr) {
    return;
  }

  const uintptr_t token = reinterpret_cast<uintptr_t>(client);
  const uint32_t clientId = client->id();
  const uint32_t now = millis();

  // Runs on the AsyncTCP task, so a crash here would otherwise be attributed
  // to whatever the main loop happened to be doing.
  breadcrumb(type == WS_EVT_CONNECT      ? kCrumbWsConnect
             : type == WS_EVT_DISCONNECT ? kCrumbWsDisconnect
                                         : kCrumbWsData,
             clientId);

  // Step markers inside this one function. The crash lands here with no
  // backtrace -- the panic handler cannot print over USB CDC, so TG1WDT resets
  // the chip first (Saved PC decodes to panic_handler itself). The breadcrumb
  // is therefore the only witness, so it records the sub-step as well as the
  // client: detail = clientId | step << 8.
  auto wsStep = [clientId](uint32_t step) {
    breadcrumb(kCrumbWsData, clientId | (step << 8));
  };
  wsStep(1);

  if (type == WS_EVT_CONNECT) {
    wsStep(2);
    AsyncWebServerRequest* request =
        static_cast<AsyncWebServerRequest*>(arg);
    if (!requestHasValidAccessToken(request)) {
      // RELY-6: gate on serialCanWrite() so a full CDC FIFO drops the line
      // instead of truncating it mid-frame. No rate-limit -- a
      // token-missing upgrade past WsAuthGuardHandler is diagnostic and
      // we want every occurrence when a host is reading.
      if (serialCanWrite()) {
        Serial.printf("ws: client #%lu rejected; activation token required\n",
                      static_cast<unsigned long>(clientId));
      }
      wsStep(3);
      client->close(1008, "Forbidden");
      wsStep(4);
      return;
    }
  }

  WsIngressGuard ingress;
  wsStep(5);
  if (!ingress) {
    wsStep(6);
    if (type != WS_EVT_DISCONNECT) {
      client->close();
    }
    wsStep(7);
    return;
  }

  WsEvent event = {};
  event.client = clientId;
  event.receivedAtMs = now;
  event.transportToken = token;

  if (type == WS_EVT_CONNECT) {
    bool accepted = false;
    bool activationFaulted = false;
    bool transportFaultClosePending = false;
    lockWsTransportState();
    event.transportGeneration =
        g_wsTransportGeneration.load(std::memory_order_relaxed);
    activationFaulted =
        g_activationTerminalFaulted.load(std::memory_order_relaxed);
    transportFaultClosePending =
        g_wsTransportFaultToken.load(std::memory_order_relaxed) != 0U &&
        g_wsTransportFaultGeneration.load(std::memory_order_relaxed) ==
            event.transportGeneration;
    if (!activationFaulted && !transportFaultClosePending &&
        g_wsTransportOwnerToken.load(std::memory_order_relaxed) == 0U) {
      g_wsTransportOwnerClient.store(clientId, std::memory_order_relaxed);
      g_wsTransportOwnerGeneration.store(
          event.transportGeneration, std::memory_order_relaxed);
      g_wsTransportLastTelemetryMs.store(0, std::memory_order_relaxed);
      g_wsTransportOwnerToken.store(token, std::memory_order_release);
      g_wsTransportOpenCount.store(1U, std::memory_order_release);
      // Arm the liveness clock AT ADMISSION.
      //
      // This line is the whole reason a phone could not connect. The stale-
      // owner reaper fires when (now - g_lastControlEventMs) exceeds 8 s, but
      // g_lastControlEventMs was only ever written when a control FRAME
      // arrived -- never when a client was admitted. It starts at 0, so a
      // client admitted at millis()=90000 was instantly judged to have been
      // silent for 90 s, and the reaper killed it on the very next loop pass,
      // before the phone had sent its first frame. Captured on hardware:
      //
      //     ws: client #1 connected
      //     Guru Meditation Error: Core 1 panic'ed (StoreProhibited)
      //
      // The owner has by definition just been heard from. Say so.
      g_lastControlEventMs = now;
      accepted = true;
    }
    unlockWsTransportState();

    if (!accepted) {
      // RELY-6: a stubborn phone can churn connect -> reject -> close ->
      // reconnect at ~2-5 Hz, which is fast enough to overrun the CDC FIFO
      // and truncate the printf mid-line. Gate on serialCanWrite() so we
      // drop cleanly rather than corrupt, and rate-limit to 1 Hz to match
      // revokeWsEventClient's pattern -- the reason string does not change
      // between reject attempts on the same fault, so once-a-second is
      // enough to see it without swamping the CDC.
      static uint32_t s_lastRejectLogMs = 0;
      const uint32_t nowMs = millis();
      if (static_cast<int32_t>(nowMs - s_lastRejectLogMs) >= 1000 &&
          serialCanWrite()) {
        s_lastRejectLogMs = nowMs;
        Serial.printf("ws: client #%lu rejected; %s\n",
                      static_cast<unsigned long>(clientId),
                      activationFaulted
                          ? "activation terminal-faulted"
                          : (transportFaultClosePending
                                 ? "faulted transport closing"
                                 : "transport owner busy"));
      }
      client->close(1008,
                    (activationFaulted || transportFaultClosePending)
                        ? "Unavailable"
                        : "Owner busy");
      return;
    }

    event.type = WsEventType::Connect;
    event.transportFlags = kWsEventTransportOwner;
    if (!enqueueWsAdmissionEventWhileGuarded(event)) {
      lockWsTransportState();
      if (g_wsTransportOwnerToken.load(std::memory_order_relaxed) == token &&
          g_wsTransportOwnerClient.load(std::memory_order_relaxed) ==
              clientId &&
          g_wsTransportOwnerGeneration.load(std::memory_order_relaxed) ==
              event.transportGeneration) {
        g_wsTransportOwnerToken.store(0, std::memory_order_release);
        g_wsTransportOwnerClient.store(0, std::memory_order_relaxed);
        g_wsTransportOwnerGeneration.store(0, std::memory_order_relaxed);
        g_wsTransportOpenCount.store(0U, std::memory_order_release);
      }
      unlockWsTransportState();
      client->close();
      return;
    }

    uint8_t frame[kTelemetryFrameSize];
    if (g_wsTransportOwnerToken.load(std::memory_order_acquire) != token ||
        g_wsTransportOwnerGeneration.load(std::memory_order_acquire) !=
            g_wsTransportGeneration.load(std::memory_order_acquire)) {
      client->close();
      return;
    }
    if (copyWsTelemetrySnapshot(frame) &&
        client->status() == WS_CONNECTED && !client->queueIsFull()) {
      client->binary(frame, sizeof(frame));
      g_wsTransportLastTelemetryMs.store(now, std::memory_order_release);
    }
    return;
  }

  if (type == WS_EVT_DISCONNECT) {
    bool ownerDisconnected = false;
    lockWsTransportState();
    event.transportGeneration =
        g_wsTransportGeneration.load(std::memory_order_relaxed);
    if (g_wsTransportFaultToken.load(std::memory_order_relaxed) == token &&
        g_wsTransportFaultGeneration.load(std::memory_order_relaxed) ==
            event.transportGeneration) {
      // Retire only the per-transport marker. The activation-wide terminal
      // latch persists until credentials are invalidated or regenerated.
      g_wsTransportFaultToken.store(0, std::memory_order_release);
      g_wsTransportFaultGeneration.store(0, std::memory_order_relaxed);
    }
    if (g_wsTransportOwnerToken.load(std::memory_order_relaxed) == token &&
        g_wsTransportOwnerClient.load(std::memory_order_relaxed) == clientId &&
        g_wsTransportOwnerGeneration.load(std::memory_order_relaxed) ==
            event.transportGeneration) {
      event.transportGeneration =
          g_wsTransportOwnerGeneration.load(std::memory_order_relaxed);
      event.transportFlags = kWsEventTransportOwner;
      if (g_driveSessionTransportToken.load(std::memory_order_acquire) ==
              token &&
          g_driveSessionTransportGeneration.load(std::memory_order_acquire) ==
              event.transportGeneration) {
        event.transportFlags |= kWsEventDriveSessionOwner;
      }
      g_wsTransportOwnerToken.store(0, std::memory_order_release);
      g_wsTransportOwnerClient.store(0, std::memory_order_relaxed);
      g_wsTransportOwnerGeneration.store(0, std::memory_order_relaxed);
      g_wsTransportLastTelemetryMs.store(0, std::memory_order_relaxed);
      g_wsTransportOpenCount.store(0U, std::memory_order_release);
      ownerDisconnected = true;
    }
    unlockWsTransportState();
    if (!ownerDisconnected) {
      return;
    }
    event.type = WsEventType::Disconnect;
    if (!enqueueWsEventForCapturedOwnerWhileGuarded(event) &&
        (event.transportFlags & kWsEventDriveSessionOwner) != 0U) {
      g_driveOwnerLossPending.store(true, std::memory_order_release);
    }
    return;
  }

  lockWsTransportState();
  event.transportGeneration =
      g_wsTransportGeneration.load(std::memory_order_relaxed);
  const bool currentOwner =
      g_wsTransportOwnerToken.load(std::memory_order_relaxed) == token &&
      g_wsTransportOwnerClient.load(std::memory_order_relaxed) == clientId &&
      g_wsTransportOwnerGeneration.load(std::memory_order_relaxed) ==
          event.transportGeneration;
  unlockWsTransportState();
  wsStep(20);
  if (!currentOwner) {
    // Ignore the frame. Do NOT close.
    //
    // Closing here is a use-after-free, and it is what crashed the board the
    // moment a phone switched WiFi networks. Leaving the AP kills the TCP with
    // no FIN, so the socket lingers; the stale-owner reaper then releases
    // ownership, which makes this once-legitimate client a non-owner. Its next
    // late frame or RST lands here, close() runs on a client AsyncTCP is
    // already tearing down, and the following _recv reads freed memory:
    //
    //     Guru Meditation Error: Core 1 panic'ed (LoadProhibited)
    //     AsyncWebSocketClient::_onData (AsyncWebSocket.cpp:639)
    //       <- _recv <- _s_recv <- _async_service_task
    //
    // Nothing needs the close. Unauthorized clients never reach here -- they
    // are refused at the HTTP upgrade by WsAuthGuardHandler ("ws: upgrade
    // refused at handshake"). A rejected client is closed in the CONNECT
    // branch, which runs before the client is live. What is left here is only
    // a ghost, and the correct treatment of a ghost's traffic is to drop it
    // and let AsyncTCP reap the socket on its own timeout.
    wsStep(21);
    return;
  }
  event.transportFlags = kWsEventTransportOwner;
  if (g_driveSessionTransportToken.load(std::memory_order_acquire) == token &&
      g_driveSessionTransportGeneration.load(std::memory_order_acquire) ==
          event.transportGeneration) {
    event.transportFlags |= kWsEventDriveSessionOwner;
  }
  if (type == WS_EVT_ERROR) {
    event.type = WsEventType::ProtocolFailure;
    terminateWsTransportForProtocolFault(client, event);
    return;
  }
  if (type != WS_EVT_DATA) {
    return;
  }

  AwsFrameInfo* info = static_cast<AwsFrameInfo*>(arg);
  if (info == nullptr || !info->final || info->index != 0 ||
      info->len != len || info->opcode != WS_BINARY || len == 0) {
    event.type = WsEventType::MalformedControl;
    terminateWsTransportForProtocolFault(client, event);
    return;
  }

  bool telemetryTraffic = false;
  bool closeAfterEvent = false;
  const uint8_t op = data[0];
  switch (op) {
    case kOpControl: {
      if (len != 4U) {
        event.type = WsEventType::MalformedControl;
        closeAfterEvent = true;
        break;
      }
      event.type = WsEventType::Control;
      event.left = static_cast<int8_t>(data[1]);
      event.right = static_cast<int8_t>(data[2]);
      event.seq = data[3];
      telemetryTraffic = true;
      break;
    }
    case kOpTrim: {
      if (len != 6U) {
        event.type = WsEventType::MalformedControl;
        closeAfterEvent = true;
      } else {
        event.type = WsEventType::Trim;
        memcpy(event.values, &data[1], sizeof(event.values));
        event.seq = data[5];
      }
      break;
    }
    case kOpMood: {
      if (len != 3U) {
        event.type = WsEventType::ProtocolFailure;
        closeAfterEvent = true;
        break;
      }
      event.type = WsEventType::Mood;
      event.values[0] = data[1];
      event.seq = data[2];
      telemetryTraffic = true;
      break;
    }
    case kOpGesture: {
      if (len != 3U) {
        event.type = WsEventType::ProtocolFailure;
        closeAfterEvent = true;
        break;
      }
      event.type = WsEventType::Gesture;
      event.values[0] = data[1];
      event.seq = data[2];
      telemetryTraffic = true;
      break;
    }
    case kOpSound: {
      if (len != 3U) {
        event.type = WsEventType::ProtocolFailure;
        closeAfterEvent = true;
        break;
      }
      event.type = WsEventType::Sound;
      event.values[0] = data[1];
      event.seq = data[2];
      telemetryTraffic = true;
      break;
    }
    default:
      event.type = WsEventType::ProtocolFailure;
      event.values[0] = op;
      closeAfterEvent = true;
      break;
  }

  if (closeAfterEvent) {
    terminateWsTransportForProtocolFault(client, event);
    return;
  }
  if (!enqueueWsEventForCapturedOwnerWhileGuarded(event)) {
    client->close();
    return;
  }
  if (!telemetryTraffic) {
    return;
  }

  const uint32_t lastTelemetryMs =
      g_wsTransportLastTelemetryMs.load(std::memory_order_acquire);
  if (static_cast<uint32_t>(now - lastTelemetryMs) <
      kOverrideTelemetryMs) {
    return;
  }
  uint8_t frame[kTelemetryFrameSize];
  breadcrumb(kCrumbWsTelemetry);
  if (!copyWsTelemetrySnapshot(frame) ||
      client->status() != WS_CONNECTED || client->queueIsFull()) {
    return;
  }
  if (g_wsTransportOwnerToken.load(std::memory_order_acquire) != token ||
      g_wsTransportOwnerGeneration.load(std::memory_order_acquire) !=
          g_wsTransportGeneration.load(std::memory_order_acquire)) {
    client->close();
    return;
  }
  uint32_t expectedTelemetryMs = lastTelemetryMs;
  if (g_wsTransportLastTelemetryMs.compare_exchange_strong(
          expectedTelemetryMs, now, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    client->binary(frame, sizeof(frame));
  }
}

void serviceDriveSessionTick(uint32_t now) {
  const DriveSession::Decision tick = g_driveSession.tick(now);
  applyDriveDecision(tick, "session-tick", g_driveSession.owner(),
                     g_lastDriveSeq, 0, 0, now);
}

bool handleWsQueueEmergency() {
  if (!g_wsQueueEmergency.load(std::memory_order_acquire)) {
    return false;
  }

  resetDriveControl("WS queue overflow");
  Serial.printf(
      "drive emergency: WS queue overflow count=%lu\n",
      static_cast<unsigned long>(
          g_wsQueueOverflowCount.load(std::memory_order_relaxed)));
  return true;
}

bool handleDriveHardwareTrip() {
  if (!g_driveHardwareTripPending.exchange(
          false, std::memory_order_acq_rel)) {
    return false;
  }

  resetDriveControl("independent 300ms drive cutoff");
  Serial.printf(
      "drive emergency: independent EN cutoff count=%lu\n",
      static_cast<unsigned long>(
          g_driveHardwareTripCount.load(std::memory_order_relaxed)));
  return true;
}

static void serviceDriveControl(uint32_t now) {
  constexpr uint8_t kMaxEventsPerService = 8;
  constexpr uint32_t kMaxEventBatchMs = 5;

  if (handleDriveHardwareTrip()) {
    return;
  }
  if (handleWsQueueEmergency()) {
    return;
  }
  serviceDriveSessionTick(now);
  if (handleWsQueueEmergency()) {
    return;
  }

  const uint32_t batchStartMs = millis();
  WsEvent event = {};
  for (uint8_t processed = 0; processed < kMaxEventsPerService; ++processed) {
    if (processed != 0U &&
        static_cast<uint32_t>(millis() - batchStartMs) >=
            kMaxEventBatchMs) {
      break;
    }
    if (handleWsQueueEmergency()) {
      return;
    }
    if (handleDriveHardwareTrip()) {
      return;
    }
    if (g_wsEventQueue == nullptr ||
        xQueueReceive(g_wsEventQueue, &event, 0) != pdTRUE) {
      break;
    }

    const uint32_t eventNow = millis();
    serviceDriveSessionTick(eventNow);
    if (handleWsQueueEmergency()) {
      return;
    }
    if (event.transportGeneration !=
        g_wsTransportGeneration.load(std::memory_order_acquire)) {
      if (event.type != WsEventType::Disconnect &&
          event.type != WsEventType::TransportFailure) {
        revokeWsEventClient(event, "Stale");
      }
      continue;
    }
    const bool terminalFault =
        (event.transportFlags & kWsEventTerminalFault) != 0U;
    if (!terminalFault && wsEventTransportFaulted(event)) {
      revokeWsEventClient(event, "Protocol fault");
      continue;
    }
    const bool ownerTransport =
        (event.transportFlags & kWsEventTransportOwner) != 0U;
    const bool driveSessionOwner =
        (event.transportFlags & kWsEventDriveSessionOwner) != 0U;
    if (event.type != WsEventType::TransportFailure && !ownerTransport) {
      if (event.type != WsEventType::Disconnect) {
        revokeWsEventClient(event, "Forbidden");
      }
      continue;
    }

    switch (event.type) {
      case WsEventType::Connect: {
        if (g_activationTerminalFaulted.load(std::memory_order_acquire)) {
          rejectWsTransportAdmission(event, "Unavailable");
          Serial.printf(
              "ws: client #%lu rejected; activation terminal-faulted\n",
              static_cast<unsigned long>(event.client));
          break;
        }
        if (!wsEventMatchesCurrentTransport(event)) {
          revokeWsEventClient(event, "Stale");
          Serial.printf("ws: client #%lu discarded; stale transport connect\n",
                        static_cast<unsigned long>(event.client));
          break;
        }
        if (g_orState == ORS_IDLE || g_systemShuttingDown || g_paused) {
          rejectWsTransportAdmission(event, "Unavailable");
          Serial.printf("ws: client #%lu rejected; override unavailable\n",
                        static_cast<unsigned long>(event.client));
          break;
        }
        if (g_driveSession.state() == DriveSession::State::Shutdown ||
            g_driveSession.state() == DriveSession::State::Manual ||
            g_driveSession.hasOwner()) {
          rejectWsTransportAdmission(
              event,
              g_driveSession.hasOwner() ? "Owner busy" : "Unavailable");
          Serial.printf("ws: client #%lu rejected; drive session %s\n",
                        static_cast<unsigned long>(event.client),
                        g_driveSession.hasOwner() ? "owner busy" : "unavailable");
          break;
        }
        const bool admittedFromWifiBoot = g_orState == ORS_WIFI_BOOT;
        const bool admittedFromGrace = g_orState == ORS_GRACE;
        DriveSession::Decision decision = {};
        if (!publishProvisionalDriveSessionTransport(event, decision)) {
          revokeWsEventClient(event, "Stale");
          Serial.printf("ws: client #%lu discarded; transport closed during admission\n",
                        static_cast<unsigned long>(event.client));
          break;
        }
        Serial.printf("ws: client #%lu connected\n",
                      static_cast<unsigned long>(event.client));
        if (decision.reason != DriveSession::Reason::Connected) {
          clearDriveSessionTransportIfMatches(event);
          rejectWsTransportAdmission(event, "Unavailable");
          Serial.printf("ws: client #%lu rejected; drive admission reason=%s\n",
                        static_cast<unsigned long>(event.client),
                        driveReasonName(decision.reason));
          break;
        }
        if (!finalizeDriveSessionTransportAdmission(event)) {
          rollbackProvisionalDriveAdmission(event, eventNow);
          revokeWsEventClient(event, "Stale");
          Serial.printf(
              "ws: client #%lu rolled back; transport closed during admission\n",
              static_cast<unsigned long>(event.client));
          break;
        }
        applyDriveDecision(
            decision, "ws-connect", event.client, 0, 0, 0, eventNow);
        if (admittedFromWifiBoot || admittedFromGrace) {
          g_orState = ORS_RUNNING;
          g_orStateEnterMs = eventNow;
          if (admittedFromWifiBoot) {
            enterLcdQrWifiMode();
            Serial.println(F("override: -> RUNNING (admitted during WIFI_BOOT)"));
          } else {
            Serial.println(F("override: GRACE -> RUNNING (WS admitted)"));
          }
        }
        g_lcdNextPollMs = 0;
        break;
      }
      case WsEventType::Disconnect: {
        if (!driveSessionOwner ||
            !driveSessionOwnerMatchesEvent(event)) {
          break;
        }
        Serial.printf("ws: client #%lu disconnected\n",
                      static_cast<unsigned long>(event.client));
        const DriveSession::Decision decision =
            g_driveSession.disconnect(event.client);
        const bool admittedOwnerLost =
            decision.reason == DriveSession::Reason::Disconnected &&
            !g_driveSession.hasOwner();
        g_driveSessionTransportToken.store(0, std::memory_order_release);
        g_driveSessionTransportGeneration.store(
            0, std::memory_order_release);
        applyDriveDecision(
            decision, "ws-disconnect", event.client, 0, 0, 0, eventNow);
        g_driveOwnerLossPending.store(
            admittedOwnerLost, std::memory_order_release);
        g_lcdNextPollMs = 0;
        break;
      }
      case WsEventType::Control: {
        if (!driveSessionOwner) {
          break;
        }
        if (!wsEventMatchesCurrentTransport(event) ||
            !driveSessionOwnerMatchesEvent(event)) {
          rejectWsTransportAdmission(event, "Forbidden");
          break;
        }
        if (static_cast<uint32_t>(eventNow - event.receivedAtMs) >=
            DriveSession::kControlTimeoutMs) {
          resetDriveControl("stale queued WS control");
          Serial.printf("ws: client #%lu stale queued control age=%lums\n",
                        static_cast<unsigned long>(event.client),
                        static_cast<unsigned long>(
                            eventNow - event.receivedAtMs));
          break;
        }
        if (g_orState != ORS_RUNNING && g_orState != ORS_GRACE) {
          rejectWsTransportAdmission(event, "Unavailable");
          break;
        }
        ++g_diagCtrlRx;
        g_lastRawDriveL = event.left;
        g_lastRawDriveR = event.right;
        g_lastControlEventMs = event.receivedAtMs;
        g_lastDriveSource = "ws";
        g_lastDriveClient = event.client;
        g_lastDriveSeq = event.seq;
        const DriveSession::Decision decision = g_driveSession.frame(
            event.client, event.seq, event.left, event.right,
            event.receivedAtMs,
            g_driveCalibration.eligible());
        // A running expression owns the motors. The session still SEES every
        // frame — starving it would trip its 300 ms control timeout and cut
        // the rail mid-gesture — but its decision is not written out, so the
        // joystick's periodic neutral frames cannot fight the gesture for the
        // output every loop.
        //
        // This could not happen before: the movement buttons lived on the LCD
        // and were only reachable with no web client connected. Now the web app
        // is the only trigger, so a gesture ALWAYS runs alongside a live
        // control session.
        if (g_movementSequencer.active()) {
          break;
        }
        applyDriveDecision(decision, "ws", event.client, event.seq,
                           event.left, event.right, eventNow);
        break;
      }
      case WsEventType::Mood: {
        if (!driveSessionOwner) {
          break;
        }
        if (!wsEventMatchesCurrentTransport(event) ||
            !driveSessionOwnerMatchesEvent(event)) {
          rejectWsTransportAdmission(event, "Forbidden");
          break;
        }
        if (!isValidMoodId(event.values[0])) {
          resetDriveControl("invalid WS mood");
          Serial.printf("ws: invalid mood %u; session reset\n",
                        static_cast<unsigned>(event.values[0]));
          break;
        }
        ++g_diagMoodRx;
        stopDemo(false);
        setEyesMood(static_cast<EyeMood>(event.values[0]));
        setEyeLoopRate(true);
        requestImmediateFrame();
        break;
      }
      case WsEventType::Gesture: {
        if (!driveSessionOwner) {
          break;
        }
        if (!wsEventMatchesCurrentTransport(event) ||
            !driveSessionOwnerMatchesEvent(event)) {
          rejectWsTransportAdmission(event, "Forbidden");
          break;
        }
        // A movement takes the drive motors, so it goes through exactly the
        // same entry point the on-screen menu used — inheriting the drive
        // mutex, the 300 ms lease, the emergency latch and the clean
        // powerdown. The web app is now the only trigger, which makes this
        // path load-bearing rather than a convenience.
        // Indexes the MOVEMENT TABLE, not the LCD menu's button list. The LCD
        // list is a 3-item fallback with a conditional "run test" slot; using
        // it here would both cap the web vocabulary at 3 and put a
        // non-movement inside the movement index space.
        const uint8_t id = event.values[0];
        const char* gestureName = MovementExpression::patternNameAt(id);
        if (gestureName == nullptr) {
          Serial.printf("ws: invalid gesture %u ignored\n",
                        static_cast<unsigned>(id));
          break;
        }
        Serial.printf("ws: gesture %s\n", gestureName);
        startMovementGesture(gestureName);
        break;
      }
      case WsEventType::Sound: {
        if (!driveSessionOwner) {
          break;
        }
        if (!wsEventMatchesCurrentTransport(event) ||
            !driveSessionOwnerMatchesEvent(event)) {
          rejectWsTransportAdmission(event, "Forbidden");
          break;
        }
        // Payload byte selects the MOOD to generate: 0=happy, 1=sad,
        // 2=confused. The mapping matches the JS `SOUNDS` array literal in
        // the served web app (["happy","sad","confused"]). Every trigger
        // composes a fresh phrase — see playMood.
        const uint8_t id = event.values[0];
        MelodyGen::Mood mood;
        switch (id) {
          case 0: mood = MelodyGen::Mood::Happy; break;
          case 1: mood = MelodyGen::Mood::Sad; break;
          case 2: mood = MelodyGen::Mood::Confused; break;
          default:
            Serial.printf("ws: invalid sound %u ignored\n",
                          static_cast<unsigned>(id));
            goto sound_done;
        }
        Serial.printf("ws: sound %s\n", MelodyGen::moodName(mood));
        playMood(mood);
      sound_done:
        break;
      }
      case WsEventType::Trim:
        if (!driveSessionOwner) {
          break;
        }
        if (!wsEventMatchesCurrentTransport(event) ||
            !driveSessionOwnerMatchesEvent(event)) {
          rejectWsTransportAdmission(event, "Forbidden");
          break;
        }
        Serial.printf("ws: client #%lu trim mutation rejected; use serial cal commands\n",
                      static_cast<unsigned long>(event.client));
        break;
      case WsEventType::MalformedControl:
        if (!driveSessionOwner) {
          break;
        }
        if (!driveSessionOwnerMatchesEvent(event)) {
          rejectWsTransportAdmission(event, "Forbidden");
          break;
        }
        resetDriveControl("malformed WS control");
        Serial.printf("ws: client #%lu malformed control; session reset\n",
                      static_cast<unsigned long>(event.client));
        break;
      case WsEventType::ProtocolFailure:
        if (!driveSessionOwner) {
          break;
        }
        if (!driveSessionOwnerMatchesEvent(event)) {
          rejectWsTransportAdmission(event, "Forbidden");
          break;
        }
        resetDriveControl("WS protocol failure");
        Serial.printf("ws: client #%lu protocol failure opcode=0x%02X\n",
                      static_cast<unsigned long>(event.client),
                      static_cast<unsigned>(event.values[0]));
        break;
      case WsEventType::TransportFailure:
        resetDriveControl("WiFi transport loss");
        Serial.println(F("drive emergency: WiFi transport loss"));
        break;
      case WsEventType::Unknown:
        if (!driveSessionOwner ||
            !driveSessionOwnerMatchesEvent(event)) {
          break;
        }
        resetDriveControl("unknown drive event");
        break;
    }

    if (handleWsQueueEmergency()) {
      return;
    }
    serviceDriveSessionTick(millis());
  }

  if (handleWsQueueEmergency()) {
    return;
  }
  if (handleDriveHardwareTrip()) {
    return;
  }
  const uint32_t tickNow = millis();
  serviceDriveSessionTick(tickNow);
  if (handleWsQueueEmergency()) {
    return;
  }
  // Gestures run before the idle power-down check so an active gesture keeps
  // refreshing the lease rather than being cut by the 1 s neutral timer during
  // its own deliberate pauses.
  serviceMovementGesture(tickNow);
  // Must run after the gesture service: it detects the spin finishing by the
  // sequencer going inactive, and keeps sampling through the coast afterwards.
  serviceTurnAutoMeasure(tickNow);
  // Runs after the measurement service so it defers to a calibration in
  // progress; it owns the gyro the rest of the time.
  serviceHeading(tickNow);
  serviceMotionEvents(tickNow);
  serviceTurnTestSequence(tickNow);
  serviceBench(tickNow);
  serviceStraightDrive(tickNow);
  serviceSurvey(tickNow);
  serviceRfScan(tickNow);
  serviceDriveIdlePowerdown(tickNow);
  serviceGestureMenu(tickNow);
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_START:
      if (g_apStartupInProgress.load(std::memory_order_acquire) &&
          g_apStartupStopSeen.load(std::memory_order_acquire)) {
        g_apStartupInProgress.store(false, std::memory_order_release);
        g_apStartupStopSeen.store(false, std::memory_order_release);
        Serial.println(F("wifi-evt: AP_START stable after startup reconfigure"));
        break;
      }
      Serial.println(F("wifi-evt: AP_START"));
      break;
    case ARDUINO_EVENT_WIFI_AP_STOP: {
      const uint32_t now = millis();
      const uint32_t startupDeadline =
          g_apStartupDeadlineMs.load(std::memory_order_acquire);
      if (g_apStartupInProgress.load(std::memory_order_acquire) &&
          static_cast<int32_t>(startupDeadline - now) >= 0) {
        g_apStartupStopSeen.store(true, std::memory_order_release);
        Serial.println(F("wifi-evt: AP_STOP during startup reconfigure; ignored"));
        break;
      }
      if (g_apExpectedStopPending.exchange(
              false, std::memory_order_acq_rel)) {
        Serial.println(F("wifi-evt: AP_STOP expected; lifecycle released"));
        break;
      }
      const bool unexpected =
          g_apLifecycleActive.exchange(false, std::memory_order_acq_rel);
      if (!unexpected) {
        Serial.println(F("wifi-evt: AP_STOP while already down; ignored"));
        break;
      }
      invalidateActivationCredentials();
      g_apStopFaultPending.store(true, std::memory_order_release);
      Serial.println(F("wifi-evt: AP_STOP unexpected; safe teardown queued"));
      break;
    }
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
      breadcrumb(kCrumbApStaConnected);
      // Heap is logged on the connect edge because a phone joining an AP does
      // not send one request — it fires a burst of captive-portal probes, and
      // AsyncTCP allocates per connection. If a restart follows a client
      // joining, this line is the last reading before it and shows whether
      // memory was already collapsing.
      //
      // RELY-6: gate on serialCanWrite() only (no rate-limit) so a full CDC
      // FIFO drops the line instead of stalling the WiFi event task mid-
      // printf. Every joining phone gets one STA_CONNECTED, so with a live
      // reader we still see them all; without one, we never lose our
      // crash-adjacent evidence to a truncated line either.
      if (serialCanWrite()) {
        Serial.printf("wifi-evt: AP_STACONNECTED  client mac=%02X:%02X:%02X:%02X:%02X:%02X"
            "  heap=%lu min=%lu largest=%lu\n",          info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
            info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3],
            info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5],
            static_cast<unsigned long>(ESP.getFreeHeap()),
            static_cast<unsigned long>(ESP.getMinFreeHeap()),
            static_cast<unsigned long>(ESP.getMaxAllocHeap()));
      }
      break;
    case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
      // RELY-6: gate on serialCanWrite() only. Paired with the connect
      // burst above -- if a phone bounces on and off, the two events
      // interleave and it is that interleave that saturates the CDC. Drop
      // the line rather than truncate.
      if (serialCanWrite()) {
        Serial.printf("wifi-evt: AP_STADISCONNECTED  client mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
            info.wifi_ap_stadisconnected.mac[0], info.wifi_ap_stadisconnected.mac[1],
            info.wifi_ap_stadisconnected.mac[2], info.wifi_ap_stadisconnected.mac[3],
            info.wifi_ap_stadisconnected.mac[4], info.wifi_ap_stadisconnected.mac[5]);
      }
      break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
      // RELY-6: gate on serialCanWrite() only. Follows CONNECT for every
      // client that requests DHCP; part of the same burst.
      if (serialCanWrite()) {
        Serial.println(F("wifi-evt: AP_STAIPASSIGNED (DHCP lease handed out)"));
      }
      break;
    case ARDUINO_EVENT_WIFI_AP_PROBEREQRECVED:
      // suppressed — fires for every scan, too noisy
      break;
    default:
      break;
  }
}

// Captive-portal flow REMOVED (user request, round-3): some networks blocked
// the AP-style probe redirect. Users scan the tokenized URL QR in real Safari.
// orHandleCaptive stays as a 404 helper to avoid leaking implementation paths.
void orHandleCaptive(AsyncWebServerRequest* req) {
  breadcrumb(kCrumbHttpOther);
  req->send(404);
}

// ---------- Round 4: LCD QR screen + mode helpers ----------
// All functions in this block MUST be called from the main Arduino task
// (loop / setup / overrideLoop). They touch g_canvas + LCD SPI.

// Draw a single QR module at the given pixel scale (scale × scale on the LCD).
static inline void qrDrawModule(int16_t x, int16_t y, uint8_t scale) {
  g_canvas->fillRect(x, y, scale, scale, kQrBlack);
}

// Render a QR matrix into the canvas at the given pixel scale.
// originX/Y = top-left of the QR core (the white background of the canvas
// serves as the quiet zone).
static void drawQrToCanvas(QRCode* qr, int16_t originX, int16_t originY, uint8_t scale) {
  for (uint8_t my = 0; my < qr->size; ++my) {
    for (uint8_t mx = 0; mx < qr->size; ++mx) {
      if (qrcode_getModule(qr, mx, my)) {
        qrDrawModule(static_cast<int16_t>(originX + mx * scale),
                     static_cast<int16_t>(originY + my * scale),
                     scale);
      }
    }
  }
}

// Centered text helper — uses the built-in 6×8 GFX font (so width = 6 × size × len).
static void qrCenterText(int16_t y, uint8_t textSize, const char* s) {
  g_canvas->setTextSize(textSize);
  g_canvas->setTextColor(kQrBlack);
  g_canvas->setTextWrap(false);
  const int16_t textW = static_cast<int16_t>(strlen(s) * 6 * textSize);
  const int16_t x     = static_cast<int16_t>((kScreenW - textW) / 2);
  g_canvas->setCursor(x, y);
  g_canvas->print(s);
}

// DSGN-6: prefer size 2 for legibility; drop to size 1 only when the
// caller passed a string long enough to overflow the 240-px canvas at
// size 2. Keeps SSID/password readable across the room without truncating
// off-screen when a longer credential lands. Text color follows qrCenterText
// (black on the Wi-Fi QR white background).
static void qrCenterTextFit(int16_t y, const char* s) {
  const size_t len = strlen(s);
  const uint8_t textSize =
      (static_cast<int16_t>(len * 6 * 2) <= kScreenW) ? 2 : 1;
  qrCenterText(y, textSize, s);
}

// Like qrCenterText, but for the "Establishing Server..." dark splash:
// white text on black background.
static void qrCenterTextDark(int16_t y, uint8_t textSize, const char* s) {
  g_canvas->setTextSize(textSize);
  g_canvas->setTextColor(kQrWhite);
  g_canvas->setTextWrap(false);
  const int16_t textW = static_cast<int16_t>(strlen(s) * 6 * textSize);
  const int16_t x     = static_cast<int16_t>((kScreenW - textW) / 2);
  g_canvas->setCursor(x, y);
  g_canvas->print(s);
}

// Paint the "scan to join Wi-Fi" screen.
// Layout (240x280): big QR with instruction line and current credentials
// below. DSGN-6: previously the SSID/password were 8 px tall and there was
// no explanation of what the QR was for. QR module count is fixed (v4-M @
// scale 6 = 198 px, near the safe encoding limit for the ricmoo/QRCode
// library -- see PROJECT-HISTORY D17), so we hold the QR size constant
// and repurpose the space around it.
//
//   y=6    WIFI QR scale 6 (198x198) at x=21 (ends y=204)
//   y=210  size 1 "Scan to join Wi-Fi" instruction
//   y=224  size 2 (or 1 if it does not fit) current SSID
//   y=246  size 2 (or 1 if it does not fit) pw:<current password>
//
// Buffer size: version 4 needs ~150 B; 200 is comfortable headroom.
// Push the whole canvas as ordered horizontal bands so the new screen arrives
// travelling in a direction, instead of appearing all at once.
//
// Design-craft §6a: motion must follow the direction of the gesture that caused
// it. Forward progress through the pairing flow reveals downward (the operator
// swiped DOWN to get here); going backwards reveals upward.
//
// Design-craft §6b: this costs NOTHING extra and cannot stall the loop. It is
// the same total pixel count as the single full-screen push it replaces -- only
// the SPI window is set 8 times instead of once. There is no delay(), no frame
// pacing and no added state, which is exactly why it is safe on a board with a
// 5 s task watchdog and a history of loop-stall blackouts. A tweened transition
// would have needed either delay() (a pending reset) or a tick state machine
// (real complexity); an ordered repaint buys the directional read for free.
void pushCanvasBanded(bool downward) {
  constexpr int16_t kBands = 8;
  constexpr int16_t kBandH = kScreenH / kBands;
  for (int16_t i = 0; i < kBands; ++i) {
    const int16_t band = downward ? i : (kBands - 1 - i);
    const int16_t y    = static_cast<int16_t>(band * kBandH);
    const int16_t h    = (band == kBands - 1)
                             ? static_cast<int16_t>(kScreenH - y)
                             : kBandH;
    pushCanvasRectToLcd(0, y, kScreenW, h);
  }
}

// Delta compositor for the sheet transition.
//
// Pushes a horizontal strip of framebuffer rows [fromRow, toRow) straight onto
// the LCD, one canvas row per LCD row, at full width. The sheet grows or
// shrinks by a handful of pixels per frame, and the ONLY rows that need
// repainting are the ones that just changed sides:
//
//   Growing by d px  -> push sheet[old..new)  (d rows from the sheet canvas)
//   Shrinking by d   -> push face[new..old)   (d rows from the face canvas)
//
// The face canvas is not repainted -- the face is already sitting on the LCD
// from the previous FACE-mode push and stays there under everything the sheet
// does not currently cover. This is what makes the drag CHEAPER than the
// full-frame push it replaced (tens of rows per frame instead of 280).
//
// Modelled on pushCanvasRectToLcd's single-transaction pattern. The
// kCrumbLcdPush 1..5 step markers stay -- a TASK_WDT was once captured inside
// a canvas push and those detail codes name the stage that hung; deleting them
// here would blind the same investigation on the sheet path.
void pushSheetStrip(uint16_t* framebuffer, int16_t fromRow, int16_t toRow) {
  if ((framebuffer == nullptr) || (g_gfx == nullptr)) return;
  if (fromRow < 0)         fromRow = 0;
  if (toRow > kScreenH)    toRow   = kScreenH;
  if (toRow <= fromRow)    return;

  ++g_lcdPushCount;
  g_lcdLastPushMs = millis();

  Arduino_TFT* tft = static_cast<Arduino_TFT*>(g_gfx);
  const int16_t h = static_cast<int16_t>(toRow - fromRow);
  breadcrumb(kCrumbLcdPush, 1);
  tft->startWrite();
  breadcrumb(kCrumbLcdPush, 2);
  tft->writeAddrWindow(0,
                       static_cast<uint16_t>(fromRow),
                       static_cast<uint16_t>(kScreenW),
                       static_cast<uint16_t>(h));
  breadcrumb(kCrumbLcdPush, 3);
  tft->writePixels(&framebuffer[static_cast<int32_t>(fromRow) * kScreenW],
                   static_cast<uint32_t>(kScreenW) *
                       static_cast<uint32_t>(h));
  breadcrumb(kCrumbLcdPush, 4);
  tft->endWrite();
  breadcrumb(kCrumbLcdPush, 5);
}

// Paint the pairing-card content into the sheet canvas.
//
// Mirrors drawEstablishingScreen() (that one targets g_canvas which still
// holds the FACE while a drag is in progress), so the card the operator drags
// down is the exact splash they will see after the AP comes up -- there is no
// content swap on commit, just a single full-canvas push that arrives with
// pixels already matching what the sheet was showing.
//
// Called once at press, not per frame. The card content does NOT animate
// during the drag; the "dots + elapsed" progress ticker is a WIFI_BOOT thing
// and only fires once ORS_WIFI_BOOT has actually been entered.
//
// Crucially: credentials are NOT generated here. A cancelled pull would
// otherwise burn a set. The QR only exists once the AP has come up and the
// state machine has entered RUNNING; the sheet-card content up to that point
// is deliberately just the splash.
// Paint the pairing card, in one of two states.
//
// TWO LABELS, NOT ONE. The card used to say "Establishing Server..." from the
// moment a finger touched the panel, which was a lie for the whole drag --
// nothing is being established until the operator commits -- and worse, it
// never told them what the card WAS. A first-time user pulling down learned
// nothing about what releasing would do, which defeats the entire point of a
// sheet: start it, see what it would do, change your mind.
//
// `armed` is the second half of that fix, and it addresses a real trap. Release
// is decided by PROJECTED position, so a peek to 30% released with even 500
// px/s of residual downward motion -- unremarkable, because a thumb rarely
// stops dead before it lifts -- projects to 51% and commits. From the finger
// that gesture was "look at it and let go". iOS has identical physics and gets
// away with it because its sheets change as they approach the threshold; ours
// did not. Now the verb changes the moment the projected endpoint crosses over,
// so the operator can see the commit coming and pull back.
//
// Edge-triggered by the caller: repainted only when the armed state actually
// flips, not once per frame.
static void drawSheetCard(bool armed, bool committed) {
  if (g_sheetCanvas == nullptr) return;
  g_sheetCanvas->fillScreen(0x0000);
  const char* line1 = committed ? "Establishing" : (armed ? "Release" : "Pull down");
  const char* line2 = committed ? "Server..."    : (armed ? "to pair" : "to pair");
  const uint8_t kSize = 2;
  const int16_t w1 = static_cast<int16_t>(strlen(line1) * 6 * kSize);
  const int16_t w2 = static_cast<int16_t>(strlen(line2) * 6 * kSize);
  g_sheetCanvas->setTextSize(kSize);
  // Armed is brighter, but the VERB is what carries the state -- never colour
  // alone (design-craft §3).
  g_sheetCanvas->setTextColor(armed || committed ? kQrWhite : 0x8410);
  g_sheetCanvas->setTextWrap(false);
  g_sheetCanvas->setCursor(static_cast<int16_t>((kScreenW - w1) / 2), 120);
  g_sheetCanvas->print(line1);
  g_sheetCanvas->setCursor(static_cast<int16_t>((kScreenW - w2) / 2), 145);
  g_sheetCanvas->print(line2);
}
//
// The flow is Wi-Fi QR -> URL QR, and nothing used to say so. Scanning the
// first code makes the screen change, and the operator cannot tell whether they
// are finished or whether something else is expected of them.
//
// Design-craft §18 (goal-gradient): never show someone a flow at zero. By the
// time this screen exists the operator has already powered the robot on and
// swiped down, so step 1 is genuinely underway -- marking it filled is honest
// reporting, not a fabricated head start. Shape carries the state (filled vs
// hollow), never colour alone (§3, DSGN-5).
static void drawPairingStepDots(int16_t y, uint8_t step) {
  if (g_canvas == nullptr) return;
  constexpr int16_t kR    = 4;
  constexpr int16_t kGap  = 18;
  const int16_t cx = kScreenW / 2;
  for (uint8_t i = 0; i < 2U; ++i) {
    const int16_t x =
        static_cast<int16_t>(cx - (kGap / 2) + (i * kGap));
    if (i < step) {
      g_canvas->fillCircle(x, y, kR, kQrBlack);
    } else {
      g_canvas->drawCircle(x, y, kR, kQrBlack);
    }
  }
}

static void drawQrScreenWifi() {
  if (g_canvas == nullptr) return;
  g_canvas->fillScreen(kQrWhite);

  ActivationCredentials credentials = {};
  if (!copyActivationCredentials(credentials)) {
    qrCenterText(/*y=*/132, /*size=*/1, "Wi-Fi unavailable");
    return;
  }
  char wifiPayload[96];
  snprintf(wifiPayload, sizeof(wifiPayload), "WIFI:T:WPA;S:%s;P:%s;;",
           credentials.ssid, credentials.password);
  char passwordLabel[sizeof("pw:") + kApPasswordMaxLength];
  snprintf(passwordLabel, sizeof(passwordLabel), "pw:%s",
           credentials.password);

  uint8_t wifiBuf[200];
  QRCode wifiQr;
  qrcode_initText(&wifiQr, wifiBuf, kQrWifiVersion, ECC_MEDIUM, wifiPayload);
  const int16_t px = static_cast<int16_t>(wifiQr.size * kQrWifiPixelScale);
  const int16_t x  = static_cast<int16_t>((kScreenW - px) / 2);
  const int16_t y  = 6;
  drawQrToCanvas(&wifiQr, x, y, kQrWifiPixelScale);

  qrCenterText(/*y=*/206, /*size=*/1, "Scan to join Wi-Fi");
  qrCenterTextFit(/*y=*/220, credentials.ssid);
  qrCenterTextFit(/*y=*/240, passwordLabel);
  // Step 1 of 2. Without this the operator scans, watches the screen change,
  // and has no way to know whether that was the end of it (design-craft §6a
  // multi-step indicators, §18 goal-gradient). Filled/hollow is a SHAPE
  // difference, so it survives the screen being photographed or read by
  // someone who cannot separate the hues (§3).
  drawPairingStepDots(/*y=*/264, /*step=*/1U);
  // The swipe is otherwise invisible: an unsignalled gesture does not exist
  // (design-craft §1, §8).
  qrCenterText(/*y=*/272, /*size=*/1, "swipe up to exit");
  secureZero(wifiBuf, sizeof(wifiBuf));
  secureZero(passwordLabel, sizeof(passwordLabel));
  secureZero(wifiPayload, sizeof(wifiPayload));
  secureZero(&credentials, sizeof(credentials));
}

// Paint the "scan to open browser" screen.
// Layout (240×280): QR with proper quiet zone, URL text below.//
//   y=20  URL QR scale 7 (203×203) centered
//   y=252 "Scan to open control" size 1 centered
//
// Quiet zones around QR:
//   - left/right/top: 20 px (2.5 modules)
//   - bottom:         32 px before label (4.0 modules) — spec-compliant
static void drawQrScreenUrl() {
  if (g_canvas == nullptr) return;
  g_canvas->fillScreen(kQrWhite);

  char accessUrl[64];
  if (!buildAccessUrl(accessUrl, sizeof(accessUrl))) {
    qrCenterText(/*y=*/132, /*size=*/1, "Access unavailable");
    return;
  }
  uint8_t urlBuf[200];
  QRCode urlQr;
  qrcode_initText(&urlQr, urlBuf, kQrUrlVersion, ECC_MEDIUM, accessUrl);
  const int16_t px = static_cast<int16_t>(urlQr.size * kQrUrlPixelScale);
  const int16_t x  = static_cast<int16_t>((kScreenW - px) / 2);
  const int16_t y  = 20;
  drawQrToCanvas(&urlQr, x, y, kQrUrlPixelScale);

  qrCenterText(/*y=*/240, /*size=*/1, "Scan to open control");
  drawPairingStepDots(/*y=*/264, /*step=*/2U);
  qrCenterText(/*y=*/272, /*size=*/1, "swipe up to exit");
  secureZero(urlBuf, sizeof(urlBuf));
  secureZero(accessUrl, sizeof(accessUrl));
}

// Switch into Wi-Fi QR mode. Idempotent.
//
// Reveals UPWARD when arriving from the URL screen, because that is a step
// BACKWARDS through the pairing flow (the phone left the AP). Downward on first
// entry, following the swipe-down that started the session. Design-craft §6a:
// motion follows the direction of the thing that caused it.
static void enterLcdQrWifiMode() {
  if (g_lcdMode == LCD_MODE_QR_WIFI) return;
  const bool steppingBack = (g_lcdMode == LCD_MODE_QR_URL);
  drawQrScreenWifi();
  pushCanvasBanded(/*downward=*/!steppingBack);
  g_lcdMode = LCD_MODE_QR_WIFI;
  Serial.println(F("lcd: -> QR_WIFI"));
}

// Switch into URL QR mode. Idempotent.
//
// Always forward progress (a station joined the AP), so it always reveals
// downward.
static void enterLcdQrUrlMode() {
  if (g_lcdMode == LCD_MODE_QR_URL) return;
  drawQrScreenUrl();
  pushCanvasBanded(/*downward=*/true);
  g_lcdMode = LCD_MODE_QR_URL;
  Serial.println(F("lcd: -> QR_URL"));
}

// Paint the gesture-button screen. `highlight` is the index currently being
// pressed, or -1. Redrawn in full on each press because the panel is small and
// a full push is well under the latency budget.
static void drawGestureMenu(int8_t highlight) {
  if (g_canvas == nullptr) return;
  g_canvas->fillScreen(kQrBlack);

  qrCenterTextDark(/*y=*/14, /*size=*/2, "TEST");

  // This screen now exists ONLY to start an armed measurement. Movements moved
  // to the web app, which is reachable untethered over the robot's own AP and
  // is a far better surface than three buttons on a 1.69" panel. What could
  // not move is the measurement trigger: a bench run needs no cable AND no
  // phone, because a tethered robot cannot complete a rotation.
  //
  // Drawn in the last slot so it sits where the footer clearance was already
  // checked.
  {
    const uint8_t i = kGestureTestIndex;
    const int16_t y = gestureButtonY(i);
    const uint16_t accent = kGestureButtonColors[i];
    const bool pressed = (highlight == static_cast<int8_t>(i));

    // Pressed state is a filled block, not merely a colour shift, so it reads
    // instantly and without relying on colour perception alone.
    if (pressed) {
      g_canvas->fillRect(kGestureBtnX, y, kGestureBtnW, kGestureBtnH, accent);
    } else {
      g_canvas->fillRect(kGestureBtnX, y, kGestureBtnW, kGestureBtnH, 0x18E3);
      g_canvas->drawRect(kGestureBtnX, y, kGestureBtnW, kGestureBtnH, accent);
      g_canvas->drawRect(kGestureBtnX + 1, y + 1,
                         kGestureBtnW - 2, kGestureBtnH - 2, accent);
    }

    const char* label = kGestureButtonNames[i];
    const uint8_t size = 2;
    const int16_t textW = static_cast<int16_t>(strlen(label) * 6 * size);
    g_canvas->setTextSize(size);
    g_canvas->setTextWrap(false);
    g_canvas->setTextColor(pressed ? kQrBlack : accent);
    g_canvas->setCursor(static_cast<int16_t>((kScreenW - textW) / 2),
                        static_cast<int16_t>(y + (kGestureBtnH / 2) - 8));
    g_canvas->print(label);
  }

  // Calibration status takes the footer while it has something to say: it runs
  // unplugged, so this is the only channel back to the operator.
  char footer[40];
  if (g_calibStatus[0] != '\0') {
    snprintf(footer, sizeof(footer), "%s", g_calibStatus);
  } else if (g_lastTouchX >= 0) {
    snprintf(footer, sizeof(footer), "%d,%d  swipe up=exit",
             static_cast<int>(g_lastTouchX), static_cast<int>(g_lastTouchY));
  } else {
    snprintf(footer, sizeof(footer), "swipe up to exit");
  }
  g_canvas->setTextSize(1);
  g_canvas->setTextColor(0x8410);
  g_canvas->setTextWrap(false);
  const int16_t fw = static_cast<int16_t>(strlen(footer) * 6);
  g_canvas->setCursor(static_cast<int16_t>((kScreenW - fw) / 2),
                      kGestureFooterY);
  g_canvas->print(footer);
}

// Switch into the gesture-button screen. Idempotent.
static void enterLcdGestureMenu() {
  if (g_lcdMode == LCD_MODE_GESTURE_MENU) return;
  drawGestureMenu(-1);
  pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
  g_lcdMode = LCD_MODE_GESTURE_MENU;
  Serial.println(F("lcd: -> GESTURE_MENU"));
}

// Switch back into face mode. EyeEngine state (mood, blinks, idle) was
// preserved across QR mode — we just need to repaint the mascot background
// and force the next eye-engine update to redraw.
static void enterLcdFaceMode() {
  if (g_lcdMode == LCD_MODE_FACE) return;
  g_canvas->fillScreen(MASCOT_A_BG);
  drawMascotToCanvas(static_cast<int16_t>(kOriginX + g_lastShiftX),
                     static_cast<int16_t>(kOriginY + g_lastShiftY));
  g_lcdMode        = LCD_MODE_FACE;
  g_forceFullPush  = true;
  g_forceEyeRedraw = true;
  g_eyes.forceRedraw();
  requestImmediateFrame();
  // Overlays (mood label + battery meter) will repaint
  // automatically on the next g_forceFullPush in loop().
  Serial.println(F("lcd: -> FACE"));
}

// Decide which LCD mode we should be in right now.
//   WS client open       → FACE   (browser is connected)
//   AP client, no WS     → QR_URL (user joined Wi-Fi, now needs to open it)
//   nothing              → QR_WIFI (waiting for Wi-Fi join)
static LcdMode computeDesiredLcdMode() {
  if (wsTransportOwnerConnected()) return LCD_MODE_FACE;
  const uint8_t apCount = WiFi.softAPgetStationNum();
  if (apCount > 0) return LCD_MODE_QR_URL;
  return LCD_MODE_QR_WIFI;
}

// Debounce per transition direction. Snap fast on positive events
// (user opened browser, user joined Wi-Fi); slow on negative events
// (tab refresh, brief Wi-Fi drop).
static uint32_t lcdDebounceMs(LcdMode from, LcdMode to) {
  if (to == LCD_MODE_FACE)                                         return 0;
  if (from == LCD_MODE_QR_WIFI && to == LCD_MODE_QR_URL)            return 0;
  if (from == LCD_MODE_FACE)                                        return 1500;
  if (to == LCD_MODE_QR_WIFI)                                       return 1000;
  return 500;
}
// ---------- end round 4 LCD helpers ----------

// =========================================================================
// §16 — "Establishing Server..." LCD splash (shown while AP is coming up)
// =========================================================================
static void drawEstablishingScreen() {
  if (g_canvas == nullptr) return;
  g_canvas->fillScreen(0x0000);                  // black
  // Centered two-line text using GFX 6x8 font.
  qrCenterTextDark(/*y=*/120, /*size=*/2, "Establishing");
  qrCenterTextDark(/*y=*/145, /*size=*/2, "Server...");
}

// DSGN-13: give the "Establishing Server..." splash a progress signal so
// a stall is distinguishable from normal startup. Two changes:
//
//   1. A row of three dots animates left-to-right at ~4 Hz so the operator
//      can tell the LCD is being serviced (not that we've frozen behind a
//      blocking WiFi call).
//   2. An "elapsed X.Y s" line ticks up so the operator can gauge how far
//      into the 10 s safety window we are.
//
// Called from the ORS_WIFI_BOOT pump in overrideTick(). Only redraws the
// progress strip (y=170..240) -- the "Establishing Server..." headline
// painted by drawEstablishingScreen() stays put.
static uint32_t g_orEstablishAnimNextMs = 0;
static uint8_t  g_orEstablishAnimPhase  = 0;

static void tickEstablishingScreen(uint32_t now) {
  if (g_canvas == nullptr) return;
  if (g_lcdMode != LCD_MODE_ESTABLISHING) return;
  if (static_cast<int32_t>(now - g_orEstablishAnimNextMs) < 0) return;
  g_orEstablishAnimNextMs = now + 250;                    // 4 Hz update
  g_orEstablishAnimPhase  = static_cast<uint8_t>((g_orEstablishAnimPhase + 1) % 4);

  // Clear the progress strip (y=170..240) so old dots/elapsed text vanish.
  const int16_t stripY = 170;
  const int16_t stripH = 240 - stripY;
  g_canvas->fillRect(0, stripY, kScreenW, stripH, 0x0000);

  // Dot row centred at y=180. Draw three dots; light up 0..3 filled based
  // on phase. 12 px diameter, 24 px pitch -> 3 dots span 60 px, centred.
  const int16_t dotY = stripY + 20;
  const int16_t dotR = 6;
  const int16_t pitch = 24;
  const int16_t totalW = pitch * 2;                       // 2 gaps between 3 dots
  const int16_t startX = static_cast<int16_t>((kScreenW - totalW) / 2);
  for (uint8_t i = 0; i < 3; ++i) {
    const int16_t cx = static_cast<int16_t>(startX + i * pitch);
    if (i < g_orEstablishAnimPhase) {
      g_canvas->fillCircle(cx, dotY, dotR, kQrWhite);
    } else {
      g_canvas->drawCircle(cx, dotY, dotR, kQrWhite);
    }
  }

  // Elapsed seconds at y=220 size 1.
  const uint32_t elapsedMs = now - g_orStateEnterMs;
  char elapsedLabel[24];
  snprintf(elapsedLabel, sizeof(elapsedLabel), "elapsed %lu.%01lu s",
           static_cast<unsigned long>(elapsedMs / 1000U),
           static_cast<unsigned long>((elapsedMs / 100U) % 10U));
  qrCenterTextDark(/*y=*/220, /*size=*/1, elapsedLabel);

  // Push only the strip we touched.
  pushCanvasRectToLcd(0, stripY, kScreenW, stripH);
}

// Switch into "Establishing Server..." mode. Idempotent.
static void enterLcdEstablishingMode() {
  if (g_lcdMode == LCD_MODE_ESTABLISHING) return;
  drawEstablishingScreen();
  pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
  g_lcdMode = LCD_MODE_ESTABLISHING;
  // DSGN-13: reset animation state so tickEstablishingScreen() fires
  // immediately on the next overrideTick.
  g_orEstablishAnimNextMs = 0;
  g_orEstablishAnimPhase  = 0;
  Serial.println(F("lcd: -> ESTABLISHING"));
}

// =========================================================================
// §16 — Touch observer body. Called by g_orTouch.control() when the chip
// reports a gesture. Runs on the main loop task (we poll), so it's safe to
// touch globals directly — but we keep it lean and just set flags so the
// state machine in overrideLoop() handles the actual transitions.
// =========================================================================
// The touch beep is deliberately GONE. It existed so the panel could be
// verified without a synchronised serial capture, which mattered while the
// touch mapping was being debugged and does not matter now — the mapping is
// fixed and tested. On a robot that expresses itself with sound, a beep on
// every tap competes with the thing the piezo is actually for.
//
// The service function remains as a no-op shell so the piezo is still released
// if anything else ever stamps a deadline; removing the call site entirely
// would leave a tone latched on if some future path set it.
static void serviceTouchBeep(uint32_t now) {
  const uint32_t until = g_touchBeepUntilMs;
  if (until == 0) return;
  if (static_cast<int32_t>(now - until) >= 0) {
    g_touchBeepUntilMs = 0;
    if (PiezoTone::active() && PiezoTone::frequency() == kTouchBeepHz) {
      PiezoTone::stop();
    }
    return;
  }
  if (!PiezoTone::active()) {
    PiezoTone::command(kTouchBeepHz, now);
  }
}

void OverrideTouchObserver::gestureNotification(int iGestureId, int x, int y, bool bCurrentlyPressed) {  g_orTouchEventCount++;
  recordTouch(iGestureId, x, y, bCurrentlyPressed);
  if (g_touchDebug) {
    Serial.printf("touch: gesture id=%d xy=(%d,%d) pressed=%d\n",
                  iGestureId, x, y, bCurrentlyPressed ? 1 : 0);
  }
  if (iGestureId == kTouchSwipeDownId) {
    // Suppress the chip's SwipeDown when the sheet controller is (or was very
    // recently) driving the transition. The CST816T runs its own gesture
    // detector in parallel with our finger sampler; it reports a completed
    // SwipeDown at release-time regardless of whether we already committed the
    // sheet on the same finger up. Without this guard the sheet's commit +
    // the chip's gesture BOTH set g_orPendingSwipeDown, and enterWifiBoot()
    // fires the AP twice: once when the sheet lands, once again 1-2 control()
    // ticks later, producing a spurious NVS wipe / re-provision.
    //
    // Fail-soft: if the sheet canvas didn't allocate, this feature is disabled
    // and the chip's gesture must go through unchanged -- that IS the fallback
    // path in that configuration.
    const bool sheetEngaged =
        (g_sheetCanvas != nullptr) &&
        ((g_sheet.state != SheetTransition::State::Closed) ||
         // SIGNED delta, per the rule in sheet_transition.h and R46. A naive
         // `millis() < deadline` collapses the window to nothing if a commit
         // lands within 800 ms of the 49.7-day millis() wrap, because the
         // deadline is then a small value while millis() is near UINT32_MAX.
         // Today a leaked SwipeDown in that window is caught downstream
         // ("already past IDLE") and costs only a log line -- but relying on a
         // second-line defence in another file is exactly how the underflow
         // that cut drive power mid-run survived review once already.
         (static_cast<int32_t>(millis() - g_sheetGestureSuppressUntilMs) < 0));
    if (sheetEngaged) {
      if (g_touchDebug) {
        Serial.println(F("touch: SwipeDown suppressed (sheet owns transition)"));
      }
      return;
    }
    g_orPendingSwipeDown = true;
  } else if (iGestureId == kTouchSwipeUpId) {
    g_orPendingSwipeUp = true;
  }
}

void OverrideTouchObserver::touchNotification(int x, int y, bool bCurrentlyPressed) {
  g_orTouchPointCount++;
  recordTouch(-1, x, y, bCurrentlyPressed);

  int16_t screenX = 0;
  int16_t screenY = 0;
  mapTouchToScreen(x, y, screenX, screenY);
  g_lastTouchX = screenX;
  g_lastTouchY = screenY;

  // Drag-to-commit sampling moved to serviceDragTransition() on the loop task
  // (it reads the CST816T's XY registers directly -- see PROJECT-HISTORY R48).
  // Nothing to stamp here.

  // Only stamp intent here. Starting a gesture takes the drive mutex and
  // paints the LCD, neither of which belongs in a touch callback.
  //
  // Latched on RELEASE, not on press. With movement notifications enabled this
  // callback now fires repeatedly for the duration of a touch, so latching on
  // any event would re-arm the same button many times per press. Release is
  // also the correct semantics regardless: it lets a finger slide off a button
  // to cancel (design-craft 8).
  if (g_lcdMode == LCD_MODE_GESTURE_MENU && !bCurrentlyPressed) {
    const int8_t hit = gestureButtonAt(screenX, screenY);
    if (hit >= 0) {
      g_gesturePendingButton = hit;
    }
  }

  if (g_touchDebug) {
    Serial.printf("touch: point raw=(%d,%d) screen=(%d,%d) pressed=%d\n", x, y,
                  static_cast<int>(screenX), static_cast<int>(screenY),
                  bCurrentlyPressed ? 1 : 0);
  }
}

// Render a drag in progress and act on its release. Loop task only.
//
// Finger position is read DIRECTLY off the CST816T (readTouchPointDirect), not
// obtained from the library. In hardware-gesture mode the library reports only
// completed gestures -- confirmed on hardware, three touchlog entries and zero
// touch points for a full drag -- and its coordinate accessors are private. The
// chip publishes position in plain registers regardless, so the library keeps
// doing gesture detection untouched and this reads alongside it. See
// PROJECT-HISTORY R48.
//
// The only supported interaction is: on the FACE screen, pull DOWN from the top
// to reveal a pairing CARD that tracks the finger 1:1. Nothing commits until
// the gesture completes. The face itself never moves -- the card is a second
// canvas overlayed row-strip by row-strip over the LCD, and the face canvas
// stays exactly as it was rendered by the eye engine.
//
// Cost model: only the rows that just changed sides are pushed each frame,
// which is tens of rows per tick instead of the whole 240x280 panel. That is
// what makes this CHEAPER than the full-screen shift it replaced (design-craft
// 6b).
//
// Fail-soft: g_sheetCanvas == nullptr means the second framebuffer could not
// be allocated. Return immediately; the chip's own SwipeDown gesture then
// drives the pairing flow as it did before this feature existed.
static void serviceDragTransition() {
  static int16_t s_renderedY = 0;
  static bool    s_pressed   = false;

  const bool onFace = (g_lcdMode == LCD_MODE_FACE) &&
                      (g_orState == ORS_IDLE);

  // Sheet only lives on the idle face screen. If the mode changed while it was
  // open (which is what a successful commit looks like: enterLcdEstablishingMode
  // flips g_lcdMode to LCD_MODE_ESTABLISHING and repaints the panel), park the
  // controller and forget the last render.
  if (!onFace) {
    if (g_sheet.state != SheetTransition::State::Closed || s_pressed) {
      g_sheet.reset();
      s_renderedY = 0;
      s_pressed   = false;
    }
    return;
  }

  if (g_sheetCanvas == nullptr) {
    // No compositor available -- chip gesture path handles everything.
    return;
  }

  // Sample the finger directly. Only here, only in drag-capable modes, so no
  // extra I2C traffic is added on any screen that cannot use it.
  int16_t rawX = 0;
  int16_t rawY = 0;
  bool pressed = false;
  const uint32_t sampleStartUs = micros();
  const bool ok = readTouchPointDirect(rawX, rawY, pressed);
  const uint32_t sampleUs = micros() - sampleStartUs;
  if (sampleUs > g_rtcTouchMaxUs) {
    // Folded into the SAME worst-case counter as the library's poll. This read
    // is on the identical bus, so if the bus is what stalls, it must be able to
    // show up here too -- keeping a separate counter would hide half the
    // evidence in the investigation this instrumentation exists for.
    g_rtcTouchMaxUs = sampleUs;
    g_rtcStallBatMv = static_cast<uint32_t>(g_batVoltage * 1000.0f);
    g_rtcStallFlags = (g_driveRailOn ? kStallFlagRail : 0U) |
                      (g_movementSequencer.active() ? kStallFlagGesture : 0U) |
                      (g_batCharging ? kStallFlagCharge : 0U);
  }

  const uint32_t nowMs = millis();

  int16_t screenX = 0;
  int16_t screenY = 0;
  if (ok) {
    mapTouchToScreen(rawX, rawY, screenX, screenY);
  }

  static bool s_armed = false;
  // Consecutive failed touch reads. A transport failure is NOT a release.
  //
  // The read fails when Wire.endTransmission or requestFrom does -- a bus
  // glitch, a stretched NACK, a chip that missed a beat. Treating that as a
  // finger lift means a single dropped transaction mid-drag runs onRelease
  // with whatever downward velocity was accumulated, projects past the
  // threshold, and starts the AP without the operator ever letting go.
  //
  // That is not hypothetical here: this is the same I2C bus carrying the
  // unexplained TASK_WDT stall still under investigation, so any pathology
  // that produces intermittent single-transaction failures lands exactly on
  // this path.
  //
  // So a failure skips the frame and retries. The counter exists so a
  // genuinely wedged chip cannot strand the sheet half-open forever: after
  // kTouchReadFailTolerance consecutive failures we accept that the finger is
  // gone and settle normally.
  static uint8_t s_readFails = 0;
  constexpr uint8_t kTouchReadFailTolerance = 3;   // ~3 loop passes
  if (!ok) {
    ++s_readFails;
    if (s_readFails < kTouchReadFailTolerance) {
      return;  // keep s_pressed; retry next tick
    }
  } else {
    s_readFails = 0;
  }
  const bool fingerGone = (!ok && s_readFails >= kTouchReadFailTolerance) ||
                          (ok && !pressed);

  if (ok && pressed && !s_pressed) {
    // First contact on the face screen. Paint the card content once so every
    // strip we push already shows the pairing splash -- deferring this to
    // the first move would flash a partial card.
    s_armed = false;
    drawSheetCard(/*armed=*/false, /*committed=*/false);
    g_sheet.onPress(screenY, nowMs);
    s_pressed = true;
  } else if (ok && pressed && s_pressed) {
    g_sheet.onMove(screenY, nowMs, kScreenH);
    // Re-evaluate the commit verb against the PROJECTED endpoint -- the same
    // number onRelease will use -- so what the card promises is exactly what
    // releasing now would do. Testing raw position instead would promise
    // "Release to pair" while a decelerating finger was about to cancel.
    //
    // Edge-triggered: the card is only repainted when the answer flips, so a
    // steady drag costs nothing extra. Rows already pushed keep the old verb
    // until the strip logic repaints them, which is why the whole visible
    // extent is re-pushed on a flip.
    const int16_t threshold =
        static_cast<int16_t>((static_cast<int32_t>(kScreenH) *
                              SheetTransition::kThresholdPct) / 100);
    const int32_t projected = SheetTransition::project(
        g_sheet.y, g_sheet.velocity, SheetTransition::kProjectMs);
    const bool armed = projected >= threshold;
    if (armed != s_armed) {
      s_armed = armed;
      drawSheetCard(armed, /*committed=*/false);
      s_renderedY = 0;  // force a full repaint of the visible card extent
    }
  } else if (s_pressed && fingerGone) {
    // Real release: either the chip reported the finger up, or reads have
    // failed long enough that we stop assuming it is still there.
    const int16_t threshold =
        static_cast<int16_t>((static_cast<int32_t>(kScreenH) *
                              SheetTransition::kThresholdPct) / 100);
    g_sheet.onRelease(kScreenH, threshold);
    s_pressed = false;
  }

  // Advance the settle animation from the loop. NO delay() here -- an animation
  // that sleeps on a watchdog-fed task is a pending reset.
  g_sheet.tick(SheetTransition::kSettlePercent);

  // Consume the one-shot committed-open signal exactly once. Hand off to the
  // existing chip-swipe path (g_orPendingSwipeDown) so drag and the chip
  // converge on the same lifecycle rather than duplicating it. Arm the
  // gesture-suppression window so the chip's own release-time SwipeDown
  // arriving in the next control() tick does not fire the AP a second time.
  if (g_sheet.takeSettledOpen()) {
    g_sheetGestureSuppressUntilMs = nowMs + kSheetGestureSuppressMs;
    g_orPendingSwipeDown = true;
    // Now the label is finally true: repaint so the fully-open card reads
    // "Establishing Server..." as the AP actually comes up.
    s_armed = false;
    drawSheetCard(/*armed=*/false, /*committed=*/true);
    s_renderedY = 0;
    Serial.println(F("sheet: committed open -> orPendingSwipeDown"));
  }

  // Visible sheet height in [0, H]. y may rubber-band outside the panel range
  // (kRubberDivisor lets it go slightly negative or slightly past H).
  int16_t y = static_cast<int16_t>(g_sheet.y);
  if (y < 0)         y = 0;
  if (y > kScreenH)  y = kScreenH;

  if (y == s_renderedY) {
    return;
  }

  uint16_t* sheetFb = g_sheetCanvas->getFramebuffer();
  uint16_t* faceFb  = (g_canvas != nullptr) ? g_canvas->getFramebuffer()
                                            : nullptr;

  if (y > s_renderedY) {
    // Opening: rows [s_renderedY, y) go from face -> sheet.
    pushSheetStrip(sheetFb, s_renderedY, y);
  } else {
    // Closing: rows [y, s_renderedY) go from sheet -> face.
    if (faceFb != nullptr) {
      pushSheetStrip(faceFb, y, s_renderedY);
    }
  }
  s_renderedY = y;
}

// Consume a pending button press: highlight it, run the gesture, and clear the
// highlight once it has been shown long enough to register.
static void serviceGestureMenu(uint32_t now) {
  const int8_t pending = g_gesturePendingButton;
  if (pending >= 0) {
    g_gesturePendingButton = -1;
    if (g_lcdMode == LCD_MODE_GESTURE_MENU &&
        pending < static_cast<int8_t>(kGestureButtonCount)) {
      // Paint the press before starting the motion, so the feedback lands
      // inside the perceptual "instant" window rather than after the gesture.
      g_gestureFlashButton = pending;
      g_gestureFlashUntilMs = now + 320;
      drawGestureMenu(pending);
      pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
      if (pending == static_cast<int8_t>(kGestureTestIndex)) {
        startTurnTestSequence();
      } else {
        startMovementGesture(kGestureButtonNames[pending]);
      }
    }
    return;
  }

  // The calibration runs unplugged, so its progress has to reach the screen.
  if (g_calibStatusDirty) {
    g_calibStatusDirty = false;
    if (g_lcdMode == LCD_MODE_GESTURE_MENU) {
      drawGestureMenu(g_gestureFlashButton);
      pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
    }
  }

  if (g_gestureFlashButton >= 0 &&
      static_cast<int32_t>(now - g_gestureFlashUntilMs) >= 0) {
    g_gestureFlashButton = -1;
    if (g_lcdMode == LCD_MODE_GESTURE_MENU) {
      drawGestureMenu(-1);
      pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
    }
  }
}

// =========================================================================
// §16 — WiFi/server start-stop (split from old initOverride)
// =========================================================================

// Register HTTP routes + WS handler EXACTLY ONCE per boot. Called from
// setup(); start/stop cycles reuse these registrations.
static void setupHttpRoutes() {
  WiFi.onEvent(onWiFiEvent);

  g_orWs.onEvent(orOnWsEvent);
  // Reject unauthorised upgrades at the HTTP layer, BEFORE the websocket
  // handler ever sees them.
  //
  // The previous design let every upgrade become a real AsyncWebSocketClient
  // and then called client->close(1008) from inside the WS_EVT_CONNECT
  // callback. That is a known-hazardous pattern in ESPAsyncWebServer: the
  // client is still being wired up when the connect event fires, so closing it
  // there re-enters the client's own teardown. Captured on hardware as
  //     ws: client #2 rejected; activation token required
  //     rst:0x8 (TG1WDT_SYS_RST)   reason = INT_WDT
  // with no backtrace, because the panic handler cannot print over USB CDC and
  // the hardware watchdog resets the chip first (Saved PC decoded to
  // panic_handler itself).
  //
  // Handlers are matched in registration order, so registering this first
  // means an upgrade with no valid token is answered with a plain 403 and no
  // websocket client is ever constructed. Nothing to close, nothing to crash.
  g_orServer.addHandler(&g_orWsAuthGuard);
  g_orServer.addHandler(&g_orWs);

  g_orServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    breadcrumb(kCrumbHttpRoot);
    if (!requestHasValidAccessToken(req)) {
      AsyncWebServerResponse* res = req->beginResponse(
          403, "text/plain", "Forbidden: valid activation token required.");
      res->addHeader("Cache-Control", "no-store");
      req->send(res);
      return;
    }
    Serial.printf("http: GET / from %s ua=%s\n",
                  req->client()->remoteIP().toString().c_str(),
                  req->getHeader("User-Agent") ?
                    req->getHeader("User-Agent")->value().c_str() : "?");
    AsyncWebServerResponse* res = req->beginResponse_P(200, "text/html", OVERRIDE_HTML);
    res->addHeader("Cache-Control", "no-store");
    req->send(res);
  });

  // Catch-all diagnostics deliberately omit the URL/query token.
  g_orServer.onNotFound([](AsyncWebServerRequest* req) {
    Serial.printf("http: 404 %s from %s\n",
                  req->methodToString(),
                  req->client()->remoteIP().toString().c_str());
    req->send(404);
  });
}

// Bring up the SoftAP + start the (already-routed) HTTP/WS server. Safe to
// call multiple times — pairs with stopWifiServer().
static bool requestExpectedSoftApStop() {
  bool pending = false;
  if (!g_apExpectedStopPending.compare_exchange_strong(
          pending, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    Serial.println(F("override: AP stop already pending; duplicate stop skipped"));
    return false;
  }
  // Callers make g_apLifecycleActive false before reaching this point. The
  // AP_STOP callback only consumes the pending marker, so after it clears the
  // marker it cannot mutate or tear down a subsequently-started activation.
  //
  // wifioff=FALSE, deliberately. With `true` the Arduino core shuts the WiFi
  // driver down inside this call, and the shutdown emits a spurious
  // AP_START -> AP_STOP pair with:
  //     E wifi_init_default: netstack cb reg failed with 12289
  // (checkpoint 062 recorded 12308; same fault, the code varies). That is the
  // driver re-registering lwIP netif callbacks against a netif chain it is in
  // the middle of destroying, and it leaves the stack able to bring an AP up
  // and hand out DHCP leases while routing nothing to AsyncWebServer.
  //
  // Reproduced on current firmware 2026-08-07 via `swipe down` / `swipe up`
  // over serial, so this is measured, not inherited.
  //
  // Two earlier attempts FAILED and must not be retried (checkpoint 062):
  //   - removing WiFi.mode(WIFI_OFF) + esp_wifi_stop() from teardown
  //   - reordering the teardown steps
  // Both left `wifioff=true` in place, which is where the fault actually is.
  //
  // With `false` the radio is left initialised and only the stations are
  // disassociated. The AP is still logically down (no SSID beacon after
  // server.end() and the state machine's own gating), which is all the
  // IDLE_DEMO state needs; the driver simply is not torn down and rebuilt.
  WiFi.softAPdisconnect(/*wifioff=*/false);
  return true;
}

static void startWifiServer() {
  if (g_apExpectedStopPending.load(std::memory_order_acquire)) {
    Serial.println(F("override: startWifiServer() deferred (AP stop pending)"));
    return;
  }
  if (g_overrideUp) {
    Serial.println(F("override: startWifiServer() ignored (already up)"));
    return;
  }
  generateActivationCredentials();
  ActivationCredentials credentials = {};
  if (!copyActivationCredentials(credentials)) {
    Serial.println(F("override: credential generation failed"));
    return;
  }
  WiFi.persistent(false);
  g_apStartupInProgress.store(true, std::memory_order_release);
  g_apStartupStopSeen.store(false, std::memory_order_release);
  g_apStartupDeadlineMs.store(millis() + 2000U, std::memory_order_release);
  WiFi.mode(WIFI_AP);
  const bool configOk =
      WiFi.softAPConfig(IPAddress(192,168,4,1),
                        IPAddress(192,168,4,1),
                        IPAddress(255,255,255,0));
  bool apOk = false;
  if (configOk) {
    g_apLifecycleActive.store(true, std::memory_order_release);
    apOk = WiFi.softAP(credentials.ssid, credentials.password,
                       kOverrideApChannel, /*ssid_hidden=*/0,
                       kOverrideApMaxClients);
    if (!apOk) {
      g_apLifecycleActive.store(false, std::memory_order_release);
    }
  }
  if (apOk) {
    esp_wifi_set_max_tx_power(kOverrideApTxPowerQdb);
  }

  Serial.printf("override AP: config=%d ok=%d SSID=\"%s\" ch=%u ip=%s mac=%s txpwr=%d.%02d dBm\n",
                configOk ? 1 : 0,
                apOk ? 1 : 0,
                credentials.ssid,
                static_cast<unsigned>(kOverrideApChannel),
                WiFi.softAPIP().toString().c_str(),
                WiFi.softAPmacAddress().c_str(),
                kOverrideApTxPowerQdb / 4, (kOverrideApTxPowerQdb % 4) * 25);
  secureZero(&credentials, sizeof(credentials));

  if (apOk) {
    g_orServer.begin();
    g_overrideUp = true;
    g_orTelemetryNextMs = millis() + kOverrideTelemetryMs;
  } else {
    g_apStartupInProgress.store(false, std::memory_order_release);
    g_apStartupStopSeen.store(false, std::memory_order_release);
    invalidateActivationCredentials();
    g_apLifecycleActive.store(false, std::memory_order_release);
    requestExpectedSoftApStop();
    g_overrideUp = false;
    g_orTelemetryNextMs = 0;
  }
  g_lcdNextPollMs     = 0;
}

// Tear down the server + softAP. Safe to call multiple times.
static void stopWifiServer() {
  g_apStartupInProgress.store(false, std::memory_order_release);
  g_apStartupStopSeen.store(false, std::memory_order_release);
  invalidateActivationCredentials();
  const bool wasActive =
      g_apLifecycleActive.exchange(false, std::memory_order_acq_rel);
  const bool wasUp = g_overrideUp;
  g_overrideUp = false;
  g_orTelemetryNextMs = 0;
  if (!wasActive && !wasUp) {
    Serial.println(
        g_apExpectedStopPending.load(std::memory_order_acquire)
            ? F("override: stopWifiServer() ignored (stop already pending)")
            : F("override: stopWifiServer() ignored (already down)"));
    return;
  }
  Serial.println(F("override: stopping..."));
  requestWsTransportReset();
  // Teardown deliberately does NOT call WiFi.mode(WIFI_OFF) or esp_wifi_stop(),
  // and calls softAPdisconnect with wifioff=FALSE. All three shut the WiFi
  // driver down, and that shutdown is what corrupts the netif chain: the AP
  // then comes up and hands out DHCP leases but routes nothing to the
  // AsyncWebServer. See the comment at the softAPdisconnect call site for the
  // measured evidence and the two fix attempts that failed.
  g_orWs.closeAll(1001, "Unavailable");
  g_orServer.end();
  requestExpectedSoftApStop();
  Serial.println(F("override: stopped (AP down)"));
}

// =========================================================================
// §16 — Override macro state machine transitions
// =========================================================================
// (Forward decls live at the top of the anon namespace.)

// §15: gate the 5V buck-boost EN line. The rail stays off through WiFi/QR
// setup, wakes on the first nonzero joystick frame, and shuts back down
// after both channels remain at zero for one second.
static bool setBoostEn(bool on) {
  bool enabled = false;
  portENTER_CRITICAL(&g_driveHardwareMux);
  const bool allowed =
      on &&
      g_driveHardwareTimerReady.load(std::memory_order_relaxed) &&
      g_driveHardwareLeaseArmed.load(std::memory_order_relaxed) &&
      !g_driveHardwareInhibit.load(std::memory_order_relaxed) &&
      !g_systemShuttingDown;
  if (BOOST_EN_GPIO >= 0) {
    gpio_set_level(
        static_cast<gpio_num_t>(BOOST_EN_GPIO), allowed ? 1 : 0);
  }
  enabled = allowed;
  g_driveRailOn.store(enabled, std::memory_order_release);
  if (!enabled) {
    g_driveHardwareLeaseArmed.store(false, std::memory_order_release);
  }
  portEXIT_CRITICAL(&g_driveHardwareMux);
  return !on || enabled;
}

static void enterIdleDemo() {
  invalidateActivationCredentials();
  g_driveOwnerLossPending.store(false, std::memory_order_release);
  g_orState        = ORS_IDLE;
  g_orStateEnterMs = millis();
  enterLcdFaceMode();          // instant snap back to face
  startDemo();                  // resume demo cycling
  Serial.println(F("override: -> IDLE_DEMO (WiFi off, boost off)"));
}

static void enterWifiBoot() {
  g_driveOwnerLossPending.store(false, std::memory_order_release);
  g_orState        = ORS_WIFI_BOOT;
  g_orStateEnterMs = millis();
  resetDriveControl("WIFI_BOOT transition");
  enterLcdEstablishingMode();   // black + "Establishing Server..."
  startWifiServer();             // brings AP up + starts server
  Serial.println(F("override: -> WIFI_BOOT"));
}

static void enterRunning() {
  g_driveOwnerLossPending.store(false, std::memory_order_release);
  g_orState        = ORS_RUNNING;
  g_orStateEnterMs = millis();
  resetDriveSessionAndPower("RUNNING transition");
  enterLcdQrWifiMode();          // QR1 (join AP). overrideTick poll will
                                  // flip us to QR_URL / FACE as state advances.
  Serial.println(F("override: -> RUNNING (QR_WIFI, boost on-demand)"));
}

static void enterGrace() {
  g_driveOwnerLossPending.store(false, std::memory_order_release);
  g_orState        = ORS_GRACE;
  g_orStateEnterMs = millis();
  // Never leave either credential-bearing QR visible during GRACE.
  enterLcdFaceMode();
  pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
  resetDriveControl("GRACE transition");
  Serial.printf("override: -> GRACE (%u s window)\n",
                static_cast<unsigned>(kOrGraceTimeoutMs / 1000));
}

static void teardownToIdle() {
  g_orState        = ORS_IDLE;
  g_orStateEnterMs = millis();
  beginWsIngressQuarantine();
  stopWifiServer();
  resetDriveSessionAndPower("teardown");
  endWsIngressQuarantine();
  enterIdleDemo();
}

// Show a failure for long enough to be read, then return to the face.
//
// Pairing used to fail SILENTLY: on timeout or an AP fault the machine called
// teardownToIdle() and the face simply came back. From the operator's side that
// is indistinguishable from "the AP came up and something went wrong later",
// "the AP never came up", and "I imagined the whole thing" -- so the learned
// response becomes re-swiping reflexively, which usually works, and a folk
// remedy replaces an explanation.
//
// Design-craft §7 says a wait over 10 s needs an exit; there was one (the
// timeout) but it was not legible. This makes the outcome visible without
// requiring a dismissal -- the auto-return makes it a toast in disguise.
//
// Blocking here is safe and deliberate: this runs on the loop task at a moment
// when the AP is already down, no drive session exists, and no gesture is in
// flight. It is well inside the 5 s task watchdog, and it is a one-shot, not
// an animation frame.
constexpr uint32_t kPairFailNoticeMs = 1600;

static void showPairingFailed(const char* reason) {
  Serial.printf("pairing failed: %s\n", reason);
  if (g_canvas == nullptr) return;
  g_canvas->fillScreen(0x0000);
  const uint8_t kSize = 2;
  const char* kL1 = "Pairing";
  const char* kL2 = "failed";
  const char* kL3 = "swipe down to retry";
  g_canvas->setTextWrap(false);
  g_canvas->setTextSize(kSize);
  g_canvas->setTextColor(kQrWhite);
  g_canvas->setCursor(
      static_cast<int16_t>((kScreenW - static_cast<int16_t>(strlen(kL1) * 6 * kSize)) / 2), 110);
  g_canvas->print(kL1);
  g_canvas->setCursor(
      static_cast<int16_t>((kScreenW - static_cast<int16_t>(strlen(kL2) * 6 * kSize)) / 2), 136);
  g_canvas->print(kL2);
  g_canvas->setTextSize(1);
  g_canvas->setTextColor(0x8410);
  g_canvas->setCursor(
      static_cast<int16_t>((kScreenW - static_cast<int16_t>(strlen(kL3) * 6)) / 2), 172);
  g_canvas->print(kL3);
  pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
  delay(kPairFailNoticeMs);
}

static void handleApLifecycleFault(const char* reason) {
  g_apStartupInProgress.store(false, std::memory_order_release);
  g_apStartupStopSeen.store(false, std::memory_order_release);
  g_apLifecycleActive.store(false, std::memory_order_release);
  g_apStopFaultPending.store(false, std::memory_order_release);
  g_overrideUp = false;
  g_orTelemetryNextMs = 0;
  g_orState = ORS_IDLE;
  g_orStateEnterMs = millis();
  invalidateActivationCredentials();

  beginWsIngressQuarantine();
  g_orWs.closeAll(1012, "Unavailable");
  g_orServer.end();
  resetDriveSessionAndPower(reason);
  endWsIngressQuarantine();

  enterIdleDemo();
  Serial.printf("override: AP lifecycle fault handled (%s)\n",
                reason == nullptr ? "unknown" : reason);
}

// =========================================================================
// §16 — Override main tick. Replaces the old overrideLoop(); runs every
// loop() iteration regardless of g_orState. Dispatches the state machine.
// =========================================================================
void overrideLoop() {
  const uint32_t now = millis();

  if (g_apStartupInProgress.load(std::memory_order_acquire)) {
    const uint32_t deadline =
        g_apStartupDeadlineMs.load(std::memory_order_acquire);
    if (static_cast<int32_t>(now - deadline) >= 0) {
      g_apStartupInProgress.store(false, std::memory_order_release);
      g_apStartupStopSeen.store(false, std::memory_order_release);
      Serial.println(F("override: AP startup reconfigure window expired"));
    }
  }

  if (g_apStopFaultPending.exchange(false, std::memory_order_acq_rel)) {
    handleApLifecycleFault("unexpected AP_STOP");
    return;
  }

  // ---- Drain pending touch gestures FIRST (so swipes always win) ----
  // The observer sets these booleans from g_touch.control() in loop().
  if (g_orPendingSwipeDown) {
    g_orPendingSwipeDown = false;
    Serial.println(F("override: gesture=SwipeDown"));
    if (g_orState == ORS_IDLE) {
      enterWifiBoot();
    } else if (g_orState == ORS_GRACE) {
      // Swipe down during GRACE means "give me the QR back so I can
      // reconnect". Honour it.
      //
      // GRACE hides both QRs on purpose -- they carry the access token, so
      // they must not sit on screen after the owner walks away. But a swipe is
      // a deliberate act by somebody standing at the robot, which is exactly
      // the condition that makes showing the code safe again.
      //
      // Without this the robot is unreachable for the full 30 s GRACE window:
      // the face is up, no QR is visible, and the swipe was refused as
      // "already past IDLE" -- reported as "it won't let me slide down to show
      // the QR code when I want to reconnect".
      //
      // Returning to RUNNING rather than tearing down is what makes it
      // instant: the AP is still up throughout GRACE, so there is no restart,
      // no boot logo and no waiting. Credentials outlive GRACE
      // (resetDriveControl does not invalidate them), so the QR is still valid.
      Serial.println(F("override: SwipeDown during GRACE -> QR restored"));
      enterRunning();
    } else {
      Serial.println(F("override: SwipeDown ignored (already past IDLE)"));
    }
  }
  if (g_orPendingSwipeUp) {
    g_orPendingSwipeUp = false;
    Serial.println(F("override: gesture=SwipeUp"));
    // SwipeUp dismisses from any non-IDLE state.
    if (g_orState != ORS_IDLE) {
      teardownToIdle();
    } else if (g_lcdMode == LCD_MODE_GESTURE_MENU) {
      // Second swipe up leaves the menu. Retained because a measurement run
      // can still open it; see enterLcdGestureMenu().
      stopMovementGesture("gesture menu closed");
      enterLcdFaceMode();
    } else if (g_lcdMode == LCD_MODE_FACE && testSlotArmed()) {
      // The movement menu is GONE from normal use — everything is triggered
      // from the web app now, which is reachable untethered over the robot's
      // own AP and is a better surface than three buttons on a 1.69" panel.
      //
      // It still opens while a bench run or scale check is armed, because
      // those measurements have to be startable with no cable and no phone:
      // a tethered robot cannot complete a rotation at all, the USB lead
      // restrains it.
      enterLcdGestureMenu();
    }
  }

  // Drain AsyncTCP events, apply DriveSession decisions, run its 300 ms
  // timeout, and service the independent 1 s neutral rail cutoff.
  serviceDriveControl(now);

  // ---- Per-state handling ----
  switch (g_orState) {
    case ORS_IDLE: {
      // WiFi off, demo cycling — nothing to do here. (loop() will keep
      // calling processDemo() since LCD is in FACE mode.)
      break;
    }

    case ORS_WIFI_BOOT: {
      // A swipe can begin WIFI_BOOT before the previous intentional AP_STOP
      // callback arrives. startWifiServer() is retried only after that callback
      // consumes the serialized expected-stop marker.
      if (!g_overrideUp &&
          !g_apLifecycleActive.load(std::memory_order_acquire) &&
          !g_apExpectedStopPending.load(std::memory_order_acquire)) {
        startWifiServer();
      }
      // DSGN-13: animate the "Establishing Server..." splash so a stall
      // reads as different from normal startup. Cheap: only touches a
      // 70 px-tall strip below the headline.
      tickEstablishingScreen(now);
      // Wait for AP IP. esp_wifi typically gives us a real IP within 200-500 ms;
      // safety abort at kOrWifiBootMaxMs (10 s).
      IPAddress ip = WiFi.softAPIP();
      const bool apReady =
          g_overrideUp &&
          g_apLifecycleActive.load(std::memory_order_acquire) &&
          (ip != IPAddress(0,0,0,0));
      if (apReady) {
        enterRunning();
      } else if ((now - g_orStateEnterMs) > kOrWifiBootMaxMs) {
        Serial.println(F("override: WIFI_BOOT timeout — aborting"));
        teardownToIdle();
        showPairingFailed("AP did not come up within the boot window");
        enterIdleDemo();
      }
      break;
    }

    case ORS_RUNNING: {
      if (!g_overrideUp) {
        Serial.println(F("override: RUNNING without server?! → IDLE"));
        handleApLifecycleFault("server down during RUNNING");
        break;
      }
      if (g_driveOwnerLossPending.load(std::memory_order_acquire) &&
          !wsTransportOwnerConnected()) {
        Serial.println(F("override: admitted drive owner lost → GRACE"));
        enterGrace();
        break;
      }
      // ---- Existing 3-state LCD machine (QR_WIFI → QR_URL → FACE) ----
      if (now >= g_lcdNextPollMs) {
        g_lcdNextPollMs = now + kLcdPollIntervalMs;
        const LcdMode desired = computeDesiredLcdMode();
        if (desired == g_lcdMode) {
          g_lcdPendingMode = desired;
          g_lcdPendingMs   = 0;
        } else if (desired != g_lcdPendingMode) {
          g_lcdPendingMode = desired;
          g_lcdPendingMs   = now;
        } else {
          const uint32_t deb = lcdDebounceMs(g_lcdMode, desired);
          if (now - g_lcdPendingMs >= deb) {
            switch (desired) {
              case LCD_MODE_FACE: {
                // First connect: pause demo so the user's mood choices stick.
                stopDemo(false);
                enterLcdFaceMode();
                break;
              }
              case LCD_MODE_QR_WIFI:      enterLcdQrWifiMode(); break;
              case LCD_MODE_QR_URL:       enterLcdQrUrlMode();  break;
              case LCD_MODE_ESTABLISHING: /* never desired here */ break;
            }
          }
        }
      }

      // ---- Reap a transport whose client vanished without closing ----
      //
      // A phone that walks out of WiFi range (or switches networks) kills the
      // TCP connection with no FIN, so AsyncWebSocket never fires
      // WS_EVT_DISCONNECT and still reports the client as connected. Captured
      // on hardware: `wsClients=1` with the last control frame 45 s old, long
      // after the phone had left.
      //
      // That is not just untidy, it locks the user out. computeDesiredLcdMode()
      // returns FACE while an owner is "connected", so the QR screen never
      // comes back, the swipe is refused ("already past IDLE"), and a fresh
      // connection is rejected as "Owner busy" -- the robot believes a phone is
      // still there forever.
      //
      // The web app streams control frames continuously, so silence from the
      // owner is a reliable liveness signal.
      //
      // Releasing ownership is done ENTIRELY through our own atomics.
      // requestWsTransportReset() bumps the transport generation, which makes
      // wsTransportOwnerConnected() false and lets a new phone in -- that is
      // the whole point of the reaper, and it needs no library call at all.
      //
      // What used to be here was `g_orWs.cleanupClients()` every pass plus
      // `g_orWs.closeAll(1001, "Stale")`, both from the LOOP task. That is a
      // heap-corrupting cross-task mutation: AsyncWebSocketClient::close()
      // goes through _queueControl, which does an unsynchronised
      // _controlQueue.add() (AsyncWebSocket.cpp:561), and AsyncTCP calls the
      // exact same path from _onData on its own task. Two tasks allocating
      // into one unlocked LinkedList wrecks the TLSF free list, and the next
      // malloc dies inside remove_free_block:
      //
      //     _onData -> _queueControl -> operator new -> tlsf_malloc
      //             -> block_locate_free -> remove_free_block   StoreProhibited
      //
      // The AsyncWebSocket object belongs to the AsyncTCP task. The loop task
      // reads our atomics and nothing else. A ghost socket left open is
      // harmless: it is no longer the owner, and if it ever does send a frame
      // it gets closed from the AsyncTCP callback, which is the safe place.
      // SIGNED delta. This must never be an unsigned subtraction.
      //
      // g_lastControlEventMs is written from the AsyncTCP task (it is
      // event.receivedAtMs, stamped when the frame landed) and compared here
      // against `now`, which the loop task captured at the top of its pass.
      // AsyncTCP runs concurrently, so a frame that arrives in between is
      // legitimately a millisecond or two IN THE FUTURE relative to `now`.
      //
      // Unsigned, that underflows to ~4.29e9, which is always greater than the
      // 8 s threshold, so the reaper fired instantly and killed a live
      // session. Captured on hardware mid-drive, right after a healthy run of
      // control frames:
      //
      //     override: ws owner silent 4294967295ms -> releasing transport
      //     drive session: SAFE_OFF reason=ws owner silent
      //     override: -> GRACE (30 s window)
      //
      // A negative delta means "heard from them a moment ago", i.e. the
      // opposite of silent. The signed form is also what makes this correct
      // across the millis() rollover at 49.7 days.
      const int32_t ownerSilentMs =
          static_cast<int32_t>(now - g_lastControlEventMs);
      if (wsTransportOwnerConnected() &&
          ownerSilentMs > static_cast<int32_t>(kWsOwnerSilenceTimeoutMs)) {
        Serial.printf("override: ws owner silent %ldms -> releasing transport\n",
                      static_cast<long>(ownerSilentMs));
        requestWsTransportReset();
        resetDriveControl("ws owner silent");
        g_lastControlEventMs = now;
      }

      // ---- Publish the callback-readable telemetry snapshot @ 5 Hz ----
      if (now >= g_orTelemetryNextMs) {
        g_orTelemetryNextMs = now + kOverrideTelemetryMs;
        publishWsTelemetrySnapshot();
      }

      // ---- 1-Hz diagnostic ----
      static uint32_t s_diagNextMs = 0;
      if (now >= s_diagNextMs) {
        s_diagNextMs = now + 1000;
        const int32_t ageMs =
            static_cast<int32_t>(now - g_lastControlEventMs);
        if (serialCanWrite())
        Serial.printf("diag: ctrl_rx=%u/s mood_rx=%u/s raw=(%d,%d) effective=(%d,%d) requestedUs=(%u,%u) readbackUs=(%u,%u) ageMs=%lu rail=%u outputValid=%u session=%s cal=%s wsClients=%u touchEvts=%u touchMaxUs=%lu loopMaxUs=%lu\n",
            static_cast<unsigned>(g_diagCtrlRx),
            static_cast<unsigned>(g_diagMoodRx),
            static_cast<int>(g_lastRawDriveL),
            static_cast<int>(g_lastRawDriveR),
            static_cast<int>(g_lastEffectiveDriveL),
            static_cast<int>(g_lastEffectiveDriveR),
            static_cast<unsigned>(g_servoLeftRequestedPulseUs),
            static_cast<unsigned>(g_servoRightRequestedPulseUs),
            static_cast<unsigned>(g_servoLeftReadbackPulseUs),
            static_cast<unsigned>(g_servoRightReadbackPulseUs),
            // Signed, and clamped at zero: this timestamp comes from the
            // AsyncTCP task, so it can be a hair ahead of the loop's `now`.
            // Unsigned it prints 4294967295, which reads as a stuck link when
            // the truth is the opposite -- a frame just arrived.
            static_cast<unsigned long>(
                ageMs < 0 ? 0 : ageMs),
            g_driveRailOn ? 1U : 0U,
            g_driveOutputValid ? 1U : 0U,
            driveSessionStateName(g_driveSession.state()),
            g_driveCalibration.eligible() ? "ELIGIBLE" : "LOCKED",
            static_cast<unsigned>(
                g_wsTransportOpenCount.load(std::memory_order_acquire)),
            static_cast<unsigned>(g_orTouchEventCount),
            static_cast<unsigned long>(g_rtcTouchMaxUs),
            static_cast<unsigned long>(g_rtcLoopMaxUs));
        // Outside the serialCanWrite() guard on purpose: these are per-SECOND
        // rates, so they must reset whether or not the line was printable. If
        // they only reset when the FIFO had room, a congested link would make
        // them read as a rising total and misreport the traffic rate.
        g_diagCtrlRx = 0;
        g_diagMoodRx = 0;
      }

      // Fallback for a transport loss whose disconnect record was not
      // observed. The admitted-owner path above is independent of LCD mode.
      if (g_lcdMode == LCD_MODE_FACE && !wsTransportOwnerConnected()) {
        enterGrace();
      }
      break;
    }

    case ORS_GRACE: {
      // Stay on FACE (paused demo). An associated phone may remain on the AP
      // after closing Safari, so only a real WebSocket reconnect cancels
      // grace. serviceDriveControl performs admission before changing state;
      // a provisional transport connection must never reset the drive owner.
      // Timeout → teardown.
      if ((now - g_orStateEnterMs) >= kOrGraceTimeoutMs) {
        Serial.println(F("override: GRACE timeout → TEARDOWN"));
        teardownToIdle();
      }
      break;
    }
  }
}

[[noreturn]] void haltAfterDisplayInitFailure() {
  g_systemShuttingDown = true;
  g_wsIngressBlocked.store(true, std::memory_order_release);
  latchDriveEventQueueEmergency();
  requestWsTransportReset();
  PiezoTone::stop();
  (void)g_driveSession.shutdown();
  g_driveSessionTransportToken.store(0, std::memory_order_release);
  g_driveSessionTransportGeneration.store(0, std::memory_order_release);
  setBoostEn(false);
  detachAllServos();
  driveSignalPinsLow();
  g_driveOutputValid = false;
  g_driveOutputHadState = false;
  g_lastEffectiveDriveL = 0;
  g_lastEffectiveDriveR = 0;
  invalidateActivationCredentials();
  g_apLifecycleActive.store(false, std::memory_order_release);
  g_apStopFaultPending.store(false, std::memory_order_release);
  g_overrideUp = false;
  // Safe here, and ONLY here. WiFi.mode(WIFI_OFF) corrupts the lwIP netif
  // chain and is banned from the AP teardown path (see the softAPdisconnect
  // call site). This is the [[noreturn]] display-init halt: the board never
  // brings an AP up again, so there is no subsequent bring-up to corrupt, and
  // dropping the radio is the right thing for a permanently halted device.
  // Kept explicit so a future cleanup does not "unify" it with teardown.
  WiFi.mode(WIFI_OFF);
  setBacklightLevel(0U);
  Serial.println(F(
      "FATAL: display/canvas initialization failed; system halted safe"));
  Serial.flush();
  for (;;) {
    delay(1000);
  }
}

}  // namespace

void setup() {
  // Preload all motor-related GPIO output latches LOW before changing pin
  // direction, then assert the battery latch. This is the first boot work.
  digitalWrite(BOOST_EN_GPIO, LOW);
  digitalWrite(kServoRightPin, LOW);
  digitalWrite(kServoLeftPin, LOW);
  pinMode(BOOST_EN_GPIO, OUTPUT);
  pinMode(kServoRightPin, OUTPUT);
  pinMode(kServoLeftPin, OUTPUT);
  // READ-10: SYS_EN bookend. This looks like three lines that do the
  // same thing; they do not. The pin is wired to the latch that keeps the
  // power controller enabled -- if it goes LOW even for a few micro-
  // seconds during boot, the whole board self-latches OFF and you have
  // to press PWR again. Sequence, from the outside in:
  //   digitalWrite(SYS_EN, HIGH)  -- set the OUTPUT latch register to
  //     HIGH BEFORE we tell the GPIO matrix this pin is an output. On
  //     ESP32-S3 the pinMode() call that follows reconfigures the pin's
  //     matrix routing, and if the output latch was still 0 (the reset
  //     default) there is a brief window where the pin drives LOW as
  //     soon as it becomes an output. Pre-loading HIGH closes that
  //     window.
  //   pinMode(SYS_EN, OUTPUT)     -- flip the pin to push-pull output.
  //   digitalWrite(SYS_EN, HIGH)  -- re-assert HIGH after the mode change
  //     as belt-and-braces, since pinMode() has historically clobbered
  //     the level on some IDF revisions. This is the visible ON assert.
  // DO NOT collapse these three lines. The battery latch depends on them
  // and a "clean up" here means the robot cannot boot from the PWR button.
  digitalWrite(SYS_EN, HIGH);
  pinMode(SYS_EN, OUTPUT);
  digitalWrite(SYS_EN, HIGH);
  pinMode(SYS_OUT, INPUT);          // PWR button (active LOW)
  g_driveRailOn = false;

  Serial.begin(kSerialBaud);
  // Defensive, not the fix for anything observed: this is USB CDC, not a UART,
  // so with no host attached a write can block until the TX timeout expires.
  // Zero means drop instead of block. (The "too few samples" failure that
  // prompted this turned out to be the 50 ms idle nap in loop(), not Serial.)
  Serial.setTxTimeoutMs(0);
  delay(200);
  Serial.printf("build: %s\n", kBuildId);
  // WHY DID WE JUST BOOT? Without this, a spontaneous restart is indistinguish-
  // able from a deliberate one, and the three plausible causes want completely
  // different fixes: BROWNOUT is electrical (supply sag on a WiFi TX spike),
  // PANIC is a crash with a backtrace, TASK_WDT/INT_WDT is something blocking
  // a task for too long. Guessing between them wastes hours; the chip already
  // knows the answer.
  {
    const esp_reset_reason_t reason = esp_reset_reason();
    const char* name = "?";
    switch (reason) {
      case ESP_RST_POWERON:  name = "POWERON (cold start)"; break;
      case ESP_RST_EXT:      name = "EXT (reset pin)"; break;
      case ESP_RST_SW:       name = "SW (esp_restart)"; break;
      case ESP_RST_PANIC:    name = "PANIC (crash - look for Guru above)"; break;
      case ESP_RST_INT_WDT:  name = "INT_WDT (interrupt watchdog)"; break;
      case ESP_RST_TASK_WDT: name = "TASK_WDT (task starved the watchdog)"; break;
      case ESP_RST_WDT:      name = "WDT (other watchdog)"; break;
      case ESP_RST_BROWNOUT: name = "BROWNOUT (supply sagged)"; break;
      case ESP_RST_DEEPSLEEP: name = "DEEPSLEEP wake"; break;
      case ESP_RST_SDIO:     name = "SDIO"; break;
      default:               name = "UNKNOWN"; break;
    }
    Serial.printf("boot: reset reason = %s (%d)\n", name,
                  static_cast<int>(reason));
    // Where were we when it happened? Only meaningful if the previous run
    // actually wrote a crumb, and only trustworthy when RTC memory survived
    // (i.e. not a cold power-on, which leaves this uninitialised).
    if (reason != ESP_RST_POWERON && g_crumbMagic == kCrumbMagic) {
      Serial.printf("boot: last breadcrumb = %s (phase=%lu detail=%lu seq=%lu)\n",
                    crumbName(g_crumbPhase),
                    static_cast<unsigned long>(g_crumbPhase),
                    static_cast<unsigned long>(g_crumbDetail),
                    static_cast<unsigned long>(g_crumbSeq));
      // Stall timings from the run that just died. Printed HERE, before setup
      // starts writing its own, because these are the numbers that say whether
      // the breadcrumb above named the real culprit or merely the last task to
      // touch it.
      if (g_rtcStallMagic == kStallMagic) {
        Serial.printf(
            "boot: stall stats from the failed run -- touch max %lu us, "
            "slow %lu, loop max %lu us in \"%s\"\n",
            static_cast<unsigned long>(g_rtcTouchMaxUs),
            static_cast<unsigned long>(g_rtcTouchSlowCount),
            static_cast<unsigned long>(g_rtcLoopMaxUs),
            crumbName(g_rtcLoopMaxPhase));
        Serial.printf(
            "boot: at that worst poll -- bat %lu mV, rail %s, gesture %s, %s\n",
            static_cast<unsigned long>(g_rtcStallBatMv),
            (g_rtcStallFlags & kStallFlagRail) ? "ON" : "off",
            (g_rtcStallFlags & kStallFlagGesture) ? "RUNNING" : "idle",
            (g_rtcStallFlags & kStallFlagCharge) ? "charging" : "on battery");
      }
      // Persist it. The RTC crumb is overwritten within milliseconds by this
      // very boot (setup writes one), so unless it is snapshotted here the
      // evidence is gone before anyone can ask for it -- which is exactly what
      // happened on the first unattended AP restart. NVS also means the record
      // survives a power cycle and any number of later reboots, so the user can
      // reproduce untethered and plug in whenever convenient.
      //
      // Only FAULT reasons are recorded. A reflash (esptool toggles the reset
      // line -> ESP_RST_UNKNOWN/EXT) and a deliberate esp_restart (SW) are not
      // failures, and letting them increment the counter buries the real events
      // in noise from ordinary development.
      const bool isFault = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                           reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT ||
                           reason == ESP_RST_BROWNOUT;
      nvs_handle_t handle = 0;
      if (isFault && nvs_open("crashlog", NVS_READWRITE, &handle) == ESP_OK) {
        CrashRecord record{};
        record.magic = kCrashRecordMagic;
        record.reason = static_cast<uint32_t>(reason);
        record.phase = g_crumbPhase;
        record.detail = g_crumbDetail;
        record.seq = g_crumbSeq;
        record.freeHeap = ESP.getFreeHeap();
        record.minHeap = ESP.getMinFreeHeap();
        uint32_t count = 0;
        nvs_get_u32(handle, "count", &count);
        record.count = count + 1;
        nvs_set_blob(handle, "rec", &record, sizeof(record));
        nvs_set_u32(handle, "count", record.count);
        nvs_commit(handle);
        nvs_close(handle);
        Serial.printf("boot: crash #%lu recorded to NVS (`crash` to review)\n",
                      static_cast<unsigned long>(record.count));
      }
    } else {
      g_crumbMagic = 0;
      g_crumbSeq = 0;
    }
    Serial.printf("boot: heap free=%lu min=%lu largest=%lu  psram free=%lu\n",
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()),
                  static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                  static_cast<unsigned long>(ESP.getFreePsram()));
  }
  // Start each run with fresh maxima, but only AFTER the failed run's numbers
  // have been printed above. A cold boot seeds them; a reset clears them so the
  // next figure describes the next run rather than the whole power-on lifetime.
  stallStatsInitIfCold();
  stallStatsReset();
  // Logged rather than assumed: if this reads LOW while nobody is touching the
  // button, GPIO40 is floating and the button guard is what stops it being
  // read as a 3-second hold.
  Serial.printf("power: SYS_OUT idle=%s SYS_EN=%d\n",
                digitalRead(SYS_OUT) == LOW ? "LOW(floating?)" : "HIGH",
                digitalRead(SYS_EN));
  const DriveCalibration::Result calibrationLoad = g_driveCalibration.load();
  if (!calibrationLoad) {
    Serial.printf("cal load FAILED: %s; drive LOCKED\n",
                  calibrationLoad.error);
  } else if (!g_driveCalibration.eligible()) {
    Serial.println(F("cal load: no eligible verified calibration; drive LOCKED"));
  } else {
    Serial.printf("cal load: eligible generation=%lu\n",
                  static_cast<unsigned long>(g_driveCalibration.generation()));
  }
  g_calibrationTouchedMask =
      g_driveCalibration.eligible() ? kAllCalibrationFieldsTouched : 0U;
  g_calibrationPending = g_driveCalibration.snapshot();
  printCalibrationSnapshot();

  // The full turn is expression tuning, not a safety limit: an absent or
  // rejected record falls back to the compiled default and only means the
  // gesture turns by the wrong amount until it is measured.
  if (g_spinCalibration.load()) {
    Serial.printf("spin load: full turn %u ms (saved)\n",
                  g_spinCalibration.fullTurnMs());
  } else {
    Serial.printf("spin load: none; using default %u ms (uncalibrated)\n",
                  g_spinCalibration.fullTurnMs());
  }

  g_piezoAvailable = PiezoTone::begin(BUZZER_PIN);
  if (g_piezoAvailable) {
    Serial.printf("piezo: GPIO%d ready\n", BUZZER_PIN);
  } else {
    Serial.printf("piezo: GPIO%d init FAILED\n", BUZZER_PIN);
  }
  randomSeed(static_cast<unsigned long>(micros()));

  if (!initBacklight()) {
    Serial.println(F("backlight: PWM unavailable; using digital fallback"));
  }

  if (mountLittleFs()) {
    Serial.printf("LittleFS mounted: total=%u free=%u\n",
                  static_cast<unsigned>(LittleFS.totalBytes()),
                  static_cast<unsigned>(LittleFS.totalBytes() - LittleFS.usedBytes()));
  } else {
    Serial.println(F("LittleFS mount FAILED -- check partition label"));
  }

  if (!initDisplay()) {
    haltAfterDisplayInitFailure();
  }

  // The ST7789 needs time after its reset and sleep-out before it reliably
  // accepts pixel data. Without this the very first pushes — the startup logo
  // and the face behind it — can be swallowed, leaving a black screen while
  // the firmware, the backlight and SYS_EN all report perfectly healthy.
  // Intermittent, and only after an esptool reset rather than a power cycle,
  // because a soft reset re-runs this init against a panel that was already
  // live. The identical animation works when invoked later from `wake`.
  delay(150);

  recomputeEyeRegionBounds();
  g_eyes.begin(g_canvas, g_eyeGeom, MASCOT_A_EYE_BG, MASCOT_A_EYE_COLOR);
  g_eyes.setOutlineColor(MASCOT_A_OUTLINE);
  g_eyes.setBackgroundSource(MASCOT_A_RGB565, kOriginX, kOriginY, MASCOT_A_W, MASCOT_A_H);
  g_eyes.setFramerate(kFastEyeFps);
  g_eyes.setAutoblinker(true);
  g_eyes.setIdleWander(true);
  g_eyeEngineReady = true;
  runStartupLogoAnimation();

  setEyeLoopRate(g_eyes.isAnimating());
  g_lastRedrawMs = millis();
  initServoTimers();   // pre-alloc PWM timer; motors stay DETACHED on boot
  // [DEBUG-DELETE] paint initial mood label on boot
  drawDemoMoodLabel();
  // [DEBUG-DELETE] END
  // Take first battery reading + paint meter on boot
  analogReadResolution(12);
  updateBatteryMeter();
  publishWsTelemetrySnapshot();
  Serial.println(F("desktop-robot eye-engine ready"));

  // §15/§16: external 5V buck-boost EN pin for the MG90D servos.
  //   Boot state: LOW (boost OFF) — saves ~12 mA quiescent + ~30 mA per
  //   detented servo while we're in IDLE_DEMO. The §16 state machine
  //   raises EN only on a nonzero joystick command, then detaches PWM and
  //   drops EN after both joystick channels remain at zero for one second.
  if (BOOST_EN_GPIO >= 0) {
    Serial.printf("boost: EN GPIO%d LOW (servo 5V rail off in IDLE)\n", BOOST_EN_GPIO);
  }

  // §16: bring up CST816T touch (I2C). Gestures arrive via observer.
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_FREQ_HZ);
  // Same bus as touch and the RTC. The IMU is a 6-axis part (accel + gyro,
  // no magnetometer) and is used to MEASURE turns rather than guess them.
  if (ImuQmi8658::begin()) {
    Serial.printf("imu: QMI8658C ready (who=0x%02X, gyro +/-2048dps @1kHz)\n",
                  ImuQmi8658::whoAmI());
    // COD gains are volatile on the device, so a stored calibration has to be
    // pushed back after every power-up or it silently reverts to the factory
    // scale — and a scale error is invisible to every other check we have.
    if (loadCodGains()) {
      const bool applied = ImuQmi8658::applyGains(g_codGains.x, g_codGains.y,
                                                  g_codGains.z);
      Serial.printf("imu: cod gains (%u,%u,%u) re-applied=%d\n",
                    static_cast<unsigned>(g_codGains.x),
                    static_cast<unsigned>(g_codGains.y),
                    static_cast<unsigned>(g_codGains.z), applied ? 1 : 0);
    }
    if (loadGyroScale()) {
      Serial.printf("imu: gyro scale correction %.4f (measured)\n",
                    static_cast<double>(ImuQmi8658::gyroScaleCorrection()));
    }
    if (loadScaleBound() && g_scaleBoundPercent > 0.0f) {
      Serial.printf("imu: gyro scale verified, error <%.3f%%\n",
                    static_cast<double>(g_scaleBoundPercent));
    }
    loadScaleCheckRevs();
    if (g_scaleCheckRevs > 0) {
      Serial.printf("imu: scale check PENDING (%u revs) - press TEST\n",
                    static_cast<unsigned>(g_scaleCheckRevs));
    }
    establishYawAxis();
  } else {
    Serial.printf("imu: QMI8658C init FAILED (who=0x%02X); `spin auto` "
                  "unavailable\n",
                  ImuQmi8658::whoAmI());
  }
  if (g_orTouch.begin(Wire, &g_orTouchObs, TP_INT, TP_RST, TP_ADDR)) {
    // NOTE ON CONTINUOUS REPORTING.
    //
    // setNotificationsOnAllEvents / setNotifyOnMovement / setMovementInterval
    // are PROTECTED on CST816Touch and are only exposed by the SW-mode
    // subclass. This board deliberately runs CST816Touch_HWMode, where the
    // chip itself detects swipes and we poll its gesture register -- a choice
    // that costs ~0 CPU when idle and whose gesture IDs were calibrated on
    // this exact hardware (kTouchSwipeDownId/UpId above).
    //
    // Switching to SW mode to unlock those setters would replace that
    // calibrated hardware gesture detection with a software reimplementation,
    // risking the primary UI to gain a transition. Not worth it, and not done
    // on a guess: whether HW mode already streams intermediate touch points
    // during a drag is a question the hardware can answer directly, via the
    // touch log. Drag-to-commit is wired up and will work the moment those
    // points arrive; if they do not, this is where the decision gets revisited.
    // Do NOT disable auto-sleep here. On this chip the interrupt pin is what
    // drives event reporting, and setAutoSleep(false) silently killed ALL
    // touch and gesture events even though I2C identity reads kept working —
    // the chip looked perfectly healthy while reporting nothing. Verified on
    // hardware: swipes stopped the moment it was added and returned when it
    // was removed.
    MDO::TouchScreenController::device_type_t deviceType;
    unsigned char firmware = 0;
    const bool haveType = g_orTouch.getDeviceType(deviceType);
    const bool haveFw = g_orTouch.getFirmwareVersion(firmware);
    Serial.printf("touch: CST816T ready type=%d(%d) fw=%d(%d)\n",
                  haveType ? static_cast<int>(deviceType) : -1, haveType ? 1 : 0,
                  haveFw ? static_cast<int>(firmware) : -1, haveFw ? 1 : 0);
  } else {
    Serial.println(F("touch: CST816T init FAILED — gestures disabled"));
  }

  // §16: register HTTP/WS routes ONCE so start/stop cycles don't dup them.
  setupHttpRoutes();

  // Auto-start demo so the device can run untethered for
  // on-camera mood review. Any serial command stops it (drainSerial calls
  // stopDemo on every non-demo command).
  startDemo();

  // §16: boot directly into IDLE_DEMO — WiFi OFF, demo cycling, LCD shows
  // mascot face (already drawn above). User swipes down to wake the WiFi
  // server + QR flow.
  g_orState        = ORS_IDLE;
  g_orStateEnterMs = millis();
  // Do NOT rely on the startup animation's final push having landed. It clears
  // the force-push flags, and nothing afterwards repaints: enterLcdFaceMode()
  // early-returns because the mode is already FACE. After an esptool reset the
  // ST7789 is not power-cycled the way it is on a real power-on, so the panel
  // can be left blank while the eye engine only refreshes its small dirty
  // region — the face never appears and the screen looks dead even though the
  // firmware, the backlight (255) and SYS_EN are all healthy. Observed
  // repeatedly after flashing; a forced full repaint fixed it every time.
  g_forceFullPush = true;
  g_forceEyeRedraw = true;
  g_eyes.forceRedraw();
  enableLoopWDT();
  Serial.println(F("drive safety: loop task WDT enabled (5s secondary reset)"));
  Serial.println(F("override: boot -> IDLE_DEMO (WiFi off; swipe down on screen to wake)"));
  breadcrumb(kCrumbSetupDone);
}

void loop() {
  ++g_loopIterations;
  // Whole-pass timer, paired with the touch-poll timer.
  //
  // A watchdog reset proves only that the loop went 5 s unfed; it does not say
  // where the time went, and the breadcrumb records the last write from ANY
  // task (which is how "websocket frame" once pointed at AsyncTCP while the
  // loop was stuck elsewhere). Measuring the whole pass alongside the touch
  // poll disambiguates it: if the two maxima track, the touch poll IS the
  // stall; if the loop max is far larger, the time is going somewhere the
  // breadcrumb never named.
  static uint32_t s_loopStartUs = 0;
  if (s_loopStartUs != 0) {
    const uint32_t passUs = micros() - s_loopStartUs;
    if (passUs > g_rtcLoopMaxUs) {
      g_rtcLoopMaxUs    = passUs;
      g_rtcLoopMaxPhase = g_crumbPhase;
    }
  }
  s_loopStartUs = micros();
  // Push/loop activity at 1 Hz, in EVERY state and before any early return.
  //
  // This is deliberately passive instrumentation. Every "fix" for the black
  // screen so far -- lcdprobe, lcdpush, sleep/wake -- required SENDING a
  // serial command, and the user observed the face returning as soon as the
  // board was talked to at all. That makes sending a byte a perturbation, so
  // the failed state has to be readable by LISTENING only.
  //
  // Reading it: pushes/s == 0 means the render path stopped (look at mode /
  // paused / loops). pushes/s > 0 with a black screen means the pushes are
  // happening and the panel is dropping them. loops/s near zero means the
  // main loop itself stalled.
  {
    static uint32_t s_nextActMs = 0;
    static uint32_t s_lastPush = 0;
    static uint32_t s_lastLoop = 0;
    const uint32_t nowMs = millis();
    if (nowMs >= s_nextActMs) {
      s_nextActMs = nowMs + 1000;
      if (serialCanWrite()) {
        Serial.printf("lcdact: pushes/s=%lu loops/s=%lu lastPushAge=%lums "
                      "mode=%d full=%d eye=%d paused=%d\n",
                      static_cast<unsigned long>(g_lcdPushCount - s_lastPush),
                      static_cast<unsigned long>(g_loopIterations - s_lastLoop),
                      static_cast<unsigned long>(nowMs - g_lcdLastPushMs),
                      static_cast<int>(g_lcdMode),
                      g_forceFullPush ? 1 : 0, g_forceEyeRedraw ? 1 : 0,
                      g_paused ? 1 : 0);
      }
      s_lastPush = g_lcdPushCount;
      s_lastLoop = g_loopIterations;
    }
  }
  breadcrumb(kCrumbLoopMelodyTick);
  PiezoTone::service(millis());
  serviceMelody(millis());
  // Repaint once more after the panel has certainly settled. See the boot
  // comment: an early push lost to the ST7789's warm-up is undetectable from
  // firmware, so the screen is simply painted a second time.
  // NOTE: a second "belt and braces" repaint used to live here, on the theory
  // that the first pixel push was being lost to panel warm-up. It was wrong on
  // both counts. The real cause was the SPI clock running above the ST7789V2's
  // 62.5 MHz write ceiling (see kLcdSpiHzDefault), which corrupts bytes in the
  // INIT sequence — and no amount of repainting fixes a panel that never woke
  // up. Worse, re-running the wake animation blanked the backlight and flashed
  // the logo ~1.2 s into every boot, which is the flicker it was supposed to
  // prevent. Removed deliberately: with a workaround in place there is no way
  // to tell whether the clock fix actually works.
  drainSerial();
  // §16: pump the CST816T (cheap — just reads INT/I2C if pending).
  //
  // TIMED, because a TASK_WDT fired with the breadcrumb sitting exactly here.
  // The measurement is two subtractions and a compare; it cannot itself be the
  // stall, and it is written to RTC memory so it survives the reset we are
  // trying to catch.
  breadcrumb(kCrumbLoopTouch);
  const uint32_t touchStartUs = micros();
  g_orTouch.control();
  const uint32_t touchUs = micros() - touchStartUs;
  if (touchUs > g_rtcTouchMaxUs) {
    g_rtcTouchMaxUs = touchUs;
    // Snapshot power/motion at the worst poll, not at the crash. By the time a
    // watchdog reset happens the rail is already down and the battery has
    // recovered, so reading it afterwards would show a healthy board and
    // wrongly exonerate the servos.
    g_rtcStallBatMv = static_cast<uint32_t>(g_batVoltage * 1000.0f);
    g_rtcStallFlags = (g_driveRailOn ? kStallFlagRail : 0U) |
                      (g_movementSequencer.active() ? kStallFlagGesture : 0U) |
                      (g_batCharging ? kStallFlagCharge : 0U);
  }
  if (touchUs > kStallWarnUs) {
    ++g_rtcTouchSlowCount;
    // Ungated and unthrottled ON PURPOSE. This fires only when a single touch
    // poll has already burned 20 ms, which by definition is rare -- and if the
    // board is about to spend 5 s in here, this line is the last thing it will
    // ever say about why.
    Serial.printf("STALL touch poll %lu us (max %lu, count %lu)\n",
                  static_cast<unsigned long>(touchUs),
                  static_cast<unsigned long>(g_rtcTouchMaxUs),
                  static_cast<unsigned long>(g_rtcTouchSlowCount));
  }
  serviceTouchBeep(millis());
  // Render any drag in progress. Immediately after the poll so the screen
  // tracks the finger with the shortest possible lag -- perceived latency is
  // the master variable (design-craft 7).
  serviceDragTransition();
  // Pump override HTTP server + drive-watchdog
  breadcrumb(kCrumbLoopOverride);
  overrideLoop();
  breadcrumb(kCrumbLoopPower);
  checkPowerButton();
  // If the sheet is engaged (open, dragging or settling) the face canvas must
  // NOT push to the LCD -- updateBatteryMeter() and the eye-engine renderer
  // below both call pushCanvasRectToLcd against the top of the panel, which
  // would tear the sheet strip we just composited. Yield 1 ms and come back;
  // the sheet controller owns the LCD until it returns to Closed. Power
  // button + web server pump still ran above so no supervisory work is lost.
  //
  // Fail-soft: guarded on g_sheetCanvas != nullptr so the null-alloc path
  // behaves exactly like it did before this feature existed.
  if ((g_sheetCanvas != nullptr) &&
      (g_sheet.state != SheetTransition::State::Closed)) {
    delay(1);
    return;
  }
  // Refresh battery meter at most once per second
  // Round 4: only paint the meter when the face is up — in QR mode the
  // bar would clobber the QR codes.
  breadcrumb(kCrumbLoopBattery);
  if (g_lcdMode == LCD_MODE_FACE) {
    updateBatteryMeter();
  }

  if ((g_gfx == nullptr) || (g_canvas == nullptr)) {
    delay(100);
    return;
  }

  if (g_paused) {
    delay(50);
    return;
  }

  // §16: in EITHER QR mode OR the ESTABLISHING splash, hold the static
  // screen — skip face-render and demo-mood-cycling. The state machine in
  // overrideLoop() will flip us back to FACE at the appropriate moment.
  if (g_lcdMode != LCD_MODE_FACE) {
    // ...but the nap is only safe when nothing time-critical is running.
    //
    // A 50 ms sleep every iteration caps the whole loop at 20 Hz. Movement
    // patterns are sequenced on a 10 ms grid, and the gyro sampler wants every
    // millisecond it can get. Measured: 990 gyro samples per probe with the
    // loop free-running, versus 20 from the gesture menu — which is exactly
    // 50 ms per sample, and is what made the on-device calibration fail with
    // "too few samples" while the identical code passed over serial. (Serial
    // was innocent; a reconnect resets the board into FACE mode, so every
    // tethered test accidentally ran on the fast path.)
    if (g_turnAutoPhase == TurnAutoPhase::Idle &&
        !g_movementSequencer.active() && !g_turnCmdActive) {
      delay(50);
    } else {
      // Yield rather than free-run: a tight loop with no delay can starve the
      // idle task and trip the task watchdog. 1 ms still gives ~1 kHz, which
      // matches the 1.0 ms mean sample interval measured on the fast path and
      // is far more than the 24-sample minimum needs.
      delay(1);
    }
    return;
  }

  breadcrumb(kCrumbLoopDemo);
  processDemo();

  if (g_forceFullPush) {
    breadcrumb(kCrumbLoopPush, 1);
    pushCanvasRectToLcd(0, 0, kScreenW, kScreenH);
    g_forceFullPush = false;
    g_forceEyeRedraw = false;
    // Re-overlay battery meter on top of full repaint.
    // PERF-2: a full push is usually driven by a mode change that also
    // repaints the mascot background, which clobbers the meter strip in
    // canvas. Invalidate so drawBatteryMeter() re-renders unconditionally
    // here even if the numeric content has not changed.
    g_batStripCanvasFresh = false;
    if (g_batSeeded) drawBatteryMeter();
  } else if (g_forceEyeRedraw) {
    breadcrumb(kCrumbLoopPush, 2);
    pushCanvasRectToLcd(g_eyeRegionX, g_eyeRegionY, g_eyeRegionW, g_eyeRegionH);
    g_forceEyeRedraw = false;
    // Eye region may extend up into the meter strip
    if (g_batSeeded && rectTouchesBatStrip(g_eyeRegionY, g_eyeRegionH)) {
      // PERF-2: the eye region overlapped the strip, so canvas strip is
      // eye pixels right now. Invalidate so drawBatteryMeter() knows to
      // rebuild the strip rather than short-circuit on unchanged data.
      g_batStripCanvasFresh = false;
      drawBatteryMeter();
    }
  }

  const uint32_t now = millis();
  if ((now - g_lastRedrawMs) < g_targetFrameMs) {
    delay(1);
    return;
  }
  g_lastRedrawMs = now;

  breadcrumb(kCrumbLoopEyes);
  const bool eyesUpdated = g_eyes.update();
  const bool headShifted = applyHeadSympathyShift();
  setEyeLoopRate(g_eyes.isAnimating());

  if (!eyesUpdated && !headShifted) {
    delay(1);
    return;
  }

  if (g_eyes.isDirty()) {
    int16_t dirtyX = 0;
    int16_t dirtyY = 0;
    int16_t dirtyW = 0;
    int16_t dirtyH = 0;
    g_eyes.getDirtyRect(dirtyX, dirtyY, dirtyW, dirtyH);
    pushCanvasRectToLcd(dirtyX, dirtyY, dirtyW, dirtyH);
    g_eyes.clearDirty();
    // Expressive-mood eyes (SURPRISED/SCARED) animate up
    // into the meter strip; repaint the meter so it stays on top.
    if (g_batSeeded && rectTouchesBatStrip(dirtyY, dirtyH)) {
      // PERF-2: eye dirty rect overlapped the strip, canvas strip is
      // eye pixels now -- invalidate so drawBatteryMeter() rebuilds.
      g_batStripCanvasFresh = false;
      drawBatteryMeter();
    }
  }
}
