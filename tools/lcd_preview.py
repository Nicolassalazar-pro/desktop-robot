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

The pull-down pairing sheet (src/sheet_transition.h) is previewed on the same
terms, one honest layer at a time:

  exact    kThresholdPct is parsed from the header, so if the commit point
           moves in firmware this filmstrip moves with it. The row-split
           composite (rows [0..sheetY) = card, rows [sheetY..H) = face) is
           the model spelled out in the header, not a re-modelled copy.
  close    the face is a labelled geometric placeholder, not the mascot
           bitmap; the pairing card is a labelled placeholder because the
           card's real layout is not yet in firmware. Both are drawn so they
           cannot be mistaken for the shipped artwork.
  ignored  rubber-band overshoot (only 0..100% is shown, not [-h/3..4h/3])
           and the release-by-projected-velocity decision (this shows sheet
           POSITION, not the gesture that produced it -- for that model see
           the header itself)

Positions and constants are PARSED FROM src/main.cpp, src/sheet_transition.h,
and platformio.ini rather than duplicated here, so the preview cannot silently
drift away from the firmware. If a y= value or a constexpr moves in the C++,
this follows it. If parsing fails it says so loudly instead of quietly
rendering a lie -- a preview that overstates itself is worse than none.

Usage:
    python tools/lcd_preview.py                    # render everything
    python tools/lcd_preview.py --sheet-only       # just the sheet filmstrip
    python tools/lcd_preview.py --out build/lcd    # choose output directory
    python tools/lcd_preview.py --check            # overlap check only, no PNGs

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
SHEET_HEADER = REPO / "src" / "sheet_transition.h"

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


def parse_constant(name: str, source: str, source_label: str = "src/main.cpp") -> int:
    """Pull `constexpr ... name = <int>;` out of the firmware.

    Falls through to platformio.ini when the constant is defined from a build
    flag (kScreenW = LCD_WIDTH), so the preview still tracks one source of
    truth instead of hardcoding a copy. `source_label` is only used in the
    error message so it can name the file we actually looked in -- the header
    doesn't live in main.cpp and the error must not pretend it did.
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
    raise SystemExit(f"could not find constant {name} in {source_label}")


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


def _font(size: int) -> ImageFont.ImageFont:
    """Consolas when available, default otherwise -- kept in one place so the
    filmstrip's five frames don't each fall back to a different font."""
    try:
        return ImageFont.truetype("consola.ttf", size)
    except OSError:
        return ImageFont.load_default()


def render_face(w: int, h: int) -> Image.Image:
    """Placeholder backdrop -- the real face is a mascot bitmap.

    Drawn with a warm tint and a plain geometric mascot (two dots + arc) so
    it reads as "the face is here" without being mistaken for the shipped
    artwork. The label at the bottom is deliberate: if someone screenshots
    this into a design review, it should announce that it is a stand-in.
    """
    img = Image.new("RGB", (w, h), (238, 220, 195))
    d = ImageDraw.Draw(img)
    cx, cy = w // 2, h // 2
    for dx in (-32, 32):
        d.ellipse([cx + dx - 8, cy - 28, cx + dx + 8, cy - 12], fill=BLACK)
    d.arc([cx - 38, cy - 12, cx + 38, cy + 34], 20, 160, fill=BLACK, width=4)
    d.text((6, h - 14), "face (placeholder mascot)", fill=(120, 96, 72), font=_font(10))
    return img


def render_sheet_card(w: int, h: int) -> Image.Image:
    """Placeholder pairing card -- the real card is not yet in firmware.

    Full-panel-height so the row-split composite in composite_sheet() can
    take any prefix of it. Contents are anchored top-down (title at the top,
    action row near the bottom) which matches how a pull-down sheet reveals
    itself: what you see first as sheetY grows is the header. When the real
    card lands, port its parse-from-source treatment from the QR screens
    rather than baking pixels here.
    """
    img = Image.new("RGB", (w, h), (250, 250, 253))
    d = ImageDraw.Draw(img)
    # drag handle: sheets without one feel like modal dialogs, not sheets
    hx = (w - 40) // 2
    d.rounded_rectangle([hx, 8, hx + 40, 12], radius=2, fill=(180, 180, 190))
    d.text((16, 28), "Pair device", fill=BLACK, font=_font(22))
    d.text((16, 60), "enter code on phone", fill=GREY, font=_font(13))
    d.line([16, 92, w - 16, 92], fill=(220, 220, 224))
    d.text((16, 104), "PAIRING CODE", fill=GREY, font=_font(10))
    d.text((16, 122), "* * * - * * *", fill=BLACK, font=_font(24))
    d.rounded_rectangle([16, h - 56, w - 16, h - 20], radius=6, fill=BLACK)
    d.text((w // 2 - 24, h - 46), "Cancel", fill=WHITE, font=_font(14))
    d.text((6, h - 14), "sheet (placeholder card)", fill=GREY, font=_font(10))
    return img


def composite_sheet(face: Image.Image, card: Image.Image, sheet_y: int) -> Image.Image:
    """Row-split composite: [0..sheetY) = card top rows, [sheetY..H) = face.

    This IS the model spelled out in src/sheet_transition.h -- the face does
    not move, the card is a second layer read row by row. Deliberately not a
    fade, translate, or blend: doing anything else here would misrepresent
    what the firmware will actually draw.
    """
    w, h = face.size
    sheet_y = max(0, min(h, sheet_y))
    out = face.copy()
    if sheet_y > 0:
        out.paste(card.crop((0, 0, w, sheet_y)), (0, 0))
    return out


def render_filmstrip(
    w: int, h: int, positions_pct: list[int], threshold_pct: int
) -> Image.Image:
    """Sheet travel at several positions, side by side, threshold marked.

    Marking the threshold matters because release is decided by projected
    velocity around exactly that line -- it is the interaction's decision
    point, and a filmstrip that hides it hides the thing worth reviewing.
    """
    face = render_face(w, h)
    card = render_sheet_card(w, h)

    margin, gutter = 24, 24
    title_h, label_h = 52, 44
    n = len(positions_pct)
    total_w = margin * 2 + n * w + (n - 1) * gutter
    total_h = title_h + h + label_h + margin

    strip = Image.new("RGB", (total_w, total_h), WHITE)
    d = ImageDraw.Draw(strip)
    d.text(
        (margin, 14),
        f"sheet travel  --  threshold {threshold_pct}% "
        f"(parsed from src/sheet_transition.h)",
        fill=BLACK,
        font=_font(18),
    )
    d.text(
        (margin, 34),
        "top rows = card, bottom rows = face; face is a placeholder",
        fill=GREY,
        font=_font(11),
    )

    for i, pct in enumerate(positions_pct):
        x = margin + i * (w + gutter)
        y = title_h
        sheet_y = round(h * pct / 100)
        strip.paste(composite_sheet(face, card, sheet_y), (x, y))
        # Threshold frame gets a heavier, warm border so it is unmistakable
        # from a thumbnail -- reviewers should not have to read the caption.
        is_threshold = (pct == threshold_pct)
        border_col = (204, 136, 0) if is_threshold else (60, 60, 60)
        border_w = 3 if is_threshold else 1
        d.rectangle(
            [x - border_w, y - border_w,
             x + w + border_w - 1, y + h + border_w - 1],
            outline=border_col, width=border_w,
        )
        label = f"{pct}%" + ("  <- commit threshold" if is_threshold else "")
        d.text((x, y + h + 8), label,
               fill=border_col if is_threshold else BLACK, font=_font(14))
        d.text((x, y + h + 26), f"sheetY = {sheet_y}/{h} px",
               fill=GREY, font=_font(11))

    return strip


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(REPO / "build" / "lcd"))
    ap.add_argument("--check", action="store_true", help="overlap check only")
    ap.add_argument(
        "--sheet-only", action="store_true",
        help="render only the sheet-transition filmstrip (checks still run)",
    )
    args = ap.parse_args()

    source = MAIN.read_text(encoding="utf-8", errors="replace")
    w = parse_constant("kScreenW", source)
    h = parse_constant("kScreenH", source)

    # Pulled from the header, not a hardcoded 40. If firmware moves the commit
    # point the filmstrip's marked frame moves with it, and if parsing fails
    # we exit loudly here rather than silently drawing the wrong decision.
    sheet_source = SHEET_HEADER.read_text(encoding="utf-8", errors="replace")
    threshold_pct = parse_constant(
        "kThresholdPct", sheet_source, "src/sheet_transition.h"
    )
    if not (0 <= threshold_pct <= 100):
        raise SystemExit(
            f"kThresholdPct={threshold_pct} is outside [0..100] -- "
            f"src/sheet_transition.h looks wrong; refusing to render a "
            f"misleading filmstrip"
        )

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
    print(f"sheet commit threshold {threshold_pct}% (from src/sheet_transition.h)")
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
        written = 0
        if not args.sheet_only:
            for name, els in screens.items():
                render(name, w, h, els).save(out / f"{name}.png")
                written += 1
            # Drag previews: the states that only exist mid-gesture and are
            # therefore the hardest to inspect on real hardware.
            for dy, armed, tag in ((-40, False, "drag_up_40"), (-80, True, "drag_up_80")):
                render("qr_wifi", w, h, screens["qr_wifi"], shift=dy, armed=armed).save(
                    out / f"qr_wifi_{tag}.png"
                )
                written += 1

        # Include the threshold explicitly and pick landmark positions around
        # it. Ordered dedup so an odd threshold (25 or 60) doesn't produce a
        # duplicate frame.
        positions_pct = sorted({0, 25, threshold_pct, 60, 100})
        render_filmstrip(w, h, positions_pct, threshold_pct).save(
            out / "sheet_filmstrip.png"
        )
        written += 1
        print(f"\nwrote {written} PNG{'s' if written != 1 else ''} to {out}")

    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
