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
}

}  // namespace

int main() {
  testGrayConversionEndpoints();
  testBlendEndpointsAndMidpoint();
  testForegroundLogoLayeringIsSymmetric();
  testSmoothstepIsMonotonicAndBounded();
  return 0;
}
