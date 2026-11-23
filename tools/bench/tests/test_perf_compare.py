"""Flattening, summaries and the before and after comparison."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import perf_compare as pc  # noqa: E402


def memory(pss: int, workers_pss: int = 0, dirty: int = 4096) -> dict:
    classes = {name: {"count": 1, "rss": 0, "pss": 0} for name in
               ("heap", "arena", "stack", "anon_other", "file", "device", "special")}
    classes["heap"]["rss"] = 1024
    app = {"rss_kib": pss, "peak_rss_kib": pss + 1024, "classes": classes, "shared_file_kib": 2048,
           "rollup": {"Rss": pss + 512, "Pss": pss, "Private_Dirty": dirty, "Private_Clean": 1024, "Anonymous": 8192, "Swap": 3072}}
    sums = {"Rss": workers_pss, "Pss": workers_pss, "Private_Dirty": 0, "Private_Clean": 0, "Anonymous": 0,
            "peak_rss_kib": workers_pss}
    return {"app": app, "workers": {"count": 1 if workers_pss else 0, "sum_kib": sums},
            "tree_pss_kib": pss + workers_pss, "tree_rss_kib": pss + workers_pss}


def point(name: str, pss: int, cpu: float, quiet: float | None = None, dirty: int = 4096) -> dict:
    found = {"name": name, "since_launch_s": 2.0, "cpu_s": {"total": cpu}, "cpu_since_previous_s": cpu,
             "wall_since_previous_s": None, "threads": {"app": 11, "tree": 12}, "memory": memory(pss, 2048, dirty),
             "busy_until_s": 1.5}
    if quiet is not None:
        found["quiet"] = {"cpu_ms": quiet, "wakeups_per_s": 3.0, "app_wakeups_per_s": 2.0, "threads_started": 0}
    return found


def run(pss: int, cpu: float = 1.0, ok: bool = True, dirty: int = 4096) -> dict:
    return {"ok": ok, "error": None if ok else "boom", "app_peak_rss_mib": 300.0, "startup": {"first-frame": 250.0},
            "checkpoints": [point("settled", pss, cpu, quiet=0.0, dirty=dirty)]}


class Flatten(unittest.TestCase):
    def test_numbers_are_in_mebibytes_and_named_by_checkpoint(self):
        flat = pc.flatten(run(102400, 1.5))
        self.assertEqual(flat[("settled", "app.pss_mib")], 100.0)
        self.assertEqual(flat[("settled", "workers.pss_mib")], 2.0)
        self.assertEqual(flat[("settled", "tree.pss_mib")], 102.0)
        self.assertEqual(flat[("settled", "app.shared_file_mib")], 2.0)
        self.assertEqual(flat[("settled", "app.swap_mib")], 3.0)
        self.assertEqual(flat[("settled", "app.peak_rss_mib")], 101.0)
        self.assertEqual(flat[("settled", "quiet.wakeups_per_s")], 3.0)
        self.assertEqual(flat[("startup", "first-frame_ms")], 250.0)
        self.assertEqual(flat[("run", "peak_tree_rss_mib")], 300.0)

    def test_a_checkpoint_without_a_quiet_window_has_no_quiet_metrics(self):
        item = run(1024)
        del item["checkpoints"][0]["quiet"]
        self.assertNotIn(("settled", "quiet.cpu_ms"), pc.flatten(item))


class Summary(unittest.TestCase):
    def test_median_and_spread_over_runs_and_failed_runs_are_left_out(self):
        runs = [run(102400), run(104448), run(106496), run(999999, ok=False)]
        item = pc.summarize(runs)[("settled", "app.pss_mib")]
        self.assertEqual((item["n"], item["median"], item["min"], item["max"], item["spread"]),
                         (3, 102.0, 100.0, 104.0, 4.0))

    def test_failures_are_listed_with_their_error(self):
        self.assertEqual(pc.failures({"a": [run(1), run(1, ok=False)]}), ["a run 1: boom"])


class Compare(unittest.TestCase):
    def rows(self, before, after):
        rows = pc.compare({"s": before}, {"s": after})
        return {(row["checkpoint"], row["metric"]): row for row in rows}

    def test_a_change_beyond_the_spread_is_flagged_and_one_within_it_is_noise(self):
        before = [run(102400), run(104448), run(103424)]
        after = [run(92160), run(93184), run(94208)]
        rows = self.rows(before, after)
        pss = rows[("settled", "app.pss_mib")]
        self.assertEqual(pss["verdict"], "lower")
        self.assertAlmostEqual(pss["delta"], -10.0)
        self.assertAlmostEqual(pss["percent"], -9.901, places=3)
        noise = self.rows(before, [run(103424 + 512), run(104448), run(102400)])[("settled", "app.pss_mib")]
        self.assertEqual(noise["verdict"], "noise")

    def test_a_single_run_has_no_spread_so_the_change_is_marked_n_equals_1(self):
        rows = self.rows([run(102400)], [run(92160)])
        self.assertEqual(rows[("settled", "app.pss_mib")]["verdict"], "n=1")
        self.assertEqual(rows[("settled", "threads.app")]["verdict"], "same")

    def test_zero_before_gives_no_percent(self):
        rows = self.rows([run(1024, 1.0), run(1024, 1.0)], [run(1024, 1.0), run(1024, 1.0)])
        self.assertEqual(rows[("settled", "quiet.cpu_ms")]["percent"], None)

    def test_scenarios_in_only_one_set_are_not_compared(self):
        self.assertEqual(pc.compare({"a": [run(1)]}, {"b": [run(1)]}), [])

    def test_table_can_hide_noise(self):
        rows = pc.compare({"s": [run(102400), run(104448)]}, {"s": [run(102400), run(104448)]})
        self.assertNotIn("app.pss_mib", pc.table(rows, significant_only=True))
        self.assertIn("app.pss_mib", pc.table(rows))


class LoadSet(unittest.TestCase):
    def test_reads_run_files_per_scenario_and_refuses_an_empty_folder(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            with self.assertRaises(pc.ResultError):
                pc.load_set(root)
            (root / "alpha").mkdir()
            (root / "alpha" / "run-0.json").write_text(json.dumps(run(1024)))
            (root / "alpha" / "run-1.json").write_text(json.dumps(run(2048)))
            self.assertEqual({key: len(value) for key, value in pc.load_set(root).items()}, {"alpha": 2})

    def test_summary_markdown_lists_checkpoints(self):
        text = pc.summary_markdown({"alpha": [run(102400), run(104448)]})
        self.assertIn("### alpha", text)
        self.assertIn("| settled |", text)
        self.assertIn("startup (ms since launch): first-frame 250.0", text)


if __name__ == "__main__":
    unittest.main()


class AfterQuietAndRealtime(unittest.TestCase):
    def test_memory_after_the_quiet_window_and_the_playback_ratio_are_metrics(self):
        found = run(100000)
        found["checkpoints"][0]["after_quiet"] = {
            "rss_kib": 2048, "peak_rss_kib": 4096, "classes": {}, "workers_sum_kib": {"Pss": 1024},
            "rollup": {"Private_Dirty": 3072, "Anonymous": 5120}}
        found["realtime"] = {"playing": {"ratio": 0.99, "advance_s": 9.9}}
        flat = pc.flatten(found)
        self.assertEqual(flat[("settled", "after_quiet.private_dirty_mib")], 3.0)
        self.assertEqual(flat[("settled", "after_quiet.anonymous_mib")], 5.0)
        self.assertEqual(flat[("settled", "after_quiet.workers_pss_mib")], 1.0)
        self.assertEqual(flat[("playing", "realtime.ratio")], 0.99)


class Playback(unittest.TestCase):
    def test_the_picture_counts_of_all_playbacks_of_a_run_are_summed(self):
        found = run(100000)
        found["playback"] = [{"delivered": 301, "taken": 250, "replaced": 51, "missed": 2},
                             {"delivered": 10, "taken": 10, "replaced": 0, "missed": 1}]
        flat = pc.flatten(found)
        self.assertEqual(flat[("run", "playback.delivered")], 311)
        self.assertEqual(flat[("run", "playback.missed")], 3)
        self.assertEqual(flat[("run", "playback.replaced")], 51)
        self.assertEqual(flat[("run", "playback.taken")], 260)


class LateTiles(unittest.TestCase):
    def test_a_burst_gives_the_time_to_the_settled_picture_and_the_worst_frame(self):
        found = run(100000)
        found["late"] = {"return-70s-top": {"tiles": 63, "unsettled": [63, 20, 0], "times_s": [0.1, 0.2, 0.3],
                                             "frames": 3, "first_all_settled_s": 0.3, "last_unsettled_s": 0.2,
                                             "interval_s": 0.1, "empty": [63, 5, 0], "last_empty_s": 0.2}}
        flat = pc.flatten(found)
        self.assertEqual(flat[("return-70s-top", "late.first_all_settled_s")], 0.3)
        self.assertEqual(flat[("return-70s-top", "late.last_unsettled_s")], 0.2)
        self.assertEqual(flat[("return-70s-top", "late.max_unsettled")], 63.0)
        self.assertEqual(flat[("return-70s-top", "late.frames")], 3.0)
        self.assertEqual(flat[("return-70s-top", "late.interval_s")], 0.1)
        self.assertEqual(flat[("return-70s-top", "late.never_settled")], 0.0)
        self.assertEqual(flat[("return-70s-top", "late.max_empty")], 63.0)
        self.assertEqual(flat[("return-70s-top", "late.empty_frames")], 2.0)

    def test_a_burst_that_never_settles_is_marked_and_has_no_time(self):
        found = run(100000)
        found["late"] = {"b": {"tiles": 9, "unsettled": [9, 9], "times_s": [0.1, 0.2], "frames": 2,
                               "first_all_settled_s": None, "last_unsettled_s": 0.2, "interval_s": 0.1}}
        flat = pc.flatten(found)
        self.assertNotIn(("b", "late.first_all_settled_s"), flat)
        self.assertEqual(flat[("b", "late.never_settled")], 1.0)
