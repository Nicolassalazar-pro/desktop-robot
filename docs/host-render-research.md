# Rendering the desktop-robot LCD UI natively on Windows — research findings

## Summary

Two options can render the real device pixels on Windows without hardware. **Only one is pixel-identical: a native g++ harness that compiles Arduino_GFX's own `Arduino_Canvas` (v1.3.9) into a Windows exe.** Wokwi is the second-best answer — it runs your actual firmware ELF, but there is no ST7789 element (the maintainer's own workaround is to use `wokwi-ili9341`), no CST816T touch model, and the display is 240×320 not 240×280, so the result is close but not pixel-identical. QEMU (Espressif fork) and Renode are dead ends for this use case. EpoxyDuino is Linux/macOS only and doesn't ship a display backend.

The critical enabling fact that unlocks the native path cheaply: **`Arduino_Canvas` used with a `nullptr` output pointer (which the firmware already does at `main.cpp:1383` for `g_sheetCanvas`) does not require the SPI bus or ST7789 driver classes to compile at all.** Its only host-hostile dependencies are `<Arduino.h>`, `<Print.h>`, `PROGMEM`, and one `#if defined(ESP32)` PSRAM branch — all trivial to shim.

---

## 1. Compiling Arduino_GFX itself natively — feasibility deep-dive

### The library only touches five host-hostile things

Verified by fetching the exact pinned tag `v1.3.9` from GitHub:

- **`src/canvas/Arduino_Canvas.h`** (moononournation/Arduino_GFX:src/canvas/Arduino_Canvas.h @ v1.3.9): the entire class is `Arduino_Canvas : public Arduino_GFX`, holds a `uint16_t *_framebuffer`, and exposes `getFramebuffer()`. First line is `#include "../Arduino_DataBus.h"` — that is the only reason it drags in `<Arduino.h>`.
- **`src/canvas/Arduino_Canvas.cpp`** (v1.3.9, lines ~26–46): allocation is the only ESP32-specific code:
  ```cpp
  size_t s = _width * _height * 2;
  #if defined(ESP32)
    if (psramFound()) _framebuffer = (uint16_t *)ps_malloc(s);
    else              _framebuffer = (uint16_t *)malloc(s);
  #else
    _framebuffer = (uint16_t *)malloc(s);
  #endif
  ```
  On the host you simply do **not** define `ESP32`, and it falls through to plain `malloc`. No code change to the library.
- **`src/Arduino_GFX.cpp`** (v1.3.9) — the base class with `writeLine`, `writeFastVLine`, `writeSlashLine`, `writePixel`, `writeFastHLine`, all bitmap drawers, `drawChar` (the 5×7 font path), rounded rect, circle, etc. Includes `<Arduino_DataBus.h>` (which needs `<Arduino.h>`), `<Arduino_GFX.h>`, `font/glcdfont.h`, `<float.h>`. Also `#if defined(ESP8266) || defined(ESP32) #include <pgmspace.h>` — again, undefine ESP32 and this is skipped.
- **`src/Arduino_GFX.h`** (v1.3.9) — includes `<Print.h>` (Arduino's `Print` class, used as base for `setCursor`/`print`), `Arduino_G.h`, `Arduino_DataBus.h`, `gfxfont.h`; defines `pgm_read_byte`/`pgm_read_word` fall-throughs for non-AVR (which is what host will use).
- **`src/font/glcdfont.h`** (v1.3.9) — the 5×7 classic font. Defines `PROGMEM` as empty for non-AVR/non-ESP already: `#else #define PROGMEM #endif`. Works verbatim on the host.

### What "Canvas only" means concretely

`Arduino_Canvas` overrides `writePixelPreclipped`, `writeFastVLine`, `writeFastHLineCore`, `writeFillRectPreclipped`, `draw16bitRGBBitmap`, and `begin`. Every method the firmware actually calls on `g_canvas` (verified from `main.cpp` grep output: `fillScreen`, `fillRect`, `fillRoundRect`, `fillCircle`, `drawCircle`, `drawRect`, `draw16bitRGBBitmap`, `setTextSize`, `setTextColor`, `setTextWrap`, `setCursor`, `print`) is implemented either in `Arduino_Canvas.cpp` or in `Arduino_GFX.cpp`. **None of those touch the databus or driver classes.**

The firmware already proves the "Canvas without output" pattern works at `Nicolassalazar-pro/desktop-robot:src/main.cpp:1383`:
```cpp
g_sheetCanvas = new Arduino_Canvas(kScreenW, kScreenH, nullptr);
```
The constructor happily accepts `nullptr` for the `Arduino_G *output` parameter (verified in `Arduino_Canvas.cpp` — `begin()` only calls `_output->begin(speed)` if `_output` is non-null and `speed != GFX_SKIP_OUTPUT_BEGIN`).

### Existing host-build path in Arduino_GFX

No, there is **no built-in `#ifdef` for host builds** in Arduino_GFX 1.3.9. The library targets AVR, ESP8266, ESP32, ARM, RP2040, etc. — every architecture branch is embedded, none is "generic-libc". However, the way the branches are structured (undefined ESP32 → plain malloc; undefined AVR → non-PROGMEM pgm_read_byte) means an unrecognised host target lands on the "sane defaults" path.

### Shim size to unblock a host build

Everything Arduino_GFX 1.3.9 needs from Arduino can be provided in ~150 lines total:

| Symbol | Where used | Host shim |
|---|---|---|
| `<Arduino.h>` | dragged in by `Arduino_DataBus.h` | tiny header providing `uint8_t/uint16_t/int16_t/int32_t` (from `<cstdint>`), `min()/max()`, `abs()`, `String` (only for `Print::print(const String&)`), and `millis()`/`micros()`/`delay()` for the Canvas' internals (Canvas does not actually call them but Arduino_GFX has some places) |
| `<Print.h>` | base class of `Arduino_GFX` | ~40-line reproduction: virtual `size_t write(uint8_t) = 0`, plus `print(const char*)`, `print(int, int)`, `println(...)` overloads that route through `write()`. Adafruit publishes this exact class. |
| `PROGMEM` / `pgm_read_byte` / `pgm_read_word` | font/bitmap access | `#define PROGMEM` empty; `#define pgm_read_byte(a) (*(const uint8_t*)(a))` — these are already the non-AVR fallbacks *inside Arduino_GFX.h itself*, so they're set correctly if `__AVR__` isn't defined. |
| `ESP32` macro | ps_malloc branch in `Arduino_Canvas.cpp` | leave undefined → falls through to malloc |
| `ATTINY_CORE`, `LITTLE_FOOT_PRINT` | AVR-tiny paths | leave undefined |
| `U8G2_FONT_SUPPORT` | U8g2 fonts | leave undefined — firmware doesn't use them |

### Effort assessment (native Arduino_GFX compile): LOW-MEDIUM, 1–2 dev-days

Blockers ranked:
1. **Print class shim** — 40 lines, one-shot. The only nuance is `print(float, int)` — firmware doesn't call it on `g_canvas`, so a stub is fine.
2. **`Arduino.h` shim** — 30–50 lines. `test/stubs/nvs.h` already sets the precedent for the pattern.
3. **Compiling `Arduino_GFX.cpp` and `Arduino_Canvas.cpp` as extra sources** — add them to the existing `test/run_motor_host_tests.ps1` invocation. They pull no bus/driver code because the harness never constructs `Arduino_ESP32SPI` or `Arduino_ST7789`.
4. **Reaching the drawing functions in `main.cpp`** — see §4 below; this is the real work, not the library shim.

**Verdict:** Native Arduino_GFX compile is **the pixel-exact path**. The library was not designed for host builds but its structure makes it fall onto a sane host path once the ~150-line Arduino compat shim exists.

---

## 2. Wokwi / QEMU / Renode — running the real firmware binary

### Wokwi — genuine mid-fidelity option, not pixel-exact

**Verified capabilities:**

- **ESP32-S3 supported.** `board-esp32-s3-devkitc-1` is a first-class part (docs.wokwi.com/diagram-format).
- **8 MB octal PSRAM supported** — this is very recent (March 2026) and directly relevant. Uri Shaked (Wokwi founder) confirmed on `wokwi/wokwi-features#1100` with a working diagram:
  ```json
  {
    "type": "board-esp32-s3-devkitc-1",
    "attrs": { "psramSize": "8", "psramType": "octal", "flashSize": "4" }
  }
  ```
  That exactly matches this firmware's ESP32-S3R8 config (`platformio.ini:43-51` `qio_opi`). Confirmed via `api.github.com/repos/wokwi/wokwi-features/issues/1100/comments`.
- **CLI runs on Windows.** `iwr https://wokwi.com/ci/install.ps1 -useb | iex` (docs.wokwi.com/wokwi-ci/cli-installation).
- **Screenshots via CLI:** `--screenshot-part lcd1 --screenshot-file out.png --screenshot-time 3000` (docs.wokwi.com/wokwi-ci/cli-usage).
- **Automation scenarios** (`--scenario file.yaml`) can `set-control`, `wait-serial`, `take-screenshot`, `expect-pin` for regression tests (docs.wokwi.com/wokwi-ci/automation-scenarios).
- **VS Code extension** gives interactive click-in-simulator on Windows (docs.wokwi.com/vscode/getting-started, docs.wokwi.com/vscode/project-config).

**Verified limitations that matter here:**

- **No native ST7789 element.** `wokwi/wokwi-features#749` (2024-03) requested it; Uri Shaked closed it 2025-01-15 with: *"The ILI9341 is compatible with the ST7789, so you can use it for testing ST7789 code. For example: https://wokwi.com/projects/391880207365485569"* (verified via `api.github.com/repos/wokwi/wokwi-features/issues/749/comments`). The example project actually uses `TFT_eSPI` with `tft.begin()` (defaulted to an ST7789 config in the project), and the diagram shows a `wokwi-ili9341`.
- **`wokwi-ili9341` is 240×320.** Verified in `wokwi/wokwi-elements:src/ili9341-element.ts` (main): `readonly screenWidth = 240; readonly screenHeight = 320;`. Your firmware uses `Arduino_ST7789` with `row_offset1=20` (`main.cpp:1352`) and `width=240 height=280`. The driver writes visible content into GRAM rows 20..299; Wokwi will render the full 240×320 canvas, so **rows 0..19 and 300..319 will appear as junk/black around your actual image**. Not pixel-identical to the panel, but the 240×280 window is inside it — crop it in the viewer.
- **Repository has no `st7789` or `wokwi-st7789` file.** Verified via `api.github.com/repos/wokwi/wokwi-elements/git/trees/main?recursive=1` — the tree has `lcd1602`, `ili9341`, `ssd1306`, `hd44780`, etc., but no ST7789.
- **Color inversion (ST7789V2 IPS mode).** `Arduino_ST7789` sends `INVON` (0x21) when `ips=true` (which the firmware sets at `main.cpp:1348`). Whether Wokwi's ILI9341 model correctly interprets ST7789 register semantics is not documented; ILI9341 and ST7789 share most CASET/RASET/RAMWR but init sequences differ. Likely you'll get a picture, possibly with inverted colors or wrong offsets. This is empirical.
- **No CST816T touch simulation.** There is no touch element in `wokwi/wokwi-elements` and no CST816 issue in `wokwi/wokwi-features`. You would need a **Custom Chip** (WASM, C source) that speaks I²C at address 0x15 and emits the CST816T register layout. The Chips API supports this (docs.wokwi.com/chips-api/getting-started) — nontrivial but doable, ~200–400 lines of C.
- **Not fully offline.** Wokwi CLI streams the compiled ELF to Wokwi's cloud simulation servers (docs.wokwi.com/wokwi-ci/getting-started explicitly: "The server receives your firmware binary, simulates it, and streams the serial output back to your CI system"). VS Code extension: same. Note the LGPL-related history in `platformio.ini:150-156` — uploading firmware to a third-party is a policy call.
- **Simulation-minute limits:** free 50 min/month, Hobby 200/month (docs.wokwi.com/wokwi-ci/getting-started). Fine for iterative UI work, non-zero.

**Wokwi net assessment for this repo:**

- Fidelity: **CLOSE, not pixel-identical.** The 240×320 canvas contains the correct 240×280 window (assuming ILI9341 emulation handles ST7789's CASET+row-offset math), but with 40 extra rows of garbage and possibly inverted colors. Text glyphs and geometry inside the 240×280 window WILL match the device because they come from the actual Arduino_GFX code running on the emulated CPU.
- Interactivity: **poor for touch.** No way to simulate the pull-down sheet drag without writing a custom I²C chip in C for the CST816T. Screenshots at fixed timestamps work fine.
- Work: **~1 hour** to get first pixels on screen; **~1–3 days** to add a CST816T custom chip that can inject synthetic drag events. Windows-native.

### QEMU (Espressif fork) — no

Verified from `espressif/esp-toolchain-docs:qemu/README.md` (main) and `qemu/esp32s3/README.md`:

- ESP32-S3 support is real: dual-core Xtensa LX7, octal PSRAM (✅), GDMA (✅), timer groups, SysTimer, AES/SHA/RSA/HMAC.
- **LEDC on ESP32-S3: ❌** — this project drives servos, piezo and backlight via LEDC (`platformio.ini:126-140`). Boot may proceed but timing-critical things won't emulate.
- **"RGB Framebuffer ✅"** — the matrix marks this as "not a real hardware peripheral, it is used to simplify GUI testing." It's a synthetic framebuffer for graphics testing, **not a virtual ST7789 wired to SPI2**. No mention in the docs of an SPI-driven display model on QEMU. Nothing you write over SPI turns into pixels.
- Windows: only build path is MinGW64 cross-compile from source. No prebuilt Xtensa QEMU binary for Windows exists in Espressif's release stream. Building it is a full day even on Linux, then bringing over the Windows cross-artifacts.

**Verdict: QEMU-Espressif does not solve this problem.** It's for CI-testing peripherals and headless business logic, not LCD preview.

### Renode — no

Verified via `renode.readthedocs.io/en/latest/introduction/supported-boards.html` and `api.github.com/repos/renode/renode/contents/scripts/single-node`: the platform list (STM32 Nucleo/Discovery, EFR32 Gecko, SAM E70, CC2538, HiFive, LiteX, Ambiq, Apollo4, Arduino Uno R4 Minima, etc.) contains **no ESP32 target**. The architecture list mentions Xtensa in principle, but there is no ESP32/ESP32-S3 board definition. Antmicro/Renode is Cortex/RISC-V-centric.

**Verdict: not a viable ESP32 host.**

### Other MCU-display simulators

- **LVGL SDL simulator** — this project doesn't use LVGL. Irrelevant, as noted in the question.
- **GUIslice with SDL backend** (`ImpulseAdventure/GUIslice`) — different GUI library, not Arduino_GFX.
- **wokwi-elements** — no ST7789, no touch. Already covered.
- Search `arduino+gfx+sdl+host` returned 0 repos; `Arduino_GFX+desktop` returned 0 repos (verified via `api.github.com/search/repositories`). There is **no existing "Arduino_GFX SDL port"**.

---

## 3. EpoxyDuino and other host-Arduino frameworks — Windows caveat

**EpoxyDuino (`bxparks/EpoxyDuino`, v1.6.0 2024-07-25):**
- README states supported hosts: *"Linux, MacOS, FreeBSD (experimental) and potentially other POSIX-like systems."* **Windows is not listed.** MinGW/MSYS2 sort-of work, WSL definitely works, native Windows toolchain is not tested.
- Provides `Serial → stdout/stdin`, stubbed `SPI`/`I2C`/`GPIO`. **No display backend.** You'd bring your own.
- Adds no help for the ~15 sites in `main.cpp` that touch `g_canvas->getFramebuffer()` — you still need Arduino_GFX to compile against it.

**Verdict:** EpoxyDuino is not the right vehicle for a Windows-first UI iteration harness on this project. The compat layer you'd need is smaller than EpoxyDuino: a ~150-line `test/stubs/Arduino.h` + `Print.h` is enough for Arduino_GFX alone. EpoxyDuino only makes sense if you also want to run `setup()`/`loop()` in the harness, which drags in WiFi/servo/I²C code you'd rather stub.

---

## 4. The refactor question — how to reach the drawing code without copy-drift

### Direct evidence of the coupling problem

Verified from `main.cpp`:

- **~15 raw-framebuffer accesses:** `Nicolassalazar-pro/desktop-robot:src/main.cpp` grep for `getFramebuffer()` returned matches at lines 2190, 2289, 2322, 2340, 2364, 2397, 2674, 4325, 10443, 10448, 10485, 10486, 11180, 11181, 11244, 11245, 11267, 11268, 11286, 11287, 11390, 11391. These are for the compositor (`pushSheetStrip`), the sheet↔face memcpy (`handOffSheetQrToPanel`, `main.cpp:10485-10491`), transition blends (`blendGarageToTarget`, `main.cpp:2320-2336`), and eye-region backup. **A stub that only implements the drawing API is not sufficient — it must expose a real `uint16_t*` framebuffer.**
- **`drawQrScreenWifi()` at `main.cpp:10523-10555`** transitively needs: `g_canvas`, `ActivationCredentials`, `copyActivationCredentials()`, `QRCode`/`qrcode_initText()` (from `ricmoo/QRCode@0.0.1`), `kQrWhite`, `kQrWifiVersion`, `kQrWifiPixelScale`, `qrCenterText`, `qrCenterTextFit`, `drawPairingStepDots`, `kApPasswordMaxLength`, `kScreenW`.
- **`drawMascotToCanvas()` at `main.cpp:2139`** transitively needs: `g_canvas`, `MASCOT_A` bitmap (from `src/mascot/mascot_a.h`), `g_lastShiftX/Y`, and reaches into `drawGarageLogoToCanvas` at `main.cpp:2285-2297` which uses `DisplayTransition::compositeWhiteOver`.
- **The eye engine** (`lib/EyeEngine`) is pinned to Arduino_GFX 1.3.9 (`lib/EyeEngine/library.json:6-9`) and takes an `Arduino_GFX*` at construction. If it compiles against your host Arduino_GFX shim, it also runs on the host.

### Cleanest refactor path (recommended)

Create **`src/screens.h` + `src/screens.cpp`** that hold every "compose the framebuffer" function. This is exactly what `src/sheet_transition.{h,cpp}` did for the drag physics (already Arduino-free per `sheet_transition.h:5-11`, already host-tested per `test/run_motor_host_tests.ps1:135-140`).

Contract:
```cpp
// src/screens.h
#pragma once
#include <Arduino_GFX_Library.h>  // host build: Arduino_Canvas + shim; device build: full lib
#include "qrcode.h"                // ricmoo/QRCode — pure C, host-buildable
#include "mascot/mascot_a.h"       // just a uint16_t[] blob

struct ScreenContext {
  Arduino_Canvas* canvas;         // required
  Arduino_Canvas* sheetCanvas;    // may be null
  // credentials + state — provided by main.cpp on device, by the host harness in preview
  const char* wifiSsid;
  const char* wifiPassword;
  const char* pairingUrl;
  uint8_t     pairingStep;
  int16_t     mascotShiftX;
  int16_t     mascotShiftY;
  // ...
};

namespace Screens {
  void drawQrWifi(const ScreenContext&);
  void drawQrUrl(const ScreenContext&);
  void drawFace(const ScreenContext&);       // face target render (no eye engine calls)
  void drawSheetCard(const ScreenContext&);
  void compositeSheetOverFace(const ScreenContext&, int16_t sheetY);
  // ...
}
```

This deliberately **injects** the ~5 firmware globals that drawing needs, so the file has zero WiFi/servo/IMU/audio deps. It stays in-tree; both firmware and host harness link the same `screens.o`, which is what kills drift.

The **`main.cpp` change** is mostly delete-and-forward: each existing `static void drawQrScreenWifi()` becomes a one-liner that fills a `ScreenContext ctx { g_canvas, g_sheetCanvas, ap.ssid, ap.password, ..., g_lastShiftX, g_lastShiftY, ... }; Screens::drawQrWifi(ctx);`.

### Alternatives evaluated

- **`#ifdef HOST_PREVIEW` through `main.cpp`.** Fights the design of a 12,000-line TU; every `#ifdef` becomes a maintenance burden and the very drift problem the refactor is supposed to prevent (a firmware-only path that stops matching a host-only path). **Reject.**
- **PlatformIO `[env:native]`.** Works for tiny modules; would still need a `screens.cpp` extraction because `[env:native]` can't compile the whole `main.cpp` (WiFi/etc.). Redundant with the CMake/PowerShell approach when you already have `run_motor_host_tests.ps1` precedent. **Redundant.**
- **Separate CMake or g++ harness driven by `test/run_host_preview.ps1`.** This is the concrete recommendation. Same style as `run_motor_host_tests.ps1:19` — plain `g++ -std=c++17 -Wall -Wextra -Werror` invocation. Zero new toolchain on Windows.

### The QR generator and font

Both `ricmoo/QRCode@0.0.1` (pinned at `platformio.ini:170`) and `moononournation/Arduino_GFX@1.3.9`'s `glcdfont.h` are plain-C, host-compilable. No stub required for either — check them out with the same tag PlatformIO already resolved (see `~/.platformio/packages/framework-arduinoespressif32/libraries/` layout on your machine, or just fetch v0.0.1 of ricmoo/QRCode into a `third_party/` dir).

---

## 5. Interactivity

`src/sheet_transition.{h,cpp}` is already Arduino-free (`sheet_transition.h:5-11` and its own docstring) and already host-tested (`test/sheet_transition_test.cpp` in `run_motor_host_tests.ps1:135-140`). So all the preview needs is:
- an SDL2 window at 240×280,
- a per-frame blit of `canvas->getFramebuffer()` using `SDL_PIXELFORMAT_RGB565`,
- SDL mouse events → `SheetTransition::Controller::onPress/onMove/onRelease` with `SDL_GetTicks()` as the millisecond clock.

That is ~120 lines of SDL2 glue. SDL2 is on `vcpkg`/`msys2` and ships prebuilt binaries for MSVC/MinGW: `SDL2-devel-2.30.x-VC.zip` from libsdl.org.

Wokwi cannot do this without a custom I²C chip modeling the CST816T. Even then, feeding a "drag from y=30 to y=250 over 400 ms" event requires programming the custom-chip state machine — not impossible, but roughly the same effort as the SDL2 glue, and only gets you a close-but-not-identical picture.

---

## Prioritized recommendation

### 🥇 Primary: Native Arduino_GFX host harness (pixel-identical)

**Concrete implementation plan:**

**Toolchain (already on the machine):**
- `g++` from PlatformIO's mingw (or MSYS2 `mingw-w64-x86_64-gcc`). Same one `run_motor_host_tests.ps1:19` already uses.
- **SDL2**: download `SDL2-devel-2.32.x-mingw.tar.gz` from libsdl.org, extract `x86_64-w64-mingw32/` next to the harness. Alternatively `vcpkg install sdl2`.

**File layout:**
```
src/
  screens.h              # NEW — public API taking ScreenContext
  screens.cpp            # NEW — every draw* function moved out of main.cpp
  main.cpp               # UNCHANGED except calls Screens::drawXxx(ctx)
test/
  stubs/
    Arduino.h            # NEW — 30 lines: cstdint typedefs, min/max, delay stub
    Print.h              # NEW — 40 lines: Print base class, write() pure virtual, print()/println()
    (pgmspace.h optional; Arduino_GFX.h has non-AVR fallbacks already)
third_party/
  Arduino_GFX/           # NEW — checkout of moononournation/Arduino_GFX @ v1.3.9
    src/Arduino_GFX.cpp
    src/Arduino_GFX.h
    src/Arduino_G.{h,cpp}
    src/Arduino_DataBus.h    (still needed for the base class def; bus impls NOT compiled)
    src/canvas/Arduino_Canvas.{h,cpp}
    src/font/glcdfont.h
    src/font/gfxfont.h
  QRCode/                # NEW — checkout of ricmoo/QRCode @ 0.0.1
  SDL2/                  # SDL2 dev files
tools/
  preview_main.cpp       # NEW — SDL2 window + event pump + calls into Screens::
  run_lcd_preview.ps1    # NEW — mirrors run_motor_host_tests.ps1 style
```

**Build command (Windows PowerShell):**
```powershell
$SDL = "third_party\SDL2\x86_64-w64-mingw32"
g++ -std=c++17 -Wall -Wextra -Werror -O2 `
    -DHOST_PREVIEW `
    -Itest\stubs -Isrc -Ithird_party\Arduino_GFX\src -Ithird_party\QRCode\src -I"$SDL\include" `
    tools\preview_main.cpp `
    src\screens.cpp `
    src\sheet_transition.cpp `
    src\display_transition.cpp `
    third_party\Arduino_GFX\src\Arduino_GFX.cpp `
    third_party\Arduino_GFX\src\Arduino_G.cpp `
    third_party\Arduino_GFX\src\canvas\Arduino_Canvas.cpp `
    third_party\QRCode\src\qrcode.c `
    -L"$SDL\lib" -lSDL2 -lSDL2main `
    -o build\lcd_preview.exe
copy "$SDL\bin\SDL2.dll" build\
build\lcd_preview.exe
```

**What must be stubbed:**
| Symbol | Shim location | Notes |
|---|---|---|
| `<Arduino.h>` | `test/stubs/Arduino.h` | typedefs, `min`/`max`, empty `delay`, `millis()=SDL_GetTicks()` |
| `<Print.h>` | `test/stubs/Print.h` | pure-virtual base with `size_t write(uint8_t)`, plus `print`/`println` overloads |
| `<pgmspace.h>` | not needed | Arduino_GFX.h already provides non-AVR fallbacks |
| `ESP32` macro | leave undefined | `Arduino_Canvas.cpp:38-46` falls through to `malloc` |
| Firmware globals (`ActivationCredentials`, `g_lastShiftX`, ...) | `preview_main.cpp` creates a `ScreenContext` with test values | injected, not stubbed |
| `mascot_a.h` bitmap | copy from `src/mascot/` — pure `const uint16_t[]` | zero-effort |
| Eye engine | link `lib/EyeEngine/EyeEngine.cpp` unchanged (it also takes `Arduino_GFX*`) | works host-side once the shim is done |

**Expected fidelity: PIXEL-EXACT.** Because:
1. Every pixel written into the framebuffer comes from the same `Arduino_Canvas::writeFillRectPreclipped`, `writeFastVLineCore`, `draw16bitRGBBitmap`, `drawChar → Adafruit glcdfont` code that runs on the ESP32-S3.
2. The framebuffer is a plain RGB565 `uint16_t[240*280]` — identical layout to the device.
3. SDL2 blits with `SDL_PIXELFORMAT_RGB565` — no colour transform, no gamma. What the driver would send to the ST7789 over SPI is exactly what SDL renders.
4. The ST7789V2's 20-row RAM offset is a driver-side transformation that happens **outside** the canvas — the canvas is 240×280 both on-device and on-host. No offset drift possible.
5. Text uses byte-identical `glcdfont.h`. This is the file the prior Python attempt was trying to reimplement (`docs/preview-harness-research.md:71-79`).

**Specific risks:**
- The `Print` shim must correctly route `print(const char*)` → `write(uint8_t)` per char, so `Arduino_GFX::write()` (the glyph-column loop in `Arduino_GFX.cpp`) gets called. Test by rendering "Wi-Fi unavailable" in both harness and device and diffing.
- The `screens.cpp` extraction is a real refactor of `main.cpp` — worth doing incrementally, one screen at a time. Start with `drawQrWifi()` (small, high value, low coupling). The gesture-menu drift-guard pattern at `run_motor_host_tests.ps1:142-158` is the model: after extraction, gate on `pio run` still building. Have the harness's `--check` mode diff a golden PNG per screen so drift shows in CI.
- If the eye engine touches `Serial.printf` or other Arduino-only things, add another 10 lines to the Arduino.h shim (there's precedent — `EyeEngine.cpp` prints to `Serial` for diagnostics).
- IMPORTANT for correctness parity: on device, `psramFound()` returns true and `ps_malloc` is used; on host, plain `malloc`. Semantically identical for the framebuffer bytes — but if any firmware code introspects `heap_caps_*` results, that needs stubbing. `main.cpp:2300` uses `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` in a transition path; if that path is exercised by preview, stub `heap_caps_malloc` to `malloc` in `Arduino.h`.

**Time estimate: 2 days.** Half a day for the shim and third-party checkouts; half a day for `preview_main.cpp` + SDL2 wiring; one day for the `screens.cpp` extraction of the four most-iterated screens (QR-WiFi, QR-URL, face, pairing sheet).

---

### 🥈 Fallback: Wokwi VS Code / CLI (close, not pixel-identical)

Use if the developer wants zero refactor and can accept the ILI9341-as-ST7789 fidelity gap.

**`wokwi.toml`:**
```toml
[wokwi]
version = 1
firmware = '.pio/build/esp32-s3-devkitc-1/firmware.bin'
elf      = '.pio/build/esp32-s3-devkitc-1/firmware.elf'
```

**`diagram.json` skeleton** (based on Uri's PSRAM example, adapted to your pin map at `platformio.ini:59-96`):
```json
{
  "version": 1, "editor": "wokwi",
  "parts": [
    {
      "type": "board-esp32-s3-devkitc-1", "id": "esp", "top": 0, "left": 0,
      "attrs": { "psramSize": "8", "psramType": "octal", "flashSize": "16" }
    },
    {
      "type": "wokwi-ili9341", "id": "lcd1", "top": 25, "left": -500, "rotate": 0,
      "attrs": {}
    }
  ],
  "connections": [
    ["esp:TX", "$serialMonitor:RX", "", []],
    ["esp:RX", "$serialMonitor:TX", "", []],
    ["esp:3V3", "lcd1:VCC", "red",    []],
    ["lcd1:GND", "esp:GND", "black",  []],
    ["lcd1:MOSI","esp:7",   "green",  []],  
    ["lcd1:SCK", "esp:6",   "yellow", []],
    ["lcd1:CS",  "esp:5",   "orange", []],
    ["lcd1:D/C", "esp:4",   "white",  []],
    ["lcd1:RST", "esp:8",   "magenta",[]]
  ]
}
```

Install: `iwr https://wokwi.com/ci/install.ps1 -useb | iex`, get token at `wokwi.com/dashboard/ci`, then:
```
$env:WOKWI_CLI_TOKEN = "wok_..."
pio run -e esp32-s3-devkitc-1
wokwi-cli . --timeout 20000 --screenshot-part lcd1 --screenshot-file preview.png --screenshot-time 15000
```

**Fidelity: close but NOT pixel-identical.** Reasons already listed: 240×320 vs 240×280 crop, possible ST7789-vs-ILI9341 register mismatch (esp. INVON 0x21), no touch. Text glyphs inside the visible area should match because they're rendered by the real firmware.
**Time estimate:** first pixels in ~1 hour; sanitised for real UI iteration in a few hours; ~1–3 days if you also write a CST816T custom chip for drag interactivity.

---

### ❌ Not recommended

- **QEMU (Espressif)**: no SPI-driven display model; LEDC on S3 unsupported; Windows build path is MinGW from source. Wrong tool.
- **Renode**: no ESP32 support in the tree. Wrong tool.
- **EpoxyDuino**: Windows unsupported officially, and it doesn't provide the display-side plumbing that's actually the interesting work.
- **Copy-and-modify drawing code into a Python/JS preview** — the developer already rejected this; the drift risk is real (`docs/preview-harness-research.md:44-56`).

---

## Verified facts vs. inference

**Verified with fetch (with links):**
- Arduino_GFX 1.3.9 tag file listing: `api.github.com/repos/moononournation/Arduino_GFX/contents/src/canvas?ref=v1.3.9`.
- `Arduino_Canvas.h` and `.cpp` v1.3.9 full source: `raw.githubusercontent.com/moononournation/Arduino_GFX/v1.3.9/src/canvas/Arduino_Canvas.{h,cpp}`. The `#if defined(ESP32)` PSRAM branch is at Arduino_Canvas.cpp ~lines 30–46.
- `Arduino_GFX.h/.cpp` v1.3.9: include chain (`<Print.h>`, `Arduino_G.h`, `Arduino_DataBus.h`, `gfxfont.h`, `float.h`) — from raw fetches.
- `glcdfont.h` v1.3.9: 5×7 bytes-per-glyph table starts at ASCII 0. `PROGMEM` defined-empty on non-AVR/non-ESP.
- Wokwi supports `board-esp32-s3-devkitc-1`: `docs.wokwi.com/diagram-format`.
- Wokwi PSRAM (`psramSize`/`psramType: octal`) works on S3-devkitc-1 as of March 2026: Uri Shaked comment on `wokwi/wokwi-features#1100` (`api.github.com/repos/wokwi/wokwi-features/issues/1100/comments`).
- Wokwi has no ST7789 element; ILI9341 recommended as replacement: Uri Shaked on `wokwi/wokwi-features#749` (`api.github.com/repos/wokwi/wokwi-features/issues/749/comments`), 2025-01-15.
- `wokwi-ili9341` is fixed 240×320: `wokwi-elements/main/src/ili9341-element.ts` lines showing `screenWidth = 240; screenHeight = 320;`.
- Wokwi CLI on Windows: `docs.wokwi.com/wokwi-ci/cli-installation` (`iwr install.ps1`).
- Wokwi automation scenarios / screenshots: `docs.wokwi.com/wokwi-ci/cli-usage`, `.../automation-scenarios`.
- QEMU Espressif S3 support matrix (RGB Framebuffer=✅ but "not real hardware peripheral", LEDC on S3=❌): `espressif/esp-toolchain-docs/blob/main/qemu/README.md`.
- QEMU S3 usage docs: `espressif/esp-toolchain-docs/blob/main/qemu/esp32s3/README.md`.
- Renode boards list: `renode.readthedocs.io/en/latest/introduction/supported-boards.html`. No ESP32 entry among the enumerated demos.
- EpoxyDuino README: `bxparks/EpoxyDuino` main — "Linux, MacOS, FreeBSD (experimental)".
- Firmware fact: `Arduino_Canvas` with `nullptr` output already used at `Nicolassalazar-pro/desktop-robot:src/main.cpp:1383`.
- Firmware fact: 15 raw framebuffer accesses via `getFramebuffer()` — grep in `Nicolassalazar-pro/desktop-robot:src/main.cpp` returned matches on lines 2190, 2289, 2322, 2340, 2364, 2397, 2674, 4325, 10443, 10448, 10485, 10486, 11180, 11181, 11244, 11245, 11267, 11268, 11286, 11287, 11390, 11391.
- Firmware fact: g++ Windows host-build precedent at `Nicolassalazar-pro/desktop-robot:test/run_motor_host_tests.ps1:19`.
- Firmware fact: sheet transition physics already Arduino-free and host-tested — `Nicolassalazar-pro/desktop-robot:src/sheet_transition.h:5-11`, `.../test/run_motor_host_tests.ps1:135-140`.
- Prior research report exists and reached the same coupling conclusion for a Python re-implementation — `Nicolassalazar-pro/desktop-robot:docs/preview-harness-research.md:44-56` — which is why extraction into `screens.cpp` is the natural next step regardless of harness language.

**Inferred (call out):**
- **Wokwi's ILI9341 model correctly interpreting ST7789 CASET/RASET + row_offset=20**: not documented anywhere; Uri says "compatible" but the failure mode of `INVON 0x21` differences is not addressed. Fidelity of the visible area on Wokwi is **empirically likely but not guaranteed** — verify by running a real known screen once and diffing.
- **Wokwi handling color inversion (`ips=true` → `INVON`)**: also empirical. If wrong, colors appear inverted.
- **Exact `Print.h` methods needed by Arduino_GFX 1.3.9**: shim needs `write(uint8_t)`, `print(const char*)`, and possibly `print(char)`. Anything beyond that (float printing, base conversion) is not called by the drawing sites in `main.cpp` per grep of `print(` on `g_canvas` — but a full sweep of `Arduino_GFX.cpp` for `Print::print(...)` calls is recommended once implementing.

**Gaps / uncertainty:**
- I could not verify a specific Wokwi issue thread reporting whether the ILI9341 model responds correctly to ST7789 register semantics under `Arduino_ST7789 ips=true`. Only Uri's assertion of "compatible" for a specific example. Recommend a 15-minute smoke test as the very first step of any Wokwi trial.
- I could not find a repo doing "Arduino_GFX SDL desktop preview" — search returned zero. Nobody has published this exact harness. That's a modest indicator that it's uncommon, not that it's hard: the same search pattern would have found `LovyanGFX SDL` if it existed.
