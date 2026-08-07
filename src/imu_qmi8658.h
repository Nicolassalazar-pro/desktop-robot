#pragma once

// ============================================================================
// imu_qmi8658.h — minimal QMI8658C driver, gyro-first.
//
// Deliberately thin. Everything that can produce a plausible-but-wrong answer
// (axis choice, bias, integration, saturation, the correction arithmetic)
// lives in turn_measure.{h,cpp} where it is unit-tested on the host. This file
// only moves bytes.
//
// Register map verified against datasheets/QMI8658C_IMU.pdf (Rev 0.6):
//   0x00 WHO_AM_I  = 0x05
//   0x02 CTRL1     bit6 SPI_AI (address auto-increment), bit0 SensorDisable
//   0x03 CTRL2     aFS<6:4>, aODR<3:0>
//   0x04 CTRL3     gFS<6:4>, gODR<3:0>
//   0x08 CTRL7     bit1 gEN, bit0 aEN
//   0x35 AX_L      accelerometer, 6 bytes, int16 little-endian
//   0x3B GX_L      gyroscope,     6 bytes, int16 little-endian
//
// NOTE the chip has magnetometer registers (0x41+) and a CTRL4, but those are
// for an EXTERNAL magnetometer part. This board does not fit one — the I2C bus
// holds only the touch controller, this IMU and the RTC. There is no compass.
// ============================================================================

#include <stdint.h>

namespace ImuQmi8658 {

// ±2048 dps full scale. A 360 in 600 ms is 600 deg/s and a hard counter-
// rotation peaks higher; the widest range makes clipping effectively
// impossible, and 16 LSB/deg/s is still far finer than this measurement needs.
// Clipping would silently UNDER-read, which looks like a valid measurement,
// so headroom is worth more here than resolution.
constexpr float kGyroDpsPerLsb = 2048.0f / 32768.0f;   // 0.0625
constexpr float kAccelGPerLsb = 2.0f / 32768.0f;

struct Sample {
  int16_t x;
  int16_t y;
  int16_t z;
};

// Probes WHO_AM_I, configures accel + gyro and enables both. Returns false if
// the device does not answer or does not identify as a QMI8658.
bool begin();

bool available();

// Raw counts, two's complement, as read from the device.
bool readGyroRaw(Sample& out);
bool readAccelRaw(Sample& out);

// Convenience conversions.
float gyroDps(int16_t raw);
float accelG(int16_t raw);

// ---------------------------------------------------------------------------
// Software gyro scale correction.
//
// Scale factor is the ONE gyro error that cannot be measured from onboard data:
// bias is visible at standstill, coast is visible after the motors cut, but a
// scale error produces zero output at zero rate and is therefore invisible to
// every self-check available. It needs exactly one external observation — a
// mark on the floor.
//
// Applied here, in the single place raw counts become degrees per second, so
// every consumer inherits it and none can bypass it.
//
// Preferred over the chip's own COD because the provenance is known: this is a
// number somebody measured against a physical reference, not an opaque gain
// whose encoding we had to guess. Run 004 showed COD moving the scale by 15%,
// five times the datasheet tolerance, with no way to tell whether that was a
// fix or a fault.
// ---------------------------------------------------------------------------
void setGyroScaleCorrection(float correction);
float gyroScaleCorrection();

// Last WHO_AM_I value seen, for diagnostics.
uint8_t whoAmI();

// ---------------------------------------------------------------------------
// Calibration On Demand (COD) — the chip calibrating its own gyro SCALE.
//
// WHY THIS IS THE ONE THING WORTH ASKING THE CHIP FOR. Scale factor is the
// conversion from raw counts to deg/s. The datasheet tolerance is +/-3%, which
// is up to 10.8 degrees of error per 360. Crucially the zero-velocity update
// CANNOT see it: ZUPT works by reading the gyro while stationary, and 3% of
// zero is still zero. Scale error is only observable while actually rotating,
// against something that already knows the true angle — so it is otherwise the
// one error term that needs an external reference. COD removes that need.
//
// Mechanism (INFERRED from the reference driver, not stated in the datasheet):
// likely the same electrostatic actuation the self-test uses — drive the proof
// mass by a known amount, measure the response, set gain = expected/measured.
// That would explain why it must be perfectly still.
//
// Protocol: CTRL7 = 0x00 (sensors off) -> CTRL9 = 0xA2 -> poll STATUSINT bit7
// -> ACK -> read COD_STATUS. Takes ~1.6 s and FAILS if the device is moved or
// vibrating, which it reports rather than silently producing a bad gain.
//
// The gains are volatile — lost on every power cycle — so the caller must
// persist them and re-apply with applyGains() at boot.
// ---------------------------------------------------------------------------

struct CodResult {
  bool ok;
  uint8_t status;      // raw COD_STATUS (0x46)
  uint16_t gainX;
  uint16_t gainY;
  uint16_t gainZ;
};

// Runs COD. Blocks for ~1.6 s. The device must be still.
bool runCod(CodResult& out);

// Re-applies previously measured gains (CTRL9 = 0xAA). Used at boot, since the
// chip forgets them on power-down.
bool applyGains(uint16_t gainX, uint16_t gainY, uint16_t gainZ);

// Decodes a COD_STATUS byte into a human-readable reason, or nullptr if the
// status indicates success.
const char* codFailureReason(uint8_t status);

// ---------------------------------------------------------------------------
// Gyro low-pass filter (CTRL5).
//
// The chip integrates the full Nyquist band by default. Mode 0 cuts the
// bandwidth to 2.66% of ODR, a ~4.3x reduction in RMS noise.
//
// IMPORTANT: this does NOT reduce the integrated-angle error from white noise.
// Integration is already an average, so filtering before it is redundant — the
// angle error is sigma*sqrt(N)*dt either way. What it genuinely buys is
// ANTI-ALIASING: servo vibration above Nyquist otherwise folds down into the
// signal band as a false low-frequency rate, and no amount of averaging can
// remove that once it has aliased.
//
// The cost is ~38 ms of group delay, which lags the running integral by
// tau*omega — about 10.8 degrees at full turn speed. Runtime-switchable so the
// effect can be measured rather than assumed.
// ---------------------------------------------------------------------------
bool setGyroLowPass(bool enabled, uint8_t mode);
bool gyroLowPassEnabled();
uint8_t gyroLowPassMode();

}  // namespace ImuQmi8658
