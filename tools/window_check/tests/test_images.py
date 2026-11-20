"""PNG reading: pixel size and whether an image is one flat color."""

import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from images import ImageError, png_colors, png_pixel, png_size  # noqa: E402


def write_png(path: Path, rows: list[bytes], width: int, color_type: int, filters: list[int]) -> None:
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    raw = b"".join(bytes([kind]) + row for kind, row in zip(filters, rows))
    header = struct.pack(">IIBBBBB", width, len(rows), 8, color_type, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw))
                     + chunk(b"IEND", b""))


class Png(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.path = Path(self.folder.name) / "image.png"

    def tearDown(self):
        self.folder.cleanup()

    def test_size_of_a_three_by_two_image(self):
        write_png(self.path, [bytes(9), bytes(9)], 3, 2, [0, 0])
        self.assertEqual(png_size(self.path), (3, 2))

    def test_flat_image_has_one_color(self):
        gray = bytes([40, 40, 40]) * 3
        write_png(self.path, [gray, gray], 3, 2, [0, 0])
        self.assertEqual(png_colors(self.path, limit=10), 1)

    def test_filtered_rows_decode_to_their_real_colors(self):
        # Row 1 uses Sub: each byte adds the pixel to its left, so 10,0,0 three times is 10, 20, 30 in red.
        # Row 2 uses Up with zero differences, so it repeats row 1.
        sub = bytes([10, 0, 0, 10, 0, 0, 10, 0, 0])
        write_png(self.path, [sub, bytes(9)], 3, 2, [1, 2])
        self.assertEqual(png_colors(self.path, limit=10), 3)

    def test_paeth_and_average_filters_with_alpha(self):
        # Average turns zero bytes under (1, 2, 3, 255) into (0, 1, 1, 127) and then (0, 1, 2, 191);
        # Paeth with zero bytes repeats those two, so three colors in all.
        pixel = bytes([1, 2, 3, 255])
        write_png(self.path, [pixel * 2, bytes(8), bytes(8)], 2, 6, [0, 3, 4])
        self.assertEqual(png_colors(self.path, limit=10), 3)

    def test_counting_stops_at_the_limit(self):
        row = bytes(range(0, 30))
        write_png(self.path, [row], 10, 2, [0])
        self.assertEqual(png_colors(self.path, limit=4), 4)

    def test_one_pixel_after_the_sub_filter(self):
        sub = bytes([10, 0, 0, 10, 0, 0, 10, 0, 0])
        write_png(self.path, [bytes(9), sub], 3, 2, [0, 1])
        self.assertEqual(png_pixel(self.path, 2, 1), (30, 0, 0))
        self.assertEqual(png_pixel(self.path, 0, 0), (0, 0, 0))

    def test_a_pixel_outside_the_image_is_refused(self):
        write_png(self.path, [bytes(9)], 3, 2, [0])
        with self.assertRaises(ImageError):
            png_pixel(self.path, 3, 0)

    def test_not_a_png(self):
        self.path.write_bytes(b"not an image")
        with self.assertRaises(ImageError):
            png_size(self.path)


if __name__ == "__main__":
    unittest.main()
