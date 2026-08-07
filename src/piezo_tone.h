#pragma once

#include <stdint.h>

namespace PiezoTone {

bool begin(uint8_t pin);
bool command(uint16_t hz, uint32_t nowMs);
void service(uint32_t nowMs);
void stop();
bool active();
uint16_t frequency();

}  // namespace PiezoTone
