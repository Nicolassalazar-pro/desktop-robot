// -----------------------------------------------------------------------------
// Print.h shim -- reproduces Arduino's Print base class narrowly enough for
// Arduino_GFX 1.3.9 to compile.
//
// WHY THIS SHIM AND NOT AN INCLUDE OF UPSTREAM
//
// Arduino_GFX derives from Print so `gfx.print("hello")` routes through the
// virtual write(uint8_t) that GFX overrides to call drawChar(). We are not
// bringing in the whole Arduino runtime, so we recreate only the parts GFX
// touches:
//
//   * virtual size_t write(uint8_t) = 0  -- Arduino_GFX overrides this and
//     that override is what routes each glyph column through drawChar into
//     the framebuffer. If this override isn't reached, text won't render.
//   * write(const uint8_t*, size_t) -- default loops over write(uint8_t);
//     Arduino_GFX inherits this unchanged, so print("string") lands in the
//     glyph path per-character.
//   * print(const char*), print(char) -- the only print overloads called by
//     the extracted screen functions in src/screens.cpp (qrCenterText prints
//     a plain const char*). Additional overloads (int, float, base) are
//     stubbed with signatures so anything that references them still links,
//     but none of the extracted screens exercise them.
//   * println(...) -- same idea; the extracted screens never call println,
//     but the vendored library declares it, so signatures live here.
//
// The Adafruit / Arduino Print API is public-domain-ish (Arduino LGPL). This
// is a clean-room reimplementation of just enough of it -- no upstream code
// copied -- because upstream Print.h drags in the Stream base and other bits
// we don't need.
// -----------------------------------------------------------------------------
#ifndef ARDUINO_HOST_SHIM_PRINT_H_
#define ARDUINO_HOST_SHIM_PRINT_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

// Arduino ships DEC/HEX/OCT/BIN/BYTE as int constants for base selection.
#ifndef DEC
#define DEC 10
#endif
#ifndef HEX
#define HEX 16
#endif
#ifndef OCT
#define OCT 8
#endif
#ifndef BIN
#define BIN 2
#endif

class __FlashStringHelper;  // forward declared in Arduino.h too; harmless

class Print {
 public:
  virtual ~Print() = default;

  // The single hook subclasses must implement. Arduino_GFX's override calls
  // drawChar() for the built-in 5x7 font path -- this is the "pixel-exact
  // text for free" that makes the whole harness worthwhile.
  virtual std::size_t write(uint8_t) = 0;

  // Default block-write loops over write(uint8_t). Subclasses may override
  // for efficiency; Arduino_GFX does not, so text rendering goes glyph by
  // glyph through the same code path the ESP32 runs.
  virtual std::size_t write(const uint8_t* buffer, std::size_t size) {
    std::size_t n = 0;
    while (size--) {
      if (write(*buffer++) == 0) break;
      ++n;
    }
    return n;
  }

  std::size_t write(const char* str) {
    if (str == nullptr) return 0;
    return write(reinterpret_cast<const uint8_t*>(str), std::strlen(str));
  }
  std::size_t write(const char* buffer, std::size_t size) {
    return write(reinterpret_cast<const uint8_t*>(buffer), size);
  }

  // ------------------------------ print overloads -------------------------
  // Only print(const char*) is called by the extracted screen functions.
  // The rest are declared so any code that references them (getTextBounds
  // has no such calls, but the header would still compile them) links.
  std::size_t print(const char* s) { return write(s); }
  std::size_t print(char c) { return write(static_cast<uint8_t>(c)); }
  std::size_t print(int n, int base = DEC)          { return printNumber(static_cast<long>(n), base); }
  std::size_t print(unsigned int n, int base = DEC) { return printNumber(static_cast<unsigned long>(n), base); }
  std::size_t print(long n, int base = DEC)         { return printNumber(n, base); }
  std::size_t print(unsigned long n, int base = DEC){ return printNumber(n, base); }
  std::size_t print(double /*n*/, int /*digits*/ = 2) { return 0; }
  std::size_t print(const __FlashStringHelper* /*s*/) { return 0; }

  std::size_t println() { return write(static_cast<uint8_t>('\n')); }
  std::size_t println(const char* s) { auto n = print(s); return n + println(); }
  std::size_t println(char c)        { auto n = print(c); return n + println(); }
  std::size_t println(int n, int base = DEC)          { auto k = print(n, base); return k + println(); }
  std::size_t println(unsigned int n, int base = DEC) { auto k = print(n, base); return k + println(); }
  std::size_t println(long n, int base = DEC)         { auto k = print(n, base); return k + println(); }
  std::size_t println(unsigned long n, int base = DEC){ auto k = print(n, base); return k + println(); }
  std::size_t println(double n, int digits = 2)       { auto k = print(n, digits); return k + println(); }
  std::size_t println(const __FlashStringHelper* s)   { auto k = print(s); return k + println(); }

  int getWriteError() { return 0; }
  void clearWriteError() {}
  void flush() {}

 private:
  // Simple long/unsigned-long formatter. Kept trivially small because none of
  // the screen renderers actually call the integer print overloads -- this
  // is purely so the symbol resolves and the harness links even if a future
  // extraction adds a numeric print().
  std::size_t printNumber(long n, int base) {
    if (base < 2) base = 10;
    char buf[8 * sizeof(long) + 2];
    char* p = buf + sizeof(buf) - 1;
    *p = '\0';
    bool negative = (n < 0 && base == 10);
    unsigned long u = negative ? static_cast<unsigned long>(-n)
                               : static_cast<unsigned long>(n);
    do {
      unsigned long d = u % static_cast<unsigned long>(base);
      *--p = (d < 10) ? static_cast<char>('0' + d)
                      : static_cast<char>('A' + d - 10);
      u /= static_cast<unsigned long>(base);
    } while (u != 0);
    if (negative) *--p = '-';
    return write(p);
  }
  std::size_t printNumber(unsigned long n, int base) {
    if (base < 2) base = 10;
    char buf[8 * sizeof(unsigned long) + 2];
    char* p = buf + sizeof(buf) - 1;
    *p = '\0';
    do {
      unsigned long d = n % static_cast<unsigned long>(base);
      *--p = (d < 10) ? static_cast<char>('0' + d)
                      : static_cast<char>('A' + d - 10);
      n /= static_cast<unsigned long>(base);
    } while (n != 0);
    return write(p);
  }
};

#endif  // ARDUINO_HOST_SHIM_PRINT_H_
