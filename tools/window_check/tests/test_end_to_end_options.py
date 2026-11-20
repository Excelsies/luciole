"""Real runs of the options and steps that reach into the app: --env, --profile, close-window and a failed step."""

import shutil
import subprocess
import unittest

from e2e_support import command, make_work_folder, private_mounts, private_processes, real_run, result_of
from images import png_pixel, png_size
from run_options import PROFILE_MARK

GRID_SETTING = 'view_mode = "grid"'
# The Grid and List view buttons of the toolbar at 1400x900, and the fill of the checked one.
GRID_BUTTON = (1196, 60)
LIST_BUTTON = (1229, 60)
HIGHLIGHT = (247, 233, 168)


@real_run
class OptionsEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work, cls.binary, cls.folder = make_work_folder()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def run_tool(self, name: str, *arguments: str) -> tuple[subprocess.CompletedProcess, dict]:
        output = self.work / name
        words = command(self.binary, self.folder, output, *arguments)
        run = subprocess.run(words, capture_output=True, text=True, timeout=180)
        return run, result_of(output)

    def assert_clean(self, result: dict) -> None:
        self.assertTrue(result["leftover_check"]["ok"], result["leftover_check"])
        self.assertTrue(result["supervisor"]["ok"], result["supervisor"])
        self.assertEqual(private_processes(), [])
        self.assertEqual(private_mounts(), [])

    def test_a_failed_step_prints_its_error_on_stderr(self):
        run, result = self.run_tool("failed-step", "move:5000,5000", "shot:never")
        self.assertEqual(run.returncode, 1)
        self.assertIn("error: step 1 'move:5000,5000' failed: position", run.stderr)
        self.assertEqual([step["ok"] for step in result["steps"]], [False])
        self.assertGreaterEqual(result["steps"][0]["duration_s"], 0.0)
        self.assert_clean(result)

    def test_close_window_lets_the_app_exit_and_the_run_go_on(self):
        run, result = self.run_tool("close", "settle", "close-window", "settle", "shot:after-close")
        self.assertEqual(run.returncode, 0, f"{result.get('error')}\n{run.stderr}")
        self.assertEqual(result["app_exit"], {"close_requested": True, "exited": True, "code": 0})
        self.assertIn("the app exited", result["steps"][1]["detail"])
        self.assertTrue(result["steps"][1]["window_id"])
        self.assertEqual(png_size(self.work / "close" / "after-close.png"), (1400, 900))
        self.assert_clean(result)

    def test_a_pointer_step_after_the_app_closed_fails(self):
        run, result = self.run_tool("close-then-move", "settle", "close-window", "move:10,10")
        self.assertEqual(run.returncode, 1)
        self.assertIn("error: step 3 'move:10,10' failed: the app has no window any more", run.stderr)
        self.assert_clean(result)

    def test_a_run_without_close_window_records_that_the_app_did_not_exit(self):
        run, result = self.run_tool("no-close", "settle", "--env", "GST_DEBUG=1", "--env", "G_MESSAGES_DEBUG=all")
        self.assertEqual(run.returncode, 0, f"{result.get('error')}\n{run.stderr}")
        self.assertEqual(result["app_exit"], {"close_requested": False, "exited": False, "code": None})
        self.assertEqual(result["app_environment_added"], {"GST_DEBUG": "1", "G_MESSAGES_DEBUG": "all"})
        # GLib prints debug messages only when G_MESSAGES_DEBUG reached the app.
        self.assertIn("-DEBUG:", (self.work / "no-close" / "app.log").read_text(errors="replace"))
        self.assertGreater(result["app_peak_rss_mib"], 10, run.stderr)
        self.assert_clean(result)

    def checked_view(self, screenshot) -> str:
        """Which of the Grid and List toolbar buttons is highlighted in a 1400x900 screenshot."""
        grid, listed = png_pixel(screenshot, *GRID_BUTTON), png_pixel(screenshot, *LIST_BUTTON)
        if grid == HIGHLIGHT and listed != HIGHLIGHT:
            return "grid"
        if listed == HIGHLIGHT and grid != HIGHLIGHT:
            return "list"
        return f"neither: grid button {grid}, list button {listed}"

    def test_a_kept_profile_brings_a_saved_choice_back_and_a_run_without_it_starts_clean(self):
        profile = self.work / "profile"
        first, result = self.run_tool("profile-1", "--profile", str(profile), "settle", "shot:before", "key:ctrl+1",
                                      "settle", "shot:grid", "wait:1500")
        self.assertEqual(first.returncode, 0, f"{result.get('error')}\n{first.stderr}")
        self.assertEqual(self.checked_view(self.work / "profile-1" / "before.png"), "list")
        self.assertEqual(self.checked_view(self.work / "profile-1" / "grid.png"), "grid")
        self.assertIn(GRID_SETTING, (profile / "config" / "luciole" / "settings.toml").read_text())

        second, result = self.run_tool("profile-2", "--profile", str(profile), "settle", "shot:restarted")
        self.assertEqual(second.returncode, 0, f"{result.get('error')}\n{second.stderr}")
        self.assertEqual(result["profile"], str(profile.resolve()))
        self.assertEqual(self.checked_view(self.work / "profile-2" / "restarted.png"), "grid")
        self.assert_clean(result)

        third, result = self.run_tool("profile-none", "settle", "shot:clean")
        self.assertEqual(third.returncode, 0, f"{result.get('error')}\n{third.stderr}")
        self.assertEqual(result["profile"], "")
        self.assertEqual(self.checked_view(self.work / "profile-none" / "clean.png"), "list")

    def test_the_compositor_never_writes_through_a_link_the_app_left_in_a_kept_profile(self):
        profile = self.work / "linked-profile"
        outside = self.work / "owner-notes.txt"
        outside.write_text("owner data\n")
        (profile / "cache" / "umbriel").mkdir(parents=True)
        (profile / PROFILE_MARK).write_text("")
        # Umbriel appends its log to $XDG_CACHE_HOME/umbriel/umbriel.log.
        (profile / "cache" / "umbriel" / "umbriel.log").symlink_to(outside)
        run, result = self.run_tool("linked-run", "--profile", str(profile), "settle")
        self.assertEqual(run.returncode, 0, f"{result.get('error')}\n{run.stderr}")
        self.assertEqual(outside.read_text(), "owner data\n")
        self.assertIn("/dev/shm", result["app"]["hidden_folders"])
        self.assert_clean(result)


if __name__ == "__main__":
    unittest.main()
