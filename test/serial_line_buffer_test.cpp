#include "../src/serial_line_buffer.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#ifdef NDEBUG
#error "Host safety tests require assertions enabled"
#endif

namespace {

void testValidLineAndCrLf() {
  SerialLineBuffer<32> buffer;
  const char* command = "cal show\r\n";
  SerialLineEvent event = SerialLineEvent::None;
  for (const char* cursor = command; *cursor != '\0'; ++cursor) {
    event = buffer.push(static_cast<uint8_t>(*cursor));
    if (*cursor == '\r') {
      assert(event == SerialLineEvent::LineReady);
      assert(strcmp(buffer.line(), "cal show") == 0);
      buffer.consume();
    } else {
      assert(event == SerialLineEvent::None);
    }
  }
}

void testEmbeddedNulRejectsPrefixImmediately() {
  SerialLineBuffer<32> buffer;
  const char* prefix = "cal erase confirm";
  for (const char* cursor = prefix; *cursor != '\0'; ++cursor) {
    assert(
        buffer.push(static_cast<uint8_t>(*cursor)) ==
        SerialLineEvent::None);
  }
  assert(buffer.push(0U) == SerialLineEvent::InvalidByte);
  assert(buffer.line() == nullptr);
  assert(buffer.push('x') == SerialLineEvent::None);
  assert(buffer.push('\n') == SerialLineEvent::None);

  assert(buffer.push('p') == SerialLineEvent::None);
  assert(buffer.push('i') == SerialLineEvent::None);
  assert(buffer.push('n') == SerialLineEvent::None);
  assert(buffer.push('g') == SerialLineEvent::None);
  assert(buffer.push('\n') == SerialLineEvent::LineReady);
  assert(strcmp(buffer.line(), "ping") == 0);
}

void testOverflowRejectsImmediatelyAndRecovers() {
  SerialLineBuffer<5> buffer;
  assert(buffer.push('1') == SerialLineEvent::None);
  assert(buffer.push('2') == SerialLineEvent::None);
  assert(buffer.push('3') == SerialLineEvent::None);
  assert(buffer.push('4') == SerialLineEvent::None);
  assert(buffer.push('5') == SerialLineEvent::Overflow);
  assert(buffer.line() == nullptr);
  assert(buffer.push('\n') == SerialLineEvent::None);
  assert(buffer.push('o') == SerialLineEvent::None);
  assert(buffer.push('k') == SerialLineEvent::None);
  assert(buffer.push('\n') == SerialLineEvent::LineReady);
  assert(strcmp(buffer.line(), "ok") == 0);
}

void testControlAndNonAsciiBytesReject() {
  SerialLineBuffer<8> control;
  assert(control.push(0x01U) == SerialLineEvent::InvalidByte);

  SerialLineBuffer<8> nonAscii;
  assert(nonAscii.push(0x80U) == SerialLineEvent::InvalidByte);

  SerialLineBuffer<8> tab;
  assert(tab.push('a') == SerialLineEvent::None);
  assert(tab.push('\t') == SerialLineEvent::None);
  assert(tab.push('b') == SerialLineEvent::None);
  assert(tab.push('\n') == SerialLineEvent::LineReady);
  assert(strcmp(tab.line(), "a\tb") == 0);
}

}  // namespace

int main() {
  testValidLineAndCrLf();
  testEmbeddedNulRejectsPrefixImmediately();
  testOverflowRejectsImmediatelyAndRecovers();
  testControlAndNonAsciiBytesReject();
  return 0;
}
