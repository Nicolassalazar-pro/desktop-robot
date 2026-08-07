#include "imu_qmi8658.h"

#include <Arduino.h>
#include <Wire.h>

namespace ImuQmi8658 {
namespace {

#ifndef IMU_ADDR
#define IMU_ADDR 0x6B
#endif

constexpr uint8_t kAddress = IMU_ADDR;

constexpr uint8_t kRegWhoAmI = 0x00;
constexpr uint8_t kRegCtrl1 = 0x02;
constexpr uint8_t kRegCtrl2 = 0x03;
constexpr uint8_t kRegCtrl3 = 0x04;
constexpr uint8_t kRegCtrl7 = 0x08;
constexpr uint8_t kRegCtrl5 = 0x06;
constexpr uint8_t kRegCtrl9 = 0x0A;
constexpr uint8_t kRegStatusInt = 0x2D;
constexpr uint8_t kRegCodStatus = 0x46;
constexpr uint8_t kRegCalOut = 0x51;   // gX/gY/gZ gain readback, 6 bytes
constexpr uint8_t kRegCal1L = 0x0B;    // gain write-back for 0xAA
constexpr uint8_t kRegAccelX = 0x35;
constexpr uint8_t kRegGyroX = 0x3B;

constexpr uint8_t kCmdAck = 0x00;
constexpr uint8_t kCmdCod = 0xA2;
constexpr uint8_t kCmdApplyGyroGains = 0xAA;

constexpr uint8_t kWhoAmIExpected = 0x05;

// CTRL1: keep SPI_BE (bit5, power-on default) and set SPI_AI (bit6) so burst
// reads auto-increment the register pointer. Without SPI_AI a six-byte read
// returns the same register six times — which looks like a stuck-but-live
// sensor rather than a configuration error.
constexpr uint8_t kCtrl1Value = 0x60;

// CTRL2: accelerometer ±2 g (aFS=000) at 500 Hz (aODR=0100). Only used to find
// which axis gravity is on, so sensitivity beats range here.
constexpr uint8_t kCtrl2Value = 0x04;

// CTRL3: gyroscope ±2048 dps (gFS=111) at 1000 Hz (gODR=0011).
constexpr uint8_t kCtrl3Value = 0x73;

// CTRL7: gEN | aEN.
constexpr uint8_t kCtrl7Value = 0x03;

bool g_available = false;
uint8_t g_whoAmI = 0;
bool g_lpfEnabled = false;
uint8_t g_lpfMode = 0;
float g_scaleCorrection = 1.0f;

bool writeRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(kAddress);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readRegisters(uint8_t reg, uint8_t* out, uint8_t count) {
  Wire.beginTransmission(kAddress);
  Wire.write(reg);
  // Repeated START: releasing the bus here would let the shared touch/RTC
  // traffic interleave between the pointer write and the read.
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(kAddress, count) != count) {
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) {
    out[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool readVector(uint8_t reg, Sample& out) {
  if (!g_available) return false;
  uint8_t raw[6] = {0};
  if (!readRegisters(reg, raw, sizeof(raw))) {
    return false;
  }
  out.x = static_cast<int16_t>(static_cast<uint16_t>(raw[0]) |
                               (static_cast<uint16_t>(raw[1]) << 8));
  out.y = static_cast<int16_t>(static_cast<uint16_t>(raw[2]) |
                               (static_cast<uint16_t>(raw[3]) << 8));
  out.z = static_cast<int16_t>(static_cast<uint16_t>(raw[4]) |
                               (static_cast<uint16_t>(raw[5]) << 8));
  return true;
}

// Issues a CTRL9 command and waits for the device to acknowledge it.
//
// The handshake is not optional: the command register is a mailbox, and
// writing a second command before the first completes silently loses one of
// them. Poll STATUSINT bit7 (CmdDone), then write the ACK so the device knows
// the result was collected.
bool ctrl9Command(uint8_t command, uint32_t timeoutMs) {
  if (!writeRegister(kRegCtrl9, command)) return false;

  const uint32_t deadline = millis() + timeoutMs;
  bool done = false;
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    uint8_t status = 0;
    if (readRegisters(kRegStatusInt, &status, 1) && (status & 0x80) != 0) {
      done = true;
      break;
    }
    delay(10);
  }
  if (!done) return false;

  // ACK, then wait for the device to drop CmdDone.
  if (!writeRegister(kRegCtrl9, kCmdAck)) return false;
  const uint32_t ackDeadline = millis() + 200;
  while (static_cast<int32_t>(millis() - ackDeadline) < 0) {
    uint8_t status = 0;
    if (readRegisters(kRegStatusInt, &status, 1) && (status & 0x80) == 0) {
      break;
    }
    delay(5);
  }
  return true;
}

}  // namespace

bool begin() {
  g_available = false;

  uint8_t who = 0;
  if (!readRegisters(kRegWhoAmI, &who, 1)) {
    return false;
  }
  g_whoAmI = who;
  if (who != kWhoAmIExpected) {
    return false;
  }

  if (!writeRegister(kRegCtrl1, kCtrl1Value)) return false;
  if (!writeRegister(kRegCtrl2, kCtrl2Value)) return false;
  if (!writeRegister(kRegCtrl3, kCtrl3Value)) return false;
  if (!writeRegister(kRegCtrl7, kCtrl7Value)) return false;

  // The gyro needs time to spin up before its output means anything.
  delay(50);
  g_available = true;
  return true;
}

bool available() {
  return g_available;
}

bool readGyroRaw(Sample& out) {
  return readVector(kRegGyroX, out);
}

bool readAccelRaw(Sample& out) {
  return readVector(kRegAccelX, out);
}

float gyroDps(int16_t raw) {
  return static_cast<float>(raw) * kGyroDpsPerLsb * g_scaleCorrection;
}

void setGyroScaleCorrection(float correction) {
  // Bounded hard. A correction outside this range is far more likely to be a
  // typo or a misread mark than a real sensitivity error, and applying it
  // would silently corrupt every angle the robot reports thereafter.
  if (!(correction > 0.5f) || !(correction < 2.0f)) return;
  g_scaleCorrection = correction;
}

float gyroScaleCorrection() {
  return g_scaleCorrection;
}

float accelG(int16_t raw) {
  return static_cast<float>(raw) * kAccelGPerLsb;
}

uint8_t whoAmI() {
  return g_whoAmI;
}

const char* codFailureReason(uint8_t status) {
  // Bits 7..4 are the gyro sensitivity limit checks — four of the eight bits,
  // which is the strongest evidence that COD is a SCALE routine rather than a
  // bias one. Bits 3..0 are the process failures.
  if ((status & 0x01) != 0) return "COD failed, no correction applied";
  if ((status & 0x02) != 0) return "COD called while the gyro was enabled";
  if ((status & 0x04) != 0) return "gyro startup failure";
  if ((status & 0x08) != 0) return "significant vibration during COD";
  if ((status & 0x10) != 0) return "gyro X sensitivity below limit";
  if ((status & 0x20) != 0) return "gyro X sensitivity above limit";
  if ((status & 0x40) != 0) return "gyro Y sensitivity below limit";
  if ((status & 0x80) != 0) return "gyro Y sensitivity above limit";
  return nullptr;
}

bool runCod(CodResult& out) {
  out.ok = false;
  out.status = 0xFF;
  out.gainX = 0;
  out.gainY = 0;
  out.gainZ = 0;
  if (!g_available) return false;

  // The device must have both sensors OFF, and it explicitly reports being
  // called with the gyro enabled as a failure.
  if (!writeRegister(kRegCtrl7, 0x00)) return false;
  delay(20);

  // ~1.5 s per the datasheet; the reference driver waits 1600 ms. The command
  // handshake is polled rather than assumed.
  const bool completed = ctrl9Command(kCmdCod, 3000);

  uint8_t status = 0xFF;
  const bool haveStatus = readRegisters(kRegCodStatus, &status, 1);
  out.status = status;

  uint8_t gains[6] = {0};
  const bool haveGains = readRegisters(kRegCalOut, gains, sizeof(gains));
  if (haveGains) {
    // Unsigned, which is what makes these multiplicative GAINS rather than
    // signed offsets — offsets have their own command (0x0A).
    out.gainX = static_cast<uint16_t>(gains[0] | (gains[1] << 8));
    out.gainY = static_cast<uint16_t>(gains[2] | (gains[3] << 8));
    out.gainZ = static_cast<uint16_t>(gains[4] | (gains[5] << 8));
  }

  // Restore the normal configuration whatever happened, so a failed
  // calibration can never leave the sensors switched off.
  writeRegister(kRegCtrl1, kCtrl1Value);
  writeRegister(kRegCtrl2, kCtrl2Value);
  writeRegister(kRegCtrl3, kCtrl3Value);
  if (g_lpfEnabled) {
    setGyroLowPass(true, g_lpfMode);
  }
  writeRegister(kRegCtrl7, kCtrl7Value);
  delay(50);

  out.ok = completed && haveStatus && haveGains &&
           (codFailureReason(status) == nullptr);
  return out.ok;
}

bool applyGains(uint16_t gainX, uint16_t gainY, uint16_t gainZ) {
  if (!g_available) return false;
  // Parameters go into CAL1..CAL3 as little-endian pairs, then the command
  // tells the device to adopt them.
  const uint8_t payload[6] = {
      static_cast<uint8_t>(gainX & 0xFF), static_cast<uint8_t>(gainX >> 8),
      static_cast<uint8_t>(gainY & 0xFF), static_cast<uint8_t>(gainY >> 8),
      static_cast<uint8_t>(gainZ & 0xFF), static_cast<uint8_t>(gainZ >> 8)};
  for (uint8_t i = 0; i < sizeof(payload); ++i) {
    if (!writeRegister(static_cast<uint8_t>(kRegCal1L + i), payload[i])) {
      return false;
    }
  }
  return ctrl9Command(kCmdApplyGyroGains, 500);
}

bool setGyroLowPass(bool enabled, uint8_t mode) {
  if (mode > 3) return false;
  uint8_t ctrl5 = 0;
  // Read-modify-write: CTRL5 also holds the ACCELEROMETER filter bits, and
  // clobbering them would silently change how gravity is sampled.
  if (!readRegisters(kRegCtrl5, &ctrl5, 1)) return false;
  ctrl5 = static_cast<uint8_t>(ctrl5 & 0x8F);   // clear gLPF_EN + gLPF_MODE
  if (enabled) {
    ctrl5 = static_cast<uint8_t>(ctrl5 | 0x10 | ((mode & 0x03) << 5));
  }
  if (!writeRegister(kRegCtrl5, ctrl5)) return false;
  g_lpfEnabled = enabled;
  g_lpfMode = mode;
  return true;
}

bool gyroLowPassEnabled() { return g_lpfEnabled; }
uint8_t gyroLowPassMode() { return g_lpfMode; }

}  // namespace ImuQmi8658
