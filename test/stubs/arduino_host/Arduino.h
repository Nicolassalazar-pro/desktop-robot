// -----------------------------------------------------------------------------
// Arduino.h shim -- the minimum needed to compile Arduino_GFX 1.3.9 into a
// Windows g++ host executable so tools/host_preview can render LCD screens
// without any hardware attached.
//
// WHY THIS EXISTS
//
// Arduino_GFX includes <Arduino.h> from Arduino_DataBus.h. We are NOT
// compiling any databus or driver .cpp files -- only Arduino_G.cpp,
// Arduino_GFX.cpp and canvas/Arduino_Canvas.cpp -- so we only need the tiny
// subset of the Arduino runtime those three files actually reference.
//
// THE ONE RULE (from docs/host-render-research.md §1)
//
//   Do NOT define ESP32, __AVR__, ESP8266, or any Arduino board macro when
//   compiling the host build. The library detects those internally and takes
//   architecture-specific paths (ps_malloc via psramFound(); AVR PROGMEM
//   pgm_read_*; ESP32-S3 esp_lcd_panel_io headers) that either need real
//   hardware headers or would require a much larger shim. On an UNKNOWN
//   target, the library falls through to plain malloc() and the non-AVR
//   pgm_read_byte fallbacks in its own headers, and compiles with ZERO
//   changes to the vendored library.
//
// THE SHIM CONTAINS ONLY WHAT THE THREE FILES REALLY REFERENCE
//
// * Fixed-width integer typedefs -- <cstdint> covers them.
// * min/max/abs macros -- Arduino_GFX.cpp uses them via _diff/_swap and
//   direct calls in a few places.
// * A minimal String class and __FlashStringHelper forward declaration --
//   Arduino_GFX.h declares getTextBounds overloads that take these; the .cpp
//   only calls .c_str() and length()/[] indexing on __FlashStringHelper.
// * millis()/micros()/delay() -- not actually called by the three files, but
//   provided anyway because the header of Arduino_DataBus.h forces their
//   existence in some compilation contexts.
//
// Kept intentionally header-only so it doesn't need its own translation unit.
// -----------------------------------------------------------------------------
#ifndef ARDUINO_HOST_SHIM_ARDUINO_H_
#define ARDUINO_HOST_SHIM_ARDUINO_H_

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
// Arduino_GFX.cpp uses sinf/cosf/fmodf/fabsf without an explicit <math.h> --
// the Arduino runtime brings math in through this header, so we do too.
#include <math.h>

// Arduino ships min/max/abs as C-style macros. We forward to std versions.
// Undefine first so Windows headers that also define these can't collide.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#ifdef abs
#undef abs
#endif
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define abs(x) (((x) < 0) ? -(x) : (x))

// Arduino's PROGMEM decorators and F() macro are Arduino-only. The three GFX
// files we compile use `PROGMEM` only via glcdfont.h, which already defines
// it as empty for non-AVR/non-ESP. Provide an empty fallback in case any
// other consumer includes this shim.
#ifndef PROGMEM
#define PROGMEM
#endif
#ifndef PGM_P
#define PGM_P const char*
#endif
#ifndef F
#define F(x) (x)
#endif

// pgm_read_byte etc are provided by Arduino_GFX.h itself for the non-AVR
// branch. Don't redefine here.

// Arduino type aliases used by the library.
typedef uint8_t byte;
typedef bool boolean;

// __FlashStringHelper is Arduino's opaque marker for F("...") strings. The
// vendored getTextBounds(const __FlashStringHelper*) casts through it to
// (const char*), so a simple type-only forward decl is enough -- we never
// construct one on the host.
class __FlashStringHelper;

// Minimal Arduino String. Arduino_GFX's only use is getTextBounds(const
// String&), which reads .c_str(). Providing a real, movable class rather
// than a forward decl keeps a compiler that tries to instantiate it happy.
class String {
 public:
  String() : buf_(nullptr), len_(0) {}
  String(const char* s) {
    len_ = (s != nullptr) ? std::strlen(s) : 0;
    buf_ = static_cast<char*>(std::malloc(len_ + 1));
    if (buf_ != nullptr) std::memcpy(buf_, s, len_ + 1);
  }
  ~String() { std::free(buf_); }

  String(const String&) = delete;
  String& operator=(const String&) = delete;

  const char* c_str() const { return (buf_ != nullptr) ? buf_ : ""; }
  std::size_t length() const { return len_; }

 private:
  char* buf_;
  std::size_t len_;
};

// Clock stubs. The three GFX .cpp files never call these, but Arduino_GFX.h
// pulls in <Arduino.h> for downstream consumers, and a preview harness may
// want them.
inline uint32_t millis()  { return 0U; }
inline uint32_t micros()  { return 0U; }
inline void delay(uint32_t /*ms*/) {}
inline void delayMicroseconds(uint32_t /*us*/) {}

// digitalWrite / pinMode / etc are NOT provided. The three GFX files never
// touch pins -- only databus and driver .cpp files do, and we don't compile
// those. If a later change to Arduino_Canvas.cpp adds a pinMode call, add
// stubs here rather than silently including a real Arduino header.

#endif  // ARDUINO_HOST_SHIM_ARDUINO_H_
