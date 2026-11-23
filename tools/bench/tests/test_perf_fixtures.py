"""Fixture builders on small inputs: markers, refusal, the picture format and the tree's shape."""

import random
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import perf_fixtures as pf  # noqa: E402


class Markers(unittest.TestCase):
    def test_a_finished_fixture_is_not_built_again_and_an_unmarked_folder_is_refused(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.assertIsNotNone(pf.begin(root, "small"))
            pf.write_marked(root / "small", "small")
            self.assertIsNone(pf.begin(root, "small"))
            (root / "other").mkdir()
            with self.assertRaises(pf.FixtureError):
                pf.begin(root, "other")

    def test_clean_removes_marked_folders_only(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            for fixture in ("small", "kinds"):
                (root / fixture).mkdir()
            pf.write_marked(root / "small", "small")
            self.assertEqual(pf.clean(root), ["small"])
            self.assertTrue((root / "kinds").is_dir())

    def test_the_editable_csv_fits_what_the_preview_shows(self):
        # The preview shows the first 10,000 rows and refuses to edit a table that has more.
        self.assertLess(pf.EDITABLE_ROWS, 10_000)

    def test_unknown_names_are_refused(self):
        with tempfile.TemporaryDirectory() as name:
            with self.assertRaises(pf.FixtureError):
                pf.build(Path(name), ["nope"])


class Picture(unittest.TestCase):
    def test_png_has_the_requested_size_and_decompresses_to_the_expected_bytes(self):
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "p.png"
            pf.write_png(path, 40, 30, seed=5)
            data = path.read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(struct.unpack(">II", data[16:24]), (40, 30))
        position, idat = 8, b""
        while position < len(data):
            length, kind = struct.unpack(">I4s", data[position:position + 8])
            body = data[position + 8:position + 8 + length]
            self.assertEqual(struct.unpack(">I", data[position + 8 + length:position + 12 + length])[0],
                             zlib.crc32(kind + body))
            idat += body if kind == b"IDAT" else b""
            position += 12 + length
        self.assertEqual(len(zlib.decompress(idat)), 30 * (1 + 40 * 3))

    def test_pictures_differ_by_seed(self):
        with tempfile.TemporaryDirectory() as name:
            first, second = Path(name) / "a.png", Path(name) / "b.png"
            pf.write_png(first, 32, 32, seed=1)
            pf.write_png(second, 32, 32, seed=2)
            self.assertNotEqual(first.read_bytes(), second.read_bytes())


class Tree(unittest.TestCase):
    def test_plan_has_the_folder_and_file_counts_and_bounded_depth(self):
        parents, counts = pf.tree_plan(random.Random(33))
        self.assertEqual(len(parents), pf.TREE_FOLDERS)
        self.assertLessEqual(max(depth for _, depth in parents), pf.MAX_DEPTH)
        self.assertAlmostEqual(sum(counts), pf.TREE_FILES, delta=pf.TREE_FOLDERS * 0.5)
        self.assertEqual(sum(parent == -1 for parent, _ in parents), pf.TOP_FOLDERS)
        self.assertTrue(all(parent < index for index, (parent, _) in enumerate(parents)))

    def test_plan_is_deterministic(self):
        self.assertEqual(pf.tree_plan(random.Random(33)), pf.tree_plan(random.Random(33)))


if __name__ == "__main__":
    unittest.main()


class Clips(unittest.TestCase):
    def probe(self, path: Path) -> str:
        import subprocess
        return subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                               "stream=width,height,r_frame_rate,pix_fmt", "-of", "csv=p=0", str(path)],
                              capture_output=True, text=True, timeout=60).stdout.strip()

    def test_a_clip_has_the_requested_size_and_frame_rate(self):
        with tempfile.TemporaryDirectory() as name:
            clip = Path(name) / "c.mp4"
            pf.make_video(clip, 1, width=320, height=240, fps=60)
            self.assertEqual(self.probe(clip), "320,240,yuv420p,60/1")

    def test_the_playback_fixtures_are_named_and_built(self):
        for name in ("video-4k", "video-60"):
            self.assertIn(name, pf.NAMES)
            self.assertIn(name, pf.BUILDERS)

    def test_the_video_fixture_holds_a_short_clip_and_a_non_media_file(self):
        self.assertIn("short-3s.mp4", pf.VIDEO_FILES)
        self.assertIn("plain.txt", pf.VIDEO_FILES)


class OpaqueAlphaPictures(unittest.TestCase):
    def test_a_picture_with_an_alpha_channel_of_255_is_a_valid_rgba_png(self):
        import struct, zlib
        with tempfile.TemporaryDirectory() as name:
            path = Path(name) / "p.png"
            pf.write_png(path, 16, 8, seed=3, alpha=True)
            data = path.read_bytes()
            width, height, depth, color = struct.unpack(">IIBB", data[16:26])
            self.assertEqual((width, height, depth, color), (16, 8, 8, 6))
            idat, position = b"", 8
            while position < len(data):
                size, kind = struct.unpack(">I4s", data[position:position + 8])
                if kind == b"IDAT":
                    idat += data[position + 8:position + 8 + size]
                position += 12 + size
            raw = zlib.decompress(idat)
            stride = 16 * 4 + 1
            self.assertEqual(len(raw), 8 * stride)
            for row in range(8):
                line = raw[row * stride + 1:(row + 1) * stride]
                self.assertEqual(set(line[3::4]), {255})

    def test_the_opaque_alpha_folder_is_a_named_fixture_and_a_scenario_reads_it(self):
        self.assertIn("pictures-rgba-300", pf.NAMES)
        self.assertIn("pictures-rgba-300", pf.BUILDERS)


class HardClips(unittest.TestCase):
    def probe(self, path: Path) -> str:
        import subprocess
        return subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                               "stream=codec_name,width,height,pix_fmt,r_frame_rate", "-of", "csv=p=0", str(path)],
                              capture_output=True, text=True, timeout=60).stdout.strip()

    def test_each_codec_makes_a_detailed_clip_of_the_asked_size_and_rate(self):
        for codec, expected in (("hevc", "hevc,320,240,yuv420p,60/1"), ("hevc10", "hevc,320,240,yuv420p10le,60/1"), ("av1", "av1,320,240,yuv420p,60/1"),
                                ("vp9", "vp9,320,240,yuv420p,60/1")):
            with tempfile.TemporaryDirectory() as name:
                clip = Path(name) / f"c.{pf.CODECS[codec][1]}"
                pf.make_hard_video(clip, codec, 1, 320, 240, 60)
                self.assertEqual(self.probe(clip), expected)

    def test_the_hard_fixtures_are_named_and_built(self):
        for name in ("video-hevc-4k60", "video-hevc-4k30", "video-av1-4k60", "video-av1-4k30", "video-vp9-4k60",
                     "video-vp9-4k30", "video-hevc10-4k60"):
            self.assertIn(name, pf.NAMES)
            self.assertIn(name, pf.BUILDERS)
