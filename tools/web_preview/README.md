# Offline UI development

Two halves of the robot's interface, both runnable with **no robot on the desk**:

| Half | What it is | Port |
|------|------------|------|
| **LCD preview** | The robot's 240x280 panel, compiled to WebAssembly from the real firmware sources. | 8770 |
| **Phone web app** | The actual 27 KB web app the firmware serves, with a fake robot behind it. | 8080 |

Together they cover the whole pairing flow: pull the sheet down on the panel,
scan the QR it renders, and land on the working web app.

---

## LCD preview (WASM)

This replaces the "flash to device" step for panel work.

The old loop was: edit `src/screens.cpp` -> `pio run -t upload` (~30 s, needs
the robot) -> look at the panel. Now: edit `src/screens.cpp` ->
`tools\web_preview\build.ps1` (~7 s, no robot) -> refresh the browser.

It compiles **the same seven translation units the firmware compiles** --
`Arduino_G.cpp`, `Arduino_GFX.cpp`, `Arduino_Canvas.cpp`, `qrcode.c`,
`lib/EyeEngine/EyeEngine.cpp`, `src/screens.cpp`, `src/sheet_transition.cpp` --
against real `Arduino_Canvas(240, 280, nullptr)` instances. There is no
re-implementation anywhere, so the preview cannot drift from the firmware the
way the earlier `tools/lcd_preview.py` did (it kept rendering a `drawSheetCard`
that had already been deleted from the firmware).

Sibling of `tools/host_preview/` (native g++ harness that writes PPMs). Both
consume the same `src/screens.*` API and the same `EyeEngine`.

```powershell
powershell -File tools\web_preview\build.ps1      # emits build\web\
powershell -File tools\web_preview\serve.ps1      # http://localhost:8770/
powershell -File tools\web_preview\serve.ps1 -Lan # also reachable from a phone
```

`-Clean` forces a full rebuild. Timings on the reference machine (emsdk 6.0.6):
cold ~16 s, no-op ~1 s, `screens.cpp` edit ~7 s, `screens.h` edit ~9 s.

### What the browser shows

- The four static pairing screens, each rendered by the actual `Screens::`
  function.
- **Drag sheet (4)** -- pull the pairing card DOWN over the face. The mouse
  drives the real `SheetTransition::Controller`: `mousedown`/`mousemove`/
  `mouseup` map to `onPress`/`onMove`/`onRelease`, and the page composites the
  two canvases using the exact row rule `serviceDragTransition()` uses.
- **Dismiss QR (5)** -- push the card UP to reveal the face, mirroring
  `serviceDismissDrag()`. This mode also models the *layer staging*
  (`stageDismissLayers` / `restoreQrCanvas`), not just the physics, because two
  shipped bugs lived in that staging rather than in the controller. The
  **`stage`** counter in the readout is the diagnostic: the firmware guards it
  to exactly 1 per gesture, so anything higher means the card's content was
  replaced mid-swipe.
- **Live eyes.** `EyeEngine` is compiled in, so the mascot blinks and
  gaze-wanders exactly as it does on the device. The mascot bitmap has no eyes
  baked in -- they are drawn separately -- which is why a harness that renders
  only `drawFaceBackdrop` shows a blank-faced robot. That is not cosmetic: it is
  the same recipe that once shipped an eyeless face on the drag-to-dismiss
  reveal.

There are **no** runtime tunables, sliders, or writeback-to-source hooks. Layout
values live in `src/screens.cpp` and are edited there.

---

## Phone web app simulator

`robot_sim.py` serves the **real** web app and fakes the robot behind it.

It extracts the HTML at runtime by slicing between `R"OD(` and `)OD"` in
`src/override_html.cpp` -- it never keeps a copy, so it cannot drift from the
firmware either.

```powershell
powershell -File tools\web_preview\serve_app.ps1
# or directly:
python tools\web_preview\robot_sim.py --host 0.0.0.0 --port 8080
```

It implements the firmware's binary WebSocket protocol by hand (stdlib only, no
`pip install`):

| Direction | Frame |
|-----------|-------|
| client -> server | 4 bytes: `0x01`, int8 left, int8 right, uint8 seq |
| client -> server | 3 bytes: op, id, seq (trim / mood / gesture / sound) |
| server -> client | 30 bytes: `0x12`, then battery mV (uint16 BE), percent, mood, charging, drive status |

The 30-byte length is not negotiable -- the page checks
`v.byteLength!==30||v.getUint8(0)!==OP_TELEMETRY` and silently ignores anything
else.

### The control-rate readout is a diagnostic

```
ctrl 33/s  L= +42 R= -17  seq_gaps=0  total=1290
```

**33-34/s with `seq_gaps=0` is the healthy baseline** (the page sends on a 30 ms
`setInterval`). There is an unresolved bug where the joystick's control rate
collapses to exactly **1/s** on the real device; the page also drops frames
silently when `ws.bufferedAmount > 1024`. If the collapse reproduces here it is
the web app; if it only happens against the robot it is the firmware or the
radio.

---

## Making the QR codes actually work

A QR is only worth rendering if scanning it goes somewhere.

`robot_sim.py` publishes `GET /pair.json` (with CORS, since the two servers are
different origins):

```json
{ "ssid": "Hanger OVERRIDE", "password": "TheGarageAtl",
  "url": "http://192.168.137.1:8080/?k=deadbeefcafe1234",
  "port": 8080, "token": "deadbeefcafe1234" }
```

The LCD preview fetches it on load and pushes the values into the wasm module
via `wp_set_pairing()`, which re-encodes the QR. Without a simulator running the
preview falls back to the firmware's own `http://192.168.4.1/?k=...`, which only
exists on the robot's softAP.

Override the discovery URL with `?sim=<url>` on the preview page.

### Hotspot: making the Wi-Fi QR real too

The URL QR works over any shared network. The **Wi-Fi** QR needs a network
actually named `Hanger OVERRIDE` to exist -- so `hotspot.ps1` turns this PC into
one:

```powershell
powershell -File tools\web_preview\hotspot.ps1 -Action Status
powershell -File tools\web_preview\hotspot.ps1 -Action Start
powershell -File tools\web_preview\hotspot.ps1 -Action Stop
```

Then restart the simulator so the QR encodes an address the phone can reach:

```powershell
python tools\web_preview\robot_sim.py --host 0.0.0.0 --port 8080 --advertise-host 192.168.137.1
```

Now both QR codes behave exactly as they do on the device: scan the first to
join, scan the second to open the app.

Notes:
- It drives the WinRT `NetworkOperatorTetheringManager`. The legacy
  `netsh wlan set hostednetwork` path is dead on modern drivers -- an Intel AX200
  reports `Hosted network supported : No`.
- PowerShell 7 removed WinRT type projection, so the script **re-launches itself
  under Windows PowerShell 5.1**. Run it from either.
- Starting the hotspot **shares this machine's current internet connection** and
  is real system state: it survives the shell that started it and stays up until
  stopped or rebooted. `-Action Stop` when done.
- Windows puts the host at `192.168.137.1`, not the firmware's `192.168.4.1`.
  That difference is why `--advertise-host` exists.

---

## Requirements

- **emsdk** activated (tested with 6.0.6). The build script checks `$env:EMSDK`,
  then `$env:USERPROFILE\Desktop\Scripts\emsdk`.
- **PlatformIO**, and `pio run` executed at least once so
  `.pio\libdeps\esp32-s3-devkitc-1\` contains the `GFX Library for Arduino` and
  `QRCode` sources the build reads.
- Python 3 for the simulator and `serve.ps1`. **Stdlib only** -- no packages.

## Files

| File | Purpose |
|------|---------|
| `build.ps1` | Compile the seven firmware TUs + `wasm_main.cpp`. Incremental; `-Clean` for a full rebuild. |
| `serve.ps1` | Serve `build\web\` on 8770. `-Lan` binds all interfaces. |
| `wasm_main.cpp` | Emscripten entry point. Exports the `wp_*` C-linkage functions. |
| `index.html` | Browser side: canvas, screen buttons, mouse-to-controller, pairing discovery. |
| `robot_sim.py` | Serves the real web app + fake robot + `/pair.json`. |
| `serve_app.ps1` | Thin wrapper over `robot_sim.py`. |
| `hotspot.ps1` | Start/stop a Windows hotspot named after the firmware's AP. |
| `.wasm-build/` | Object-file cache. |

`.wasm-build/` and `build\web\` are gitignored.

## Known limitations

- The simulator fakes **telemetry only**. Anything depending on real servo, IMU,
  motor, battery or physical safety behaviour will not be truthful. Layout,
  styling, joystick feel and reconnect logic will be.
- The preview renders the mascot at a neutral head shift. The firmware nudges it
  a pixel or two toward the gaze (`g_lastShiftX/Y`), which lives in the demo/mood
  state machine in `main.cpp` rather than in `EyeEngine`.
- The native harness (`tools/host_preview`) deliberately runs with autoblink and
  idle wander **off**, so its PPMs are byte-stable for visual diffing. The web
  harness leaves them on.
