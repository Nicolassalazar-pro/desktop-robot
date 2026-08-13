// -----------------------------------------------------------------------------
// screens.h -- pure drawing surface for the pairing-flow LCD screens.
//
// WHY THIS FILE EXISTS
//
// src/main.cpp is ~12 000 lines and mixes WiFi/servo/IMU/touch state with the
// draw calls. A previous host preview (tools/lcd_preview.py) tried to
// reimplement the drawing in Python by parsing coordinates out of C++ -- and
// drifted: it kept rendering a drawSheetCard() placeholder that had already
// been deleted from the firmware. That drift is exactly what this extraction
// exists to prevent.
//
// The functions below take an explicit `Arduino_GFX*` target and their data
// as plain parameters. On the firmware, main.cpp passes `g_canvas` and the
// real credentials/URL, still gated by copyActivationCredentials() and
// buildAccessUrl(). On the host preview harness, tools/host_preview/main.cpp
// creates its own Arduino_Canvas(240, 280, nullptr) and passes fake strings.
//
// Both callers compile THE SAME src/screens.cpp -- there is no reimplementation
// path, and therefore no drift. If someone deletes drawSheetCard from screens.
// cpp the host preview loses it too, in the same commit.
//
// Deliberately excluded from this file:
//  - stageOpenCardWithQr / handOffSheetQrToPanel -- they orchestrate SPI
//    handoff and touch g_gfx/pushCanvas*; not drawing.
//  - drawEstablishingScreen -- currently still in main.cpp because it composes
//    with the mascot BATTERY strip and other firmware state. Reuses
//    Screens::qrCenterTextDark from here.
//  - copyActivationCredentials / buildAccessUrl -- they own the security
//    envelope. Callers retrieve the plaintext, hand the strings in, and
//    secureZero after the draw. screens.cpp only zeroes its own local encoding
//    buffers.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

// Forward decl -- avoids pulling the whole GFX header into everything that
// includes screens.h. .cpp files that actually need the type include
// <Arduino_GFX.h> (or the library aggregate) themselves.
class Arduino_GFX;

namespace Screens {

// Mirrors of the panel constants from main.cpp / platformio.ini.
// Kept as constexpr in this header so the host preview does not need to
// include main.cpp or the -D LCD_WIDTH define from platformio.ini.
// The static_assert lower down in this header pins them to the -D values on
// the firmware build so the two cannot silently drift.
constexpr int16_t  kScreenW           = 240;
constexpr int16_t  kScreenH           = 280;

// Palette / geometry duplicated from main.cpp lines 6119-6124. Keeping local
// copies means screens.cpp is self-contained; main.cpp still owns the
// originals and both are visited whenever the QR palette or scale changes.
constexpr uint16_t kQrWhite           = 0xFFFF;
constexpr uint16_t kQrBlack           = 0x0000;
constexpr uint8_t  kQrWifiPixelScale  = 6;
constexpr uint8_t  kQrUrlPixelScale   = 7;
constexpr uint8_t  kQrWifiVersion     = 4;   // 33x33 modules
constexpr uint8_t  kQrUrlVersion      = 3;   // 29x29 modules

// The firmware defines LCD_WIDTH / LCD_HEIGHT on the g++ command line via
// platformio.ini. If someone changes those without updating this header we
// want a compile break, not a runtime drift.
#if defined(LCD_WIDTH) && defined(LCD_HEIGHT)
static_assert(kScreenW == LCD_WIDTH,
              "Screens::kScreenW drifted from -D LCD_WIDTH");
static_assert(kScreenH == LCD_HEIGHT,
              "Screens::kScreenH drifted from -D LCD_HEIGHT");
#endif

// -- Text primitives (built-in 5x7 GFX font, 6 px advance) --------------------
// All three assume kQrWhite / kQrBlack contrast unless noted. They all disable
// wrap so an over-long line lands off-canvas rather than reflowing.
void qrCenterText(Arduino_GFX* gfx, int16_t y, uint8_t textSize, const char* s);
void qrCenterTextFit(Arduino_GFX* gfx, int16_t y, const char* s);
void qrCenterTextDark(Arduino_GFX* gfx, int16_t y, uint8_t textSize, const char* s);

// Two-dot step indicator: filled at step positions less than `step`, hollow
// otherwise. See drawPairingStepDots comment in main.cpp for the design-craft
// reasoning; the shape distinction survives colour-blind / photographed
// screens.
void drawPairingStepDots(Arduino_GFX* gfx, int16_t y, uint8_t step);

// The two pairing QR screens. `ssid`, `password`, `url` are C strings owned
// by the caller. The caller is responsible for zeroing them after the draw
// (secureZero in main.cpp; not the host harness's concern).
void drawQrScreenWifi(Arduino_GFX* gfx, const char* ssid, const char* password);
void drawQrScreenUrl(Arduino_GFX* gfx, const char* url);

// One-shot "Pairing failed" full-screen notice.  The wait / breadcrumbing /
// panel push that used to happen at the same call site stays in main.cpp so
// the host does not have to fake delay() / breadcrumb().
void drawPairingFailed(Arduino_GFX* gfx);

// Blit the mascot bitmap into `gfx` at (originX, originY). No side effects on
// the firmware's `g_batStripCanvasFresh` flag -- main.cpp's wrapper still
// clears that before delegating, because only main.cpp knows the flag exists.
void drawMascotToCanvas(Arduino_GFX* gfx, int16_t originX, int16_t originY);

// The idle-face backdrop the host preview needs to composite the drag frames
// against. The firmware never calls this by name -- setup() paints the same
// pixels by clearing to MASCOT_A_BG and calling drawMascotToCanvas -- but
// having one entry point in screens.cpp means the host preview cannot drift
// from that recipe either.
void drawFaceBackdrop(Arduino_GFX* gfx, int16_t originX, int16_t originY);

}  // namespace Screens
