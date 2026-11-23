"""Helpers of the orchestrator that need no app: processor sets, spread, the Bink link and the run merge."""

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import perf_bench as pb  # noqa: E402
from test_perf_compare import run  # noqa: E402


class Cpus(unittest.TestCase):
    def test_ranges_and_lists(self):
        self.assertEqual(pb.parse_cpus("6-8,10"), {6, 7, 8, 10})

    def test_a_processor_the_machine_lacks_is_refused(self):
        with self.assertRaises(ValueError):
            pb.parse_cpus(f"0-{os.cpu_count()}")


class Spread(unittest.TestCase):
    def test_spread_of_private_dirty_memory_and_cpu_at_the_last_checkpoint(self):
        runs = [run(1, 1.0, dirty=102400), run(1, 1.0, dirty=112640), run(1, 1.0, dirty=107520)]
        self.assertAlmostEqual(pb.spread_percent(runs), 10 / 105 * 100, places=3)

    def test_one_good_run_has_no_spread(self):
        self.assertEqual(pb.spread_percent([run(1024), run(1024, ok=False)]), 0.0)


class Merge(unittest.TestCase):
    def args(self):
        return SimpleNamespace(label="t", cpus="6-11")

    def test_a_missing_result_is_a_failed_run_with_the_log_named(self):
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / "run-0.d"
            out.mkdir()
            merged = pb.merge(self.args(), {"name": "s"}, out, 0, {}, 3, {})
            self.assertFalse(merged["ok"])
            self.assertIn("no result.json", merged["error"])

    def test_checkpoint_sample_errors_fail_the_run(self):
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / "run-0.d"
            out.mkdir()
            point = {"name": "a", "errors": ["worker 5: cannot read"]}
            (out / "result.json").write_text(json.dumps({"ok": True, "error": None, "bench": {"checkpoints": [point]},
                                                         "app_peak_rss_mib": 1.0}))
            merged = pb.merge(self.args(), {"name": "s"}, out, 0, {}, 0, {})
            self.assertFalse(merged["ok"])
            self.assertIn("checkpoint samples failed", merged["error"])

    def test_startup_times_outside_the_bound_are_refused(self):
        with tempfile.TemporaryDirectory() as name:
            out = Path(name)
            (out / "app.log").write_text("luciole-trace 5000000 first-frame\n")
            with self.assertRaises(RuntimeError):
                pb.startup_events(out, 5_000_000 - (pb.MAX_STARTUP_MS + 1) * 1_000_000)
            self.assertEqual(pb.startup_events(out, 1_000_000)["first-frame"], 4.0)


class Bink(unittest.TestCase):
    def test_the_link_has_a_neutral_name_and_is_removed_afterward(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            source = root / "secret-title.bk2"
            source.write_bytes(b"data")
            os.environ["LUCIOLE_BINK2_SAMPLE"] = str(source)
            try:
                with pb.BinkLink(root):
                    self.assertEqual([item.name for item in (root / "bink").iterdir()], ["sample.bk2"])
                    self.assertEqual((root / "bink" / "sample.bk2").stat().st_ino, source.stat().st_ino)
                self.assertFalse((root / "bink").exists())
                self.assertTrue(source.exists())
            finally:
                del os.environ["LUCIOLE_BINK2_SAMPLE"]


if __name__ == "__main__":
    unittest.main()


class PlaybackLines(unittest.TestCase):
    def test_every_playback_line_of_the_trace_is_read_in_order(self):
        with tempfile.TemporaryDirectory() as name:
            log = Path(name) / "app.log"
            log.write_text("luciole-trace 5 first-frame\n"
                           "luciole-trace 9 playback delivered=301 taken=250 replaced=51 missed=2\n"
                           "noise\n"
                           "luciole-trace 12 playback delivered=10 taken=10 replaced=0 missed=0\n")
            self.assertEqual(pb.playback_lines(log), [
                {"delivered": 301, "taken": 250, "replaced": 51, "missed": 2},
                {"delivered": 10, "taken": 10, "replaced": 0, "missed": 0}])

    def test_no_log_or_no_line_is_an_empty_list(self):
        with tempfile.TemporaryDirectory() as name:
            self.assertEqual(pb.playback_lines(Path(name) / "none.log"), [])


class LateMerge(unittest.TestCase):
    def test_the_bursts_are_read_into_the_run_and_their_screenshots_are_removed(self):
        from test_perf_tiles import COLUMNS, ROWS, Frame, grid
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / "run-0.d"
            out.mkdir()
            Frame(grid(COLUMNS, ROWS, skip={(0, 0)})).write(out / "burst-x-000.ppm")
            Frame(grid(COLUMNS, ROWS)).write(out / "burst-x-001.ppm")
            Frame(grid(COLUMNS, ROWS)).write(out / "burst-x-settled.ppm")
            burst = {"name": "x", "t0": 10.0, "frames": [{"file": "burst-x-000.ppm", "t": 10.2},
                                                           {"file": "burst-x-001.ppm", "t": 10.4}],
                     "settled": {"file": "burst-x-settled.ppm", "t": 12.0}}
            (out / "result.json").write_text(json.dumps({"ok": True, "error": None, "app_peak_rss_mib": 1.0,
                                                         "bench": {"checkpoints": [], "bursts": [burst]}}))
            merged = pb.merge(SimpleNamespace(label="t", cpus="6-11"), {"name": "s"}, out, 0, {}, 0, {})
            self.assertTrue(merged["ok"], merged["error"])
            self.assertEqual(merged["late"]["x"]["unsettled"], [1, 0])
            self.assertEqual(list(out.glob("*.ppm")), [])

    def test_a_burst_that_cannot_be_read_fails_the_run(self):
        with tempfile.TemporaryDirectory() as name:
            out = Path(name) / "run-0.d"
            out.mkdir()
            burst = {"name": "x", "t0": 1.0, "frames": [], "settled": {"file": "missing.ppm", "t": 2.0}}
            (out / "result.json").write_text(json.dumps({"ok": True, "error": None, "app_peak_rss_mib": 1.0,
                                                         "bench": {"checkpoints": [], "bursts": [burst]}}))
            merged = pb.merge(SimpleNamespace(label="t", cpus="6-11"), {"name": "s"}, out, 0, {}, 0, {})
            self.assertFalse(merged["ok"])
            self.assertIn("burst", merged["error"])
