#include "piezo_tone.h"

#include <Arduino.h>
#include <esp32-hal-ledc.h>

namespace PiezoTone {
namespace {

constexpr uint16_t kMinFrequencyHz = 200;
constexpr uint16_t kMaxFrequencyHz = 6000;
constexpr uint32_t kWatchdogMs = 750;
constexpr uint8_t kToneChannel = 2;
constexpr uint32_t kInitialFrequencyHz = 1000;
constexpr uint8_t kResolutionBits = 10;
constexpr uint8_t kInvalidPin = UINT8_MAX;

uint8_t g_pin = kInvalidPin;
uint16_t g_frequencyHz = 0;
uint32_t g_lastCommandMs = 0;
bool g_initialized = false;
bool g_active = false;
bool g_attached = false;

void forceInactiveLow() {
  if (g_initialized) {
    ledcWriteTone(kToneChannel, 0);
    ledcWrite(kToneChannel, 0);
  }

  if (g_attached) {
    ledcDetachPin(g_pin);
    g_attached = false;
  }

  if ((g_pin != kInvalidPin) && digitalPinCanOutput(g_pin)) {
    pinMode(g_pin, OUTPUT);
    digitalWrite(g_pin, LOW);
  }

  g_active = false;
  g_frequencyHz = 0;
  g_lastCommandMs = 0;
}

}  // namespace

bool begin(uint8_t pin) {
  forceInactiveLow();

  g_initialized = false;
  g_pin = kInvalidPin;

  if (!digitalPinCanOutput(pin)) {
    return false;
  }

  g_pin = pin;
  pinMode(g_pin, OUTPUT);
  digitalWrite(g_pin, LOW);

  // On Arduino-ESP32 2.x, LEDC channel 2 is routed through timer 1.
  if (ledcSetup(kToneChannel, kInitialFrequencyHz, kResolutionBits) == 0U) {
    digitalWrite(g_pin, LOW);
    g_pin = kInvalidPin;
    return false;
  }

  ledcWrite(kToneChannel, 0);
  g_initialized = true;
  return true;
}

bool command(uint16_t hz, uint32_t nowMs) {
  if ((hz != 0) && ((hz < kMinFrequencyHz) || (hz > kMaxFrequencyHz))) {
    forceInactiveLow();
    return false;
  }
  if (!g_initialized) {
    forceInactiveLow();
    return false;
  }

  if (hz == 0) {
    // A valid stop is immediate and leaves the watchdog disarmed; it does not
    // create a new 750 ms lease because there is no active output to protect.
    stop();
    return true;
  }

  if (!g_attached) {
    ledcAttachPin(g_pin, kToneChannel);
    g_attached = true;
  }

  if (ledcWriteTone(kToneChannel, hz) == 0U) {
    forceInactiveLow();
    return false;
  }

  g_frequencyHz = hz;
  g_lastCommandMs = nowMs;
  g_active = true;
  return true;
}

void service(uint32_t nowMs) {
  const uint32_t elapsedMs = nowMs - g_lastCommandMs;
  if (g_active && (elapsedMs >= kWatchdogMs)) {
    stop();
  }
}

void stop() {
  forceInactiveLow();
}

bool active() {
  return g_active;
}

uint16_t frequency() {
  return g_frequencyHz;
}

}  // namespace PiezoTone
