"""Run options that reach into the app's private world: extra app variables and a kept profile."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from isolation import IsolationError  # noqa: E402
from run_options import PROFILE_MARK, app_environment, guard_profile  # noqa: E402

RESERVED = {"HOME", "PATH", "XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "LUCIOLE_ASSETS_DIR"}


class AppEnvironment(unittest.TestCase):
    def assert_refused(self, word: str, fragment: str) -> None:
        with self.assertRaises(ValueError, msg=word) as caught:
            app_environment([word], RESERVED)
        self.assertIn(fragment, str(caught.exception))

    def test_gstreamer_luciole_and_glib_debug_names_are_accepted(self):
        words = ["GST_DEBUG=2", "LUCIOLE_STARTUP_TRACE=1", "G_MESSAGES_DEBUG=all", "GST_DEBUG_NO_COLOR=1"]
        self.assertEqual(app_environment(words, RESERVED), {"GST_DEBUG": "2", "LUCIOLE_STARTUP_TRACE": "1",
                                                            "G_MESSAGES_DEBUG": "all", "GST_DEBUG_NO_COLOR": "1"})

    def test_a_value_keeps_its_equals_signs_and_may_be_empty(self):
        self.assertEqual(app_environment(["GST_DEBUG=a=b", "LUCIOLE_X="], RESERVED),
                         {"GST_DEBUG": "a=b", "LUCIOLE_X": ""})

    def test_no_option_gives_no_variables(self):
        self.assertEqual(app_environment([], RESERVED), {})

    def test_session_and_loader_names_are_refused(self):
        for name in ("HOME", "PATH", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME", "WAYLAND_DISPLAY", "WAYLAND_SOCKET",
                     "DISPLAY", "DBUS_SESSION_BUS_ADDRESS", "DBUS_SYSTEM_BUS_ADDRESS", "LD_PRELOAD",
                     "LD_LIBRARY_PATH", "PULSE_SERVER", "PIPEWIRE_REMOTE", "WINDOW_CHECK_RUN", "GVFS_DISABLE_FUSE"):
            self.assert_refused(f"{name}=x", "only GST_*, LUCIOLE_* and G_MESSAGES_DEBUG")

    def test_names_that_only_look_allowed_are_refused(self):
        for name in ("GST_", "LUCIOLE_", "gst_debug", "G_MESSAGES_DEBUG_X", "XGST_DEBUG", "GST_DE-BUG", "GST_DEBUG "):
            self.assert_refused(f"{name}=x", "only GST_*, LUCIOLE_* and G_MESSAGES_DEBUG")

    def test_a_name_the_tool_sets_is_refused(self):
        self.assert_refused("LUCIOLE_ASSETS_DIR=/elsewhere", "the tool sets LUCIOLE_ASSETS_DIR")

    def test_a_word_without_a_value_or_name_is_refused(self):
        self.assert_refused("GST_DEBUG", "NAME=VALUE")
        self.assert_refused("=2", "NAME=VALUE")

    def test_a_name_given_twice_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            app_environment(["GST_DEBUG=1", "GST_DEBUG=2"], RESERVED)
        self.assertIn("twice", str(caught.exception))

    def test_a_long_value_is_refused(self):
        self.assert_refused("GST_DEBUG=" + "x" * 4097, "4096")


class KeptProfile(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.repo = Path(folder.name) / "repo"
        for name in (".tmp", ".cache", "src"):
            (self.repo / name).mkdir(parents=True)
        self.fixture = self.repo / ".tmp" / "fixture"
        self.fixture.mkdir()
        self.output = self.repo / ".tmp" / "out"
        self.run_folder = self.repo / ".cache" / "harness" / "run"
        self.apart = [self.fixture, self.output, self.run_folder]

    def guard(self, profile: Path) -> Path:
        return guard_profile(profile, self.repo, self.apart)

    def assert_refused(self, profile: Path, fragment: str) -> None:
        with self.assertRaises(IsolationError, msg=str(profile)) as caught:
            self.guard(profile)
        self.assertIn(fragment, str(caught.exception))

    def test_a_new_folder_is_created_and_marked(self):
        profile = self.guard(self.repo / ".tmp" / "unit" / "profile")
        self.assertTrue(profile.is_dir())
        self.assertTrue((profile / PROFILE_MARK).is_file())

    def test_a_marked_folder_is_accepted_again(self):
        first = self.guard(self.repo / ".cache" / "profile")
        (first / "config").mkdir()
        self.assertEqual(self.guard(self.repo / ".cache" / "profile"), first)
        self.assertTrue((first / "config").is_dir())

    def test_an_empty_folder_counts_as_new(self):
        (self.repo / ".tmp" / "empty").mkdir()
        self.assertTrue((self.guard(self.repo / ".tmp" / "empty") / PROFILE_MARK).is_file())

    def test_a_folder_with_files_and_no_mark_is_refused(self):
        folder = self.repo / ".tmp" / "someone-else"
        folder.mkdir()
        (folder / "notes.txt").write_text("keep\n")
        self.assert_refused(folder, PROFILE_MARK)
        self.assertFalse((folder / PROFILE_MARK).exists())

    def test_a_mark_that_is_a_symlink_or_a_folder_is_refused(self):
        folder = self.repo / ".tmp" / "forged"
        folder.mkdir()
        (self.repo / ".tmp" / "real-mark").write_text("")
        (folder / PROFILE_MARK).symlink_to(self.repo / ".tmp" / "real-mark")
        self.assert_refused(folder, PROFILE_MARK)
        other = self.repo / ".tmp" / "mark-folder"
        (other / PROFILE_MARK).mkdir(parents=True)
        self.assert_refused(other, PROFILE_MARK)

    def test_folders_outside_tmp_and_cache_are_refused(self):
        for profile in (self.repo / "src" / "profile", self.repo / "profile", self.repo / ".tmp", self.repo / ".cache",
                        self.repo, Path(tempfile.gettempdir()) / "window-check-profile", Path.home()):
            self.assert_refused(profile, "must lie inside")

    def test_a_symlink_into_tmp_from_elsewhere_counts_where_it_leads(self):
        outside = self.repo / "src" / "target"
        outside.mkdir()
        (self.repo / ".tmp" / "link").symlink_to(outside)
        self.assert_refused(self.repo / ".tmp" / "link", "must lie inside")

    def test_a_profile_overlapping_the_folder_the_output_or_the_run_folder_is_refused(self):
        for profile in (self.fixture, self.fixture / "profile", self.repo / ".tmp", self.output / "profile",
                        self.run_folder, self.run_folder / "profile", self.repo / ".cache" / "harness"):
            with self.assertRaises(IsolationError, msg=str(profile)):
                self.guard(profile)

    def test_a_profile_that_contains_the_folder_is_refused(self):
        inner = self.repo / ".tmp" / "outer" / "fixture"
        inner.mkdir(parents=True)
        with self.assertRaises(IsolationError) as caught:
            guard_profile(self.repo / ".tmp" / "outer", self.repo, [inner])
        self.assertIn("overlaps", str(caught.exception))

    def test_a_file_in_place_of_the_folder_is_refused(self):
        (self.repo / ".tmp" / "file").write_text("")
        self.assert_refused(self.repo / ".tmp" / "file", "not a folder")


if __name__ == "__main__":
    unittest.main()
