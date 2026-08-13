# tools/host_preview -- native LCD screen preview

Renders the ESP32 firmware's pairing-flow LCD screens on Windows with `g++`,
no hardware attached.

## Why

`tools/lcd_preview.py` used to reimplement the drawing by parsing coordinates
out of C++, and it drifted -- it kept rendering a `drawSheetCard()` placeholder
that had already been deleted from the firmware. This harness fixes that by
compiling the **same** `src/screens.cpp` the firmware compiles, against a real
`Arduino_Canvas(240, 280, nullptr)` from Arduino_GFX 1.3.9. If someone edits
`screens.cpp`, both firmware and preview move together in the same commit.

See `docs/host-render-research.md` for the enabling facts (Arduino_Canvas needs
no bus/driver when constructed with a `nullptr` output; the classic 5x7 GFX
glyph font already has `PROGMEM=<empty>` for non-AVR, so text renders
pixel-exact for free).

## Run it

From the repo root, in PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\host_preview\build.ps1
```

The script compiles Arduino_GFX + QRCode from `.pio\libdeps\...` (unmodified)
plus `src\screens.cpp`, `src\sheet_transition.cpp` and the harness `main.cpp`,
links `tools\host_preview\.host-build\lcd_preview.exe`, and runs it. Output
images land under `build\preview\`:

| file                        | contents                                                          |
|-----------------------------|-------------------------------------------------------------------|
| `face_idle.ppm`             | mascot backdrop at `(kOriginX, kOriginY)`                         |
| `qr_wifi.ppm`               | Wi-Fi QR + SSID/pw labels + step-1 dots + exit hint               |
| `qr_wifi_unavailable.ppm`   | the fallback the firmware renders when credentials are unavailable|
| `qr_url.ppm`                | URL QR + "Scan to open control" + step-2 dots + exit hint         |
| `pairing_failed.ppm`        | full-screen "Pairing failed / swipe down to retry" notice         |
| `drag_00_closed.ppm`        | filmstrip frame 0/4 -- sheet fully closed (pure face)             |
| `drag_01_25pct.ppm`         | filmstrip frame 1/4                                               |
| `drag_02_50pct.ppm`         | filmstrip frame 2/4                                               |
| `drag_03_75pct.ppm`         | filmstrip frame 3/4                                               |
| `drag_04_open.ppm`          | filmstrip frame 4/4 -- sheet fully open (pure sheet)              |

The filmstrip is driven by `SheetTransition::Controller` (the shipping code) so
the emitted frames trace a real gesture, not linearly-spaced Y values.

## Convert to PNG (optional)

PPM (P6) opens directly in IrfanView / GIMP / VS Code (with the PPM extension)
and is easy to diff by pixel. If you want PNGs, use PlatformIO's bundled Python
+ Pillow:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools\host_preview\to_png.py
```

## Constraints this satisfies

- **Do not copy drawing code.** All drawing goes through `src/screens.cpp`, which
  the firmware also links. `tools/lcd_preview.py` is preserved for reference but
  is not on the drawing path.
- **Do not modify `.pio/libdeps/`.** Arduino_GFX 1.3.9 and ricmoo/QRCode compile
  unmodified. The shim under `test\stubs\arduino_host\` supplies just enough
  Arduino API (`Arduino.h`, `Print.h`) to satisfy the library headers -- and
  `ESP32` / `__AVR__` are deliberately **undefined** so the library falls
  through to `malloc()` and the generic `pgm_read_byte` fallbacks.
- **No hardware touched.** `Arduino_Canvas(w, h, nullptr)` allocates a
  framebuffer and never opens SPI.
