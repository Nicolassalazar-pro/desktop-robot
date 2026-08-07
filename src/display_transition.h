#pragma once

#include <stdint.h>

namespace DisplayTransition {

inline uint16_t grayToRgb565(uint8_t gray) {
  return static_cast<uint16_t>(
      ((static_cast<uint16_t>(gray) >> 3U) << 11U) |
      ((static_cast<uint16_t>(gray) >> 2U) << 5U) |
      (static_cast<uint16_t>(gray) >> 3U));
}

inline uint16_t blendRgb565(
    uint16_t from, uint16_t to, uint8_t alpha) {
  const uint16_t inverse = static_cast<uint16_t>(255U - alpha);
  const uint16_t fromR = static_cast<uint16_t>((from >> 11U) & 0x1FU);
  const uint16_t fromG = static_cast<uint16_t>((from >> 5U) & 0x3FU);
  const uint16_t fromB = static_cast<uint16_t>(from & 0x1FU);
  const uint16_t toR = static_cast<uint16_t>((to >> 11U) & 0x1FU);
  const uint16_t toG = static_cast<uint16_t>((to >> 5U) & 0x3FU);
  const uint16_t toB = static_cast<uint16_t>(to & 0x1FU);
  const uint16_t outR =
      static_cast<uint16_t>((fromR * inverse + toR * alpha + 127U) / 255U);
  const uint16_t outG =
      static_cast<uint16_t>((fromG * inverse + toG * alpha + 127U) / 255U);
  const uint16_t outB =
      static_cast<uint16_t>((fromB * inverse + toB * alpha + 127U) / 255U);
  return static_cast<uint16_t>((outR << 11U) | (outG << 5U) | outB);
}

inline uint8_t multiplyAlpha(uint8_t first, uint8_t second) {
  return static_cast<uint8_t>(
      (static_cast<uint16_t>(first) *
           static_cast<uint16_t>(second) +
       127U) /
      255U);
}

inline uint16_t compositeWhiteOver(
    uint16_t background, uint8_t alpha) {
  return blendRgb565(background, 0xFFFFU, alpha);
}

// The face brightens from black behind a white foreground logo whose alpha
// falls away. The logo remains visually in front for the entire transition.
inline uint16_t startupPixel(
    uint16_t face, uint8_t logoMask, uint8_t progress) {
  const uint16_t faceBehind =
      blendRgb565(0x0000U, face, progress);
  const uint8_t logoAlpha =
      multiplyAlpha(logoMask, static_cast<uint8_t>(255U - progress));
  return compositeWhiteOver(faceBehind, logoAlpha);
}

// Exact inverse of startupPixel: the face darkens behind while the white
// foreground logo fades in.
inline uint16_t shutdownPixel(
    uint16_t face, uint8_t logoMask, uint8_t progress) {
  const uint16_t faceBehind =
      blendRgb565(face, 0x0000U, progress);
  const uint8_t logoAlpha = multiplyAlpha(logoMask, progress);
  return compositeWhiteOver(faceBehind, logoAlpha);
}

inline uint8_t smoothstep8(uint8_t step, uint8_t finalStep) {
  if (finalStep == 0U || step >= finalStep) {
    return 255U;
  }
  const uint32_t x =
      static_cast<uint32_t>(step) * 255U / finalStep;
  const uint32_t x2 = x * x;
  const uint32_t eased =
      x2 * (765U - 2U * x) / (255U * 255U);
  return static_cast<uint8_t>(eased > 255U ? 255U : eased);
}

}  // namespace DisplayTransition
