// -----------------------------------------------------------------------------
// screens.cpp -- pairing-flow LCD drawing. Compiled by BOTH firmware and the
// host preview harness. See screens.h for the "why" of the extraction.
//
// This file must not reach for firmware globals (g_canvas, credentials, WiFi
// state). Every input comes in through the function signature. Every temporary
// (QR encode buffer, formatted password label) is local and is zeroed before
// the function returns, mirroring the security posture of the pre-extraction
// main.cpp.
// -----------------------------------------------------------------------------

#include "screens.h"

#include <cstdio>
#include <cstring>

// The upstream aggregate Arduino_GFX_Library.h pulls in every databus/driver
// header. We only need the base class and the plain 5x7 font support, so
// include the two narrower headers directly. This is important on the host
// build, where Arduino_GFX_Library.h would drag in ESP32 SPI stubs.
#include "Arduino_GFX.h"

// qrcode.h is C-linkage; the QRCode/ vendored library ships one plain-C
// implementation and the encoder is used verbatim on both targets.
extern "C" {
#include "qrcode.h"
}

// mascot_a.h is shared bitmap data used by both the firmware idle screen
// and (during the drag transition) by the host preview harness.
#include "mascot/mascot_a.h"

namespace Screens {
namespace {

// Local copy of secureZero from main.cpp. The QR encoder scratch buffer holds
// the encoded credentials for the duration of the draw; zero it out before
// leaving the stack frame. Kept as a private static so screens.cpp does not
// have to link against main.cpp's copy.
void secureZeroLocal(void* data, std::size_t length) {
  volatile std::uint8_t* bytes = static_cast<volatile std::uint8_t*>(data);
  while (length-- > 0U) {
    *bytes++ = 0U;
  }
}

// Draw a single QR module at the given pixel scale (scale x scale). Inline
// because the two-nested-for loop below can hit thousands of calls per QR.
inline void qrDrawModule(Arduino_GFX* gfx,
                         int16_t x, int16_t y, std::uint8_t scale) {
  gfx->fillRect(x, y, scale, scale, kQrBlack);
}

// Render a QR matrix into `gfx` at the given pixel scale. The caller's white
// background acts as the quiet zone; the modules are painted in black.
void drawQrToCanvas(Arduino_GFX* gfx, QRCode* qr,
                    int16_t originX, int16_t originY, std::uint8_t scale) {
  for (std::uint8_t my = 0; my < qr->size; ++my) {
    for (std::uint8_t mx = 0; mx < qr->size; ++mx) {
      if (qrcode_getModule(qr, mx, my)) {
        qrDrawModule(gfx,
                     static_cast<int16_t>(originX + mx * scale),
                     static_cast<int16_t>(originY + my * scale),
                     scale);
      }
    }
  }
}

}  // namespace

// ---------------------------- Text primitives -------------------------------

void qrCenterText(Arduino_GFX* gfx, int16_t y, std::uint8_t textSize,
                  const char* s) {
  if ((gfx == nullptr) || (s == nullptr)) return;
  gfx->setTextSize(textSize);
  gfx->setTextColor(kQrBlack);
  gfx->setTextWrap(false);
  const int16_t textW =
      static_cast<int16_t>(std::strlen(s) * 6 * textSize);
  const int16_t x = static_cast<int16_t>((kScreenW - textW) / 2);
  gfx->setCursor(x, y);
  gfx->print(s);
}

// DSGN-6: prefer size 2 for legibility; drop to size 1 only when the caller
// passed a string long enough to overflow the 240-px canvas at size 2. Keeps
// SSID/password readable across the room without truncating off-screen when
// a longer credential lands.
void qrCenterTextFit(Arduino_GFX* gfx, int16_t y, const char* s) {
  if (s == nullptr) return;
  const std::size_t len = std::strlen(s);
  const std::uint8_t textSize =
      (static_cast<int16_t>(len * 6 * 2) <= kScreenW) ? 2 : 1;
  qrCenterText(gfx, y, textSize, s);
}

// Like qrCenterText, but for the "Establishing Server..." dark splash:
// white text on black background.
void qrCenterTextDark(Arduino_GFX* gfx, int16_t y, std::uint8_t textSize,
                      const char* s) {
  if ((gfx == nullptr) || (s == nullptr)) return;
  gfx->setTextSize(textSize);
  gfx->setTextColor(kQrWhite);
  gfx->setTextWrap(false);
  const int16_t textW =
      static_cast<int16_t>(std::strlen(s) * 6 * textSize);
  const int16_t x = static_cast<int16_t>((kScreenW - textW) / 2);
  gfx->setCursor(x, y);
  gfx->print(s);
}

// ---------------------------- Step dots -------------------------------------

void drawPairingStepDots(Arduino_GFX* gfx, int16_t y, std::uint8_t step) {
  if (gfx == nullptr) return;
  constexpr int16_t kR   = 4;
  constexpr int16_t kGap = 18;
  const int16_t cx = kScreenW / 2;
  for (std::uint8_t i = 0; i < 2U; ++i) {
    const int16_t x =
        static_cast<int16_t>(cx - (kGap / 2) + (i * kGap));
    if (i < step) {
      gfx->fillCircle(x, y, kR, kQrBlack);
    } else {
      gfx->drawCircle(x, y, kR, kQrBlack);
    }
  }
}

// ---------------------------- QR screens ------------------------------------

// Paint the "scan to join Wi-Fi" screen. Layout mirrors the pre-extraction
// firmware; every offset here was previously in main.cpp. See the comment on
// drawQrScreenWifi in main.cpp for the DSGN-6 / design-craft reasoning that
// drove these choices.
void drawQrScreenWifi(Arduino_GFX* gfx, const char* ssid, const char* password) {
  if (gfx == nullptr) return;
  gfx->fillScreen(kQrWhite);
  if ((ssid == nullptr) || (password == nullptr)) {
    qrCenterText(gfx, /*y=*/132, /*size=*/1, "Wi-Fi unavailable");
    return;
  }

  char wifiPayload[96];
  std::snprintf(wifiPayload, sizeof(wifiPayload),
                "WIFI:T:WPA;S:%s;P:%s;;", ssid, password);

  // pw:<password> label. Sized against the same 63-byte password cap the
  // firmware advertises via kApPasswordMaxLength.
  char passwordLabel[64];
  std::snprintf(passwordLabel, sizeof(passwordLabel), "pw:%s", password);

  std::uint8_t wifiBuf[200];
  QRCode wifiQr;
  qrcode_initText(&wifiQr, wifiBuf, kQrWifiVersion, ECC_MEDIUM, wifiPayload);
  const int16_t px = static_cast<int16_t>(wifiQr.size * kQrWifiPixelScale);
  const int16_t x  = static_cast<int16_t>((kScreenW - px) / 2);
  const int16_t y  = 6;
  drawQrToCanvas(gfx, &wifiQr, x, y, kQrWifiPixelScale);

  qrCenterText(gfx, /*y=*/206, /*size=*/1, "Scan to join Wi-Fi");
  qrCenterTextFit(gfx, /*y=*/220, ssid);
  qrCenterTextFit(gfx, /*y=*/240, passwordLabel);
  drawPairingStepDots(gfx, /*y=*/264, /*step=*/1U);
  qrCenterText(gfx, /*y=*/272, /*size=*/1, "swipe up to exit");

  secureZeroLocal(wifiBuf, sizeof(wifiBuf));
  secureZeroLocal(passwordLabel, sizeof(passwordLabel));
  secureZeroLocal(wifiPayload, sizeof(wifiPayload));
}

// Paint the "scan to open browser" screen. Layout (240x280):
//   y=20  URL QR scale 7 (203x203) centered
//   y=240 "Scan to open control" size 1 centered
//   y=264 step dot indicator (2 of 2)
//   y=272 exit hint
void drawQrScreenUrl(Arduino_GFX* gfx, const char* url) {
  if (gfx == nullptr) return;
  gfx->fillScreen(kQrWhite);
  if (url == nullptr) {
    qrCenterText(gfx, /*y=*/132, /*size=*/1, "Access unavailable");
    return;
  }

  std::uint8_t urlBuf[200];
  QRCode urlQr;
  qrcode_initText(&urlQr, urlBuf, kQrUrlVersion, ECC_MEDIUM, url);
  const int16_t px = static_cast<int16_t>(urlQr.size * kQrUrlPixelScale);
  const int16_t x  = static_cast<int16_t>((kScreenW - px) / 2);
  const int16_t y  = 20;
  drawQrToCanvas(gfx, &urlQr, x, y, kQrUrlPixelScale);

  qrCenterText(gfx, /*y=*/240, /*size=*/1, "Scan to open control");
  drawPairingStepDots(gfx, /*y=*/264, /*step=*/2U);
  qrCenterText(gfx, /*y=*/272, /*size=*/1, "swipe up to exit");

  secureZeroLocal(urlBuf, sizeof(urlBuf));
}

// ---------------------------- Pairing failed notice -------------------------

void drawPairingFailed(Arduino_GFX* gfx) {
  if (gfx == nullptr) return;
  gfx->fillScreen(0x0000);
  const std::uint8_t kSize = 2;
  const char* kL1 = "Pairing";
  const char* kL2 = "failed";
  const char* kL3 = "swipe down to retry";
  gfx->setTextWrap(false);
  gfx->setTextSize(kSize);
  gfx->setTextColor(kQrWhite);
  gfx->setCursor(
      static_cast<int16_t>((kScreenW -
                            static_cast<int16_t>(std::strlen(kL1) * 6 * kSize)) / 2),
      110);
  gfx->print(kL1);
  gfx->setCursor(
      static_cast<int16_t>((kScreenW -
                            static_cast<int16_t>(std::strlen(kL2) * 6 * kSize)) / 2),
      136);
  gfx->print(kL2);
  gfx->setTextSize(1);
  gfx->setTextColor(0x8410);
  gfx->setCursor(
      static_cast<int16_t>((kScreenW -
                            static_cast<int16_t>(std::strlen(kL3) * 6)) / 2),
      172);
  gfx->print(kL3);
}

// ---------------------------- Mascot / face backdrop ------------------------

void drawMascotToCanvas(Arduino_GFX* gfx, int16_t originX, int16_t originY) {
  if (gfx == nullptr) return;
  gfx->draw16bitRGBBitmap(originX,
                          originY,
                          const_cast<std::uint16_t*>(MASCOT_A_RGB565),
                          MASCOT_A_W,
                          MASCOT_A_H);
}

void drawFaceBackdrop(Arduino_GFX* gfx, int16_t originX, int16_t originY) {
  if (gfx == nullptr) return;
  gfx->fillScreen(MASCOT_A_BG);
  drawMascotToCanvas(gfx, originX, originY);
}

}  // namespace Screens
