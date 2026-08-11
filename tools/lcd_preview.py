"""Render the robot's LCD screens on the host, without flashing.

Every LCD layout bug so far has cost a full build-flash-look cycle, and one of
them (the step dots overlapping the password row by 2 px) was caught only
because the vertical budget happened to get checked by hand. This renders the
same screens to PNG so layout can be reviewed, and overlaps detected
automatically, before anything touches the panel.

It is a LAYOUT preview, not a pixel-exact emulator. That distinction matters:

  exact    screen size, element Y positions, text metrics, QR module count and
           scale, and therefore every overlap and every overflow -- which is
           the entire class of bug this exists to catch
  close    the GFX 5x7 glyph shapes are approximated by a system font
  ignored  colour depth (RGB565 banding) and SPI timing

Positions are PARSED FROM src/main.cpp rather than duplicated here, so the
preview cannot silently drift away from the firmware. If a y= value moves in the
C++, this follows it. If parsing fails it says so loudly instead of quietly
rendering a lie.

Usage:
    python tools/lcd_preview.py                  # render all screens
    python tools/lcd_preview.py --out build/lcd  # choose output directory
    python tools/lcd_preview.py --check          # overlap check only, no PNGs

Exit code is non-zero if any screen has an overlap or overflows the panel, so
this can gate a build.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit(
        "Pillow is required: install from tools/requirements.txt using the "
        "PlatformIO venv python (see that file's header)."
    )

REPO = pathlib.Path(__file__).resolve().parent.parent
MAIN = REPO / "src" / "main.cpp"

# Adafruit_GFX built-in font: every glyph advances 6 px and is 8 px tall at
# size 1, scaling linearly. The firmware relies on exactly this in
# qrCenterText (`strlen(s) * 6 * textSize`), so reproducing it here is not an
# approximation -- it is the same arithmetic.
GFX_ADVANCE = 6
GFX_HEIGHT = 8

WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
DIM = (33, 33, 33)
GREY = (132, 132, 132)


def parse_constant(name: str, source: str) -> int:
    """Pull `constexpr ... name = <int>;` out of the firmware.

    Falls through to platformio.ini when the constant is defined from a build
    flag (kScreenW = LCD_WIDTH), so the preview still tracks one source of
    truth instead of hardcoding a copy.
    """
    m = re.search(rf"constexpr\s+\w+\s+{re.escape(name)}\s*=\s*(\d+)", source)
    if m:
        return int(m.group(1))
    m = re.search(rf"constexpr\s+\w+\s+{re.escape(name)}\s*=\s*([A-Z_][A-Z0-9_]*)", source)
    if m:
        macro = m.group(1)
        ini = (REPO / "platformio.ini").read_text(encoding="utf-8", errors="replace")
        mm = re.search(rf"-D\s+{re.escape(macro)}\s*=\s*(\d+)", ini)
        if mm:
            return int(mm.group(1))
        raise SystemExit(
            f"{name} is defined as macro {macro}, which is not a -D flag in "
            f"platformio.ini"
        )
    raise SystemExit(f"could not find constant {name} in src/main.cpp")


def parse_function_body(name: str, source: str) -> str:
    """Grab a function body by brace matching from its opening brace."""
    m = re.search(rf"(?:static\s+)?void\s+{re.escape(name)}\s*\([^)]*\)\s*\{{", source)
    if not m:
        raise SystemExit(f"could not find function {name} in src/main.cpp")
    i = m.end() - 1
    depth = 0
    for j in range(i, len(source)):
        if source[j] == "{":
            depth += 1
        elif source[j] == "}":
            depth -= 1
            if depth == 0:
                return source[i : j + 1]
    raise SystemExit(f"unbalanced braces reading {name}")


class Element:
    def __init__(self, kind: str, y: int, height: int, label: str):
        self.kind = kind
        self.y = y
        self.height = height
        self.label = label

    @property
    def bottom(self) -> int:
        return self.y + self.height

    def __repr__(self) -> str:
        return f"{self.label} y={self.y}..{self.bottom}"


def text_elements(body: str) -> list[Element]:
    """Every qrCenterText / qrCenterTextFit call, with its true pixel height.

    Elements drawn BEFORE the QR is placed are on an early-return error path
    ("Wi-Fi unavailable" when credentials are missing) and never coexist with
    the normal layout. Splitting on the QR draw is not a heuristic about
    wording -- it mirrors the actual control flow, where the credential check
    returns before drawQrToCanvas is ever reached. Without this the checker
    reports overlaps between two screens that can never be on the panel at the
    same time, which is exactly the kind of false alarm that gets a tool
    ignored.
    """
    split = body.find("drawQrToCanvas")
    if split < 0:
        split = len(body)
    out: list[Element] = []
    for m in re.finditer(
        r"qrCenterText\(\s*/\*y=\*/\s*(\d+)\s*,\s*/\*size=\*/\s*(\d+)\s*,\s*\"([^\"]*)\"",
        body,
    ):
        if m.start() < split:
            continue  # error path
        y, size, txt = int(m.group(1)), int(m.group(2)), m.group(3)
        out.append(Element("text", y, GFX_HEIGHT * size, f'"{txt}" (size {size})'))
    for m in re.finditer(r"qrCenterTextFit\(\s*/\*y=\*/\s*(\d+)\s*,\s*(\w+)", body):
        if m.start() < split:
            continue
        y, var = int(m.group(1)), m.group(2)
        # Fit helper starts at size 2 and falls back to 1; assume the worst
        # case, because that is the case that overlaps.
        out.append(Element("textfit", y, GFX_HEIGHT * 2, f"{var} (fit, size 2)"))
    return out


def dot_elements(body: str) -> list[Element]:
    out: list[Element] = []
    for m in re.finditer(
        r"drawPairingStepDots\(\s*/\*y=\*/\s*(\d+)\s*,\s*/\*step=\*/\s*(\d+)", body
    ):
        y, step = int(m.group(1)), int(m.group(2))
        # Radius 4 in the firmware; a circle spans centre-r .. centre+r.
        out.append(Element("dots", y - 4, 9, f"step dots ({step}/2)"))
    return out


def qr_element(body: str, source: str, which: str) -> Element:
    """The QR block: modules * scale, positioned at the literal y in the code."""
    version = parse_constant(f"kQr{which}Version", source)
    scale = parse_constant(f"kQr{which}PixelScale", source)
    modules = version * 4 + 17
    px = modules * scale
    m = re.search(r"const\s+int16_t\s+y\s*=\s*(\d+)\s*;", body)
    if not m:
        raise SystemExit(f"could not find QR origin y for {which}")
    y = int(m.group(1))
    return Element("qr", y, px, f"{which} QR v{version} ({modules}x{modules} @ {scale})")


def draw_text_centred(d: ImageDraw.ImageDraw, w: int, el: Element, text: str, size: int):
    """Centre using the FIRMWARE's metric, not the render font's."""
    advance = GFX_ADVANCE * size
    x = (w - len(text) * advance) // 2
    try:
        font = ImageFont.truetype("consola.ttf", GFX_HEIGHT * size)
    except OSError:
        font = ImageFont.load_default()
    d.text((x, el.y), text, fill=BLACK, font=font)


def render(name: str, w: int, h: int, elements: list[Element], shift: int = 0,
           armed: bool = False) -> Image.Image:
    img = Image.new("RGB", (w, h), WHITE)
    d = ImageDraw.Draw(img)
    for el in elements:
        y = el.y + shift
        if el.kind == "qr":
            d.rectangle([(w - el.height) // 2, y, (w + el.height) // 2, y + el.height],
                        outline=BLACK, width=2)
            d.text(((w - el.height) // 2 + 8, y + el.height // 2), el.label, fill=GREY)
        elif el.kind == "dots":
            cx, r = w // 2, 4
            for i in range(2):
                x = cx - 9 + i * 18
                filled = "1/2" in el.label and i == 0 or "2/2" in el.label
                box = [x - r, y, x + r, y + 2 * r]
                d.ellipse(box, fill=BLACK if filled else WHITE, outline=BLACK)
        else:
            label = el.label.split('"')[1] if '"' in el.label else el.label.split(" (")[0]
            size = 2 if "size 2" in el.label else 1
            draw_text_centred(d, w, Element(el.kind, y, el.height, ""), label, size)
    if shift:
        band_h = abs(shift)
        band_y = 0 if shift > 0 else h - band_h
        d.rectangle([0, band_y, w, band_y + band_h], fill=BLACK if armed else DIM)
        if band_h >= 22:
            msg = "RELEASE" if armed else "keep going"
            size = 2 if armed else 1
            x = (w - len(msg) * GFX_ADVANCE * size) // 2
            d.text((x, band_y + band_h // 2 - 4 * size), msg,
                   fill=WHITE if armed else GREY)
    d.rectangle([0, 0, w - 1, h - 1], outline=(255, 0, 0))
    return img


def check(name: str, elements: list[Element], h: int) -> list[str]:
    problems = []
    ordered = sorted(elements, key=lambda e: e.y)
    for a, b in zip(ordered, ordered[1:]):
        if b.y < a.bottom:
            problems.append(
                f"{name}: OVERLAP {a.label} ends {a.bottom}, {b.label} starts {b.y}"
            )
    for el in ordered:
        if el.bottom > h:
            problems.append(f"{name}: OVERFLOW {el.label} ends {el.bottom} > {h}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(REPO / "build" / "lcd"))
    ap.add_argument("--check", action="store_true", help="overlap check only")
    args = ap.parse_args()

    source = MAIN.read_text(encoding="utf-8", errors="replace")
    w = parse_constant("kScreenW", source)
    h = parse_constant("kScreenH", source)

    screens = {}
    wifi_body = parse_function_body("drawQrScreenWifi", source)
    screens["qr_wifi"] = (
        [qr_element(wifi_body, source, "Wifi")]
        + text_elements(wifi_body)
        + dot_elements(wifi_body)
    )
    url_body = parse_function_body("drawQrScreenUrl", source)
    screens["qr_url"] = (
        [qr_element(url_body, source, "Url")]
        + text_elements(url_body)
        + dot_elements(url_body)
    )

    problems = []
    for name, els in screens.items():
        problems += check(name, els, h)

    print(f"panel {w}x{h}")
    for name, els in screens.items():
        print(f"\n{name}:")
        for el in sorted(els, key=lambda e: e.y):
            print(f"   {el.y:3}..{el.bottom:3}  {el.label}")

    if problems:
        print("\nPROBLEMS:")
        for p in problems:
            print(f"   {p}")
    else:
        print("\nno overlaps, nothing overflows the panel")

    if not args.check:
        out = pathlib.Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        for name, els in screens.items():
            render(name, w, h, els).save(out / f"{name}.png")
        # Drag previews: the states that only exist mid-gesture and are
        # therefore the hardest to inspect on real hardware.
        for dy, armed, tag in ((-40, False, "drag_up_40"), (-80, True, "drag_up_80")):
            render("qr_wifi", w, h, screens["qr_wifi"], shift=dy, armed=armed).save(
                out / f"qr_wifi_{tag}.png"
            )
        print(f"\nwrote {len(screens) + 2} PNGs to {out}")

    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
