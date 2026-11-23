"""Playback position from a screenshot: the filled part of the preview's seek bar, read with the standard library.

The harness takes two screenshots a known time apart while a clip plays; the fill grows with the position, so the
position advance over the wall time says whether playback keeps real time.
"""

import struct
import zlib
from pathlib import Path

TOLERANCE = 12
MIN_TRACK = 250
FILL_BRIGHTNESS = 150
# The preview column's right part, where the seek bar lies at 1920x1080.
SCAN_FROM = 1324
SCAN_ROWS = range(1040, 1080)


class RealtimeError(RuntimeError):
    pass


def paeth(left: int, up: int, corner: int) -> int:
    estimate = left + up - corner
    da, db, dc = abs(estimate - left), abs(estimate - up), abs(estimate - corner)
    return left if da <= db and da <= dc else up if db <= dc else corner


def unfilter(kind: int, row: bytearray, above: bytearray, bpp: int) -> None:
    if kind == 0:
        return
    for index in range(len(row)):
        left = row[index - bpp] if index >= bpp else 0
        up = above[index]
        corner = above[index - bpp] if index >= bpp else 0
        if kind == 1:
            row[index] = (row[index] + left) & 255
        elif kind == 2:
            row[index] = (row[index] + up) & 255
        elif kind == 3:
            row[index] = (row[index] + ((left + up) >> 1)) & 255
        elif kind == 4:
            row[index] = (row[index] + paeth(left, up, corner)) & 255
        else:
            raise RealtimeError(f"unknown PNG filter {kind}")


def read_rows(path: Path, wanted: range) -> dict[int, list[tuple[int, int, int]]]:
    """The pixels of the wanted rows of an 8-bit RGB or RGBA PNG; the rows above them are decoded too."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RealtimeError(f"{path} is not a PNG")
    position, pixels = 8, b""
    width = height = depth = color = interlace = 0
    while position < len(data):
        size, kind = struct.unpack(">I4s", data[position:position + 8])
        chunk = data[position + 8:position + 8 + size]
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            pixels += chunk
        position += 12 + size
    if depth != 8 or color not in (2, 6) or interlace != 0:
        raise RealtimeError(f"{path}: only 8-bit RGB or RGBA PNG without interlacing is read")
    bpp = 3 if color == 2 else 4
    raw = zlib.decompress(pixels)
    stride = width * bpp
    if len(raw) != height * (stride + 1):
        raise RealtimeError(f"{path}: the pixel data has the wrong size")
    last = min(max(wanted) + 1, height)
    above, found = bytearray(stride), {}
    for y in range(last):
        base = y * (stride + 1)
        row = bytearray(raw[base + 1:base + 1 + stride])
        unfilter(raw[base], row, above, bpp)
        if y in wanted:
            found[y] = [(row[x * bpp], row[x * bpp + 1], row[x * bpp + 2]) for x in range(width)]
        above = row
    return found


def near(a: tuple[int, int, int], b: tuple[int, int, int]) -> bool:
    return all(abs(x - y) <= TOLERANCE for x, y in zip(a, b))


def bar_extent(row: list[tuple[int, int, int]]) -> tuple[int, int, int] | None:
    """(left, fill_end, right) of the longest run that differs from the background (the pixel at the left of the
    scanned part), or None. The fill is the
    leading part of the run brighter than FILL_BRIGHTNESS in red; with no fill, fill_end equals left."""
    segment = row[SCAN_FROM:]
    background = segment[0]
    best, start = (0, 0), None
    for x, pixel in enumerate(segment + [background]):
        if not near(pixel, background):
            start = x if start is None else start
        elif start is not None:
            best = max(best, (x - start, start))
            start = None
    length, start = best
    if length < MIN_TRACK:
        return None
    # The fill is the bright part (the dark theme's accent); the unfilled track stays dark.
    fill_end = start
    while fill_end < start + length and segment[fill_end][0] >= FILL_BRIGHTNESS:
        fill_end += 1
    return SCAN_FROM + start, SCAN_FROM + fill_end, SCAN_FROM + start + length


def position_fraction(path: Path) -> float:
    """The filled fraction of the seek bar in a screenshot, 0 to 1."""
    rows = read_rows(path, SCAN_ROWS)
    found = [extent for y in SCAN_ROWS if (extent := bar_extent(rows[y])) is not None]
    if not found:
        raise RealtimeError(f"{path}: no seek bar found")
    # The bar's centre rows are the widest and brightest; the anti-aliased edge rows are dimmer.
    left, fill_end, right = max(found, key=lambda item: (item[2] - item[0], item[1]))
    return (fill_end - left) / (right - left)


def playback(folder: Path, entry: dict) -> dict:
    """The position advance of one realtime entry against the wall time between its two screenshots."""
    start = position_fraction(folder / entry["first"]["file"]) * entry["duration_s"]
    end = position_fraction(folder / entry["second"]["file"]) * entry["duration_s"]
    wall = entry["second"]["t"] - entry["first"]["t"]
    return {"start_s": start, "end_s": end, "advance_s": end - start, "wall_s": wall,
            "ratio": (end - start) / wall if wall > 0 else 0.0}
