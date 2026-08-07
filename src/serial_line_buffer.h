#pragma once

#include <stddef.h>
#include <stdint.h>

enum class SerialLineEvent : uint8_t {
  None,
  LineReady,
  InvalidByte,
  Overflow,
};

template <size_t Capacity>
class SerialLineBuffer {
 public:
  static_assert(Capacity >= 2, "serial line capacity is too small");

  SerialLineEvent push(uint8_t byte) {
    if (discarding_) {
      if (byte == '\r' || byte == '\n') {
        reset();
      }
      return SerialLineEvent::None;
    }

    if (byte == '\r' || byte == '\n') {
      if (length_ == 0U) {
        return SerialLineEvent::None;
      }
      buffer_[length_] = '\0';
      ready_ = true;
      return SerialLineEvent::LineReady;
    }

    if (byte == 0U || ((byte < 0x20U) && (byte != '\t')) ||
        byte > 0x7EU) {
      length_ = 0;
      ready_ = false;
      discarding_ = true;
      return SerialLineEvent::InvalidByte;
    }

    if (length_ >= Capacity - 1U) {
      length_ = 0;
      ready_ = false;
      discarding_ = true;
      return SerialLineEvent::Overflow;
    }

    buffer_[length_++] = static_cast<char>(byte);
    return SerialLineEvent::None;
  }

  char* line() {
    return ready_ ? buffer_ : nullptr;
  }

  void consume() {
    reset();
  }

 private:
  void reset() {
    length_ = 0;
    buffer_[0] = '\0';
    ready_ = false;
    discarding_ = false;
  }

  char buffer_[Capacity] = {};
  size_t length_ = 0;
  bool ready_ = false;
  bool discarding_ = false;
};
