# HARDWARE.md — Waveshare ESP32-S3-Touch-LCD-1.69 (Desktop Robot)

> Pin map and gotchas for the **NEW** revision of this board. Verified against
> the official `waveshareteam/ESP32-S3-Touch-LCD-1.69` HARDWARE_REFERENCE.md,
> `pin_config.h`, and the ESP-IDF BSP header. If you ever read a comment that
> contradicts this file, *this file wins*.

**Product:** Waveshare ESP32-S3-Touch-LCD-1.69 (1.69″ rectangular IPS, 240×280)
**Amazon ASIN:** `B0D59P327V`
**Vendor wiki:** <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.69>
**Schematic V2.1:** <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.69/blob/main/hardware/schematics/ESP32-S3-Touch-LCD-1.69_V2.1.pdf>

---

## 1. SoC

| Item       | Value                                                   |
|------------|---------------------------------------------------------|
| MCU        | **ESP32-S3R8** (Xtensa LX7 dual-core)                   |
| Clock      | up to 240 MHz                                           |
| SRAM       | 512 KB internal                                         |
| **PSRAM**  | **8 MB octal (OPI)** — use `memory_type = qio_opi`      |
| **Flash**  | **16 MB W25Q128** (QIO)                                 |
| Wi-Fi      | 802.11 b/g/n, 2.4 GHz                                   |
| Bluetooth  | BLE 5.0                                                 |
| USB        | **Native USB-CDC** on GPIO19/20 — *no* UART bridge      |

> ⚠ **R8 vs R2:** This board is the **R8** (8 MB *octal* PSRAM) variant.
> Use `board_build.arduino.memory_type = qio_opi`. `qio_qspi` will crash at
> boot.

---

## 2. Pin map (NEW revision)

### ST7789V2 LCD (SPI, 240×280)

| Signal      | GPIO   | Notes                                                 |
|-------------|--------|-------------------------------------------------------|
| DC          | **4**  | data / command                                        |
| CS          | **5**  | active LOW                                            |
| SCK         | **6**  | **max 62.5 MHz** — see "SPI clock" below; NOT 80 MHz  |
| MOSI        | **7**  | write-only, no MISO                                   |
| RST         | **8**  | active LOW                                            |
| BL          | **15** | backlight, PWM (LEDC) recommended                     |

The ST7789V2 holds a 240×320 framebuffer; the physical panel is 240×280 mapped
onto rows 20–299. **You must set a 20-pixel row offset** in the driver, or
the top 20 px of any output will be garbage. `Arduino_ST7789` exposes this as
`row_offset1`. Colors are also inverted (`ips=true` flag in Arduino_GFX).

#### SPI clock: the cause of the intermittent black/glitched screen

The project shipped at **80 MHz**, inherited and never checked against the
panel. It is out of spec, and it is the root cause of the recurring "screen is
black after a reflash, fine after a power cycle" fault.

From `datasheets/ST7789V2_LCD.pdf` — **Table 6, "4-line serial Interface
Characteristics", p.44** (this is our wiring):

| Symbol | Parameter | MIN |
|--------|-----------|-----|
| `TSCYCW` | serial clock cycle (**write**) | **16 ns** → **62.5 MHz ceiling** |
| `TSHW` / `TSLW` | SCL high / low pulse width (write) | 7 ns each |
| `TSCYCR` | serial clock cycle (**read**) | 150 ns → 6.67 MHz |

At 80 MHz the cycle is 12.5 ns and each half-cycle 6.25 ns, so the cycle time
*and* both pulse widths are violated — an overclock of about **1.28×**.

That small margin is precisely why the fault is **intermittent** rather than
constant: the part tolerates it most of the time and fails only when
temperature, supply ripple and silicon lot line up badly. A corrupted byte in
the *init* sequence (`SLPOUT` 0x11, `DISPON` 0x29, `COLMOD` 0x3A) leaves the
panel asleep or misconfigured — and **there is no MISO on this board**, so
nothing can be read back. Firmware then reports `backlight=255 pwm=1 sysEn=1`,
every later pixel push "succeeds", and the screen stays dark. Every status the
firmware can report is true and irrelevant.

> **Do not repeat this mistake:** the commonly quoted "66 ns / 15 MHz" limit is
> `TWC` from *Table 4* (p.42) — the **8080 parallel** write cycle. It does not
> apply to a 4-line serial panel. Check the serial table.

Reset timing (Table 9, p.48) is *not* implicated: `TRW` ≥ 10 µs and `TRT`
≤ 120 ms, and `Arduino_ST7789::tftInit()` already waits 120 ms after reset and
120 ms after `SLPOUT`. The library's init sequence is correct.

**Current setting: 40 MHz** (25 ns, comfortably in spec). Waveshare's own BSP
for this board uses 24 MHz. Runtime-settable and persisted in NVS namespace
`lcdcfg` so it can be A/B tested without a reflash per trial:

```
lcdspi              show active value
lcdspi 24000000     store 24 MHz (reboot to apply)
```

The clock is fixed when the SPI bus object is constructed, so a change only
takes effect on the next boot. Boot logs the active value as `lcd: SPI ...`.

Two earlier workarounds for this fault have been **removed**, because both
treated the symptom and neither could fix a corrupted init — and while they
were in place there was no way to tell whether a real fix worked:

- a 1200 ms post-boot repaint (only re-pushed pixels), and
- a later version that re-ran the full wake animation, which blanked the
  backlight and flashed the logo on every boot — visible flicker, caused by
  the "fix".

Also note the canvas framebuffer is allocated with `ps_malloc`, so it lives in
**OPI PSRAM**; every push reads PSRAM and writes SPI. Not the primary cause,
but it adds bus contention and is worth remembering if timing is ever
marginal again.

### Shared I²C bus (touch + IMU + RTC)

| Signal | GPIO    | Notes                                |
|--------|---------|--------------------------------------|
| SDA    | **11**  | shared by all three I²C devices      |
| SCL    | **10**  | keep ≤400 kHz (CST816T limit)        |

| Device          | I²C addr | Interrupt GPIO        |
|-----------------|----------|-----------------------|
| CST816T touch   | `0x15`   | INT=**14**, RST=**13**|
| QMI8658C IMU    | `0x6B`   | INT2=**38** (INT1 NC) |
| PCF85063A RTC   | `0x51`   | INT=**39**            |

### Power, button, buzzer, ADC

| Signal       | GPIO  | Notes                                                  |
|--------------|-------|--------------------------------------------------------|
| `SYS_EN`     | **41**| **OUTPUT, drive HIGH in setup() or board powers off**  |
| `SYS_OUT`    | **40**| INPUT, PWR button (LOW when pressed)                   |
| `BUZZER`     | **42**| Passive piezo — PWM only; never hold the pin HIGH       |
| `BAT_ADC`    | **1** | 200 kΩ / 100 kΩ divider → multiply ADC reading by 3    |

### Expansion solder pads (PCB underside, SMD — not breadboardable as-is)

| Pad   | Signal                  |
|-------|-------------------------|
| 5V    | direct VBUS, no fuse    |
| 3V3   | LDO output (800 mA cap) |
| GND   |                         |
| GPIO2 | **free**                |
| GPIO3 | external servo buck-boost `EN` |
| GPIO17| physical RIGHT servo PWM       |
| GPIO18| physical LEFT servo PWM        |
| GPIO10| shared I²C SCL          |
| GPIO11| shared I²C SDA          |
| GPIO43| U0TXD (UART expansion)  |
| GPIO44| U0RXD (UART expansion)  |

**GPIO19/20** are the native USB D-/D+. **GPIO33–37** are wired internally to
the R8's octal PSRAM — do not use.

### Sensor inventory — what exists and what uses it

| Sensor | Where | Used by firmware? |
|---|---|---|
| CST816T capacitive touch | I²C `0x15` | **Yes** — menu button taps, swipe up/down |
| QMI8658C accelerometer (3-axis) | I²C `0x6B` | **Yes** — finds the yaw axis from gravity at rest |
| QMI8658C gyroscope (3-axis) | same chip | **Yes** — full-turn calibration |
| QMI8658C die temperature | regs `0x33/0x34` | No |
| PCF85063A RTC (coin-cell backed) | I²C `0x51` | **No** — never initialised |
| Battery voltage | GPIO1 ADC, 200 k/100 k ÷ (×3) | **Yes** — battery meter |
| Power button | GPIO40 `SYS_OUT`, active-LOW | **Yes** — 3 s hold powers off |
| ESP32-S3 internal temperature | on-chip | No |

**Not fitted on this board** — do not design around them:

- **No magnetometer.** The IMU is 6-axis. It has magnetometer registers
  (`CTRL4`, `0x41`+) but those address an *external* part that is not
  populated; the I²C bus holds only touch, IMU and RTC.
- No microphone, no camera, no ambient-light sensor, no proximity/ToF.

Radios: **Wi-Fi** is used for the QR/WebSocket joystick flow. **BLE 5.0** is
present in silicon and entirely unused.

Spare capacity worth knowing about: the **accelerometer is already initialised
and streaming at 500 Hz** but is only sampled once per calibration to locate
gravity — tilt, pickup, knocked-over and shake detection are all free from
hardware that is already running. The **RTC** survives power loss on its coin
cell, so it can give the robot a sense of time of day with no network.

### MG90D continuous-rotation drive

The physical LEFT and RIGHT MG90D drive servos use native **1000–2000 µs**
pulses at **50 Hz**. ESP32-S3 LEDC timer 0 is configured for **14-bit**
resolution, about **1.22 µs per tick** over the 20 ms period. Verified on the
assembled chassis 2026-08-03: **GPIO18 is the LEFT wheel and GPIO17 is the
RIGHT wheel.** These must stay matched to the physical wiring, otherwise
differential steering inverts.

Calibration values are requested microseconds, not exact measured waveform
widths. ESP32Servo floors requests to 14-bit timer ticks, then reconstructs
its readback from cached ticks. For example, a 1500 µs request reports a
nominal 1499 µs; adjacent requests can produce the same duty. Diagnostics
therefore call this a **nominal readback**, never an electrical measurement.

Calibration schema 3 stores five microsecond values per motor: `Stop`,
`ForwardStart`, `ReverseStart`, `ForwardMax`, and `ReverseMax`. `Stop` must
be 1450–1550 µs; starts must be on opposite sides of stop; each maximum must
remain on its start's side and be at least as far from stop. Old schema-2
records are erased and never translated, so explicit recalibration is
mandatory after upgrading.

`0 µs` is reserved for the detached/off state. Production output never sends
it to the servo library.

#### Power-button shutdown: mechanism, and what it is NOT

A 3-second hold on the side button runs `runPowerOffSequence()`, which fades
the backlight to 0 and drives `SYS_EN` LOW. On battery that ends power. **On
USB the board stays electrically alive and keeps enumerating as a COM port**,
so the signature is a black screen with a working serial link.

That mechanism is real, but it was **wrongly blamed** for a black screen seen
after reflashing on 2026-08-05. Measured with the `power` command immediately
after a flash:

```
power: sysEn=1 sysOut=1 armed=1 triggered=0 backlight=255 pwm=1 paused=0 shuttingDown=0
```

`triggered=0`, `sysEn=1` and `backlight=255` together rule out soft-off, a
dropped latch and a dark backlight. A full post-flash serial capture also shows
a completely normal boot with no `power:` shutdown lines. **A reflash does not
put this board into soft-off.**

Also measured: **`SYS_OUT` idles HIGH**, so GPIO40 is not floating despite
`pinMode(SYS_OUT, INPUT)` having no internal pull-up — an external pull-up
exists. `INPUT_PULLUP` is not required.

The button handler is nonetheless edge-triggered now (it must observe the line
HIGH once before any press counts, and ignores the first 1500 ms after boot),
and logs the measured hold duration whenever it fires. That closes a real
latent failure mode — a stuck-low pin would previously have powered the board
down silently — without changing the electrical configuration.

Use `power` to read the latch and backlight state over serial; it answers
"is the screen actually lit" and "is the battery latch actually closed"
without needing eyes on the robot.

#### TRAP — the main loop naps 50 ms in every non-FACE LCD mode

`loop()` returns early with `delay(50)` whenever `g_lcdMode != LCD_MODE_FACE`,
which is correct for holding a static QR or menu screen but caps the **entire
loop at 20 Hz**. Anything time-critical started from the gesture menu inherits
that: movement patterns are sequenced on a 10 ms grid, and the gyro sampler
gets one reading per 50 ms.

Measured 2026-08-05: **990 gyro samples** per probe with the loop free-running
versus **20** from the gesture menu. The on-device calibration failed with
"too few samples" while byte-identical code passed over serial.

The reason it looked like a tethered-vs-untethered problem is worth recording,
because it wasted two wrong diagnoses:

- **Opening the USB serial port resets the board** (DTR/RTS), and a reset lands
  in `LCD_MODE_FACE`. So every tethered test accidentally ran on the fast path,
  and every untethered test ran from the menu on the slow path. The variable
  that actually mattered was the LCD mode, not the cable.
- USB CDC blocking on a detached host is a real hazard and `Serial.setTxTimeoutMs(0)`
  is set defensively, but it was **not** the cause here.

The nap is now skipped while a gesture or a turn measurement is running
(`delay(1)` instead, to keep yielding for the task watchdog).

#### TRAP — a serial reconnect resets the board

Any tool that opens the port with DTR/RTS asserted (the pyserial default, and
`pio device monitor`) reboots the ESP32-S3. Menu state, LCD mode and staged
(unsaved) calibration values do not survive it. Saved NVS values do — which is
why the on-device calibration writes to NVS itself on success rather than
waiting for a `spin save confirm` that can only be typed over the cable that
just wiped the state.

**This also silently changes test conditions**, which cost a wrong diagnosis: a
reset lands in `LCD_MODE_FACE`, so every tethered test accidentally ran on the
fast loop path while every untethered test ran the throttled menu path.

Deassert both lines *before* opening to observe the board without disturbing
it:

```python
ser = serial.Serial()
ser.port, ser.baudrate = "COM4", 115200
ser.dtr = False
ser.rts = False
ser.open()          # no reset
```

Verified: connecting this way reports a running `uptime` instead of a fresh
boot.

#### TRAP — a 3 second power-button hold looks like a dead board on USB

Covered above under the power-button section, including the measurement showing
it was **not** the cause of the post-flash black screen.

#### Full-turn duration is a measured chassis property

How long full-speed counter-rotation takes to sweep the body through a
complete 360° depends on wheel diameter, track width, surface grip and the
servo calibration. It is **not** derivable on paper, and it is **not** two
half-turns.

This was originally wrong in the firmware: `kFullTurnMs` was written as
`kHalfTurnMs * 2`, where the half-turn was a bench value measured for the
*frustrated* gesture. Confirmed on hardware 2026-08-05 that the resulting
600 ms is not a full turn — it under-rotates. Both turns pay the same
standing-start acceleration, so doubling a half-turn systematically
under-rotates.

#### TRAP — rotation cannot be measured while USB-tethered

Measured 2026-08-05 with the gyro. The cable physically restrains the pivot:

| commanded | measured yaw |
|---|---|
| 600 ms | −83.6° |
| 600 ms | −50.1° |
| 2000 ms | −101.1° |
| 2000 ms | −54.6° |

Three separate tells that this is mechanical restraint and not a slow chassis
or a bad measurement:

1. **It does not scale.** 3.3× the drive time produced barely 2× the rotation.
   A merely slow chassis is roughly linear.
2. **Repeats vary by 2×** for an identical command, consistent with cable
   tension depending on how the lead happens to be lying.
3. **The measurement itself was sound** — gravity read `(1.02, 0.12, −0.09)` so
   the yaw axis is unambiguously X, and X rotated 10× more than Y or Z.

So the turn calibration is only meaningful **unplugged**, which is why it is on
the touch menu (4th button) and not just the serial CLI, and why it saves to
NVS itself on convergence rather than waiting for a `spin save confirm` that
can only be typed while tethered.

Measure it with the on-screen `calibrate` button, or with `spin auto` over
serial once the robot can actually pivot freely. Measure a **single continuous
turn from a standstill**, because that is what the gesture's opening turn does.

The measured value persists in its own `spincal` NVS namespace and is applied
at gesture-build time, so it takes effect without a reflash.

**Measured on this chassis 2026-08-05: a full turn is 1260 ms** (about
285 °/s at full wheel command), obtained from an untethered on-device
calibration run and saved to NVS. That makes `excited` 4140 ms end to end
(2 × 1260 for the bookend turns, plus 1620 ms of fixed middle and tail).

For reference, the same measurement while USB-tethered produced a fitted model
of `rate = 0.91 × speed − 24.2 dps`, predicting 5548 ms for a full turn — the
cable restraint made the chassis look four times slower than it is. Never
calibrate rotation with the cable attached.

### The IMU is 6-axis — there is no compass

The QMI8658C is an accelerometer + gyroscope. It has magnetometer registers
(`CTRL4`, `0x41`+) but those drive an **external** magnetometer part, and this
board does not fit one — the I²C bus holds only the touch controller (`0x15`),
this IMU (`0x6B`) and the RTC (`0x51`).

For measuring a turn that is fine, and arguably better: a magnetometer this
close to two DC motors, a LiPo under load and steel gearing would be reading
their fields as much as the Earth's. A gyro measures angular rate directly.

Verified register map (datasheet Rev 0.6, `datasheets/QMI8658C_IMU.pdf`):

| Register | Addr | Notes |
|---|---|---|
| `WHO_AM_I` | `0x00` | reads `0x05` |
| `CTRL1` | `0x02` | bit6 `SPI_AI` — **must** be set or burst reads return the same byte repeatedly |
| `CTRL2` | `0x03` | `aFS<6:4>`, `aODR<3:0>` |
| `CTRL3` | `0x04` | `gFS<6:4>`, `gODR<3:0>` — `111` = ±2048 dps, `0011` = 1000 Hz |
| `CTRL7` | `0x08` | bit1 `gEN`, bit0 `aEN` |
| `AX_L` | `0x35` | accel, 6 bytes, int16 little-endian |
| `GX_L` | `0x3B` | gyro, 6 bytes, int16 little-endian |

Gyro sensitivity is `32768 / FS_dps` LSB per °/s, so ±2048 dps gives 16 LSB
per °/s.

**Unexploited registers, with what they are actually worth.** Researched in
depth; full detail in `docs/research/research-qmi8658.md`.

| Register / command | What it does | Worth |
|---|---|---|
| `CTRL5` `0x06` | Low-pass filters. b0 `aLPF_EN`, b2:1 `aLPF_MODE`, b4 `gLPF_EN`, b6:5 `gLPF_MODE`. Modes = 2.66 / 3.63 / 5.39 / 13.37 % of ODR | 4.3× RMS noise reduction — **but ~38 ms group delay ⇒ 10.8° of late-stop at full speed** until the coast constant re-learns. Never enable blind |
| `CTRL9 = 0xA2` | **COD** on-demand calibration, then `0xAA` to apply | **Implemented** as `cod run`. Calibrates **scale, not bias** — the one error ZUPT cannot see. First run returned `status=0x00` (clean) with gains `(10301, 11266, 12316)`. **The 19.6% axis spread is unexplained** and larger than a ±3% scale tolerance implies, so the encoding may not be a plain multiplier |
| `COD_STATUS` `0x46` | 4 of 8 bits report gyro *sensitivity* limit failures | Proves COD is a sensitivity routine |
| `0x51`–`0x56` | `uint16_t gX/gY/gZ_gain` readback | **Unsigned** ⇒ multiplicative gain, not an offset |
| `0x33`/`0x34` | On-die temperature, 256 LSB/°C | Negligible given per-turn bias re-estimation |
| FIFO + INT2 | Watermark interrupt | 128–256 ms stall immunity. Insurance, not accuracy |

**Do not** lower gyro full-scale to ±512 dps (only 1.8× headroom ⇒ silent
clipping) and **do not** use the on-chip AttitudeEngine (coning error here is
already ≈0, and the fixed-point output format is unverified).

**Trap — the accelerometer's zero-g offset can be the same size as the tilt.**
The datasheet's Initial Offset Tolerance is **±100 mg**, so a small off-axis
gravity reading is ambiguous: "the board is tilted" and "the accelerometer is
offset" imply *opposite* corrections and look identical from a static sample.
Resolve it by magnitude and by the gyro's own component ratios during a turn.
On this board, measured `g = (0.97, 0.08, −0.27)`: **Z is 270 mg, 2.7× the
tolerance, so that tilt is real** (projection gain 1.041, worth 14.3° per 360°),
while **Y at 80 mg is inside the tolerance and is mostly sensor offset** —
confirmed by gyro ratios X:Z = 11.2:1 (matching the predicted 10.1:1) versus
X:Y = 50:1 (not matching). Prefer measuring the spin axis from the gyro, which
is immune to accel offset by construction.

**Trap — projecting onto gravity means every gyro axis bias reaches heading.**
`ω_yaw = ω·ĝ` is a weighted sum of all three axes, so all three biases arrive,
each scaled by its gravity component. Estimating bias on only the dominant axis
leaves the others leaking in: with ĝ_z ≈ −0.26 an uncorrected 0.2 dps Z bias
became 0.052 dps of apparent yaw, measured as **3.39° of drift in 65 s**,
accumulating. A single-axis integrator never read Y or Z, so this only appeared
when the projection was added — the projection was correct, its precondition was
not met. Estimate bias on **all three axes**.

**Trap — never sample gravity while turning.** At 285 °/s the centripetal term
creates a **4.3° false tilt**. Gravity is only meaningful at standstill.

**Trap — `degrees()` is an Arduino macro.** `<Arduino.h>` defines
`#define degrees(rad) ((rad)*RAD_TO_DEG)`, so any method named `degrees()`
silently macro-expands at its call site and the compiler reports the error
*inside Arduino.h*, not at your code. Host tests don't include `<Arduino.h>`,
so this breaks only the firmware build. `TurnMeasure::Integrator::totalDegrees()`
is named that way for exactly this reason.

Normal joystick updates do **not** toggle the boost rail. GPIO3 remains HIGH
while both already-configured channels receive their next paired duties.
Cold start/rebuild uses EN LOW, channel setup, paired writes, a 25 ms PWM
commit wait, hardware frequency/duty verification, then EN HIGH.

Production also runs a 10 ms `esp_timer` poll around a renewable 300 ms motor
lease. Expiry atomically latches an inhibit and drives GPIO3 LOW without
waiting for the Arduino loop or the drive mutex. The loop-task watchdog
resets the MCU after five seconds as secondary recovery.

`cal reset confirm` resets only the active draft and deliberately retains the
last verified NVS slot as an atomic fallback. That prior calibration reloads
after reboot. When replacing either motor, the safe sequence is:

1. Run `cal erase confirm`.
2. Verify `retained=0`, `retainedGen=0`, and drive state `LOCKED`.
3. Calibrate one physically connected motor at a time.
4. Set all ten fields, validate, and run `cal save confirm`.

The temporary calibrator has one shared GPIO3-controlled power rail. It can
select only one PWM signal, not one power branch, so unplug the unselected
motor during calibration.

#### Measured dead band (2026-08-03)

A bench MG90D showed **no shaft motion for any requested pulse in 1423–1533 µs
inclusive**. Both installed units come from the same batch, so the same band is
applied to each side. Everything else is derived from it:

| Field | left (GPIO18) | right (GPIO17) | Tick |
|---|---|---|---|
| `stop` | 1478 | 1478 | 1210 |
| `fwdstart` | 1534 | 1422 | 1256 / 1164 |
| `revstart` | 1422 | 1534 | 1164 / 1256 |
| `fwdmax` | 2000 | 1000 | 1638 / 819 |
| `revmax` | 1000 | 2000 | 819 / 1638 |

`stop` is the band midpoint, leaving 55 µs of margin to each edge so a
batch-mate with a slightly wider band still holds still. The two sides carry
mirrored polarity because the servos are mounted facing opposite directions.

Comparisons must happen in **tick space, not microseconds**. Requests are
floored to 14-bit ticks, so several distinct requests share one shaft response —
1532 and 1533 µs are both tick 1255. The band spans ticks 1165–1255; a start
value is only usable if its tick falls strictly outside that range, and `stop`
is only safe if its tick falls inside it.

`tools/apply_calibration.py` derives and stages all ten values, refusing any
combination that fails the firmware validator or collapses back into the band:

```
robot-cal --dry-run                      # print the plan, touch no port
robot-cal                                # stage all ten via cal set
robot-cal --left-forward low             # flip one side's polarity
robot-cal --deadzone-low 1420 --deadzone-high 1540
```

It never runs `cal save confirm`; persisting the record stays a manual step.

#### Identifying which motor is on which pin

Done for the current build on 2026-08-03; repeat after any rewiring or motor
swap. With the calibrator firmware flashed, `pin17 1560` drives only the GPIO17
motor and `pin18 1560` only the GPIO18 motor, so one pulse per pin resolves
both the side mapping and the polarity. 1560 µs is roughly 26 µs past the dead
band, which is a slow crawl that is easy to read; each pulse self-stops after
the 5 s lease.

1. `pin17 1560` — note which wheel turns, and which way it drives the chassis.
   Judge left/right from the **robot's** point of view, not the observer's.
2. Wait for the lease to expire, then `pin18 1560` and repeat.
3. If the wheel that moved on GPIO17 is the physical LEFT wheel, set
   `kServoLeftPin = 17` / `kServoRightPin = 18` in `src/main.cpp`; otherwise
   the current 18/17 assignment is correct.
4. If a wheel drives the chassis backwards at 1560 µs, that side's forward is
   the low pulse: pass `--left-forward low` or `--right-forward low`.

Result for the assembled chassis: GPIO17 = RIGHT wheel driving backwards at
1560 µs, GPIO18 = LEFT wheel driving forwards. Pin constants were swapped to
18/17, after which the default polarity (left forward high, right forward low)
is correct, so `robot-cal` needs no flags.

### Reset-time motor safety

Firmware drives GPIO3, GPIO17, and GPIO18 LOW at the first opportunity in
`setup()`, but cannot control them during reset, ROM boot, or the bootloader.
Hardware must make the servo rail default OFF:

- Fit approximately **10 kΩ from GPIO3 / boost EN to GND**.
- Inspect and remove any EN-to-VIN or default-enable solder bridge on the
  buck-boost module.
- Optional 10 kΩ pulldowns on GPIO17 and GPIO18 keep signal inputs quiet.

Do not connect the motors for flashing/calibration until the EN pulldown and
module default-enable state have been checked.

---

## 3. Power architecture

```
USB-C 5V ─┬─ ETA6098N3A LiPo charger ─→ MX1.25 battery jack
          │
          └─ power-path / SYS_EN MOSFET (gated by GPIO41)
                   │
                   ↓
              ME6217C33M5G LDO (800 mA, 300 mV dropout)
                   │
                   ↓
              3V3 rail (ESP32-S3, LCD, touch, IMU, RTC, buzzer)
```

### Critical SYS_EN behaviour

`pinMode(41, OUTPUT); digitalWrite(41, HIGH);` **must run within the first
~second of `setup()`** when on battery — otherwise the latch MOSFET opens
when the user releases the PWR button and the board powers off. Harmless
when on USB.

### Power-button (GPIO40) handling pattern (from Waveshare demo 09)

```cpp
if (digitalRead(SYS_OUT) == LOW) {           // button pressed
  if (longPress > 2 s) {
    saveState();
    digitalWrite(SYS_EN, LOW);               // cut battery → board powers off
  }
}
```

Production uses a **3-second hold**. Before releasing `SYS_EN`, firmware
shuts down the drive rail and networking, darkens the current display behind
the foreground white Garage icon while that icon fades in, then fades the
backlight off. On battery the latch opens and power ends. USB bypasses the
latch, so firmware waits in a soft-off state for button release followed by
one new press, then restarts.

Backlight fading uses LEDC channel 4 / timer 2. Servo channels 0/1 use timer
0 and piezo channel 2 uses timer 1, so the three PWM roles are isolated.

### Power numbers

| Mode                                | Approx draw         |
|-------------------------------------|---------------------|
| Deep sleep, LCD off                 | 5–20 µA             |
| Active, Wi-Fi off, LCD on           | 50–80 mA            |
| Active, Wi-Fi on, LCD on            | 250–355 mA peak     |
| + charging battery                  | + 300–500 mA on VBUS|

### 5 V pad current rating

- **Direct VBUS, no fuse.** No PD negotiation.
- From a standard USB-A port: USB 2.0 spec = **500 mA** total.
- Board itself eats ~300 mA worst case → **~150–200 mA headroom** for
  external accessories on the 5V pad.
- From a dedicated 2 A USB-C power adapter: limited by **trace width**
  (~800 mA safe ceiling).
- Driving both Wi-Fi + display + charging a depleted battery from a 500 mA
  USB port WILL sag. Use a 2 A adapter for that.

### ETA6098 charge rate

- Chip: **ETA6098N3A**, single-cell linear charger.
- Termination voltage: 4.2 V ± 1 % (fixed).
- Max ICHG: 500 mA. Programmed via `RISET` resistor: `ICHG (mA) = 1000 / RISET (kΩ)`.
- The exact RISET on this PCB requires opening the schematic PDF — likely
  **2 kΩ (500 mA)** or **3.3 kΩ (≈300 mA)** based on Waveshare's other boards.
- **No STAT pin** broken out to a GPIO and **no charge LED**. Infer charge
  state from the GPIO1 battery-voltage trend.

---

## 4. Onboard passive piezo (GPIO42)

The firmware drives GPIO42 directly with dedicated **LEDC channel 2**. On
Arduino-ESP32 2.x that channel uses **LEDC timer 1**; the servo library
pre-allocates **timer 0** at 50 Hz/14-bit for the MG90D outputs, so changing
piezo pitch cannot retune servo PWM.
There is no amplifier, DAC, volume control, or polyphony: output is one
fixed-duty square-wave tone at a time.

The serial command is exactly `tone <hz>` with one integer argument. Valid
values are `0` or **200..6000 Hz**. Active commands are leases: the host
MIDI bridge refreshes the current frequency every **250 ms**, and the
firmware watchdog stops output after **750 ms** without a refresh.

`tone 0`, watchdog expiry, sleep, and other firmware stop paths are
synchronous: duty and tone generation are cleared, channel 2 is detached,
and GPIO42 is configured as an output and driven **LOW**.

### Safety and practical limits

This is a passive piezo, not a general-purpose speaker. Never hold GPIO42
HIGH or feed it DC; the board documentation warns that doing so draws
continuous current and can heat the LDO. Keep it PWM-driven, leave it LOW
between sounds, and do not hold the 2–6 kHz element close to an ear.

The accepted firmware range is **200–6000 Hz**, but acceptance does not mean
equal loudness. Empirical behavior on this board:

| Range          | Behaviour                                          |
| -------------- | -------------------------------------------------- |
| < 200 Hz       | rejected by firmware; effectively inaudible         |
| 200 – 500 Hz   | very weak, mostly clicky                           |
| 500 – 800 Hz   | audible but thin                                   |
| 800 – 2 kHz    | solid, clearly tonal                               |
| **2 – 4 kHz**  | **loudest — element's mechanical resonance**       |
| 4 – 6 kHz      | sharp and harsh                                    |
| > 6 kHz        | rejected by firmware                               |

Attack is roughly 10–20 ms, so notes shorter than about 40 ms tend to click
rather than sound tonal. The live MIDI bridge therefore enforces a 45 ms
minimum final-note gate and folds every MIDI pitch class into **A6–G♯7
(1760–3322 Hz)**.

---

## 4b. CST816T touchscreen (I2C 0x15) — orientation

**The touch panel reports 90° rotated from the display.** The graphics are
correct; the touch coordinates are not. Any on-screen hit-testing must
transform them:

```
screenY = rawX
screenX = kScreenW - rawY      // 240 - rawY
```

This is not a guess. It follows from the swipe IDs that were confirmed
on-device: a screen swipe **down** arrives as the chip's `GESTURE_RIGHT`
(id=1), so increasing screen-Y is increasing raw-X, and the two axes are
swapped. `kTouchSwipeDownId`/`kTouchSwipeUpId` in `main.cpp` encode the same
fact for gestures.

The **X mirror** is the one part the swipe evidence does not pin down. The
gesture-menu buttons are full-width rows specifically so that only Y selects
between them, which makes the ambiguity harmless — `gesture_menu_test.cpp`
asserts that property rather than leaving it to chance. Verify the mirror
before building any UI that needs precise X.

### Traps found the hard way

- **Do NOT call `setAutoSleep(false)`.** It looks like the right fix for "the
  chip went quiet", but on this board the interrupt pin drives event reporting
  and disabling auto-sleep killed *all* touch and gesture events — while I2C
  identity reads (`getDeviceType` → 0xB5, `getFirmwareVersion` → 1) kept
  succeeding, so the chip looked perfectly healthy while reporting nothing.
- **A silent serial capture proves nothing.** Diagnosing this over a timed
  serial window repeatedly produced "0 events" that meant *nobody was tapping*,
  not *the panel is broken*. The fix was to close the feedback loop on the
  device: `touchdebug on` beeps the piezo on every reported touch, and
  `touchlog` keeps a 24-entry ring buffer of recent touches that can be read
  back at any time. Use those instead of a synchronised capture.

Serial helpers: `touchinfo` (chip identity + event counters), `touchlog`
[`clear`], `touchreset`, `touchdebug <on|off>`.

---

**Trap — COD gains are volatile.** The chip forgets them on every power cycle,
so a measured calibration must be persisted by the host and pushed back with
`CTRL9 = 0xAA` at boot. Without that the device silently reverts to the factory
scale, and a scale error is invisible to every other check available — ZUPT
cannot see it, and the resting gravity magnitude only validates the
accelerometer. Verified: `imu: cod gains (10301,11266,12316) re-applied=1`.

**Trap — COD invalidates gyro-derived constants.** Anything measured *through*
the gyro moves with the scale. For `alpha = w^2/(2*coast)`, both `w` and `coast`
scale by `k`, so `alpha -> k*alpha`; the calibrated full-turn duration shifts
too. Re-run the bench after any `cod run`.

**Trap — a blocking WiFi scan reboots the board.** `WiFi.scanNetworks()` in
synchronous mode parks the loop task for several seconds, which starves the task
watchdog: `E (62620) task_wdt: Aborting` followed by
`rst:0xc (RTC_SW_CPU_RST)`. Scan asynchronously and collect the result from
`loop()`. `rfscan` does this.

**Trap — most measurements are invalid unless the robot is untethered, on the
desk, on its own wheels.** Three independent reasons, each sufficient: the USB
cable restrains rotation; the stand takes the wheels out of the friction path
and roughly doubles the board tilt (~8° desk vs ~16.5° stand, which changes the
gravity projection from ~3.5°/360° to ~14.3°/360°); and reconnecting serial
resets the board, destroying anything in RAM. A measurement taken tethered or on
the stand is not a slightly worse measurement, it is a different quantity. Use
the `bench` run (see README) rather than ad-hoc turns.

## 5. Programming & flashing

- USB-C → **ESP32-S3 native USB CDC**. No CH343, no CP210x.
- PlatformIO build flags **must** include:
  ```
  -D ARDUINO_USB_MODE=1
  -D ARDUINO_USB_CDC_ON_BOOT=1
  ```
  After this, `Serial.println(...)` shows up in the native USB serial.
- **First flash of a new board** (or after a sketch that disabled USB):
  hold **BOOT**, tap **RESET**, release **BOOT**, then upload. Once a
  working USB-CDC sketch is on the board, esptool auto-resets.
- Recommended baud: upload 921600. The monitor baud is irrelevant for
  native USB (it's a fake number) — leave it at 115200.

### PlatformIO config crib-sheet

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32@7.0.1
board    = esp32-s3-devkitc-1
framework = arduino

board_build.mcu        = esp32s3
board_build.flash_mode = qio
board_build.flash_size = 16MB
board_upload.flash_size = 16MB
board_upload.maximum_size = 16777216
board_build.arduino.memory_type = qio_opi    ; R8: 8 MB OCTAL PSRAM

build_flags =
    -D BOARD_HAS_PSRAM
    -D ARDUINO_USB_MODE=1
    -D ARDUINO_USB_CDC_ON_BOOT=1
```

---

## 6. "Is it alive?" checklist

1. `platformio.ini` has `board = esp32-s3-devkitc-1`, `memory_type = qio_opi`, flash 16 MB.
2. `ARDUINO_USB_MODE=1` *and* `ARDUINO_USB_CDC_ON_BOOT=1` are in `build_flags`.
3. `setup()` calls `pinMode(SYS_EN, OUTPUT); digitalWrite(SYS_EN, HIGH);` **first**.
4. Backlight on **GPIO15** driven HIGH before talking to the display.
5. Display init uses **ST7789V2**, 240×280, IPS=true, **row_offset1 = 20**,
   pins **DC=4, CS=5, SCK=6, MOSI=7, RST=8**.
6. I²C uses **SDA=11, SCL=10, ≤400 kHz**. QMI8658 WHO_AM_I = `0x05`, CST816T
   addr = `0x15`, PCF85063 addr = `0x51`.
7. Battery ADC on **GPIO1**, multiply by 3.
8. Buzzer on **GPIO42**: LEDC channel 2/timer 1, isolated from servo timer 0;
   PWM only, and every stop path must synchronously drive the pin LOW.
9. MG90D drive outputs use GPIO17/18, 50 Hz, 14-bit (~1.22 µs ticks), and
   attach/write only within 1000–2000 µs.
10. `Arduino_GFX` pinned `~1.3.7` (1.4.x requires ESP32-Arduino v3.x core which
   PlatformIO's `espressif32` platform doesn't ship yet).

---

## 7. Differences from DeskNotif (LCD-1.28 round)

| Feature        | LCD-1.28 round       | LCD-1.69 (this board)              |
|----------------|----------------------|------------------------------------|
| Display driver | GC9A01A              | **ST7789V2** (+20 px row offset)   |
| Resolution     | 240×240              | **240×280**                        |
| LCD pins       | DC=8/CS=9/SCK=10/MOSI=11/RST=12/BL=40 | **DC=4/CS=5/SCK=6/MOSI=7/RST=8/BL=15** |
| I²C SDA/SCL    | 6 / 7                | **11 / 10**                        |
| Touch INT pin  | 5                    | **14**                             |
| IMU INT        | INT1=47, INT2=48     | **INT2=38 only**                   |
| RTC            | —                    | **PCF85063 on GPIO39 INT**         |
| Buzzer         | —                    | **GPIO42 passive piezo**           |
| Power latch    | —                    | **GPIO41 SYS_EN MUST be HIGH**     |
| USB            | CH343P UART bridge   | **ESP32-S3 native USB CDC**        |
| PSRAM          | 2 MB QSPI (R2)       | **8 MB OPI (R8)**                  |

---

## 8. Key URLs

- Wiki: <https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-1.69>
- Official GitHub: <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.69>
- Hardware reference: <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.69/blob/main/HARDWARE_REFERENCE.md>
- `pin_config.h`: <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.69/blob/main/examples/Arduino/libraries/Mylibrary/pin_config.h>
- ESP-IDF BSP header: <https://github.com/waveshareteam/Waveshare-ESP32-components/blob/master/bsp/esp32_s3_touch_lcd_1_69/include/bsp/esp32_s3_touch_lcd_1_69.h>
- Schematic V2.1: <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.69/blob/main/hardware/schematics/ESP32-S3-Touch-LCD-1.69_V2.1.pdf>
- Keys_Bee demo (power-button + buzzer reference): <https://github.com/waveshareteam/ESP32-S3-Touch-LCD-1.69/blob/main/examples/Arduino/examples/09_LVGL_Keys_Bee/09_LVGL_Keys_Bee.ino>
- ETA6098 datasheet: <https://files.waveshare.com/wiki/common/ETA6098.pdf>
