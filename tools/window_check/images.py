"""Minimal PNG reading for screenshots: 8-bit RGB or RGBA, not interlaced."""

import struct
import zlib
from pathlib import Path

SIGNATURE = b"\x89PNG\r\n\x1a\n"
CHANNELS = {2: 3, 6: 4}


class ImageError(ValueError):
    pass


def _chunks(data: bytes):
    if not data.startswith(SIGNATURE):
        raise ImageError("not a PNG file")
    offset = len(SIGNATURE)
    while offset + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        yield kind, data[offset + 8:offset + 8 + length]
        offset += 12 + length


def _header(data: bytes) -> tuple[int, int, int, int, int]:
    for kind, body in _chunks(data):
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            return width, height, depth, color, interlace
    raise ImageError("PNG has no header")


def png_size(path: Path) -> tuple[int, int]:
    width, height, *_ = _header(path.read_bytes())
    return width, height


def _paeth(left: int, up: int, corner: int) -> int:
    estimate = left + up - corner
    distances = (abs(estimate - left), abs(estimate - up), abs(estimate - corner))
    return (left, up, corner)[distances.index(min(distances))]


def _rows(data: bytes):
    width, height, depth, color, interlace = _header(data)
    if depth != 8 or color not in CHANNELS or interlace != 0:
        raise ImageError(f"unsupported PNG: depth {depth}, color type {color}, interlace {interlace}")
    step = CHANNELS[color]
    stride = width * step
    raw = zlib.decompress(b"".join(body for kind, body in _chunks(data) if kind == b"IDAT"))
    previous = bytearray(stride)
    for row in range(height):
        start = row * (stride + 1)
        kind, line = raw[start], bytearray(raw[start + 1:start + 1 + stride])
        for index in range(stride):
            left = line[index - step] if index >= step else 0
            up = previous[index]
            corner = previous[index - step] if index >= step else 0
            predictor = (0, left, up, (left + up) // 2, _paeth(left, up, corner))[kind]
            line[index] = (line[index] + predictor) & 0xFF
        yield step, bytes(line)
        previous = line


def png_colors(path: Path, limit: int) -> int:
    """Counts distinct pixel values, stopping once `limit` is reached."""
    colors: set[bytes] = set()
    for step, line in _rows(path.read_bytes()):
        for index in range(0, len(line), step):
            colors.add(line[index:index + step])
            if len(colors) >= limit:
                return limit
    return len(colors)


def png_pixel(path: Path, x: int, y: int) -> tuple[int, int, int]:
    """The red, green and blue values of one pixel."""
    width, height = png_size(path)
    if not (0 <= x < width and 0 <= y < height):
        raise ImageError(f"pixel {x},{y} is outside the {width}x{height} image")
    for row, (step, line) in enumerate(_rows(path.read_bytes())):
        if row == y:
            return tuple(line[x * step:x * step + 3])
    raise ImageError(f"{path} has fewer than {y + 1} rows")
