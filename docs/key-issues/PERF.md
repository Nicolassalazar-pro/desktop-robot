## Performance [PERF]

> Reviewer: perf-review agent (embedded-translated scope)
> Last reviewed: 2026-08-07 | **Revised 2026-08-11**
> Open: 6 (PERF-2..6) | Fixed: 1 (PERF-1, commit `9fc63c8`) | Informational: 2 (PERF-7 documented cost, PERF-9 documented bound) | Verified: 1 (PERF-8)
> Scope: ESP32-S3R8 firmware — render/loop hot path, memory footprint, bus contention, task starvation.
> Read-only pass. No source changes. Costs tagged **measured / calculated / estimated** with arithmetic.
> Baseline: 240×280 ST7789V2 @ 40 MHz SPI, full-screen push = 32.5 ms (HARDWARE.md), 50 fps animating / 15 fps idle, loop 850–890 iter/s, WDT 5 s, drive lease 300 ms, gyro ~1.0 ms mean sample interval.
>
> User-perceptible ranking: dropped animation frames > control-frame latency > gesture stall > watchdog reset > wasted flash at 37 % usage. Ordered accordingly.

---

### PERF-1 — 45 KB of internal RAM locked up by `g_eyeRegionBackup` for a feature that is OFF by default
- Severity: HIGH
- Status: **FIXED (commit `9fc63c8`).** `g_eyeRegionBackup` is now `nullptr` at declaration (`src/main.cpp:1074`) and lazily allocated via `ps_malloc` at the first frame that actually needs it (`src/main.cpp:2277`), moving the buffer out of internal RAM into PSRAM and out of the always-on cost path — 45 KB of internal RAM returned, feature intact when enabled.
- File: `src/main.cpp:1068` (declaration), `src/main.cpp:242-253` (size constants), `src/main.cpp:5189-5192` (only real caller)
- Problem:
  ```
  uint16_t g_eyeRegionBackup[kEyeBackupMaxW * kEyeBackupMaxH] = {0};
  ```
  is a `.bss` array in internal SRAM, sized to the WORST-CASE eye backup rectangle:

  **Calculated**:
  `kEyeBackupMaxW = kEyeCenterSeparation + 2·kSurprisedCenterOffset + 2·kEyeMotionXMax + (kRenderMaxBaseW + ⌈kRenderMaxBaseW/4⌉) + 2·kSurprisedRingExtent + 2·kEyeDirtyMargin`
  `= 30 + 32 + 60 + 45 + 20 + 4 = 191`
  `kEyeBackupMaxH = 2·kEyeMotionYMax + kRenderMaxBaseH + 2·kSurprisedRingExtent + 2·kEyeDirtyMargin`
  `= 40 + 56 + 20 + 4 = 120`
  `191 · 120 · 2 B = 45,840 B ≈ 44.8 KB`.

  That is **~14 % of the 320 KB internal-RAM report** and roughly **44 % of the 103 KB currently reported in use**.

- Why it matters: the only caller is `applyHeadSympathyShift()` (line 5189), which fires only when `g_headShiftXDiv != 0` or `g_headShiftYDiv != 0`. Both default to 0 (line 407-408) — the head-sympathy feature is disabled at boot and only turns on via the `headgain` serial command. When idle, this array is 45 KB of zero-initialised .bss that never gets read or written. It also eats internal RAM that could hold cache-hot state today or a future SPI DMA bounce buffer.

- Fix (read-only proposal — do not implement here):
  1. `ps_malloc()` the buffer on the first call after `headgain` is set non-zero; free it back when both divisors go to 0.
  2. Alternatively size it to `g_eyeRegionW · g_eyeRegionH`, which for the current defaults (`g_eyeBaseW=12, g_eyeBaseH=36`, `g_eyeMotionX=14, g_eyeMotionY=10`) computes at runtime to roughly `(2·30 + 2·14 + 12 + 3 + 4) × (2·10 + 36 + 4) = 107 × 60 = 6,420 B` — a **7× reduction** even in the "feature enabled" case, and still allocated in internal RAM so the memcpy stays fast.
  Both fixes are compatible with the existing `backupEyeRegionFromCanvas` / `restoreEyeRegionToCanvas` API — only the buffer sizing changes.

- Cost tag: **calculated** from the constants above; recoverable RAM is exact. PROJECT-HISTORY §3 does not list this as tried-and-rejected.

---

### PERF-2 — Battery-meter strip is re-pushed at 50 Hz even though its data updates at 1 Hz

- Severity: HIGH (dropped frames, bus contention)
- Status: Open
- File: `src/main.cpp:2139-2181` (`drawBatteryMeter`), `src/main.cpp:10790, 10797-10799, 10830-10832` (three overlap-repaint sites), `src/main.cpp:1687-1690` (`rectTouchesBatStrip`)
- Problem: every time the dirty-rect eye push touches y<40, the loop unconditionally calls `drawBatteryMeter()` which does a full canvas fill + text draw + `pushCanvasRectToLcd(0, 4, 240, 36)`. The strip data (`g_batVoltage`, `g_batPercent`, `g_batCharging`) only actually change once every `kBatUpdateIntervalMs = 1000` ms (line 1085) — but the strip is redrawn and re-pushed on every eye frame whose dirty rect intrudes into `y<40`.

  **Measured / calculated**:
  - Strip push = 240·36·2 B = **17,280 B** over SPI.
  - Full-screen push (240·280·2 B = 134,400 B) = **32.5 ms measured**. Linear-scaled by pixel count → strip push ≈ 32.5 · 17,280 / 134,400 ≈ **4.2 ms per repaint** (pure SPI clock alone at 40 MHz would be 3.5 ms; the extra ~0.7 ms is window setup + the `SPI.transfer` loop overhead already amortised in the 32.5 ms measurement).
  - SURPRISED / SCARED and blinks that shift up: `kSurprisedCenterOffset = 16` (line 239) plus `kSurprisedRingExtent = 10` plus `kEyeMotionYMax = 20` pushes the top of the eye dirty rect above y=40 for large fractions of the animation. Under continuous SURPRISED animation at 50 fps, the strip is re-pushed on essentially every frame.
  - Worst-case wasted SPI time: **50 frames/s × 4.2 ms = 210 ms/s = ~21 % of wall clock** spent re-pushing a strip that only changes at 1 Hz. That is directly consumed from the 50 fps budget — the eye's own dirty-rect push is on top of this — and it is a plausible cause of visible frame drops during SURPRISED bursts.

- Why it matters: the render loop targets `kFastFrameMs = 20` (50 fps). One eye dirty-rect push is small (typical `g_eyeRegionW · g_eyeRegionH ≈ 107·60 = 6,420 px = 12.8 KB`, ≈ 3.1 ms), so a redundant strip re-push on the same frame roughly **doubles the SPI cost of the animating frame**. That either lands the loop past the 20 ms deadline (frame drop) or drives SPI DMA/CPU contention that starves the WS transport task, exactly the kind of thing the user asked about.

- Fix (proposal):
  1. Add a `g_batStripDirty` flag set by `updateBatteryMeter()` whenever `g_batVoltageRaw`, `g_batVoltage`, `g_batPercent`, or `g_batCharging` change. Only call `drawBatteryMeter()` on eye-region-overlap when this flag is set (then clear it). At other times, since the strip is already correct in the PSRAM canvas, the eye-region push at `y<40` already carries the correct strip pixels — no separate strip push is required.
  2. Or split `drawBatteryMeter` into `renderBatteryMeter()` (writes to canvas — cheap PSRAM writes) and `pushBatteryMeter()` (SPI push). Skip the push when the strip content did not change and the dirty-rect eye push has already covered the intersected pixels.

- Cost tag: **measured baseline (full-screen push 32.5 ms) · calculated (pixel ratio scale)** for the per-repaint cost; frame-count is **estimated** but bounded — the code path unconditionally hits `drawBatteryMeter()` on every overlap.

---

### PERF-3 — `applyHeadSympathyShift` costs a near-full-screen push on every frame it fires (currently latent, but easy to trip)

- Severity: HIGH (frame drops, latent) — user-perceptible only if head-sympathy is turned on
- Status: Open (latent)
- File: `src/main.cpp:5156-5199`
- Problem: when `g_headShiftXDiv` or `g_headShiftYDiv` is non-zero, every time the gaze shifts the code computes the union of the old and new mascot rectangles, `fillRect`s the union with `MASCOT_A_BG`, `drawMascotToCanvas`es the entire mascot at the new origin, restores the eye backup, then `pushCanvasRectToLcd(unionX, unionY, unionW, unionH)`.

  **Calculated**:
  - Mascot dims: `MASCOT_A_W = 220, MASCOT_A_H = 216` → mascot pixel count = **47,520 px** (≈95 KB).
  - Union with a 1-pixel gaze delta ≈ 221 × 217 px (mascot area + 1 in each dim). At 50 fps eye animation, `g_eyes.gazeOffsetX/Y` changes almost every frame during eye-motion tweens, so `newShiftX != g_lastShiftX` fires almost every animating frame.
  - Union SPI push ≈ 32.5 ms · (221·217·2) / (240·280·2) = 32.5 · 47,957 / 67,200 = **~23.2 ms per push**.
  - Add the PSRAM canvas cost: `fillRect(220·216) + drawMascotToCanvas(220·216)` writes ≈ 95 KB to PSRAM plus the mascot source read (from flash `.rodata`, so cheap). At an assumed octal-PSRAM sustained write ≈ 40 MB/s, that is ≈ **2.4 ms of PSRAM writes** (estimated).
  - Total added cost per firing frame ≈ **~25 ms**, on top of the eye dirty-rect push. That exceeds the 20 ms `kFastFrameMs` budget by itself → guaranteed to drop the target 50 fps to ~25 fps whenever head-sympathy is on and the gaze is animating.

- Why it matters: the feature is off at boot but is enabled by a one-line serial command (`headgain <x> <y>`). Once enabled, any 50 fps eye animation immediately loses half its frames. This is not a "might be slow" — the numbers say the frame budget is exceeded by ~25 %.

- Fix (proposal): only fill and push the *difference* rects (the small "reveal" strip at the leading edge plus the "erase" strip at the trailing edge — a total of ≈ `|dx|·MASCOT_A_H + |dy|·MASCOT_A_W` pixels rather than the full union). For a 1 px shift, that is ~440 px instead of 47,957 — ~100× smaller.

- Cost tag: **measured baseline + calculated ratio**. Confirms with hardware profiling would need `headgain` enabled and `lcdact` counters.

---

### PERF-4 — `batteryReadVoltage()` blocks the loop task for ~3–5 ms every 1 s

- Severity: MEDIUM (single frame stall once per second)
- Status: Open
- File: `src/main.cpp:2116-2123`, `src/main.cpp:1083` (`kBatSampleCount = 16`)
- Problem:
  ```
  for (uint16_t i = 0; i < kBatSampleCount; i++) {
    sumMv += analogReadMilliVolts(BAT_ADC);
  }
  ```
  Runs synchronously on the Arduino loop task once per `kBatUpdateIntervalMs = 1000 ms`.

  **Estimated**: `analogReadMilliVolts` on ESP32-S3 with `esp_adc_cal` lookup is roughly 150–250 µs per call (documented in ESP-IDF sample code; not measured on this board). 16 × 200 µs ≈ **3.2 ms** blocking. At 200 µs it is 3.2 ms; at 250 µs it is 4 ms.

- Why it matters: 3–5 ms is comparable to the eye dirty-rect push cost (~3.1 ms). Once per second the render frame takes ~2× as long as usual. Not enough to trip either watchdog (5 s task WDT, 300 ms drive lease), but visible as a periodic 1-frame stutter at 50 fps and it eats into the gyro sampler's carve-out window on that iteration.

- Fix (proposal): amortise across 16 iterations by keeping a running `sumMv` and index. One `analogReadMilliVolts` per loop iteration for 16 iterations spreads the cost from 3.2 ms in one hit down to ~200 µs per iteration for 16 iterations, which is comfortably below one frame budget. `kBatUpdateIntervalMs` is 1 s so there is enormous headroom for the interleave.

- Cost tag: **estimated** — per-call ADC cost was not measured on this board. Order of magnitude is safe; the cost bound would be exact after 5 lines of instrumentation.

---

### PERF-5 — 5-pass halo in `EyeEngine::update()` draws the base eye 5× per eye per frame

- Severity: MEDIUM (canvas write cost, does not touch SPI)
- Status: Open
- File: `lib/EyeEngine/EyeEngine.cpp:393-407`, `lib/EyeEngine/EyeEngine.cpp:808-844` (`drawEye`), `src/main.cpp:10557` (halo enabled at boot: `g_eyes.setOutlineColor(MASCOT_A_OUTLINE)` where `MASCOT_A_OUTLINE = 0x18C5` — non-zero, so the halo path is ALWAYS on).
- Problem: to produce a 1-pixel outer ring, the engine draws the *full* eye shape 4 times at `(±1, 0)` and `(0, ±1)` offsets in outline colour, then a 5th time in real colours. Every `drawEye()` starts with `fillRoundRect(x0, y0, eyeW, eyeH, radius, eyeColor)` (line 838) which paints the *entire* eye rectangle — the halo passes overwrite ~95 % of their pixels with the centre pass, so ≈95 % of the halo work is thrown away.

  **Calculated**:
  - Default eye = 12 × 36 = 432 px. SURPRISED = 36 × 36 = 1,296 px.
  - Per frame: 5 passes × 2 eyes × 432 px = **4,320 px canvas write** (default), or 5 × 2 × 1,296 = **12,960 px** (SURPRISED).
  - Useful outline is the perimeter only: 2·(12 + 36) + 4 = 100 px per eye × 2 = 200 px. Ratio: **4,320 / 200 = 21.6× overdraw** at default eye size.
  - PSRAM canvas write cost (estimated 40 MB/s, 2 B/px): 4,320 × 2 / 40 MB/s ≈ **216 µs per frame** (default), 12,960 × 2 / 40 MB/s ≈ **648 µs per frame** (SURPRISED). At 50 fps, SURPRISED consumes ~32 ms/s of PSRAM write bandwidth just for redundant fill.

- Why it matters: this is not on the SPI bus but it competes with SPI-push DMA reads for the octal-PSRAM controller. During a dirty-rect push the SPI DMA is *reading* PSRAM; if a canvas write is issued at the same time, both stall on the PSRAM controller. And 200–650 µs / frame is a real slice of the 20 ms frame budget when SURPRISED animation is also being pushed to SPI.

- Fix (proposal): draw the halo as an *outline only* (four 1-px thick `drawFastHLine`/`drawFastVLine` runs at the outer edge, plus corner arcs) rather than filling the whole eye rect 4 times. This is a specific EyeEngine.cpp refactor; not trivial (mood-specific overlays like `MOOD_ANNOYED` lashes also need a halo pass), so keep this as MEDIUM.

- Cost tag: **calculated** for pixel counts; PSRAM write cost is **estimated** (typical octal-PSRAM sustained rate). Instrumentation with `esp_timer_get_time()` around the 5-pass block would nail it in one afternoon.

---

### PERF-6 — Non-FACE LCD modes still spend the loop budget on service calls before napping

- Severity: MEDIUM (loop iterations wasted; not user-visible until user is in QR mode)
- Status: Open
- File: `src/main.cpp:10709-10741` (loop head), `src/main.cpp:10757-10779` (non-FACE nap)
- Problem: when the LCD is in a non-FACE mode (`LCD_MODE_QR_URL`, `LCD_MODE_QR_WIFI`), the render decision tree short-circuits and the loop `delay(50)`s (line 10770). But *before* that nap, every iteration still runs `PiezoTone::service`, `serviceMelody`, `drainSerial`, `g_orTouch.control()` (I²C poll), `overrideLoop()`, `checkPowerButton`, `updateBatteryMeter`. At 20 Hz (delay 50) this is fine, but during movement or turn the code falls through to `delay(1)` (line 10776) and runs all six services 1,000×/s in a mode where the screen is a static QR. `overrideLoop()` in particular publishes WS telemetry at 5 Hz and does allocation-free state work every call, but it is *always* invoked.

  **Calculated / measured**: loop runs at 850–890 iter/s (given). Each of the six services costs on the order of 1–20 µs (estimated). If all six average 10 µs each, that is 60 µs × 850 = **51 ms/s of loop CPU** for services that do nothing user-visible in QR mode. That is real but not user-perceptible (the QR is not animated) — hence MEDIUM.

- Why it matters: the movement carve-out to `delay(1)` (line 10776) exists specifically so the gyro sampler can hit its ~1 ms cadence. Every microsecond spent on unrelated service work in non-FACE mode is a microsecond the gyro sampler is NOT running. This is the "starves the gyro sampler" concern the user listed.

- Fix (proposal): in non-FACE modes, gate `overrideLoop` and `updateBatteryMeter` to a slower cadence (e.g. every 20 ms) — they don't need to run at 1 kHz. Keep `drainSerial` and `checkPowerButton` at 1 kHz.

- Cost tag: **loop rate measured (850–890 iter/s)**; **per-service cost estimated** — no per-service timing exists yet.

---

### PERF-7 — 4.5 KB `MelodyGen::Memo` in `.bss` internal RAM (documented cost, not a real problem)

- Severity: LOW (footprint only, verified small)
- Status: Open (informational)
- File: `src/melody_gen.cpp:369` (`uint8_t table[(kMaxAllowed+1) * kMaxLength * 4 * 2]`), `src/melody_gen.cpp:678` (`static Memo memo`)
- Problem: the memo table cited in the review brief.
  **Calculated**: `(kMaxAllowed+1) · kMaxLength · 4 · 2 = 33 · 17 · 4 · 2 = 4,488 B ≈ 4.4 KB`. Declared `static` inside the caller, so it lives in `.bss` internal SRAM, not on the loop stack (loop stack is 8 KB — this would not fit anyway).
- Why it matters: 4.4 KB is 1.4 % of the 320 KB budget. Well within margins. `Phrase` is ~500 B; not on the loop stack either (also confirmed static in the melody player). This is the compiled cost of correctness, and the memoisation makes the recursive `canFinish` search bounded — removing it would risk mood-generation latency spikes.
- Fix: **none — leave alone**. Listed because the brief specifically flagged it. If you wanted the RAM back the Memo table could be tri-state-packed 2-per-byte (2,244 B) or moved to PSRAM (accepting slower memoised lookups), neither is a good trade at 37.5 % flash / 31.5 % RAM.
- Cost tag: **calculated** exactly.

---

### PERF-8 — `beginResponse_P` on `OVERRIDE_HTML` does NOT copy to heap — good, keep it that way

- Severity: INFO (no action)
- Status: Verified
- File: `src/main.cpp:8238` (`const char OVERRIDE_HTML[] PROGMEM = R"OD(…)OD";` — ends at line 8512), `src/main.cpp:9882` (served with `beginResponse_P`)
- Problem: the brief asked to verify the inlined web app is not being copied to heap.

  **Calculated**: `OVERRIDE_HTML` spans lines 8238–8512 ≈ **~18 KB** (a 20 KB budget). It is `PROGMEM`-qualified so it lives in `.rodata` / flash. `beginResponse_P(int code, const char*, const uint8_t*)` on AsyncWebServer constructs an `AsyncProgmemResponse` that streams from flash in small chunks (typically `TCP_MSS`, ~1436 B) — no full copy to heap. That leaves only one `AsyncProgmemResponse` object (~64 B) on the heap per HTTP GET /. Confirmed the code path uses the PROGMEM overload.

- Fix: **none. Do not "optimise" this by pre-rendering to a `String` or by loading into a buffer — that would defeat the current zero-copy design.** Also do not switch to a filesystem-backed serve; the current design avoids LittleFS and its wear characteristics deliberately.
- Cost tag: **verified** (code path confirmed).

---

### PERF-9 — `drainSerial` is bounded per iteration, but `dispatchCommand()` can be slow synchronously

- Severity: LOW-INFO (latent worst case)
- Status: Open (informational)
- File: `src/main.cpp:5072-5103` (`drainSerial`), `dispatchCommand` scattered above
- Problem: `drainSerial` is well-shaped — 64 bytes per iteration cap, `return` on first `LineReady`, so at most one command is dispatched per loop iteration. However some serial commands do heavy synchronous work (e.g. `melody`, `spin auto`, `wander`) that could stall the render loop for tens or hundreds of ms. These are user-typed commands so a stall during them is expected UX. No fix proposed.
- Cost tag: **inspection** — no measurements taken. Only listed for completeness.

---

## Summary

**Findings by severity**: 1 CRITICAL, 2 HIGH, 3 MEDIUM, 3 LOW/INFO.

**Largest measurable win (rank order):**
1. **PERF-1** — recover **~45 KB internal RAM** (~14 % of the 320 KB report) by moving `g_eyeRegionBackup` to PSRAM on demand or sizing it to `g_eyeRegionW · g_eyeRegionH`. Pure calculated saving, no runtime cost when head-sympathy is disabled.
2. **PERF-2** — recover **up to 210 ms/s of SPI bus time** (≈ 21 % of wall clock) during SURPRISED / SCARED animation by gating the battery-strip re-push on actual data change instead of pixel-rectangle overlap. Fixes the most plausible current cause of visible frame drops during expressive mood animation.
3. **PERF-3** — latent 25 ms-per-frame regression the moment `headgain` is enabled; fix by pushing only difference strips instead of the full mascot union. Not affecting anyone today, but a one-command trip to a 50 → 25 fps regression is worth closing.

**Anything I could not tell without on-hardware profiling:**
- The 200 µs per-call estimate for `analogReadMilliVolts` (PERF-4) — order of magnitude is safe, exact cost is one `esp_timer_get_time()` block away.
- Octal-PSRAM sustained write bandwidth (used in PERF-5). I assumed 40 MB/s; 20 MB/s or 60 MB/s would change the halo overdraw finding's µs count but not its 21.6× overdraw ratio, which is exact.
- The precise cost of each of the six per-iteration services (PERF-6). The finding stands on a rough 10 µs average; if any single service turns out to be an outlier (say, 100 µs) that changes the priority.

**Not proposed (already ruled out by PROJECT-HISTORY §3):**
- Raising the SPI clock past 40 MHz (datasheet-measured ceiling — PROJECT-HISTORY §4).
- Row-by-row bitmap pushes (already replaced by single-transaction pushes).
- Sprite sheets / GIF (§3 R1), tiered audio synthesis (§3 R13), WiFiManager (§3 R3), HTTP-poll control (§3 R2).
