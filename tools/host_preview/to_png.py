"""Convert build/preview/*.ppm -> build/preview/*.png with Pillow.

Zero-dep alternative to writing PNG in C++ from the harness. Pillow ships with
PlatformIO's virtualenv at %USERPROFILE%/.platformio/penv/Scripts/python.exe.

Usage:
  & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" tools\host_preview\to_png.py

Kept intentionally boring -- if this ever grows a second responsibility it should
move into a dedicated tool and stop being an afterthought of the harness.
"""

from __future__ import annotations

import pathlib
import sys


def main() -> int:
    try:
        from PIL import Image
    except ImportError:
        print(
            "to_png.py: Pillow is not installed. Run with PlatformIO's python "
            "(%USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe) or "
            "`pip install pillow`.",
            file=sys.stderr,
        )
        return 1

    out_dir = pathlib.Path("build") / "preview"
    if not out_dir.exists():
        print(f"to_png.py: no {out_dir} yet -- run build.ps1 first.",
              file=sys.stderr)
        return 2

    converted = 0
    for ppm in sorted(out_dir.glob("*.ppm")):
        with Image.open(ppm) as im:
            png_path = ppm.with_suffix(".png")
            im.save(png_path)
            print(f"  wrote {png_path}")
            converted += 1
    print(f"to_png.py: converted {converted} image(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
