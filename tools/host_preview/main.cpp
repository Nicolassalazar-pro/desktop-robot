// -----------------------------------------------------------------------------
// tools/host_preview/main.cpp -- native Windows harness that renders the ESP32
// firmware's LCD screens pixel-for-pixel with no hardware attached.
//
// WHY THIS EXISTS
//
// The previous Python preview (tools/lcd_preview.py) reimplemented the drawing
// by parsing coordinates out of C++, and it drifted -- it kept rendering a
// drawSheetCard() placeholder that had already been deleted from the firmware.
// This harness fixes that by compiling THE SAME src/screens.cpp the firmware
// compiles, against a real Arduino_Canvas(240, 280, nullptr) from Arduino_GFX
// 1.3.9. If someone edits screens.cpp, both firmware and preview move together.
//
// See docs/host-render-research.md for why the library compiles unmodified on
// the host: the ESP32/__AVR__ macros are deliberately undefined here, which
// steers Arduino_Canvas to plain malloc() and steers Arduino_GFX to the
// generic non-AVR pgm_read_byte fallbacks. The 5x7 GFX glyph font is defined
// with PROGMEM = <empty> on non-AVR, so text renders pixel-exact for free.
//
// OUTPUT
//
// Writes PPM (P6) images to build/preview/*.ppm. PPM is a 15-line writer with
// zero dependencies; Windows viewers such as IrfanView/GIMP open it directly,
// and tools/host_preview/to_png.py converts to PNG via Pillow if wanted.
//
// DRAG COMPOSITE
//
// The drag filmstrip mirrors serviceDragTransition() in src/main.cpp:
//   rows [0, y)  from the SHEET canvas at (row + (kScreenH - y))  -- translated
//   rows [y, H)  from the FACE  canvas at row                      -- 1:1
// so the sheet slides down into the face without translating the face itself.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// The library aggregate would drag in every databus/driver, most of which
// depend on ESP32 SDK headers. We only need Arduino_Canvas + the base class,
// so include the two narrow headers by their in-tree path.
#include "canvas/Arduino_Canvas.h"

#include "screens.h"
#include "sheet_transition.h"
#include "mascot/mascot_a.h"
#include "EyeEngine.h"

namespace {

// Match main.cpp: kOriginX/Y position the mascot so the face is centred on the
// 240x280 panel. See src/main.cpp lines 357-360.
constexpr int16_t kOriginX = (Screens::kScreenW - MASCOT_A_W) / 2;   // 10
constexpr int16_t kOriginY = (Screens::kScreenH - MASCOT_A_H) / 2;   // 32

// -- eye engine -------------------------------------------------------------
//
// The mascot bitmap has NO eyes baked in; lib/EyeEngine draws them live. A
// harness that renders only the backdrop therefore produces a blank-faced
// mascot -- and reviewing an eyeless face as if it were the real thing is
// exactly how the drag-to-dismiss reveal shipped with no eyes once already.
//
// Geometry mirrors src/main.cpp lines 374-445; the centres are derived from
// the bitmap's own eye-socket metrics in mascot_a.h, so the harness cannot
// disagree with the firmware about where the eyes belong without the bitmap
// itself changing.
constexpr int16_t kLeftEyeCenterX  = kOriginX + MASCOT_A_EYE_L_X + (MASCOT_A_EYE_L_W / 2);
constexpr int16_t kLeftEyeCenterY  = kOriginY + MASCOT_A_EYE_L_Y + (MASCOT_A_EYE_L_H / 2);
constexpr int16_t kRightEyeCenterX = kOriginX + MASCOT_A_EYE_R_X + (MASCOT_A_EYE_R_W / 2);
constexpr int16_t kRightEyeCenterY = kOriginY + MASCOT_A_EYE_R_Y + (MASCOT_A_EYE_R_H / 2);

EyeEngine g_eyes;
bool      g_eyesReady = false;

void beginEyes(Arduino_GFX* gfx) {
  const EyeEngineGeom geom = {
      kLeftEyeCenterX,  kLeftEyeCenterY,
      kRightEyeCenterX, kRightEyeCenterY,
      /*baseW=*/12, /*baseH=*/36, /*borderRadius=*/5,
      /*motionRangeX=*/14, /*motionRangeY=*/10,
  };
  g_eyes.begin(gfx, geom, MASCOT_A_EYE_BG, MASCOT_A_EYE_COLOR);
  g_eyes.setOutlineColor(MASCOT_A_OUTLINE);
  g_eyes.setBackgroundSource(MASCOT_A_RGB565, kOriginX, kOriginY,
                             MASCOT_A_W, MASCOT_A_H);
  // Autoblink and idle wander are deliberately OFF here. This harness renders
  // still frames for visual diffing, and a blink or a gaze shift landing on an
  // arbitrary frame would make two runs of the same code produce different
  // PNGs. The web harness leaves them on, because there the point is motion.
  g_eyes.setAutoblinker(false);
  g_eyes.setIdleWander(false);
  g_eyesReady = true;
}

// Mirror of main.cpp's renderFaceTargetToCanvas(): backdrop + mascot + EYES.
// This is the only function in the harness that yields a complete face.
void renderFaceTargetToCanvas(Arduino_GFX* gfx) {
  Screens::drawFaceBackdrop(gfx, kOriginX, kOriginY);
  if (g_eyesReady) {
    g_eyes.forceRedraw();
    (void)g_eyes.update();
    g_eyes.clearDirty();
  }
}

// PPM (P6) writer. 15 lines, no deps. Each RGB565 word explodes to three 8-bit
// samples via the standard 5-6-5 -> 8-8-8 expansion.
bool writePpm(const char* path, const std::uint16_t* fb,
              int width, int height) {
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "host_preview: cannot open %s for write\n", path);
    return false;
  }
  std::fprintf(f, "P6\n%d %d\n255\n", width, height);
  const int total = width * height;
  for (int i = 0; i < total; ++i) {
    const std::uint16_t c = fb[i];
    const std::uint8_t r5 = static_cast<std::uint8_t>((c >> 11) & 0x1F);
    const std::uint8_t g6 = static_cast<std::uint8_t>((c >>  5) & 0x3F);
    const std::uint8_t b5 = static_cast<std::uint8_t>( c        & 0x1F);
    // 5->8 = (v<<3) | (v>>2); 6->8 = (v<<2) | (v>>4). Standard rounding-neutral
    // scale-up so a pure 0x1F red maps to 0xFF, not 0xF8. Matches what the
    // ST7789 hardware pushes when it displays each RGB565 word.
    const std::uint8_t rgb[3] = {
        static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)),
        static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)),
        static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2)),
    };
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
  return true;
}

// Composite one drag frame per the rule in serviceDragTransition(). The sheet
// framebuffer contributes the TOP y rows, translated by (kScreenH - y); the
// face framebuffer supplies the rest at 1:1. Output goes to `out`, which the
// caller allocates as W*H uint16_t.
void compositeDragFrame(std::uint16_t* out,
                        const std::uint16_t* sheet,
                        const std::uint16_t* face,
                        int y) {
  const int W = Screens::kScreenW;
  const int H = Screens::kScreenH;
  if (y < 0) y = 0;
  if (y > H) y = H;
  const int srcOffset = H - y;

  for (int row = 0; row < y; ++row) {
    const int srcRow = row + srcOffset;
    // Same "refuse rather than clamp" guard used by pushSheetStrip: reading
    // outside the sheet framebuffer would show garbage rows and hide the bug.
    if (srcRow < 0 || srcRow >= H) continue;
    std::memcpy(&out[row * W], &sheet[srcRow * W], W * sizeof(std::uint16_t));
  }
  for (int row = y; row < H; ++row) {
    std::memcpy(&out[row * W], &face[row * W], W * sizeof(std::uint16_t));
  }
}

}  // namespace

int main() {
  const int W = Screens::kScreenW;
  const int H = Screens::kScreenH;

  Arduino_Canvas canvas(W, H, nullptr);
  if (!canvas.begin()) {
    std::fprintf(stderr,
        "host_preview: Arduino_Canvas::begin() returned false -- framebuffer "
        "alloc failed. This is unexpected on a host build (uses plain malloc)."
        "\n");
    return 2;
  }

  // Fake but representative data so the layouts exercise every branch:
  // - qrCenterTextFit will pick size 2 for a short SSID, size 1 for password.
  // - the URL length here matches the real access-URL prefix + a 8-hex key.
  const char* kFakeSsid    = "robot-1234";
  const char* kFakePwd     = "correct-horse-battery-staple";
  const char* kFakeUrl     = "http://192.168.4.1/?k=abcdef01";

  const char* kOutDir = "build/preview";

  auto dump = [&](const char* name) {
    char path[256];
    std::snprintf(path, sizeof(path), "%s/%s.ppm", kOutDir, name);
    if (!writePpm(path, canvas.getFramebuffer(), W, H)) {
      std::fprintf(stderr, "host_preview: PPM write failed for %s\n", name);
      std::exit(3);
    }
    std::printf("  wrote %s\n", path);
  };

  std::printf("host_preview: rendering base screens to %s/\n", kOutDir);

  beginEyes(&canvas);

  // 1. Idle face -- backdrop AND eyes, which is what the panel actually shows.
  renderFaceTargetToCanvas(&canvas);
  dump("face_idle");

  // 2. QR Wi-Fi screen.
  Screens::drawQrScreenWifi(&canvas, kFakeSsid, kFakePwd);
  dump("qr_wifi");

  // 3. QR URL screen.
  Screens::drawQrScreenUrl(&canvas, kFakeUrl);
  dump("qr_url");

  // 4. Pairing-failed notice.
  Screens::drawPairingFailed(&canvas);
  dump("pairing_failed");

  // 5. Wi-Fi "unavailable" fallback (nullptr credentials path).
  Screens::drawQrScreenWifi(&canvas, nullptr, nullptr);
  dump("qr_wifi_unavailable");

  // 6. Drag filmstrip. The firmware keeps two canvases -- the SHEET holds the
  // pairing card and the FACE holds the mascot. Rebuild both here into their
  // own framebuffer copies so compositeDragFrame can read them side by side
  // without repainting between rows.
  std::printf("host_preview: rendering drag filmstrip to %s/\n", kOutDir);
  const std::size_t pixels = static_cast<std::size_t>(W) * H;
  std::uint16_t* sheetFb = static_cast<std::uint16_t*>(
      std::malloc(pixels * sizeof(std::uint16_t)));
  std::uint16_t* faceFb  = static_cast<std::uint16_t*>(
      std::malloc(pixels * sizeof(std::uint16_t)));
  std::uint16_t* dragFb  = static_cast<std::uint16_t*>(
      std::malloc(pixels * sizeof(std::uint16_t)));
  if ((sheetFb == nullptr) || (faceFb == nullptr) || (dragFb == nullptr)) {
    std::fprintf(stderr, "host_preview: OOM for filmstrip buffers\n");
    return 4;
  }

  Screens::drawQrScreenWifi(&canvas, kFakeSsid, kFakePwd);
  std::memcpy(sheetFb, canvas.getFramebuffer(), pixels * sizeof(std::uint16_t));

  renderFaceTargetToCanvas(&canvas);
  std::memcpy(faceFb,  canvas.getFramebuffer(), pixels * sizeof(std::uint16_t));

  // Drive SheetTransition::Controller with synthetic press-move-move-release
  // so the emitted frames trace a real gesture, not just linearly-spaced Y
  // values. The controller is the shipping code; using it here means the
  // filmstrip proves the physics as well as the rendering.
  SheetTransition::Controller ctrl;
  uint32_t nowMs = 0;
  const int16_t startY = 8;  // finger presses near the top edge
  ctrl.onPress(startY, nowMs);

  auto emit = [&](const char* tag, int16_t sheetY) {
    compositeDragFrame(dragFb, sheetFb, faceFb, sheetY);
    char path[256];
    std::snprintf(path, sizeof(path), "%s/drag_%s.ppm", kOutDir, tag);
    writePpm(path, dragFb, W, H);
    std::printf("  wrote %s  (sheetY=%d)\n", path, static_cast<int>(sheetY));
  };

  emit("00_closed", 0);

  // Move the finger down in 4 steps; sample the CONTROLLER's y each time so
  // rubber-banding and 1:1 tracking are both visible in the frames.
  const int16_t stepTargets[] = {70, 140, 210, 280};
  const char*   stepTags[]    = {"01_25pct", "02_50pct", "03_75pct", "04_open"};
  for (std::size_t i = 0; i < sizeof(stepTargets) / sizeof(stepTargets[0]); ++i) {
    nowMs += 40;  // ~25 fps sample cadence
    const int16_t fingerY = static_cast<int16_t>(startY + stepTargets[i]);
    ctrl.onMove(fingerY, nowMs, H);
    emit(stepTags[i], ctrl.y);
  }

  std::free(sheetFb);
  std::free(faceFb);
  std::free(dragFb);

  std::printf("host_preview: done.\n");
  return 0;
}
