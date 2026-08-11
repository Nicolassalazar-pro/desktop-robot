#include "../src/display_transition.h"

#include <assert.h>
#include <stdint.h>

#ifdef NDEBUG
#error "Host safety tests require assertions enabled"
#endif

namespace {

void testGrayConversionEndpoints() {
  assert(DisplayTransition::grayToRgb565(0U) == 0x0000U);
  assert(DisplayTransition::grayToRgb565(255U) == 0xFFFFU);
}

void testBlendEndpointsAndMidpoint() {
  assert(
      DisplayTransition::blendRgb565(0x1234U, 0xABCDU, 0U) ==
      0x1234U);
  assert(
      DisplayTransition::blendRgb565(0x1234U, 0xABCDU, 255U) ==
      0xABCDU);
  const uint16_t midpoint =
      DisplayTransition::blendRgb565(0x0000U, 0xFFFFU, 128U);
  assert(midpoint == 0x8410U);
}

void testForegroundLogoLayeringIsSymmetric() {
  constexpr uint16_t face = 0xF800U;
  constexpr uint8_t mask = 255U;

  assert(
      DisplayTransition::startupPixel(face, mask, 0U) ==
      0xFFFFU);
  assert(
      DisplayTransition::startupPixel(face, mask, 255U) ==
      face);
  assert(
      DisplayTransition::shutdownPixel(face, mask, 0U) ==
      face);
  assert(
      DisplayTransition::shutdownPixel(face, mask, 255U) ==
      0xFFFFU);

  const uint8_t progress = 128U;
  const uint16_t faceBehind =
      DisplayTransition::blendRgb565(0x0000U, face, progress);
  const uint8_t startupLogoAlpha =
      DisplayTransition::multiplyAlpha(mask, 127U);
  assert(
      DisplayTransition::startupPixel(face, mask, progress) ==
      DisplayTransition::compositeWhiteOver(
          faceBehind, startupLogoAlpha));

  const uint16_t shutdownFaceBehind =
      DisplayTransition::blendRgb565(face, 0x0000U, progress);
  const uint8_t shutdownLogoAlpha =
      DisplayTransition::multiplyAlpha(mask, progress);
  assert(
      DisplayTransition::shutdownPixel(face, mask, progress) ==
      DisplayTransition::compositeWhiteOver(
          shutdownFaceBehind, shutdownLogoAlpha));
}

void testSmoothstepIsMonotonicAndBounded() {
  uint8_t previous = 0U;
  for (uint8_t step = 0U; step <= 12U; ++step) {
    const uint8_t value = DisplayTransition::smoothstep8(step, 12U);
    assert(value >= previous);
    previous = value;
  }
  assert(DisplayTransition::smoothstep8(0U, 12U) == 0U);
  assert(DisplayTransition::smoothstep8(12U, 12U) == 255U);

  // The endpoint assertions above are silently satisfied by a broken
  // smoothstep that returns 0 for every intermediate step and 255 only
  // at step==12 -- which visually would be a snap, not a fade. Assert
  // that SOMETHING between the endpoints is strictly between 0 and 255,
  // and specifically that the midpoint has already climbed past a
  // quarter of full brightness. If it hasn't, the fade will look flat
  // for its first half and then snap on.
  const uint8_t mid = DisplayTransition::smoothstep8(6U, 12U);
  assert(mid > 64U);
  assert(mid < 191U);
}

}  // namespace

int main() {
  testGrayConversionEndpoints();
  testBlendEndpointsAndMidpoint();
  testForegroundLogoLayeringIsSymmetric();
  testSmoothstepIsMonotonicAndBounded();
  return 0;
}
