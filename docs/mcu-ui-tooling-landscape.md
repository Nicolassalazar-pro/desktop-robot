# MCU display UI tooling — landscape study

Six parallel research threads, 2026-08-13. Question: **is there a real gap in
tooling for designing and iterating UI for a microcontroller-driven display?**

Prompted by direct experience — building a pull-down pairing sheet for a 240×280
ST7789 on an ESP32-S3 meant a flash cycle per visual tweak. A Python preview that
parsed coordinates out of C++ **drifted** (it rendered a card deleted from the
firmware, in the wrong font). A native g++ harness compiling the real
`Arduino_Canvas` worked and is pixel-exact, but cost a day plus an extraction.

---

## 1. The verdict

The hypothesis under test was:

> Good tooling exists for retained-mode GUI frameworks (LVGL, TouchGFX, Slint).
> It is absent for the immediate-mode direct-draw ecosystem (Adafruit_GFX,
> Arduino_GFX, TFT_eSPI, U8g2).

**FALSE as stated. TRUE when narrowed.**

**LovyanGFX falsifies it.** It is an immediate-mode library that ships a
first-party, in-tree SDL2 desktop backend — `src/lgfx/v1/platforms/sdl/`
(`Panel_sdl.cpp`, `Touch_sdl`), autoselected by the *same* `LGFX_AUTODETECT.hpp`
used on hardware, documented for Windows via both Visual Studio + CMake and
VS Code + PlatformIO + MSYS2, with mouse-as-touch and keyboard-as-GPIO.
`Panel_sdl` inherits from the mainline `Panel_FrameBufferBase` that real panels
use, so it is not a bolt-on. An existing sketch runs on the desktop with roughly
**seven added lines**. Latest release 1.2.26 (2026-07-22).

So the honest statement is narrower and more useful:

> Desktop preview is absent specifically for the **Adafruit_GFX / Arduino_GFX /
> TFT_eSPI** branch of the immediate-mode ecosystem — which is the most widely
> used branch, and the one this project happens to sit on.

### Per-library truth table

| Library | Renders to in-RAM canvas | Ships a supported desktop backend |
|---|---|---|
| **LovyanGFX** | Yes (`LGFX_Sprite`) | **Yes** — SDL2, OpenCV, framebuffer, WASM |
| **U8g2** | Yes | **Yes, but monochrome 128×64** — irrelevant to RGB565 |
| **Arduino_GFX** | Yes (`Arduino_Canvas`) | **No** |
| **Adafruit_GFX** | Yes (`GFXcanvas16`) | **No** |
| **TFT_eSPI** | Yes (`TFT_eSprite`) | **No** — only the abandoned `CURTLab/SDL_TFT_eSPI` fork |

The distinction that matters: *every* one of these can render into RAM (that is
what our harness exploited). Only LovyanGFX ships **a window, input handling, and
build files you can just use**.

---

## 2. Landscape

| Tool | Simulator (Win) | Editor | Licence / price | Cost to adopt from direct-draw |
|---|---|---|---|---|
| **LovyanGFX** | **Yes, first-party** | No | BSD | Hours — drawing verbs are API-compatible |
| **GUIslice** | Yes (SDL1/SDL2) | Yes (Builder) | MIT | Partial rewrite — retained-mode wrapper, but ships Adafruit_GFX + TFT_eSPI drivers so one codebase runs on board *or* PC |
| **LVGL** | Yes (SDL) | LVGL Pro / SquareLine / EEZ | MIT lib | **Full rewrite** |
| **SquareLine Studio** | In-editor | Yes | $790–2,200/yr | LVGL only |
| **EEZ Studio** | Yes (Electron) | Yes | GPLv3 | LVGL or Envox runtime |
| **TouchGFX** | Yes | Yes | Free **on STM32 only** | Rewrite + change silicon vendor |
| **Embedded Wizard** | Yes | Yes | €2,990–7,300/seat | Full rewrite (Chora) |
| **Crank Storyboard** | Yes | Yes | Quote-only enterprise | Full rewrite |
| **Qt for MCUs** | Yes | Yes | ~$3,950/dev/yr + royalty | Full rewrite (QML) |
| **Slint** | Yes | Yes (+ Figma plugin) | GPL / commercial | Full rewrite (.slint DSL) |
| **µGFX** | Historically | No | GPL / commercial | **Abandoned** (2019) |
| **Lopaka** | N/A — generator | Yes (pixel editor) | Apache-2.0 | **Emits direct-draw C** for Adafruit_GFX, TFT_eSPI, U8g2, **Arduino_GFX**, GxEPD2, LovyanGFX |
| **Wokwi** | Cloud | No | Freemium | Runs real firmware; no ST7789 element (ILI9341 stand-in, 240×320), no touch model |

---

## 3. The two halves of the problem

The study kept surfacing a distinction that no single tool spans:

- **design → code** — draw a layout, generate drawcalls. **Lopaka** does this,
  and is the only tool that emits direct-draw C. But you re-create the design
  inside it; it cannot consume Figma, and it cannot show you what your existing
  code renders.
- **code → preview** — take firmware that already exists and show its output.
  **LovyanGFX** does this for its own users. Nothing does it for Arduino_GFX,
  Adafruit_GFX or TFT_eSPI.

Our problem was the second. That is also the harder half, because the moment a
generator's output is hand-edited, the design tool is dead to that screen —
**no tool in the entire landscape round-trips.** SquareLine explicitly cannot
re-import edited C.

---

## 4. Is the pain real?

**Yes, and it is bimodal by library.** LVGL and LovyanGFX users have simulators
and the community answer is "use the SDL simulator". Everyone else has nothing.

Roughly **15 independent home-grown PC preview tools** were found spanning
2019–2025 — TFT_eSPI 5+, Adafruit_GFX 3+, U8g2 3+ — and **effectively none for
Arduino_GFX**, which is precisely why we ended up writing one. Representative
(Hackster, 2025):

> *"I started looking for a desktop tool that would allow me to simulate a TFT
> screen … Surprisingly, I couldn't find anything that really fit this use case.
> So I decided to build my own solution."*

Newer tools are visibly more polished, consistent with the cheap-ESP32-TFT boom
(the "CYD" boards). TFT_eSPI's maintainer states it cannot work in Wokwi because
it uses low-level register calls.

**Strongest counter-evidence:** a substantial share of the community sidesteps
the problem entirely by **choosing LVGL because it has a simulator**.

---

## 5. Why the gap persists (ranked by evidence)

1. **No silicon vendor incentive (STRONG).** Every free professional GUI tool is
   subsidised by a chip vendor: ST→TouchGFX, NXP→GUI Guider, Renesas/Infineon→
   emWin. **Espressif is the only high-volume MCU vendor with no first-party
   visual GUI designer** — their own guidance points at LVGL raw or SquareLine,
   an external company. Espressif sells chips, not displays, so nobody owns
   "ESP32 + arbitrary TFT". This is a business-model gap, not a technical one.
2. **Willingness to pay is too low (STRONG).** The population is hobbyists on $12
   boards. SquareLine went from ~$19/mo to $790/yr and took community flak.
   **GUIslice-Builder — a working, free, OSS, cross-vendor drag-drop builder for
   exactly this stack, maintained since 2019 — has 239 stars.** If the pain were
   acute *and* the population large, it would have thousands.
3. **Fragmentation (MEDIUM).** TFT_eSPI alone ships ~70 `User_Setup` combos.
   Every ecosystem that shipped a great simulator (Pebble 3 variants, Playdate 1,
   PineTime 1, PICO-8 a fixed spec) collapsed the hardware axes to a constant.
4. **Immediate-mode is harder to intercept than a retained scene graph
   (MEDIUM).** Real, but **LovyanGFX disproves that it is fatal** — this is
   "hasn't spread", not "impossible".
5. **"MCUs are too weird to emulate" (WEAK).** Mostly false. QEMU, Renode and
   Wokwi already do it.

---

## 6. Precedents worth copying

**InfiniSim** (PineTime) is the closest analogue: it compiles the **real firmware
sources** natively against a `sim/` folder of faked drivers (`St7789`, `Cst816s`,
`FreeRTOS`, `nrfx`) and renders through LVGL's SDL2 driver. Left-click is touch.
It started as a community PR and was later blessed into the org. *This is
essentially the architecture we arrived at independently for our harness.*

**Pebble** was the platinum standard — vendor-built QEMU running the actual
firmware binary, bundled with the SDK, plus CloudPebble in a browser tab. Still
alive via Rebble; the founder has since compiled it to WASM.

**The transferable pattern:** fake the hardware, not the CPU; shim at one HAL
boundary; draw via SDL2; mouse+keyboard input from day one; **bundle it with the
library** (a `platformio.ini` `env:native` that just works); accept UI fidelity
and drop timing fidelity.

---

## 7. What this means for a new entrant

**Honest negatives, stated plainly:**

- The most obvious product (a WYSIWYG builder for direct-draw) **already exists
  twice** — GUIslice-Builder since 2019 and Lopaka today — and neither has
  achieved escape velocity.
- The market failure looks like **absence of demand at a sustainable price**, not
  absence of product.
- The best technical answer for an individual developer today is simply
  **"migrate to LovyanGFX"** — hours of work, and the simulator is free.
- Wokwi is the only nearby commercial success and is a small bootstrapped
  business, not a venture-scale outcome.

**What is genuinely unoccupied:**

- **code → preview for the Adafruit_GFX / Arduino_GFX / TFT_eSPI branch.** No
  maintained tool does this. ~15 people built their own and abandoned them.
- **Round-tripping.** Nothing in the entire landscape supports it.
- **Figma → direct-draw.** Does not exist anywhere — not even TouchGFX imports
  Figma, a perennial request at a $10B silicon vendor.
- **Live-reload of drawcalls** for direct-draw. openHASP does it for LVGL via
  JSONL; nothing equivalent below that.

---

## 8. Verified vs inferred

**Verified** — LovyanGFX SDL backend (file listing, class inheritance, release
date); absence of SDL/native in Adafruit_GFX, Arduino_GFX, TFT_eSPI (repo
listings + zero-result code search); Lopaka's Arduino_GFX exporter (2025-09-22
blog); SquareLine pricing and the LVGL/SquareLine split; TouchGFX STM32-only
licence (SLA0048); GUIslice SDL + Adafruit_GFX/TFT_eSPI drivers in one tree;
InfiniSim architecture (source inspection); Pebble QEMU architecture (Rebble
devblog); µGFX last release 2019; ~15 home-grown tools with URLs and dates.

**Inferred** — Qt for MCUs pricing (third-party sourced; Qt publishes none); the
239-stars-means-no-demand reading (a defensible interpretation, not a
measurement); the ranking of causes in §5 (argued, not surveyed); Playdate
Simulator internals (Panic publishes nothing).

**Corrected during the study** — two threads independently claimed TFT_eSPI has
a PC simulator. Direct verification found only the abandoned third-party fork
`CURTLab/SDL_TFT_eSPI`; upstream has none. Worth recording as a reminder that
secondary claims need primary verification.
