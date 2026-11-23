"""Late thumbnails: how long after a folder is shown again its tiles show their pictures, read from screenshots.

The harness takes PPM screenshots as fast as grim allows after an action, then one settled shot. The tiles are the
pictures found in the settled shot (bright runs, as the file names under them are too thin to make one); a frame's
tile is unsettled when its pixels differ from the settled shot's. Standard library only.
"""

from pathlib import Path

# The part of the window that holds the grid at 1920x1080: right of the sidebar, below the toolbar, left of the
# preview column.
AREA = (215, 100, 1590, 1078)
# A pixel belongs to a picture when its brightest channel passes this; the app background and the selection
# highlight stay below it.
BRIGHT = 105
# Rows (and columns) a picture covers at least: the file names are 12 rows tall.
MIN_ROWS = 24
MIN_COLUMNS = 8
ROW_COUNT = 40
COLUMN_SHARE = 0.3
# A tile matches when its pixels differ from the settled ones by at most this much on average, per channel.
TOLERANCE = 6.0


class TileError(RuntimeError):
    pass


class Image:
    def __init__(self, width: int, height: int, data: bytes):
        self.width, self.height, self.data = width, height, data

    def row(self, y: int, x0: int = 0, x1: int | None = None) -> bytes:
        base = y * self.width * 3
        return self.data[base + x0 * 3: base + (self.width if x1 is None else x1) * 3]


def read_ppm(path: Path) -> Image:
    data = path.read_bytes()
    parts = data.split(b"\n", 3)
    if len(parts) < 4 or parts[0] != b"P6":
        raise TileError(f"{path} is not a binary PPM (P6)")
    try:
        width, height = (int(item) for item in parts[1].split())
    except ValueError:
        raise TileError(f"{path}: the PPM size line is wrong") from None
    if parts[2] != b"255" or len(parts[3]) != width * height * 3:
        raise TileError(f"{path}: the PPM must be 8-bit and hold {width}x{height} pixels")
    return Image(width, height, parts[3])


def runs(flags: list[bool], minimum: int) -> list[tuple[int, int]]:
    found, start = [], None
    for index, flag in enumerate(flags + [False]):
        if flag and start is None:
            start = index
        elif not flag and start is not None:
            if index - start >= minimum:
                found.append((start, index))
            start = None
    return found


def bright_count(image: Image, y: int, x0: int, x1: int) -> int:
    return sum(1 for index in range(0, (x1 - x0) * 3, 3)
               if max(image.row(y, x0, x1)[index:index + 3]) > BRIGHT)


def find_tiles(image: Image) -> list[tuple[int, int, int, int]]:
    """(x0, y0, x1, y1) of every picture in the grid area: the row bands with many bright pixels, then the columns
    of each band. Pixel x of a picture row is bright when its brightest channel passes BRIGHT."""
    x0, y0, x1, y1 = AREA
    y1 = min(y1, image.height)
    x1 = min(x1, image.width)
    counts = [bright_count(image, y, x0, x1) for y in range(y0, y1)]
    bands = runs([count >= ROW_COUNT for count in counts], MIN_ROWS)
    tiles = []
    for top, bottom in bands:
        top, bottom = top + y0, bottom + y0
        share = [0] * (x1 - x0)
        for y in range(top, bottom):
            row = image.row(y, x0, x1)
            for x in range(x1 - x0):
                if max(row[x * 3:x * 3 + 3]) > BRIGHT:
                    share[x] += 1
        for left, right in runs([count >= COLUMN_SHARE * (bottom - top) for count in share], MIN_COLUMNS):
            tiles.append((left + x0, top, right + x0, bottom))
    return tiles


def different(frame: Image, settled: Image, tile: tuple[int, int, int, int]) -> bool:
    x0, y0, x1, y1 = tile
    total = 0
    for y in range(y0, y1):
        a, b = frame.row(y, x0, x1), settled.row(y, x0, x1)
        if a != b:
            total += sum(abs(p - q) for p, q in zip(a, b))
    return total / ((x1 - x0) * (y1 - y0) * 3) > TOLERANCE


def unsettled(frame: Image, settled: Image, tiles: list[tuple[int, int, int, int]]) -> int:
    """How many of the settled shot's tiles the frame does not show as the settled shot does."""
    return sum(different(frame, settled, tile) for tile in tiles)


def has_picture(frame: Image, tile: tuple[int, int, int, int]) -> bool:
    """Whether the tile's region holds a picture: enough pixels brighter than the background, as in find_tiles."""
    x0, y0, x1, y1 = tile
    bright = sum(bright_count(frame, y, x0, x1) for y in range(y0, y1))
    return bright >= COLUMN_SHARE * (x1 - x0) * (y1 - y0)


def empty(frame: Image, tiles: list[tuple[int, int, int, int]]) -> int:
    """How many of the settled shot's tiles show no picture at all (the generic icon or nothing), as against another
    picture, which is what the view before the action shows."""
    return sum(not has_picture(frame, tile) for tile in tiles)


def late_tiles(folder: Path, entry: dict) -> dict:
    """The burst of `entry` ({"t0", "frames": [{"file", "t"}], "settled": {"file"}}) read against its settled shot:
    the tile count, the unsettled tiles of each frame, the time of the first frame with none (from t0), the time of
    the last frame that had any, the tiles showing no picture at all in each frame (and the time of the last frame
    that had any: the generic icon or a blank, which is a late thumbnail, where an unsettled tile may only show the
    view before the action) and the mean interval between frames."""
    settled = read_ppm(folder / entry["settled"]["file"])
    tiles = find_tiles(settled)
    if not tiles:
        raise TileError(f"no tiles found in the settled screenshot {entry['settled']['file']}")
    counts, blanks, times = [], [], []
    for frame in entry["frames"]:
        image = read_ppm(folder / frame["file"])
        counts.append(unsettled(image, settled, tiles))
        blanks.append(empty(image, tiles))
        times.append(frame["t"] - entry["t0"])
    first = next((time for time, count in zip(times, counts) if count == 0), None)
    last = next((time for time, count in reversed(list(zip(times, counts))) if count > 0), None)
    last_empty = next((time for time, count in reversed(list(zip(times, blanks))) if count > 0), None)
    gaps = [b - a for a, b in zip(times, times[1:])]
    return {"tiles": len(tiles), "unsettled": counts, "times_s": times, "frames": len(counts),
            "first_all_settled_s": first, "last_unsettled_s": last, "empty": blanks, "last_empty_s": last_empty,
            "interval_s": sum(gaps) / len(gaps) if gaps else None}
