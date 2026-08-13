# Live Preview Harness — Research Report

## Executive summary

The right architecture is **Option (a) extended, not Option (b) rewritten**. `tools/lcd_preview.py` is already the correct instinct: parse the C++ for the ground truth, render a faithful bitmap in Python, and gate on it. What is missing is (i) **pixel-exact glyph rendering using the actual `glcdfont.c` byte table** (this eliminates ~all of the residual drift risk for text — currently the only "close, not exact" category in the tool's own docstring at `tools/lcd_preview.py:14`), (ii) a **stdlib-only HTTP server with SSE live reload** watching `src/main.cpp` and `src/override_html.cpp`, and (iii) an **HTML extractor + local WebSocket shim** that speaks the 4-byte control / 30-byte telemetry protocol so the phone UI is fully interactive in the same side panel. Option (b) — natively compiling the real firmware drawing code against a stub `Arduino_GFX` — is technically feasible (there is precedent for host builds in `test/run_motor_host_tests.ps1`) but the surface area is large: `main.cpp` reaches into `g_canvas->getFramebuffer()` in ~15 places (framebuffer memcpy, banded push, sheet compositor, eye engine backup regions), and stubbing those correctly is more code than the current preview and buys almost nothing once the font table is faithful.

---

## Verified facts (evidence gathered from repo + upstream)

1. **The 5x7 font in Arduino_GFX is a straight import from Adafruit-GFX-Library.** `Arduino_GFX.cpp` header comment: `"start rewrite from: https://github.com/adafruit/Adafruit-GFX-Library.git"` and it includes `font/glcdfont.h`. Verified via upstream fetch of `moononournation/Arduino_GFX/src/Arduino_GFX.cpp` (first ~15 lines).
2. **The classic font is 5 column-bytes per glyph, LSB = top row.** Verified via fetch of `adafruit/Adafruit-GFX-Library/glcdfont.c`. The header comment "5x7 font" is literal. Glyph advance is 6 px (5 columns + 1 blank), height 8 px at size 1. The firmware relies on exactly this arithmetic at `src/main.cpp:10213` — `const int16_t textW = strlen(s) * 6 * textSize;`.
3. **The library dep is pinned:** `moononournation/GFX Library for Arduino@1.3.9` — `platformio.ini:121`. Same version pinned in `lib/EyeEngine/library.json`. That is the file whose glyph table must be reused.
4. **Panel geometry:** 240 × 280 ST7789V2, `-D LCD_WIDTH=240 -D LCD_HEIGHT=280 -D LCD_ROW_OFFSET=20` — `platformio.ini:66-68`. `kScreenW`/`kScreenH` in `src/main.cpp` are compile-time-bound to those macros (that's why `parse_constant` in `lcd_preview.py:81-104` has a `-D` fallback path).
5. **Arduino_GFX API surface actually used on the canvas** (from `grep g_canvas->` at 50-match cap): `fillScreen`, `fillRect`, `fillRoundRect` (via `g_gfx` at `src/main.cpp:10464`), `fillCircle`, `drawCircle`, `drawRect`, `draw16bitRGBBitmap`, `setTextSize`, `setTextColor`, `setTextWrap`, `setCursor`, `print`. That's a small enough set that a Python re-implementation is tractable (Pillow already covers all of it except `print` — which is exactly where the font-table reuse plugs in).
6. **Raw framebuffer access exists in ~15 places.** `g_canvas->getFramebuffer()` returns `uint16_t*` (RGB565) and is used for `pushCanvasBanded`, sheet compositor (`pushSheetStrip` — `src/main.cpp:10443-10450`), eye engine region backup, mascot blit, and battery label repaint. **This is the reason Option (b) is expensive**: to run the real functions you must supply not just an Arduino_GFX class but a `getFramebuffer()` that returns real RGB565 memory that other code then reads and writes directly.
7. **Sheet compositor model is documented and integer-only:** `src/sheet_transition.h:1-201`. It has no Arduino deps by design ("integer maths over positions, velocities and timestamps. That keeps it host-testable against the REAL shipped code" — `sheet_transition.h:7-11`). Already host-tested via `test/sheet_transition_test.cpp` in `run_motor_host_tests.ps1:135-140`. The commit constant is `kCommitPct = 85` — `sheet_transition.h:80`, already parsed by `lcd_preview.py:437-439`.
8. **The phone HTML lives in one C++ raw-string literal** — `src/override_html.cpp:16` opens `R"OD(...)` and closes `)OD";` at line 438. Extraction is a two-line regex. Length: 438 lines total. The doc comment even calls out that this was extracted so it "no longer sits between the WebSocket ingress code and the telemetry builder in main.cpp" (`override_html.cpp:2-6`).
9. **Wire protocol is fixed and documented in-line:**
   - Opcodes: `OP_CONTROL=0x01, OP_TELEMETRY=0x12, OP_TRIM=0x03, OP_MOOD=0x04, OP_GESTURE=0x05, OP_SOUND=0x06` — `override_html.cpp:199`.
   - Control frame: 4 bytes `[u8 op=0x01][i8 L][i8 R][u8 seq]` — `override_html.cpp:298-300`.
   - Telemetry frame: 30 bytes, opcode at byte 0, `u16` display voltage×1000 at byte 3, `u8` pct at 5, `u8` mood at 6, `u8` chg at 7, `u8` eligible at 8, `u8` state at 9 — `override_html.cpp:272-288`. Bytes 10..29 are read but not touched by the current UI (verify against `test/test_ws_protocol_drift.py` — that Python test already knows the layout).
   - Client sends at 33 Hz: `setInterval(()=>sendCtrl(curL,curR),30)` — `override_html.cpp:305`.
   - Handshake: `ws://.../ws?k=<ACCESS_TOKEN>` from `location.search` — `override_html.cpp:236,266`. `code=1008 reason=Forbidden` means auth failed; `Owner busy`/`Stale`/`Unavailable` are other close reasons the UI decodes — `override_html.cpp:269`.
   - CSS state driven by `document.body.dataset.drive` = `LOCKED | WAIT NEUTRAL | ARMED | SAFE OFF | LINK STALE | AUTH REQUIRED | AUTH FAILED | OWNER BUSY | STALE | UNAVAILABLE | RETRYING` — enumerated in `override_html.cpp:212,222-229,232,269`.
10. **Native host-build precedent:** `test/run_motor_host_tests.ps1` compiles firmware translation units (`src/drive_session.cpp`, `src/sheet_transition.cpp`, etc.) with `g++ -std=c++17 -Wall -Wextra -Werror` and runs `.exe`s. Include stubs live in `test/stubs/` — currently just `nvs.h` (`test/stubs/nvs.h:1-51`). There is no Arduino_GFX stub today.
11. **The existing preview has already caught two real bugs.** The tool's docstring says so (`tools/lcd_preview.py:3-7` — "step dots overlapping the password row by 2 px"; `tools/lcd_preview.py:284-301` — the sheet card lines were originally at the panel centre, leaving the operator "dragging a blank rectangle for the first half of the travel"). This is the argument for extending it, not replacing it.
12. **Pillow is available in the PIO venv**, and `lcd_preview.py:56-62` already assumes it. No new Python dependency is required.

---

## Option comparison (concrete)

### (a) Extend `tools/lcd_preview.py` + tiny HTTP server + live reload — **RECOMMENDED**

**Fidelity after font fix:** exact for geometry (parsed from source), exact for text metrics (already correct at `lcd_preview.py:70-73, 199`), **exact for glyph pixels** once `glcdfont.c` is reused byte-for-byte. The remaining "close" category (mascot bitmap on the sheet filmstrip) is a deliberate design choice per the docstring (`lcd_preview.py:24-27`) — it labels itself a placeholder.

**Cost:** ~200 LOC on top of the existing tool. Zero new dependencies (Python stdlib http.server + Pillow, both present).

**Drift risk:** the same as today — nil for anything that has a `constexpr` or a numeric literal parsed by regex. If a screen learns a new call type (e.g. `drawFastVLine`), `text_elements`/`dot_elements` won't catch it silently — the existing `--check` mode compares against `h` and reports overflow, so a completely new element type would need a new parser. This is worth documenting but is not new.

### (b) Native compile of firmware drawing functions against a stub Arduino_GFX

Feasibility: **medium**, value: **low once (a) has the font**. Concretely to make `drawQrScreenWifi()` link and run on the host you would need:

- A stub `Arduino_GFX_Library.h` and `Arduino_GFX`/`Arduino_Canvas`/`Arduino_DataBus` class hierarchy exposing every method in the "API used" list from fact 5 above (~12 methods).
- The `print(const char*)` path — which routes through `Arduino_GFX::write(uint8_t)` → per-glyph column loop reading `glcdfont` PROGMEM — either faithfully copied from Adafruit-GFX, or you use just the font table and re-implement `write()` yourself (which is essentially what option (a) does anyway).
- Working `getFramebuffer()` returning `uint16_t*` to a real 240×280×2 buffer, because 15 sites in `main.cpp` bypass the drawing API and touch bytes directly (fact 6).
- Stubs for `Arduino.h`, `PROGMEM`, `pgm_read_byte`, `Serial.println(F(...))`, `snprintf` guards.
- Stubs for `qrcode_initText` (ricmoo library — small, could vendor) OR a mock QR.
- Stubs for the credentials/auth code path so `copyActivationCredentials` returns something sensible (`main.cpp:10527-10531`).
- A way to isolate the ~20 firmware globals `drawQrScreenWifi` transitively touches (`kQrWhite`, `kQrWifiVersion`, `kQrWifiPixelScale`, `g_canvas`, `g_lcdMode`, `secureZero`, `ActivationCredentials`, …) without dragging in Wi-Fi, WebSocket, IMU, servo, and mascot code — because `main.cpp` is one 11,000-line translation unit (per grep line numbers).

The last point is the killer. Unlike `sheet_transition.cpp` — which was deliberately factored out with no Arduino deps (`sheet_transition.h:5-11`) — the drawing functions live in `main.cpp` alongside everything else. You cannot compile them without either (i) heavy `#ifdef HOST_PREVIEW` surgery through `main.cpp`, or (ii) copying the functions into a new TU (which reintroduces drift, defeating the entire purpose).

**Recommendation on (b):** keep it as a possible future step *only if the QR module bitmap itself starts to matter*. Everything else can be modelled exactly in (a).

### (c) JS/HTML re-implementation of the LCD screens

Fast to iterate but every constant becomes a separate parse-and-inject path. `lcd_preview.py` already proved that source-parsing works; doing it twice (once for the checker, once for the JS renderer) doubles the surface for silent drift. **Reject** for the LCD side. (Keep JS *composition* — the panel layout — but the pixels come from Python-rendered PNGs.)

### (d) Alternatives evaluated

- **wasm-compile drawing code**: same source-of-truth problem as (b) plus a toolchain the repo doesn't have. Reject.
- **Headless SDL / LVGL simulator**: this project doesn't use LVGL (Arduino_GFX + Arduino_Canvas). Reject.
- **QEMU esp32 target with framebuffer capture**: massively over-scoped; needs the whole peripheral model. Reject.
- **VNC to running hardware**: violates the "must not interfere with COM4" constraint and requires a flash cycle per iteration, which is exactly what the developer is trying to avoid. Reject.

---

## Font fidelity — the specific plan

The single biggest fidelity risk today is `lcd_preview.py:201-204`, which falls back to `consola.ttf` (a wildly different font) for text. This is the fix.

**Approach:** copy the raw `font[]` byte array out of `glcdfont.c` into a small Python module and render it column-by-column with `Pillow.putpixel` (or, faster, a numpy blit — but numpy is not required; a per-pixel loop over 5×7 glyphs is fine at 240×280).

The Adafruit classic font is 96 printable glyphs starting at ASCII 0x00, then extended set. Each glyph is exactly 5 bytes. Byte `n` is the pixel column `n`; bit `b` (LSB=0) is row `b`. The 6th column is blank (advance). At `setTextSize(N)` each source pixel becomes an N×N filled square. This mirrors the `Adafruit_GFX::drawChar` inner loop and is what the firmware's `strlen(s) * 6 * textSize` metric already assumes (`main.cpp:10213`).

**Where to source the table.** The Arduino_GFX library at `moononournation/GFX Library for Arduino@1.3.9` uses the identical bytes as `adafruit/Adafruit-GFX-Library/glcdfont.c` (verified via upstream fetch, and the Arduino_GFX source explicitly says "start rewrite from: … Adafruit-GFX-Library" — see fact 1). Either one works; I recommend copying from the moononournation copy so the source repo matches the pinned library.

**Concrete code sketch — `tools/gfx_font.py`:**

```python
"""Adafruit classic 5x7 font, byte-identical to Arduino_GFX 1.3.9's
font/glcdfont.h, itself imported from Adafruit-GFX-Library/glcdfont.c.

Copied here so the host preview renders the SAME pixels the ST7789 will,
not a system font that happens to look 5x7-ish. This is the single change
that lets tools/lcd_preview.py stop caveating text rendering as 'close'
in its docstring.

Table extraction (run once, then commit the .py):
    curl -sL https://raw.githubusercontent.com/moononournation/Arduino_GFX/1.3.9/src/font/glcdfont.h \
      | python -c "import re,sys; d=sys.stdin.read(); \
                   b=re.findall(r'0x[0-9A-Fa-f]{2}', d); \
                   print('FONT = bytes([' + ','.join(b) + '])')"
"""
from PIL import Image

# Paste the extracted FONT = bytes([...]) here (1280 bytes = 256 glyphs * 5).
FONT: bytes = bytes([...])   # noqa: replaced by the one-shot extractor above

GLYPH_W = 5
GLYPH_H = 7
ADVANCE = 6   # 5 pixel columns + 1 blank -- matches main.cpp:10213 arithmetic
HEIGHT = 8    # 7 pixel rows + 1 blank; matches lcd_preview.py:73

def draw_char(img: Image.Image, x: int, y: int, ch: str, size: int,
              fg: tuple, bg: tuple | None = None) -> None:
    """Draw one glyph exactly the way Adafruit_GFX::drawChar does.

    Reference: Adafruit-GFX-Library/Adafruit_GFX.cpp drawChar() -- inner
    loop reads 5 column-bytes, LSB=top-row, and paints an NxN filled block
    per source pixel. No transformation, no antialias.
    """
    c = ord(ch)
    if c >= 256:
        c = ord('?')
    base = c * 5
    px = img.load()
    for col in range(5):
        column_byte = FONT[base + col]
        for row in range(8):        # loop 0..7 -- row 7 is the glyph descender row
            on = (column_byte >> row) & 1
            colour = fg if on else bg
            if colour is None:
                continue
            # NxN block: mirror the size scaling in Adafruit_GFX::write()
            for dx in range(size):
                for dy in range(size):
                    xx = x + col * size + dx
                    yy = y + row * size + dy
                    if 0 <= xx < img.width and 0 <= yy < img.height:
                        px[xx, yy] = colour

def draw_string(img: Image.Image, x: int, y: int, s: str, size: int,
                fg: tuple, bg: tuple | None = None) -> None:
    for i, ch in enumerate(s):
        draw_char(img, x + i * ADVANCE * size, y, ch, size, fg, bg)
```

Then in `lcd_preview.py` replace `draw_text_centred` (currently `lcd_preview.py:197-205`):

```python
def draw_text_centred(img, w, el, text, size, fg=BLACK, bg=None):
    from gfx_font import draw_string, ADVANCE
    x = (w - len(text) * ADVANCE * size) // 2   # same metric as firmware
    draw_string(img, x, el.y, text, size, fg, bg)
```

**Why this is drift-proof, not just accurate today.** The FONT table is a byte-for-byte copy of what the firmware links against, gated on the pinned `Arduino_GFX@1.3.9`. If DEP-6 (`platformio.ini:24-31`) is ever bumped to a version that changes the font, the pin change is the review moment; add a one-line hash check in `lcd_preview.py --check` that hashes `FONT` and warns if the pinned version differs from the version this table was extracted from. That reduces the drift risk to "someone changes the pin without re-extracting", which is a review-visible action.

**Text colour.** Firmware colours are RGB565 (`kQrBlack=0x0000`, `kQrWhite=0xFFFF`, `0xF800`, `0x8410`, etc. — `main.cpp:2469, 10681`). Convert once with the standard expansion `r5<<3 | r5>>2` per channel. That's exact for the 5 or 6 hard-coded values used by drawing paths and should be a small helper `rgb565_to_rgb888(u16) -> (r,g,b)`.

---

## Live reload — stdlib-only design

**Design:** `python -m tools.preview_server` (a new file, ~120 LOC) spawns:

1. an `http.server.ThreadingHTTPServer` bound to `127.0.0.1:8765`,
2. a background thread that re-runs `lcd_preview.main()` any time `src/main.cpp`, `src/sheet_transition.h`, `src/override_html.cpp`, or `platformio.ini` mtime changes,
3. an SSE endpoint `/events` that pushes `event: reload\n\n` whenever the re-render finishes.

**Why SSE, not WebSocket, and not `watchdog`.** SSE is one HTTP GET that stays open, is stdlib-friendly (`self.wfile.write(b"...")` in a `do_GET`), and requires zero JS beyond a 3-line `EventSource` in the panel HTML. `watchdog` adds a package for something that a 3-line `os.stat` poll does at 500 ms cadence with negligible CPU. Injected-JS polling of the PNGs also works but reloads the whole panel, which loses scroll/selection.

**File watch loop:**

```python
# tools/preview_watch.py
import os, time, pathlib
REPO = pathlib.Path(__file__).resolve().parent.parent
WATCHED = [REPO/"src/main.cpp", REPO/"src/sheet_transition.h",
           REPO/"src/override_html.cpp", REPO/"platformio.ini"]

def snapshot():
    return tuple((p, p.stat().st_mtime_ns) for p in WATCHED if p.exists())

def watch(callback, poll_ms=500):
    prev = snapshot()
    while True:
        time.sleep(poll_ms / 1000)
        cur = snapshot()
        if cur != prev:
            prev = cur
            try: callback()
            except Exception as e: print(f"[preview] rebuild failed: {e}")
```

**SSE handler (stdlib):**

```python
# tools/preview_server.py -- excerpt
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import threading, queue, json

subscribers: list[queue.Queue] = []
subs_lock = threading.Lock()

def broadcast(kind: str, payload: dict = None):
    msg = f"event: {kind}\ndata: {json.dumps(payload or {})}\n\n".encode()
    with subs_lock:
        for q in list(subscribers):
            try: q.put_nowait(msg)
            except queue.Full: pass

class H(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            q = queue.Queue(maxsize=64)
            with subs_lock: subscribers.append(q)
            try:
                # kick a comment every 15 s so proxies don't close the stream
                while True:
                    try: chunk = q.get(timeout=15)
                    except queue.Empty: chunk = b": keepalive\n\n"
                    try: self.wfile.write(chunk); self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError): break
            finally:
                with subs_lock:
                    if q in subscribers: subscribers.remove(q)
            return
        # ... static routes for /index.html, /lcd/*.png, /phone/ ...
```

**Panel-side JS (3 lines):**

```html
<script>
new EventSource("/events").addEventListener("reload", () => {
  document.querySelectorAll("img.lcd").forEach(img => {
    img.src = img.dataset.src + "?t=" + Date.now();   // bust cache
  });
});
</script>
```

**Not-interfering with COM4:** the server binds `127.0.0.1:8765`, never opens a serial port, and the file watcher is read-only. Nothing here touches PlatformIO's monitor.

---

## Phone web app — extraction + WebSocket fake

**Extraction** is a one-line regex against `src/override_html.cpp`: the payload is bracketed by `R"OD(` and `)OD"`. Verified at `override_html.cpp:16` and `override_html.cpp:438`:

```python
# tools/extract_override_html.py
import re, pathlib
src = pathlib.Path("src/override_html.cpp").read_text(encoding="utf-8")
m = re.search(r'R"OD\((.*?)\)OD";', src, re.DOTALL)
if not m: raise SystemExit("could not find R\"OD(...)OD\" in override_html.cpp")
html = m.group(1)
# The page reads ACCESS_TOKEN from URLSearchParams -- inject one so the WS
# opens instead of stopping at driveStatus='AUTH REQUIRED' (override_html.cpp:264).
# The token doesn't have to match anything real -- our local fake WS ignores it.
pathlib.Path("build/lcd/phone.html").write_text(html, encoding="utf-8")
```

The panel iframe should point at `/phone/?k=preview` so the URL carries a query-string token and `openWs()` (`override_html.cpp:263-266`) does not early-return.

**WebSocket fake** — the phone HTML dials `ws://<host>/ws?k=<token>`. If we serve the HTML from `127.0.0.1:8765`, it dials `ws://127.0.0.1:8765/ws?k=...`. Python 3.12 stdlib does not include a WebSocket server, so pick one of two paths:

**Path 1 (preferred, zero deps): hand-implement RFC 6455 accept + frames.** The protocol needed is a strict subset: `Sec-WebSocket-Accept` from the SHA-1 of key + magic (stdlib `hashlib`, `base64`), read text/binary frames, write masked/unmasked binary frames. A minimal server is ~180 LOC and there are many public reference implementations. This is the honest cost of avoiding a dependency here.

**Path 2 (pragmatic, one dep): `websockets` from PyPI, ~single import.** This is the shortest path from zero to working. Recommend this if the developer will accept one PyPI package. If not, do Path 1.

**Behaviour the fake must exhibit** (all directly readable in `override_html.cpp:199-305`):

- Accept `GET /ws?k=<anything>`. Do not check the token in the local fake — the developer isn't testing auth, they're testing UI. (Add a `--reject-auth` flag if they ever want to preview the AUTH FAILED state — sends close code 1008, reason "Forbidden".)
- On connect, start a 5 Hz `asyncio.sleep(0.2)` loop that sends a 30-byte telemetry frame: `[0x12, 0, 0, dsp_hi, dsp_lo, pct, mood, chg, eligible, state, 20 padding bytes]`. Fill the padding with zeros. Vary `pct` slowly (100→0 over 60 s), toggle `chg`, cycle `mood` and `state` on `?scenario=` query — this is how the developer previews the `data-drive` CSS states listed in `override_html.cpp:212, 222-229`.
- Read incoming binary frames; if byte 0 is `0x01` and length is 4, log `L=<i8> R=<i8>` (interpreted at `override_html.cpp:298-300`). No response needed.
- Read `0x03` trim, `0x04` mood, `0x05` gesture, `0x06` sound — log them so the developer can watch what the mood/sound picker actually emits.

**Sketch (using `websockets`, ~40 lines):**

```python
# tools/fake_ws.py
import asyncio, itertools, argparse, websockets, struct

STATES = ["LOCKED", "WAIT NEUTRAL", "ARMED", "SAFE OFF"]  # UI decodes eligible+state (override_html.cpp:232)

def telemetry(pct, mood, chg, eligible, state, dsp_mv=8100):
    # 30-byte frame per override_html.cpp:272-288
    # Bytes: [0]=0x12  [1..2]=? (unused by UI)  [3..4]=dsp_mv u16  [5]=pct  [6]=mood
    #        [7]=chg   [8]=eligible             [9]=state          [10..29]=pad
    buf = bytearray(30)
    buf[0] = 0x12
    buf[3] = (dsp_mv >> 8) & 0xff
    buf[4] = dsp_mv & 0xff
    buf[5], buf[6], buf[7], buf[8], buf[9] = pct, mood, chg, eligible, state
    return bytes(buf)

async def session(ws, scenario):
    pct_cycle = itertools.cycle(list(range(100, -1, -2)))  # 100→0 over ~10 s at 5 Hz
    state_cycle = itertools.cycle([(0, 0), (1, 0), (1, 1), (1, 2)])  # LOCKED, WAIT NEUTRAL, ARMED
    async def rx():
        try:
            async for msg in ws:
                if isinstance(msg, (bytes, bytearray)) and len(msg) == 4 and msg[0] == 0x01:
                    l, r = struct.unpack_from("bb", msg, 1)
                    print(f"CTRL L={l:+4d} R={r:+4d} seq={msg[3]}")
                else:
                    print(f"OTHER opcode=0x{msg[0]:02x} len={len(msg)}")
        except websockets.ConnectionClosed: pass
    async def tx():
        while True:
            pct = next(pct_cycle)
            elig, state = next(state_cycle)
            await ws.send(telemetry(pct, mood=0, chg=1, eligible=elig, state=state))
            await asyncio.sleep(0.2)   # 5 Hz, matches real firmware cadence
    await asyncio.gather(rx(), tx())

async def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--port", type=int, default=8766)
    a = ap.parse_args()
    async with websockets.serve(lambda ws: session(ws, "default"), "127.0.0.1", a.port):
        print(f"fake_ws on ws://127.0.0.1:{a.port}/ws")
        await asyncio.Future()

if __name__ == "__main__":
    asyncio.run(main())
```

**Iframe wiring** — the phone HTML dials `location.host`, so serve the phone HTML on the *same* port as the fake WS (or use a tiny JS shim injected during extraction that overrides `WebSocket` to point at `:8766`; less clean because it changes what runs vs what ships). Cleaner is to run the SSE+static server and the fake WS on the same port via a merged asyncio server (using `aiohttp` costs one dep; sticking with stdlib means running them on adjacent ports and injecting `<base>` or a one-line `const WS_URL_OVERRIDE = "ws://127.0.0.1:8766/ws"` — patchable in one place in `override_html.cpp:266` via query-string).

**Recommendation:** inject one line into the extracted HTML during extraction:
```python
# in extract_override_html.py, after m.group(1):
html = html.replace(
    "ws=new WebSocket((location.protocol==='https:'?'wss://':'ws://')+location.host+'/ws?k=",
    "ws=new WebSocket('ws://127.0.0.1:8766/ws?k=",
    1,
)
```
That is a targeted, single-site rewrite (the exact line is `override_html.cpp:266`), and if the source line ever moves the rewrite fails loudly (assert the `replace` count is 1) — same "fail loud rather than silently wrong" discipline as `lcd_preview.py`.

---

## Unified panel — layout & routes

**Single page at `http://127.0.0.1:8765/`.** Grid layout, no framework:

```
+-----------------------------------------------------------------+
| Desktop Robot preview   [rebuild: 2ms ago]   [reload: manual]   |
+-----------------------------------------------------------------+
| LCD screens (240 x 280, x2 scale)                               |
| +--------+  +--------+  +--------+  +--------+                  |
| |  idle  |  | qr_wifi|  | qr_url |  | failed |                  |
| |  face  |  |        |  |        |  |        |                  |
| +--------+  +--------+  +--------+  +--------+                  |
|                                                                 |
| Sheet transition filmstrip (parsed commit = 85%)                |
| [wide horizontal image, 5 frames]                               |
+-----------------------------------------------------------------+
| Phone control UI (390 x 844 iframe, one-x scale)                |
| +----------------------+   Telemetry cycle:                     |
| |  <iframe src=/phone/>|   pct 100 -> 0 (10s)                   |
| |                      |   state LOCKED -> WAIT -> ARMED        |
| |                      |   CTRL log:                            |
| |                      |   L=+40 R=-12 seq=0x1a                 |
| +----------------------+                                        |
+-----------------------------------------------------------------+
| Overlap/overflow check output (from lcd_preview.py --check)     |
| qr_wifi: no overlaps                                            |
| qr_url:  no overlaps                                            |
+-----------------------------------------------------------------+
```

**Routes:**

| Path | Serves | Backed by |
|---|---|---|
| `/` | dashboard HTML | static file, `~150 lines` |
| `/lcd/qr_wifi.png` | rendered PNG | `lcd_preview.main()` output |
| `/lcd/qr_url.png` | rendered PNG | same |
| `/lcd/idle_face.png` | rendered PNG | (new) — `render_face()` reuse |
| `/lcd/pairing_failed.png` | rendered PNG | (new) — `drawPairingFailed()` model |
| `/lcd/sheet_filmstrip.png` | rendered PNG | existing filmstrip |
| `/phone/index.html` | extracted phone UI | `extract_override_html.py` output |
| `/events` | SSE stream | reload broadcaster |
| `/check` | JSON `{problems:[...]}` | `lcd_preview.check()` return |
| `ws://127.0.0.1:8766/ws` | fake WebSocket | `fake_ws.py` (separate process or same event loop) |

The dashboard is the panel URL the developer opens in Copilot's `browser` canvas.

---

## Concrete implementation plan

**File layout (new files only):**

```
tools/
  gfx_font.py            (~40 LOC)  glcdfont bytes + draw_char/draw_string
  lcd_preview.py         (edit)     swap system font for gfx_font; add face/failed screens
  preview_watch.py       (~30 LOC)  os.stat polling loop
  preview_server.py     (~200 LOC)  http.server + SSE + static file mux
  extract_override_html.py(~30 LOC) R"OD(...)OD" extractor + WS URL rewrite
  fake_ws.py             (~80 LOC)  telemetry stream + control-frame logger
  preview.html           (~150 LOC) dashboard shell + EventSource + iframe
```

**Commands** (Windows / PowerShell, PIO venv Python):

```powershell
$py = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"

# one-shot: extract the font table (only when Arduino_GFX pin changes)
& $py tools\extract_gfx_font.py    # writes tools/gfx_font.py

# start the preview
& $py tools\preview_server.py      # http on 127.0.0.1:8765, SSE watch
& $py tools\fake_ws.py             # ws on 127.0.0.1:8766, telemetry cycle

# open http://127.0.0.1:8765/ in the Copilot browser canvas
```

Wrap both in a tiny `tools/preview.ps1` that starts them as background jobs and prints the URL, so the developer types one command.

**Optional single-process variant:** run `fake_ws.py` inside `preview_server.py` on the *same* port using stdlib `socket` selectors + a hand-rolled WS accept — saves one port and one process. Costs ~120 LOC of RFC-6455 code. Worth it once the design is settled; not worth it for the first iteration.

---

## Specific risks & mitigations

1. **Regex fragility in `lcd_preview.py` extends to new screens.** Adding an `idle_face` PNG means either (i) hard-coding the mascot bitmap in the preview (drift risk — the bitmap is 47,520 uint16 in `src/mascot/mascot_a.h:24`) or (ii) parsing the auto-generated header. Mitigation: the mascot header carries a header comment identifying its source frame (`mascot_a.h:1-4`) — read the constants (`MASCOT_A_W`, `MASCOT_A_H`, `MASCOT_A_BG`, and the RGB565 array via `re.findall(r"0x[0-9A-Fa-f]{4}", body)`) into a numpy-less Python bytes buffer, then decode as RGB565 into a Pillow image. That is the same "read the constants, don't duplicate them" discipline as the existing tool.
2. **Font table license.** Adafruit-GFX-Library is BSD. Vendoring the byte table (a data-only fragment, no code) is fine, and `tools/LICENSING-NOTES.md` already tracks vendor licences — add a paragraph pointing at the extraction source and pin.
3. **`fake_ws.py` needs `websockets` (or 180 LOC of hand-rolled RFC 6455).** The user asked for "minimal new dependencies" and stdlib preferred. Two honest options; I would ship the stdlib version and note the trade in the file header.
4. **Port collisions.** 8765/8766 are arbitrary. Make them CLI-configurable and let the process fail loud if bound. Do not auto-detect COM ports of any kind — the constraint is explicit at the task level.
5. **Copilot browser canvas caching.** SSE reload with cache-busting `?t=Date.now()` on the `<img>` tags avoids stale PNGs. The iframe reload for the phone page needs an explicit `iframe.contentWindow.location.reload()` on the SSE event — the query-string trick doesn't work through the iframe boundary reliably.
6. **`main.cpp` line numbers drift.** All my citations are against the current tree. The regexes in `lcd_preview.py` are text-based and survive line-number drift; the `override_html.cpp` extractor is anchored on `R"OD(...)OD"`, not lines. Nothing in the design depends on specific line numbers, only on the parsed content.
7. **The `Adafruit_GFX::print` path also handles wrap and cursor advancement.** The firmware sets `setTextWrap(false)` before every text draw (`main.cpp:10212, 10236, 10662, 10681`), so wrap is not exercised. The current tool doesn't handle wrap and doesn't need to. Document this to prevent someone "helpfully" adding wrap later.
8. **The failed pairing screen isn't yet in `lcd_preview.py`.** I did not grep for a `drawPairingFailed()` function — my earlier grep only hit `drawQrScreenWifi/Url/GestureMenu/PairingStepDots/qrCenter*`. **Unverified: whether that screen is currently a distinct function or just an early-return branch inside `drawQrScreenWifi` (see `main.cpp:10527-10531` `"Wi-Fi unavailable"`)**. The user's request says such a screen exists — worth a follow-up grep for `failed|Failed|unavailable` before hard-coding a `/lcd/pairing_failed.png` route.

---

## Verified vs inferred — explicit split

| Claim | Status |
|---|---|
| Panel is 240×280, macros in `platformio.ini` | verified (`platformio.ini:66-68`) |
| `lcd_preview.py` parses constants from source and has `--check` | verified (`tools/lcd_preview.py:81-104, 243-254, 418`) |
| Font used is the Adafruit classic 5×7, 6-px advance, 8-px height at size 1 | verified (upstream `Arduino_GFX.cpp`; `Adafruit_GFX/glcdfont.c`; firmware metric at `main.cpp:10213`) |
| Arduino_GFX 1.3.9 uses byte-identical glcdfont as Adafruit-GFX-Library | verified via upstream fetch of both source trees |
| Phone HTML is a single `R"OD(...)OD"` literal | verified (`override_html.cpp:16, 438`) |
| Control frame layout, telemetry frame layout, 33 Hz cadence | verified (`override_html.cpp:199, 272-288, 298-300, 305`) |
| Native host builds already work in this repo (g++ + gtest-less) | verified (`test/run_motor_host_tests.ps1:19`) |
| `sheet_transition.cpp` is Arduino-free and host-testable | verified (`sheet_transition.h:5-11`; runner line 135-140) |
| `main.cpp` reaches raw framebuffer in ~15 places (Option (b) blocker) | verified (15 `getFramebuffer()` hits) |
| Existence and shape of a `drawPairingFailed()` distinct function | **inferred** from the task description — not verified in-repo |
| The Copilot canvas iframe correctly proxies SSE and WebSocket connections | **inferred**; the browser canvas is described as "preview live web pages or local dev servers", and both SSE and WS to `127.0.0.1` should work in a normal browsing context. Worth smoke-testing early. |
| The `websockets` PyPI package is acceptable | **not verified** — user explicitly said "minimal new dependencies" and "stdlib strongly preferred". Recommend hand-rolled RFC 6455 unless the developer accepts one dep. |

---

## Follow-up searches suggested for the main agent

- Grep for `drawPairingFailed|Wi-Fi unavailable|Access unavailable` in `src/main.cpp` to confirm the "pairing failed notice" is a distinct function vs an early-return branch (relevant to whether it needs its own preview route).
- Grep for `qrCenterTextDark|drawGestureMenu|drawFaceScreen` to see whether the current `lcd_preview.py` is missing any screens the developer expects to see in the panel.
- Check `docs/` for any existing preview roadmap the developer already sketched — the `HARDWARE.md`, `IMU-RESEARCH.md`, and `docs/PROJECT-HISTORY.md` (referenced from `platformio.ini`) may already contain design notes that constrain this.
- Confirm whether `test/test_ws_protocol_drift.py` already asserts the 30-byte telemetry layout in a machine-checkable way; if so, `fake_ws.py`'s frame constructor can `import` its constants instead of re-hard-coding the offsets (yet another drift-elimination move).
