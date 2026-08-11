# Third-party licensing notes

This is a factual reference, not legal advice. It documents the licence of
each library the firmware links against so the compliance boundary is visible
*before* any distribution question comes up. Cross-referenced from a short
note in `platformio.ini` near the affected `lib_deps` entries.

## Why this file exists (short version)

The firmware is a single, monolithically-linked ESP32 image. The moment that
image is handed to someone outside this project — Kickstarter, product, gift,
sale, employer transfer — every library that ships inside it has a licence
that has to be honoured. Two of the libraries are LGPL-3.0, and LGPL has
non-trivial obligations for statically-linked binaries. This is currently a
**non-issue only because distribution is not happening** (the repo is private
and no device is being handed off). It becomes an issue on the first shipment
and by then it is too late to structure the release around it.

## Per-dep summary

| Library | Version | Licence | Ships LICENSE file in the PIO tarball? |
| --- | --- | --- | --- |
| `bblanchon/ArduinoJson` | 7.4.3 | MIT | Yes |
| `moononournation/GFX Library for Arduino` | 1.3.9 | BSD-like (per upstream README) | **No** — not shipped in the PIO tarball |
| `madhephaestus/ESP32Servo` | 1.2.1 | LGPL-2.1 (per upstream) | **No** — not shipped in the PIO tarball |
| `esphome/AsyncTCP-esphome` | 2.1.4 | **LGPL-3.0** | Yes |
| `esphome/ESPAsyncWebServer-esphome` | 3.4.0 | **LGPL-3.0** (declared in `library.json`) | **No** — LICENSE file is missing from the PIO tarball |
| `ricmoo/QRCode` | 0.0.1 | MIT | Yes |
| `mjdonders/CST816_TouchLib` | 2.2.0 | See upstream repo | Verify at ship time |

Versions above are the resolved versions on the machine that reviewed this,
and match the exact pins in `platformio.ini`. `pio pkg list -e esp32-s3-devkitc-1`
regenerates this table.

## The LGPL-3.0 obligation, concretely

`esphome/AsyncTCP-esphome` and `esphome/ESPAsyncWebServer-esphome` are the two
LGPL-3.0 libraries in the tree. Both are statically linked into a single
`firmware.bin` — there is no dynamic linker on ESP32 and no way to swap them
at runtime.

LGPL-3.0 §4 and §5, in plain language, require that when the combined work
is distributed, the recipient must be able to substitute a modified version
of the LGPL-covered library. For a statically-linked embedded binary this is
usually satisfied by one of:

1. **Ship source for the whole work under a compatible licence.** Simplest to
   comply with; largest strategic cost.
2. **Ship the LGPL-covered library's source, plus enough of our own build
   (object files, or a documented rebuild path with the exact toolchain
   versions and build commands) that the recipient could relink `firmware.bin`
   with a modified `AsyncTCP` / `ESPAsyncWebServer`.** This is the "corresponding
   source + relinkable form" path. It is the harder path in practice — a
   monolithically flashed image without a documented rebuild recipe does not
   qualify.
3. **Replace both libraries with permissively-licensed alternatives.** There
   is no drop-in replacement today. The upstream `me-no-dev` originals stopped
   shipping in 2022 and do not build cleanly on the Arduino-ESP32 v2.x line
   this firmware targets — see the note in `platformio.ini` and
   `docs/PROJECT-HISTORY.md` on why the esphome forks are used at all.

Additionally, `library.json` in `esphome/ESPAsyncWebServer-esphome` declares
`"license": "LGPL-3.0"` but the PlatformIO tarball ships no `LICENSE` file
next to the source — see the table above. Whichever compliance path is
chosen, this needs to be repaired at ship time (fetch the actual LICENSE
text from the fork's GitHub repo at the pinned tag) so the resulting image
is not distributed with an incomplete licence notice.

## What is safe today

Everything is safe today because:

- The repo is private (`docs/GIT-NOTES.md`).
- No firmware image is being distributed.
- No hardware is being handed to anyone outside the project.

Under those conditions the LGPL obligations do not activate — LGPL is
triggered by distribution, not by private development or internal use.

## What needs a decision *before* distribution

Before the first time a device is handed to anyone outside this project, one
of these three has to be chosen and executed:

- **Option A — Publish firmware source** under a compatible copyleft-friendly
  licence, with a `THIRD_PARTY_LICENSES.md` at the repo root that enumerates
  every `lib_deps` entry, its resolved version, its licence, and the upstream
  URL at the pinned tag/commit.
- **Option B — Ship a relinkable build.** Publish LICENSE text for both
  esphome forks alongside the shipped `firmware.bin`, ship this project's
  object files (or a fully reproducible build recipe: exact PlatformIO
  version, exact `pio pkg install` output hashes, exact `pio run` command,
  and the two esphome tarballs at these pinned versions), so a recipient can
  swap either library and relink.
- **Option C — Replace the forks** with permissive alternatives. There is no
  drop-in today; treat this as a research task on the order of the WebSocket
  concurrency work already logged in `docs/PROJECT-HISTORY.md`. Do not attempt
  this without repeating the "phone leaves WiFi mid-drive" bench case (D8)
  and the 4-client soak.

Whichever path is chosen, the decision needs to be *made* first, not
retrofitted after the first unit ships.

## Non-LGPL notes that are still worth flagging

- `GFX Library for Arduino` and `ESP32Servo` also do not ship a LICENSE file
  in their PlatformIO tarballs. Upstream is BSD-like and LGPL-2.1
  respectively, per their GitHub READMEs, but "trust the README" is not a
  compliance strategy. Whichever distribution path is chosen, pull LICENSE
  text from each upstream repo at the exact pinned version and bundle it.
- `ArduinoJson` and `QRCode` are MIT and already ship a LICENSE file in the
  tarball. These are fine as-is — attribution only.
