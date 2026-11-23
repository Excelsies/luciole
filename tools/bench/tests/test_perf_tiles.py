"""The late-thumbnail reader: tiles found in a settled screenshot, frames compared with it, a burst reduced to times."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import perf_tiles as pt  # noqa: E402

BACKGROUND = (26, 27, 53)
WIDTH, HEIGHT = 1920, 1080


def picture(x: int, y: int, w: int, h: int, seed: int):
    """Diagonal stripes of two grays, brighter than the background; `seed` shifts the stripes."""
    return {(x + i, y + j): ((190, 190, 190) if (i + j + seed) % 8 < 4 else (110, 110, 110))
            for i in range(w) for j in range(h)}


def label(x: int, y: int):
    """A line of text: thin bright strokes 12 rows tall, as a file name under a tile."""
    return {(x + i, y + j): (230, 230, 230) for i in range(0, 90, 3) for j in range(12)}


class Frame:
    """A 1920x1080 image as P6 bytes with the given pixel overrides on a flat background."""

    def __init__(self, pixels: dict):
        row = bytearray(bytes(BACKGROUND) * WIDTH)
        self.rows = [bytearray(row) for _ in range(HEIGHT)]
        for (x, y), color in pixels.items():
            self.rows[y][x * 3:x * 3 + 3] = bytes(color)

    def write(self, path: Path) -> None:
        path.write_bytes(f"P6\n{WIDTH} {HEIGHT}\n255\n".encode() + b"".join(bytes(row) for row in self.rows))


def grid(columns: list[int], rows: list[int], skip=(), seed: int = 0, shift: int = 0):
    """Tiles 96x72 at the given x and y origins; `skip` is a set of (column index, row index) left out."""
    pixels: dict = {}
    for r, y in enumerate(rows):
        for c, x in enumerate(columns):
            if (c, r) not in skip:
                pixels |= picture(x, y, 96, 72, seed + c + r + shift)
        pixels |= label(columns[0], y + 80)
    return pixels


COLUMNS = [252, 402, 552]
ROWS = [240, 384]


class Tiles(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.path = Path(self.folder.name)

    def tearDown(self):
        self.folder.cleanup()

    def shot(self, name: str, pixels: dict) -> Path:
        path = self.path / name
        Frame(pixels).write(path)
        return path

    def test_the_tiles_are_the_pictures_and_not_the_text_under_them(self):
        found = pt.find_tiles(pt.read_ppm(self.shot("a.ppm", grid(COLUMNS, ROWS))))
        self.assertEqual(sorted(found), sorted((x, y, x + 96, y + 72) for y in ROWS for x in COLUMNS))

    def test_a_picture_cut_by_the_top_edge_of_the_grid_is_still_a_tile(self):
        found = pt.find_tiles(pt.read_ppm(self.shot("a.ppm", grid([252], [100]))))
        self.assertEqual(found, [(252, 100, 348, 172)])

    def test_a_frame_equal_to_the_settled_one_has_no_unsettled_tile(self):
        settled = pt.read_ppm(self.shot("s.ppm", grid(COLUMNS, ROWS)))
        tiles = pt.find_tiles(settled)
        self.assertEqual(pt.unsettled(settled, settled, tiles), 0)

    def test_a_missing_picture_and_a_different_one_each_count_and_a_blank_frame_counts_all(self):
        settled = pt.read_ppm(self.shot("s.ppm", grid(COLUMNS, ROWS)))
        tiles = pt.find_tiles(settled)
        missing = pt.read_ppm(self.shot("m.ppm", grid(COLUMNS, ROWS, skip={(0, 0), (2, 1)})))
        self.assertEqual(pt.unsettled(missing, settled, tiles), 2)
        blank = pt.read_ppm(self.shot("b.ppm", {}))
        self.assertEqual(pt.unsettled(blank, settled, tiles), 6)
        softer = pt.read_ppm(self.shot("d.ppm", grid(COLUMNS, ROWS, shift=2)))
        self.assertEqual(pt.unsettled(softer, settled, tiles), 6)

    def test_a_small_difference_in_brightness_still_matches(self):
        settled = pt.read_ppm(self.shot("s.ppm", grid(COLUMNS, ROWS)))
        near = {k: tuple(min(255, v + 2) for v in color) for k, color in grid(COLUMNS, ROWS).items()}
        self.assertEqual(pt.unsettled(pt.read_ppm(self.shot("n.ppm", near)), settled, pt.find_tiles(settled)), 0)

    def test_a_tile_with_no_picture_is_empty_and_one_showing_another_picture_is_not(self):
        settled = pt.read_ppm(self.shot("s.ppm", grid(COLUMNS, ROWS)))
        tiles = pt.find_tiles(settled)
        # Two tiles show nothing (the generic icon is dark on the background), one shows other stripes.
        frame = pt.read_ppm(self.shot("f.ppm", grid(COLUMNS, ROWS, skip={(0, 0), (2, 1)}, shift=3)))
        self.assertEqual(pt.unsettled(frame, settled, tiles), 6)
        self.assertEqual(pt.empty(frame, tiles), 2)
        self.assertEqual(pt.empty(settled, tiles), 0)

    def entry(self, times: list[float], frames: list[dict], t0: float = 100.0) -> dict:
        names = []
        for index, pixels in enumerate(frames):
            names.append({"file": f"f{index}.ppm", "t": t0 + times[index]})
            self.shot(f"f{index}.ppm", pixels)
        self.shot("settled.ppm", grid(COLUMNS, ROWS))
        return {"name": "x", "t0": t0, "frames": names, "settled": {"file": "settled.ppm", "t": t0 + 9}}

    def test_a_burst_reports_when_every_tile_first_matches_and_when_the_last_mismatch_was(self):
        full = grid(COLUMNS, ROWS)
        entry = self.entry([0.1, 0.2, 0.3, 0.4, 0.5], [
            {}, grid(COLUMNS, ROWS, skip={(0, 0), (1, 0), (2, 0)}), grid(COLUMNS, ROWS, skip={(1, 1)}), full, full])
        result = pt.late_tiles(self.path, entry)
        self.assertEqual(result["tiles"], 6)
        self.assertEqual(result["unsettled"], [6, 3, 1, 0, 0])
        self.assertAlmostEqual(result["first_all_settled_s"], 0.4)
        self.assertAlmostEqual(result["last_unsettled_s"], 0.3)
        self.assertEqual(result["frames"], 5)
        self.assertAlmostEqual(result["interval_s"], 0.1)
        # The blank frame and the frames that miss tiles show nothing there: those tiles are empty, not late pictures.
        self.assertEqual(result["empty"], [6, 3, 1, 0, 0])
        self.assertAlmostEqual(result["last_empty_s"], 0.3)

    def test_a_flicker_after_the_first_match_is_visible_in_the_last_unsettled_time(self):
        full = grid(COLUMNS, ROWS)
        entry = self.entry([0.1, 0.2, 0.3], [full, grid(COLUMNS, ROWS, skip={(0, 0)}), full])
        result = pt.late_tiles(self.path, entry)
        self.assertAlmostEqual(result["first_all_settled_s"], 0.1)
        self.assertAlmostEqual(result["last_unsettled_s"], 0.2)
        self.assertAlmostEqual(result["last_empty_s"], 0.2)

    def test_a_burst_that_never_settles_has_no_first_time(self):
        entry = self.entry([0.1, 0.2], [{}, {}])
        result = pt.late_tiles(self.path, entry)
        self.assertIsNone(result["first_all_settled_s"])
        self.assertAlmostEqual(result["last_unsettled_s"], 0.2)

    def test_a_settled_shot_with_no_tiles_is_an_error_and_not_a_pass(self):
        entry = self.entry([0.1], [{}])
        Frame({}).write(self.path / "settled.ppm")
        with self.assertRaises(pt.TileError):
            pt.late_tiles(self.path, entry)

    def test_a_file_that_is_not_a_ppm_is_refused(self):
        (self.path / "x.ppm").write_bytes(b"P5\n2 2\n255\nabcd")
        with self.assertRaises(pt.TileError):
            pt.read_ppm(self.path / "x.ppm")


if __name__ == "__main__":
    unittest.main()
