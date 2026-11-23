"""The seek bar reader, from PNGs written here with every filter kind."""

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import perf_realtime as pr  # noqa: E402

BG, TRACK, FILL = (26, 27, 53), (62, 65, 102), (247, 233, 168)
WIDTH, HEIGHT = 1920, 1080


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def filtered(kind: int, row: bytes, above: bytes, bpp: int) -> bytes:
    out = bytearray()
    for i, value in enumerate(row):
        left = row[i - bpp] if i >= bpp else 0
        up = above[i]
        corner = above[i - bpp] if i >= bpp else 0
        predictor = {0: 0, 1: left, 2: up, 3: (left + up) // 2, 4: pr.paeth(left, up, corner)}[kind]
        out.append((value - predictor) & 255)
    return bytes([kind]) + bytes(out)


def write_png(path: Path, fill_px: int, color: int = 2) -> None:
    """A 1920x1080 image whose rows use the filter kinds in turn and whose seek bar is 1384 to 1825 wide."""
    bpp = 3 if color == 2 else 4
    pad = b"" if color == 2 else b"\xff"
    rows, above = [], bytes(WIDTH * bpp)
    for y in range(HEIGHT):
        pixels = [BG] * WIDTH
        if 1066 <= y <= 1068:
            pixels[1384:1825] = [TRACK] * 441
            pixels[1384:1384 + fill_px] = [FILL] * fill_px
        row = b"".join(bytes(p) + pad for p in pixels)
        rows.append(filtered(y % 5, row, above, bpp))
        above = row
    header = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, color, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(b"".join(rows)))
                     + chunk(b"IEND", b""))


class SeekBar(unittest.TestCase):
    def check(self, fill_px: int, color: int = 2) -> float:
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "shot.png"
            write_png(path, fill_px, color)
            return pr.position_fraction(path)

    def test_the_fraction_is_the_filled_share_of_the_track(self):
        self.assertAlmostEqual(self.check(147), 147 / 441, places=3)

    def test_an_empty_bar_reads_zero_and_a_full_one_reads_one(self):
        self.assertEqual(self.check(0), 0.0)
        self.assertEqual(self.check(441), 1.0)

    def test_rgba_screenshots_read_the_same(self):
        self.assertAlmostEqual(self.check(220, color=6), 220 / 441, places=3)

    def test_no_bar_is_an_error(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "plain.png"
            rows = b"".join(b"\x00" + bytes(BG) * WIDTH for _ in range(HEIGHT))
            header = struct.pack(">IIBBBBB", WIDTH, HEIGHT, 8, 2, 0, 0, 0)
            path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows))
                             + chunk(b"IEND", b""))
            with self.assertRaises(pr.RealtimeError):
                pr.position_fraction(path)

    def test_a_file_that_is_not_a_png_is_an_error(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "x.png"
            path.write_bytes(b"nope")
            with self.assertRaises(pr.RealtimeError):
                pr.position_fraction(path)


if __name__ == "__main__":
    unittest.main()


class Playback(unittest.TestCase):
    def test_the_advance_is_read_from_two_shots_and_set_against_the_wall_time(self):
        with tempfile.TemporaryDirectory() as name:
            folder = Path(name)
            write_png(folder / "a.png", 44)
            write_png(folder / "b.png", 176)
            entry = {"duration_s": 30, "first": {"file": "a.png", "t": 100.0}, "second": {"file": "b.png", "t": 110.0}}
            play = pr.playback(folder, entry)
            self.assertAlmostEqual(play["advance_s"], 132 / 441 * 30, places=2)
            self.assertAlmostEqual(play["ratio"], 132 / 441 * 3, places=2)
            self.assertEqual(play["wall_s"], 10.0)
