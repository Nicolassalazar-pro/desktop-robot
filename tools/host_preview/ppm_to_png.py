"""Minimal PPM (P6) -> PNG converter. No third-party deps.

The host_preview harness emits P6 PPM because writing one is ~10 lines of C++
with no image library. PNG is what the agent's `view` tool can actually
display, and zlib + struct is all it takes, so this stays dependency-free
rather than pulling Pillow into the project for one conversion.
"""
import struct
import sys
import zlib
from pathlib import Path


def read_ppm(path: Path):
    data = path.read_bytes()
    # P6 header: magic, width, height, maxval -- each separated by whitespace,
    # with '#' comments legal between them. Parse tokens rather than assuming
    # a fixed layout.
    tokens, i = [], 0
    while len(tokens) < 4:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i : i + 1] != b"\n":
                i += 1
            continue
        start = i
        while i < len(data) and not data[i : i + 1].isspace():
            i += 1
        tokens.append(data[start:i])
    i += 1  # single whitespace byte after maxval
    magic, w, h, maxv = tokens[0], int(tokens[1]), int(tokens[2]), int(tokens[3])
    if magic != b"P6" or maxv != 255:
        raise ValueError(f"{path}: expected 8-bit P6, got {magic!r} maxval={maxv}")
    return w, h, data[i : i + w * h * 3]


def write_png(path: Path, w: int, h: int, rgb: bytes):
    raw = b"".join(
        b"\x00" + rgb[row * w * 3 : (row + 1) * w * 3] for row in range(h)
    )

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(png)


def main(argv):
    if len(argv) < 2:
        print("usage: ppm_to_png.py <file.ppm|dir> [...]", file=sys.stderr)
        return 2
    targets = []
    for a in argv[1:]:
        p = Path(a)
        targets.extend(sorted(p.glob("*.ppm")) if p.is_dir() else [p])
    for src in targets:
        w, h, rgb = read_ppm(src)
        dst = src.with_suffix(".png")
        write_png(dst, w, h, rgb)
        print(f"{dst}  ({w}x{h}, {dst.stat().st_size / 1024:.1f} KB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
