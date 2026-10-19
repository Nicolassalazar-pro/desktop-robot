# Third-party notices

Desktop Robot downloads its firmware dependencies through PlatformIO. They are
not relicensed by this repository.

| Dependency | Pinned version | License | Upstream |
|---|---:|---|---|
| ArduinoJson | 7.4.3 | MIT | <https://github.com/bblanchon/ArduinoJson> |
| GFX Library for Arduino | 1.3.9 | BSD-style upstream `license.txt` | <https://github.com/moononournation/Arduino_GFX/tree/v1.3.9> |
| ESP32Servo | 1.2.1 | LGPL-2.1-or-later | <https://github.com/madhephaestus/ESP32Servo> |
| AsyncTCP-esphome | 2.1.4 | LGPL-3.0 | <https://github.com/esphome/AsyncTCP> |
| ESPAsyncWebServer-esphome | 3.4.0 | LGPL-3.0 | <https://github.com/esphome/ESPAsyncWebServer> |
| QRCode | 0.0.1 | MIT | <https://github.com/ricmoo/qrcode> |
| CST816_TouchLib | 2.2.0 | Unlicense | <https://github.com/mjdonders/CST816_TouchLib> |

The exact dependency pins are defined in `platformio.ini`. Run the following
after installing the environment to inspect the resolved packages:

```powershell
pio pkg list -e esp32-s3-devkitc-1
```

## Firmware binary distribution

The ESP32 firmware statically links LGPL-covered libraries. Publishing this
source repository does not remove the obligations that apply when a compiled
firmware image or a device containing that image is distributed. A distributor
must preserve the relevant license notices and provide the relinkable/source
materials required by the applicable LGPL version.

The detailed engineering note is in
[`tools/LICENSING-NOTES.md`](tools/LICENSING-NOTES.md).

## Vendor documents and trademarks

Vendor PDFs and diagrams under `datasheets/` retain their original ownership
and terms. Microsoft Garage names and logo artwork are used with authorization;
no trademark or logo rights are granted by the project licenses.
