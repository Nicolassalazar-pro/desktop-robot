// -----------------------------------------------------------------------------
// tools/web_preview/wasm_main.cpp -- WebAssembly entry point for the browser
// LCD preview. Compiles THE SAME src/screens.cpp + src/sheet_transition.cpp the
// firmware compiles, targets Emscripten instead of ESP32, and exports a handful
// of C-linkage functions the accompanying index.html calls through the module.
//
// WHY THIS EXISTS
//
// The native harness (tools/host_preview) already proves that the firmware's
// drawing code compiles unmodified against Arduino_Canvas(240, 280, nullptr).
// This file takes the next step: replace the "flash to device" iteration step
// with a "compile to WASM" one. The developer's edit loop becomes
//   edit src/screens.cpp -> tools/web_preview/build.ps1 -> refresh browser
// with no robot on the desk. The alternative -- a JavaScript re-implementation
// of the drawing -- is exactly the drift the native harness was built to kill,
// and the reason src/screens.h exists in the first place.
//
// WHAT THIS FILE DOES NOT DO
//
// It deliberately exposes NO tunables, sliders, or writeback-to-source hooks.
// The developer edits screens.cpp in their editor as they always have and
// recompiles per edit. There is no visual editor here and there will not be
// one; that is the explicit rule stated in the task brief.
//
// COMPOSITE RULE
//
// Two Arduino_Canvas(240, 280, nullptr) instances are kept, exactly as the
// firmware keeps g_canvas + g_sheetCanvas. Composited per row per the same
// rule serviceDragTransition() uses in src/main.cpp:
//   rows [0, y)   from the SHEET canvas at (row + (kScreenH - y))  -- translated
//   rows [y, H)   from the FACE  canvas at row                     -- 1:1
// so the sheet card slides down over the face without the face itself moving.
//
// PHYSICS
//
// The drag is driven by SheetTransition::Controller, the real shipping code.
// onPress / onMove / onRelease / tick / reachedCommit / takeSettledOpen are all
// invoked here -- there is no re-implementation of the physics, and the header
// tests in test/sheet_transition_test.cpp continue to cover the same struct.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <emscripten.h>

// The library aggregate header (Arduino_GFX_Library.h) would drag in every
// databus/driver, most of which need ESP32 SDK headers. Only Arduino_Canvas is
// needed here, so include the narrow header directly by its in-tree path -- the
// same technique tools/host_preview/main.cpp uses.
#include "canvas/Arduino_Canvas.h"

#include "screens.h"
#include "sheet_transition.h"
#include "mascot/mascot_a.h"
#include "EyeEngine.h"

// ---------------------------------------------------------------------------
// State kept for the duration of the WASM instance.
//
// The two canvases plus a scratch RGBA buffer live in module-static storage
// because Emscripten instantiates the module once per page load; there is no
// per-frame allocation on the hot path, matching the firmware which keeps
// both canvases alive for the life of the process.
// ---------------------------------------------------------------------------
namespace {

// Match main.cpp: kOriginX/Y centre the mascot on the 240x280 panel. See
// src/main.cpp lines 357-360 and the identical constants in
// tools/host_preview/main.cpp -- both harnesses must position the face the
// same way or the "same screen" claim is a lie.
constexpr int16_t kOriginX = (Screens::kScreenW - MASCOT_A_W) / 2;   // 10
constexpr int16_t kOriginY = (Screens::kScreenH - MASCOT_A_H) / 2;   // 32

// Pairing data shown in the QR screens.
//
// Defaults mirror the firmware's own constants (src/main.cpp kOverrideSsid /
// kOverridePassword / kAccessUrlPrefix) so a preview with nothing configured
// still shows realistic content. They are MUTABLE because a QR is only useful
// if scanning it actually reaches something: when the host simulator
// (tools/web_preview/robot_sim.py) is running, index.html overwrites the URL
// with the machine's real LAN address so a phone can scan the panel and land
// on the real web app. On the device these values come from
// copyActivationCredentials()/buildAccessUrl() and never leave main.cpp.
//
// Sized to the firmware's own limits: SSID and password are capped at 32 chars
// (kApSsidMaxLength / kApPasswordMaxLength) and the access URL is a 22-char
// prefix plus a 16-hex token, so 96 bytes is generous without being sloppy.
char g_ssid[33] = "Hanger OVERRIDE";
char g_pwd [33] = "TheGarageAtl";
char g_url [96] = "http://192.168.4.1/?k=deadbeefcafe1234";

// The two canvases the firmware runs (g_canvas + g_sheetCanvas). Constructed
// with a nullptr output because there is no physical panel to push to; on
// the host build Arduino_Canvas::begin() falls through to plain malloc().
Arduino_Canvas g_faceCanvas (Screens::kScreenW, Screens::kScreenH, nullptr);
Arduino_Canvas g_sheetCanvas(Screens::kScreenW, Screens::kScreenH, nullptr);
bool           g_canvasesReady = false;

// The sheet controller is the SAME struct the firmware uses. All the physics
// -- rubber-band, velocity accumulation, commit crossings -- run here through
// the real code path, not a JS re-implementation.
SheetTransition::Controller g_sheet;

// ---------------------------------------------------------------------------
// Dismiss-direction state, mirroring src/main.cpp's serviceDismissDrag().
//
// The open direction (pull the card DOWN over the face) and the dismiss
// direction (push the card UP to reveal the face) share one controller and one
// composite rule, but they differ in which canvas holds what and in the
// staging step that swaps them. Two shipped bugs have already lived in that
// staging -- the revealed face had no eyes, and re-pressing mid-settle turned
// the card into the face -- so the preview models the staging itself rather
// than just the physics. A preview that only reproduced the physics could not
// have shown either bug.
//
// g_mode mirrors "which service owns the panel this frame":
//   4 = open drag   (serviceDragTransition)
//   5 = dismiss drag(serviceDismissDrag)
// anything else = a static screen, no drag service running.
int  g_mode          = 4;
// Mirrors main.cpp's file-scope g_dismissActive.
bool g_dismissActive = false;
// Mirrors serviceDismissDrag's function-static s_pressed.
bool g_pressed       = false;
// 0 = nothing yet, 1 = dismissed for real (AP would tear down),
// 2 = sprang back / changed mind (QR restored). Surfaced to the UI so the
// operator can see which branch a gesture actually took.
int  g_lastEvent     = 0;
// Counts how many times stageDismissLayers() has run for the CURRENT gesture.
// The firmware guards this to exactly one; if the guard is wrong this climbs
// above 1 and the card visibly turns into the face. Exposed to the UI because
// "the card got replaced mid-swipe" is precisely the symptom under
// investigation and a counter turns it from a judgement call into a number.
int  g_stageCount    = 0;

// ---------------------------------------------------------------------------
// Eye engine, mirroring main.cpp's setup.
//
// The mascot bitmap has no eyes baked in -- lib/EyeEngine draws them live, with
// blinking, idle gaze wander and mood shapes. Compiling it here is what makes
// the preview show the ACTUAL idle face rather than a blank-eyed mascot, and it
// is the same anti-drift argument as screens.cpp: one implementation, compiled
// by both the firmware and the preview.
//
// Geometry constants are copied from main.cpp lines 374-445. They are derived
// from the mascot bitmap's own eye-socket metrics in mascot_a.h, so the two
// cannot silently disagree about where the eyes go without the bitmap changing.
EyeEngine g_eyes;
bool      g_eyesReady = false;

constexpr int16_t kLeftEyeCenterX  = kOriginX + MASCOT_A_EYE_L_X + (MASCOT_A_EYE_L_W / 2);
constexpr int16_t kLeftEyeCenterY  = kOriginY + MASCOT_A_EYE_L_Y + (MASCOT_A_EYE_L_H / 2);
constexpr int16_t kRightEyeCenterX = kOriginX + MASCOT_A_EYE_R_X + (MASCOT_A_EYE_R_W / 2);
constexpr int16_t kRightEyeCenterY = kOriginY + MASCOT_A_EYE_R_Y + (MASCOT_A_EYE_R_H / 2);

// main.cpp's g_eyeBaseW/H/Radius and motion range defaults.
constexpr uint8_t kEyeBaseW   = 12;
constexpr uint8_t kEyeBaseH   = 36;
constexpr uint8_t kEyeRadius  = 5;
constexpr int8_t  kEyeMotionX = 14;
constexpr int8_t  kEyeMotionY = 10;
constexpr uint8_t kFastEyeFps = 50;

// Scratch RGBA buffer handed back to JS via wp_framebuffer_rgba(). Sized to the
// panel; the JS side reads it into an ImageData once per animation frame. Kept
// as raw uint8_t so an Emscripten HEAPU8.subarray on the returned pointer works
// with no further indirection.
uint8_t g_rgba[Screens::kScreenW * Screens::kScreenH * 4];

// Lazily initialize both canvas framebuffers. begin() calls malloc() under the
// hood; on WASM that is emmalloc(). Called from every entry point so the JS
// side cannot forget to call an init function first.
// True while g_faceCanvas actually holds the FACE (as opposed to a QR or the
// pairing-failed notice). The eye engine draws straight into that canvas, so
// running it while the canvas holds a QR would scribble eyes across the QR --
// which is why the firmware suppresses face rendering entirely in QR mode
// (main.cpp's "hold the static screen" early-return).
bool g_faceCanvasHasFace = false;

// Mirror of main.cpp's renderFaceTargetToCanvas(): backdrop + mascot + EYES.
//
// This is the one function that produces a complete face. Painting the
// backdrop alone leaves the mascot blank-eyed, because the eye engine draws
// the eyes separately -- a distinction that has already caused one shipped bug
// (the drag-to-dismiss reveal showed an expressionless face).
//
// main.cpp additionally offsets the mascot by g_lastShiftX/Y, the "sympathetic
// head shift" that nudges the body a pixel or two toward the gaze. That lives
// in the demo/mood state machine rather than in the eye engine, so the preview
// renders at a neutral shift of 0. The eyes themselves are fully live.
void renderFaceTargetToCanvas() {
  Screens::drawFaceBackdrop(&g_faceCanvas, kOriginX, kOriginY);
  if (g_eyesReady) {
    g_eyes.forceRedraw();
    (void)g_eyes.update();
    g_eyes.clearDirty();
  }
  g_faceCanvasHasFace = true;
}

void ensureReady() {
  if (g_canvasesReady) return;
  g_faceCanvas.begin();
  g_sheetCanvas.begin();

  // Bring the eye engine up against the FACE canvas, exactly as main.cpp does
  // in setup(). setBackgroundSource lets the engine restore real mascot pixels
  // when it clears its dirty rect, instead of punching bg-coloured holes in the
  // face -- without it the eyes leave rectangular scars as they move.
  const EyeEngineGeom geom = {
      kLeftEyeCenterX,  kLeftEyeCenterY,
      kRightEyeCenterX, kRightEyeCenterY,
      kEyeBaseW, kEyeBaseH, kEyeRadius,
      kEyeMotionX, kEyeMotionY,
  };
  g_eyes.begin(&g_faceCanvas, geom, MASCOT_A_EYE_BG, MASCOT_A_EYE_COLOR);
  g_eyes.setOutlineColor(MASCOT_A_OUTLINE);
  g_eyes.setBackgroundSource(MASCOT_A_RGB565, kOriginX, kOriginY,
                             MASCOT_A_W, MASCOT_A_H);
  g_eyes.setFramerate(kFastEyeFps);
  g_eyes.setAutoblinker(true);
  g_eyes.setIdleWander(true);
  g_eyesReady = true;

  // Paint the face and sheet with plausible content once; individual screens
  // will overwrite as the developer changes selection.
  renderFaceTargetToCanvas();
  Screens::drawQrScreenWifi (&g_sheetCanvas, g_ssid, g_pwd);
  g_canvasesReady = true;
}

// RGB565 -> RGBA8888 for a single row. The 5->8 / 6->8 expansion is the same
// standard bit-doubling the native harness's PPM writer uses so a pure 0x1F
// red maps to 0xFF, not 0xF8 -- matches what the ST7789 pushes.
inline void row565ToRgba(uint8_t* dst, const uint16_t* src, int width) {
  for (int x = 0; x < width; ++x) {
    const uint16_t c  = src[x];
    const uint8_t  r5 = static_cast<uint8_t>((c >> 11) & 0x1F);
    const uint8_t  g6 = static_cast<uint8_t>((c >>  5) & 0x3F);
    const uint8_t  b5 = static_cast<uint8_t>( c        & 0x1F);
    dst[0] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    dst[1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    dst[2] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    dst[3] = 0xFF;
    dst += 4;
  }
}

// Composite the two canvases into the RGBA framebuffer per the exact rule
// serviceDragTransition() uses in main.cpp. Same rule tools/host_preview's
// compositeDragFrame() uses -- copied here rather than shared because the
// two harnesses target different output formats (RGBA vs. RGB565 -> PPM)
// and pulling out a shared header would grow the include graph for very
// little payoff.
void compositeAndConvertRgba(int y) {
  const int W = Screens::kScreenW;
  const int H = Screens::kScreenH;
  if (y < 0) y = 0;
  if (y > H) y = H;
  const int srcOffset = H - y;

  const uint16_t* sheetFb = g_sheetCanvas.getFramebuffer();
  const uint16_t* faceFb  = g_faceCanvas .getFramebuffer();

  for (int row = 0; row < y; ++row) {
    const int srcRow = row + srcOffset;
    if (srcRow < 0 || srcRow >= H) {
      // Same "refuse rather than clamp" guard as pushSheetStrip: reading
      // outside the sheet framebuffer would show garbage rows and hide the
      // bug. Zero-fill so the JS side still sees a clean row.
      std::memset(&g_rgba[row * W * 4], 0, W * 4);
      continue;
    }
    row565ToRgba(&g_rgba[row * W * 4], &sheetFb[srcRow * W], W);
  }
  for (int row = y; row < H; ++row) {
    row565ToRgba(&g_rgba[row * W * 4], &faceFb[row * W], W);
  }
}

// Convert the FACE canvas (only) to RGBA. Used by the four static-screen
// entries where the sheet is not visible: we render into g_faceCanvas and
// hand it back verbatim.
void faceToRgba() {
  const int W = Screens::kScreenW;
  const int H = Screens::kScreenH;
  const uint16_t* faceFb = g_faceCanvas.getFramebuffer();
  for (int row = 0; row < H; ++row) {
    row565ToRgba(&g_rgba[row * W * 4], &faceFb[row * W], W);
  }
}

// Mirror of main.cpp's stageDismissLayers(). Whatever is currently on the
// panel (the QR) becomes the CARD, and the face is painted underneath it.
//
// Correct exactly once per gesture. Running it a second time copies the FACE
// into the card, so the card stops being the QR and starts dragging a stale
// face around -- the firmware guards this with g_dismissActive and the guard
// is reproduced faithfully at the call site rather than hidden in here, so a
// broken guard shows up in the preview instead of being papered over.
void stageDismissLayers() {
  uint16_t* sheetFb = g_sheetCanvas.getFramebuffer();
  uint16_t* faceFb  = g_faceCanvas .getFramebuffer();
  if ((sheetFb == nullptr) || (faceFb == nullptr)) return;
  constexpr size_t kBytes =
      static_cast<size_t>(Screens::kScreenW) *
      static_cast<size_t>(Screens::kScreenH) * sizeof(uint16_t);
  std::memcpy(sheetFb, faceFb, kBytes);   // what's on screen becomes the card
  // The firmware calls renderFaceTargetToCanvas() here, and so do we -- a plain
  // fillScreen + drawMascotToCanvas would leave the mascot with no eyes, which
  // is the exact bug this call site shipped once already.
  renderFaceTargetToCanvas();
  ++g_stageCount;
}

// Mirror of main.cpp's restoreQrCanvas(). Puts the QR back in the face canvas
// so the ordinary QR-mode logic keeps working after a dismiss springs back.
void restoreQrCanvas() {
  uint16_t* sheetFb = g_sheetCanvas.getFramebuffer();
  uint16_t* faceFb  = g_faceCanvas .getFramebuffer();
  if ((sheetFb == nullptr) || (faceFb == nullptr)) return;
  constexpr size_t kBytes =
      static_cast<size_t>(Screens::kScreenW) *
      static_cast<size_t>(Screens::kScreenH) * sizeof(uint16_t);
  std::memcpy(faceFb, sheetFb, kBytes);
  g_faceCanvasHasFace = false;   // the QR is back in the face canvas
}

}  // namespace

extern "C" {
// -- Static geometry queries. Reported once, not per frame, so ccall overhead
// is a non-issue.
EMSCRIPTEN_KEEPALIVE int wp_width()  { return Screens::kScreenW; }
EMSCRIPTEN_KEEPALIVE int wp_height() { return Screens::kScreenH; }

// Pointer to the RGBA8888 framebuffer. JS reads it as
//   new Uint8ClampedArray(HEAPU8.buffer, wp_framebuffer_rgba(), W*H*4)
// and hands it to an ImageData for putImageData(). Guarantees the buffer is
// initialised on first call so the JS side never sees garbage.
EMSCRIPTEN_KEEPALIVE uint8_t* wp_framebuffer_rgba() {
  ensureReady();
  return g_rgba;
}

// Repaint a static screen. Called each time the developer clicks a screen
// button in index.html, plus once at page load.
//   0 = face backdrop
//   1 = QR wifi
//   2 = QR url
//   3 = pairing failed
//   4 = "drag" mode -- caller drives with wp_touch / wp_tick instead
//
// For 0..3 we paint into g_faceCanvas and convert straight from it, ignoring
// the sheet. For 4 we ensure the sheet canvas holds the QR content (that is
// what the firmware shows above the face) and the face canvas holds the
// backdrop, then let wp_tick's composite blit reflect the current g_sheet.y.
EMSCRIPTEN_KEEPALIVE void wp_render_screen(int id) {
  ensureReady();
  // Any screen change abandons an in-flight dismiss. Mirrors the firmware,
  // where leaving QR mode clears g_dismissActive at the top of
  // serviceDismissDrag().
  g_mode          = id;
  g_dismissActive = false;
  g_pressed       = false;
  g_lastEvent     = 0;
  g_stageCount    = 0;
  switch (id) {
    case 0:
      renderFaceTargetToCanvas();
      faceToRgba();
      break;
    case 1:
      Screens::drawQrScreenWifi  (&g_faceCanvas, g_ssid, g_pwd);
      g_faceCanvasHasFace = false;
      faceToRgba();
      break;
    case 2:
      Screens::drawQrScreenUrl   (&g_faceCanvas, g_url);
      g_faceCanvasHasFace = false;
      faceToRgba();
      break;
    case 3:
      Screens::drawPairingFailed (&g_faceCanvas);
      g_faceCanvasHasFace = false;
      faceToRgba();
      break;
    case 5:
      // Dismiss mode: the panel is showing the QR, exactly as it does after a
      // successful pairing pull. NOTHING is staged yet -- the firmware stages
      // on the first press inside serviceDismissDrag, not on mode entry, and
      // staging early here would hide the very guard under investigation.
      g_sheet.reset();
      Screens::drawQrScreenWifi(&g_faceCanvas, g_ssid, g_pwd);
      g_faceCanvasHasFace = false;
      faceToRgba();
      break;
    case 4:
    default:
      // Reset the sheet so the operator starts each drag session with a
      // closed card, no accumulated velocity, and no armed one-shot. Without
      // the reset a previous "pulled to commit" would fire the moment the
      // caller polled wp_sheet_open again.
      g_mode = 4;
      g_sheet.reset();
      renderFaceTargetToCanvas();
      Screens::drawQrScreenWifi(&g_sheetCanvas, g_ssid, g_pwd);
      compositeAndConvertRgba(0);
      break;
  }
}

// Feed a mouse sample into the real Controller. Coordinates are in panel
// pixels (0..W, 0..H) -- the JS side is responsible for undoing the CSS
// scale. `pressed` is 1 while the button is held, 0 on release. `nowMs` is
// a monotonic ms clock; performance.now() from JS is fine.
//
// This wires the browser mouse to the SAME onPress / onMove / onRelease the
// firmware calls from its touch service. There is no substitute physics.
EMSCRIPTEN_KEEPALIVE void wp_touch(int x, int y, int pressed, uint32_t nowMs) {
  ensureReady();
  (void)x;  // Sheet controller is 1D; horizontal position is ignored.

  if (g_mode == 5) {
    // ---- dismiss direction: mirror of serviceDismissDrag()'s press handling.
    if (pressed && !g_pressed) {
      // Stage ONCE per gesture, not once per press. The guard is the whole
      // point of reproducing this path -- see stageDismissLayers().
      if (!g_dismissActive) {
        stageDismissLayers();
        g_sheet.reset();
        g_sheet.y       = Screens::kScreenH;  // the card starts fully down
        g_dismissActive = true;
      }
      g_sheet.onPress(static_cast<int16_t>(y), nowMs);
      g_pressed = true;
    } else if (pressed && g_pressed) {
      g_sheet.onMove(static_cast<int16_t>(y), nowMs, Screens::kScreenH);
    } else if (!pressed && g_pressed) {
      g_sheet.onRelease(Screens::kScreenH, 0);
      g_pressed = false;
    }
    return;
  }

  // ---- open direction (mode 4), unchanged.
  static bool s_wasPressed = false;
  if (pressed && !s_wasPressed) {
    g_sheet.onPress(static_cast<int16_t>(y), nowMs);
  } else if (pressed && s_wasPressed) {
    g_sheet.onMove(static_cast<int16_t>(y), nowMs, Screens::kScreenH);
  } else if (!pressed && s_wasPressed) {
    // Same call the firmware makes in serviceDragTransition() when the finger
    // lifts. Threshold arg is ignored by onRelease (see sheet_transition.cpp)
    // but kept for API parity with the shipping code.
    g_sheet.onRelease(Screens::kScreenH, 0);
  }
  s_wasPressed = (pressed != 0);
}

// Advance one animation frame. Runs the settle tick (a no-op unless something
// put the controller into Settling; see sheet_transition.h) and re-composites
// the RGBA framebuffer from the current sheet Y.
//
// The commit one-shot is consumed here so a browser session cannot get stuck
// re-firing "just settled open" every frame after the card completes a pull.
// The JS side does not need to know -- polling wp_sheet_open bare is enough.
EMSCRIPTEN_KEEPALIVE void wp_tick(uint32_t /*nowMs*/) {
  ensureReady();

  // Advance the eyes BEFORE compositing, but only while the face canvas really
  // holds the face. update() is internally frame-rate limited (setFramerate),
  // so calling it every animation frame costs nothing when it isn't due; it
  // draws straight into g_faceCanvas, which is what makes the blink and idle
  // gaze wander show up in whatever the compositor emits next.
  if (g_eyesReady && g_faceCanvasHasFace) {
    (void)g_eyes.update();
    g_eyes.clearDirty();
  }

  if (g_mode == 5) {
    // ---- dismiss direction: mirror of serviceDismissDrag()'s render + settle.
    if (!g_dismissActive) {
      // No gesture in flight: the panel just shows whatever the face canvas
      // holds (the QR before a drag, or the revealed face after one).
      faceToRgba();
      return;
    }
    g_sheet.tick(SheetTransition::kSettlePercent);
    // Starting at y = H the card is already past the commit line, so the
    // controller arms the one-shot on the first move. Pairing is already up on
    // this screen, so discard it -- same as the firmware.
    (void)g_sheet.takeSettledOpen();
    compositeAndConvertRgba(g_sheet.y);

    if (g_pressed) return;

    if (g_sheet.state == SheetTransition::State::Closed) {
      // Card left the screen: this is a real dismiss. The firmware raises
      // g_orPendingSwipeUp here and the AP tears down; the preview has no AP,
      // so it just paints the face and reports the branch taken.
      g_dismissActive = false;
      g_lastEvent     = 1;
      renderFaceTargetToCanvas();
      faceToRgba();
    } else if (g_sheet.state == SheetTransition::State::Open) {
      // Changed their mind -- put the QR back where the rest of the code
      // expects it.
      g_dismissActive = false;
      g_lastEvent     = 2;
      g_sheet.reset();
      restoreQrCanvas();
    }
    return;
  }

  // ---- open direction (mode 4).
  if (g_mode == 4) {
    g_sheet.tick(SheetTransition::kSettlePercent);
    (void)g_sheet.takeSettledOpen();  // absorb one-shot; nothing to do here
    compositeAndConvertRgba(g_sheet.y);
    return;
  }

  // ---- static screens (0..3). Hand back the face canvas verbatim. Routing
  // these through the compositor happened to work only because g_sheet.y was
  // usually 0; switching to a static screen mid-drag would otherwise composite
  // a stale sheet position over it.
  faceToRgba();
}

// Current sheet travel in panel pixels. Displayed by the browser UI so the
// developer can see the snap threshold and rubber-band bounds numerically as
// they drag. Read-only; there is no setter.
EMSCRIPTEN_KEEPALIVE int wp_sheet_y() {
  return static_cast<int>(g_sheet.y);
}

// Controller state as a small integer, mirroring SheetTransition::State so the
// JS side can render "Closed / Dragging / Settling / Parked / Open" without
// having to duplicate the enum.
EMSCRIPTEN_KEEPALIVE int wp_sheet_state() {
  return static_cast<int>(g_sheet.state);
}

// True while a dismiss gesture owns the panel (mirrors g_dismissActive).
EMSCRIPTEN_KEEPALIVE int wp_dismiss_active() {
  return g_dismissActive ? 1 : 0;
}

// How many times stageDismissLayers() has run for the current gesture. The
// firmware's guard makes this exactly 1; anything higher means the card was
// re-staged mid-gesture and its content was replaced with the face, which is
// the failure mode being investigated.
EMSCRIPTEN_KEEPALIVE int wp_stage_count() {
  return g_stageCount;
}

// Which branch the last completed dismiss took: 0 none, 1 dismissed for real,
// 2 sprang back and restored the QR.
EMSCRIPTEN_KEEPALIVE int wp_last_event() {
  return g_lastEvent;
}

// Replace the pairing data the QR screens encode.
//
// Exists so a scanned QR actually WORKS: with the host simulator running, the
// browser passes the machine's real LAN URL here, so pointing a phone at the
// panel opens the genuine web app instead of the firmware's 192.168.4.1, which
// is unreachable from anywhere but the robot's own softAP.
//
// Any argument may be null to leave that field unchanged. Copies are bounded
// and always NUL-terminated; over-long input is truncated rather than
// rejected, because a truncated SSID renders visibly wrong in the QR (which
// the developer will notice) whereas a silent no-op would look like the setter
// simply did not run.
EMSCRIPTEN_KEEPALIVE void wp_set_pairing(const char* ssid, const char* pwd,
                                         const char* url) {
  auto copyInto = [](char* dst, size_t cap, const char* src) {
    if ((src == nullptr) || (*src == '\0')) return;
    std::strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
  };
  copyInto(g_ssid, sizeof(g_ssid), ssid);
  copyInto(g_pwd,  sizeof(g_pwd),  pwd);
  copyInto(g_url,  sizeof(g_url),  url);
}

}  // extern "C"
