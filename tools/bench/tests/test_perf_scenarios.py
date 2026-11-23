"""The scenario definitions: every step parses, every checkpoint is named once, the hash follows the content."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import perf_session  # noqa: E402,F401  (puts tools/window_check on the path)
import perf_scenarios as ps  # noqa: E402
from perf_one import resolve  # noqa: E402
from steps import parse_step  # noqa: E402

MANIFEST = {"kinds": {"entries": 27, "folders": 1}}
EXPECTED = ["start-details", "list-20k", "list-20k-icons", "workbench-100k", "previews", "pictures", "pictures-rgba",
            "pictures-warm", "pictures-leave", "pictures-return", "pictures-scrollback", "video", "video-bink", "video-4k", "video-60", "video-hevc-4k60", "video-hevc-4k30", "video-av1-4k60", "video-av1-4k30",
            "video-vp9-4k60", "video-vp9-4k30", "video-hevc10-4k60", "video-end", "video-switch", "video-tab-playing", "video-tab-paused", "text-big", "churn", "tabs", "previews-rest", "churn-rest"]


def walk(actions):
    for item in actions:
        yield item
        if item[0] == "repeat":
            yield from walk(item[2])


class Scenarios(unittest.TestCase):
    def setUp(self):
        self.all = ps.scenarios(MANIFEST)

    def test_workbench_list_rows_are_hit_at_their_centers(self):
        # Measured on screenshots of the merged build at 1920x1080: rows are 42 px tall and the first spans 390 to 432.
        centers = [411, 453, 495, 537]
        self.assertEqual([ps.ROW_ONE[1], ps.ROW_TWO[1], ps.ROW_THREE[1], ps.ROW_FOUR[1]], centers)

    def test_the_stock_set_names_real_scenarios_that_never_need_the_workbench(self):
        import perf_bench
        for name in perf_bench.STOCK_SCENARIOS.split(","):
            self.assertIn(name, self.all)
            self.assertNotIn("workbench", self.all[name]["settings"])
        self.assertEqual(perf_bench.STOCK_SCENARIOS.split(","),
                         ["start-details", "list-20k", "list-20k-icons", "pictures", "pictures-warm", "tabs"])

    def test_every_playback_scenario_reads_the_position_and_ends_by_leaving_the_folder(self):
        for name in ("video", "video-bink", "video-4k", "video-60"):
            names = [item[1] for item in walk(self.all[name]["steps"]) if item[0] == "cp"]
            self.assertEqual(names[-1], "left-folder")
            self.assertIn("paused-40s", names)
            resumed = next(item for item in walk(self.all[name]["steps"]) if item[:2] == ("cp", "resumed"))
            self.assertEqual(resumed[2]["hold"], 10)
            self.assertLess(names.index("paused-40s"), names.index("resumed"))
            playing = next(item for item in walk(self.all[name]["steps"]) if item[:2] == ("cp", "playing-10s"))
            self.assertEqual(playing[2]["hold"], 10)
            self.assertGreater(playing[2]["realtime"], 10)

    def test_the_video_scenario_ends_on_a_selection_that_holds_no_media(self):
        names = [item[1] for item in walk(self.all["video"]["steps"]) if item[0] == "cp"]
        self.assertLess(names.index("paused-40s"), names.index("nonmedia-selected"))
        self.assertLess(names.index("nonmedia-selected"), names.index("left-folder"))

    def test_each_leaving_scenario_plays_first_and_ends_on_its_own_checkpoint(self):
        for name, last in (("video-switch", "media-selected"), ("video-tab-playing", "tab-hidden"),
                           ("video-tab-paused", "tab-hidden")):
            names = [item[1] for item in walk(self.all[name]["steps"]) if item[0] == "cp"]
            self.assertEqual((names[-1], "playing" in names), (last, True))
        paused = [item[1] for item in walk(self.all["video-tab-paused"]["steps"]) if item[0] == "cp"]
        self.assertLess(paused.index("paused"), paused.index("tab-hidden"))

    def test_a_hard_clip_scenario_plays_ten_seconds_with_the_position_read_and_is_left(self):
        for name in ("video-hevc-4k60", "video-av1-4k30", "video-vp9-4k60"):
            steps = list(walk(self.all[name]["steps"]))
            playing = next(item for item in steps if item[:2] == ("cp", "playing-10s"))
            self.assertEqual((playing[2]["hold"], playing[2]["realtime"]), (10, 30))
            self.assertEqual([item[1] for item in steps if item[0] == "cp"][-1], "left-folder")

    def test_the_end_scenario_waits_past_the_end_of_the_short_clip(self):
        steps = self.all["video-end"]["steps"]
        self.assertGreater(sum(item[1] for item in steps if item[0] == "wait"), 3)
        self.assertIn("ended", [item[1] for item in steps if item[0] == "cp"])

    def test_leaving_the_pictures_samples_memory_at_the_agreed_times_after_the_leave(self):
        steps = self.all["pictures-leave"]["steps"]
        self.assertEqual(self.all["pictures-leave"]["prime"], "pictures")
        self.assertEqual(self.all["pictures-leave"]["profile"], "kept")
        mark = steps.index(("mark", "left"))
        self.assertEqual(steps[mark + 1], ("do", "key:Return"))
        after = steps[mark + 2:]
        self.assertEqual([item[1:] for item in after if item[0] == "until"],
                         [("left", seconds) for seconds in (0, 35, 70, 130, 360)])
        names = [item[1] for item in after if item[0] == "cp"]
        self.assertEqual(names, [f"left-{seconds}s" for seconds in (0, 35, 70, 130, 360)])
        self.assertTrue(after[1][2]["settle"] is False and "settle" not in after[3][2])

    def test_the_picture_scenarios_page_through_the_whole_folder_before_they_leave(self):
        # 300 pictures at nine per row and about seven rows a page: eight pages reach the end, so every thumbnail is asked for.
        for name in ("pictures-leave", "pictures-return", "pictures-scrollback"):
            steps = list(walk(self.all[name]["steps"]))
            paging = [item for item in self.all[name]["steps"] if item[0] == "repeat"]
            self.assertEqual(paging[0][:2], ("repeat", 8), name)
            self.assertIn(("do", "key:Page_Down"), steps)

    def test_a_scenario_never_asks_to_sample_before_its_mark_exists(self):
        for name, scenario in self.all.items():
            marks = set()
            for item in walk(scenario["steps"]):
                if item[0] == "mark":
                    marks.add(item[1])
                if item[0] == "until":
                    self.assertIn(item[1], marks, name)
                if item[0] == "burst":
                    self.assertIn(item[2]["mark"], marks, name)

    def test_returning_to_the_pictures_measures_late_tiles_at_the_top_and_at_the_end_after_70_and_130_s(self):
        steps = self.all["pictures-return"]["steps"]
        bursts = [item[1] for item in steps if item[0] == "burst"]
        self.assertEqual(bursts, ["return-70s-top", "return-70s-end", "return-130s-top", "return-130s-end"])
        for item in steps:
            if item[0] == "burst":
                self.assertGreaterEqual(item[2]["seconds"], 4)
                self.assertLessEqual(item[2]["seconds"], 10)
        until = [item[2] for item in steps if item[0] == "until"]
        self.assertIn(70, until)
        self.assertIn(130, until)
        # The click that returns is the last thing before its burst's first screenshot.
        first = steps.index(next(item for item in steps if item[0] == "burst"))
        self.assertEqual(steps[first - 1], ("do", "key:Return"))

    def test_scrolling_back_up_after_70_and_130_s_at_the_far_end_measures_late_tiles(self):
        steps = self.all["pictures-scrollback"]["steps"]
        self.assertEqual([item[1] for item in steps if item[0] == "burst"], ["scrollback-70s", "scrollback-130s"])
        self.assertEqual([item[2] for item in steps if item[0] == "until"], [70, 130])
        presses = [item for item in walk(steps) if item[0] == "do" and item[1] in ("key:End", "key:Home")]
        self.assertEqual([item[1] for item in presses], ["key:End", "key:Home", "key:End", "key:Home"])

    def test_the_rest_variants_are_the_same_run_followed_by_samples_after_the_last_use(self):
        for plain, rest in (("previews", "previews-rest"), ("churn", "churn-rest")):
            base, more = self.all[plain]["steps"], self.all[rest]["steps"]
            self.assertEqual(more[:len(base)], base)
            tail = more[len(base):]
            self.assertEqual([item[1:] for item in tail if item[0] == "until"],
                             [("done", seconds) for seconds in (35, 70, 130)])
            self.assertEqual([item[1] for item in tail if item[0] == "cp"], ["rest-35s", "rest-70s", "rest-130s"])
            self.assertEqual(tail[0], ("mark", "done"))

    def test_the_rgba_pictures_scenario_is_the_pictures_scenario_on_the_opaque_alpha_folder(self):
        plain, rgba = self.all["pictures"], self.all["pictures-rgba"]
        self.assertEqual(rgba["steps"], plain["steps"])
        self.assertEqual(rgba["start"], "pictures-rgba-300")
        self.assertEqual(plain["start"], "pictures-300")

    def test_the_named_scenarios_exist(self):
        self.assertEqual(list(self.all), EXPECTED)

    def test_every_input_word_is_a_valid_window_check_step(self):
        for scenario in self.all.values():
            for item in resolve(scenario["steps"], Path("/fixtures")):
                if item[0] == "do":
                    parse_step(item[1])

    def test_checkpoint_names_are_unique_within_a_scenario_and_waits_are_bounded(self):
        for name, scenario in self.all.items():
            names = [item[1] for item in walk(scenario["steps"]) if item[0] == "cp"]
            self.assertEqual(len(names), len(set(names)), name)
            for item in walk(scenario["steps"]):
                if item[0] == "wait":
                    self.assertLessEqual(item[1], 40)
                if item[0] == "until":
                    self.assertLessEqual(item[2], 360)
                if item[0] == "cp" and len(item) > 2:
                    self.assertLessEqual(item[2].get("settle_timeout", 30), 300)

    def test_previews_and_churn_follow_the_fixture_entry_count(self):
        presses = lambda scenario: sum(item == ("do", "key:Down") for item in scenario["steps"])  # noqa: E731
        self.assertEqual(presses(self.all["previews"]), 27)
        self.assertEqual(presses(self.all["churn"]), 27 * 5)
        other = ps.scenarios({"kinds": {"entries": 5, "folders": 1}})
        self.assertEqual(presses(other["previews"]), 5)

    def test_the_warm_run_primes_with_the_cold_scenario_and_one_profile(self):
        self.assertEqual(self.all["pictures-warm"]["prime"], "pictures")
        self.assertEqual(self.all["pictures-warm"]["profile"], "kept")
        self.assertEqual(self.all["pictures"]["profile"], "fresh")
        self.assertEqual(self.all["video-bink"]["needs_env"], "LUCIOLE_BINK2_SAMPLE")

    def test_hash_is_stable_and_changes_with_the_definition(self):
        first = ps.definition_hash(self.all["tabs"])
        self.assertEqual(first, ps.definition_hash(ps.scenarios(MANIFEST)["tabs"]))
        changed = dict(self.all["tabs"], steps=self.all["tabs"]["steps"] + [("wait", 1)])
        self.assertNotEqual(first, ps.definition_hash(changed))

    def test_root_is_substituted_in_typed_paths_only_at_run_time(self):
        steps = [("do", "type:{root}/small")]
        self.assertEqual(resolve(steps, Path("/data/fx")), [("do", "type:/data/fx/small")])
        self.assertIn("{root}", str(self.all["workbench-100k"]["steps"]))


if __name__ == "__main__":
    unittest.main()
