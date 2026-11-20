"""Profile folders of a run: new for every run, or kept between runs in a folder that only plain folders fill."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from compositor import CompositorError  # noqa: E402
from isolation import IsolationError, Profile, real_folder  # noqa: E402
from session import Options, Session  # noqa: E402


class KeptProfileFolders(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.base = Path(folder.name)
        self.run = self.base / "run"
        self.kept = self.base / "kept"
        self.kept.mkdir()

    def test_without_a_kept_folder_every_run_starts_clean(self):
        profile = Profile(self.run)
        profile.create({})
        (profile.state / "luciole").mkdir()
        Profile(self.run).create({})
        self.assertEqual(list(profile.state.iterdir()), [])
        self.assertEqual(profile.writable(), [self.run])

    def test_a_kept_folder_holds_home_and_xdg_folders_and_survives_the_next_run(self):
        profile = Profile(self.run, self.kept)
        profile.create({})
        for folder in (profile.home, profile.config, profile.data, profile.state, profile.cache):
            self.assertEqual(folder.parent, self.kept)
            self.assertTrue(folder.is_dir())
        self.assertEqual(profile.runtime, self.run / "runtime")
        (profile.state / "saved.toml").write_text("width = 300\n")
        (self.run / "leftover").write_text("")
        Profile(self.run, self.kept).create({})
        self.assertEqual((profile.state / "saved.toml").read_text(), "width = 300\n")
        self.assertFalse((self.run / "leftover").exists())
        self.assertEqual(profile.writable(), [self.run, self.kept])

    def test_a_kept_folder_with_a_link_in_place_of_a_folder_is_refused(self):
        elsewhere = self.base / "elsewhere"
        elsewhere.mkdir()
        (self.kept / "config").symlink_to(elsewhere)
        with self.assertRaises(IsolationError) as caught:
            Profile(self.run, self.kept).create({})
        self.assertIn("not a plain folder", str(caught.exception))
        self.assertEqual(list(elsewhere.iterdir()), [])

    def test_a_file_in_place_of_a_folder_is_refused(self):
        (self.kept / "state").write_text("")
        with self.assertRaises(IsolationError):
            Profile(self.run, self.kept).create({})

    def test_real_folder_creates_a_missing_folder_once(self):
        self.assertEqual(real_folder(self.kept / "new"), self.kept / "new")
        self.assertEqual(real_folder(self.kept / "new"), self.kept / "new")
        self.assertTrue((self.kept / "new").is_dir())


# Appends a line to umbriel/umbriel.log under every folder its environment names, following links as Umbriel's log
# does.
CLIENT = """\
#!/bin/sh
for folder in "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_STATE_HOME" "$XDG_CACHE_HOME"; do
    mkdir -p "$folder/umbriel" && printf 'compositor line\\n' >> "$folder/umbriel/umbriel.log"
done
"""
CALLER = {"PATH": "/usr/bin:/bin"}
FOLDERS = ("home", "config", "data", "state", "cache")


class KeptProfileOutsideTheSandbox(unittest.TestCase):
    """The app can leave links in a kept profile; children outside the sandbox must never write through them."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.base = Path(folder.name)
        self.kept = self.base / "kept"
        self.kept.mkdir()
        self.outside = self.base / "owner-notes.txt"
        self.outside.write_text("owner data\n")
        client = self.base / "umbriel"
        client.write_text(CLIENT)
        client.chmod(0o755)
        (self.base / "out").mkdir()
        options = Options(Path("/usr/bin/true"), self.base, 1400, 900, 1.0, None, self.base / "out", 10, 3072, {},
                          self.kept)
        self.session = Session(options, [], CALLER, client, "marker", self.base / "run")
        Profile(self.base / "run", self.kept).create(CALLER)
        # What the app could leave in its kept folders during an earlier run.
        for name in FOLDERS:
            (self.kept / name / "umbriel").mkdir()
            (self.kept / name / "umbriel" / "umbriel.log").symlink_to(self.outside)

    def test_the_compositor_and_its_client_never_follow_a_link_left_in_the_kept_profile(self):
        config = self.base / "run" / "umbriel.toml"
        config.write_text("")
        with self.assertRaises(CompositorError):
            # The stand-in validates the configuration, starts and exits at once without a socket.
            self.session.compositor.start(config, self.base / "compositor.log")
        self.session.compositor.close_window("db253ea1ff0da9131d08b4f4fd93b970")
        self.assertEqual(self.outside.read_text(), "owner data\n")
        for name in FOLDERS:
            self.assertEqual((self.base / "run" / name / "umbriel" / "umbriel.log").read_text(),
                             "compositor line\n" * 3, name)

    def test_the_app_and_its_bus_still_get_the_kept_folders(self):
        for env in (self.session.bus_environment(), self.session.app_environment()):
            self.assertEqual(env["HOME"], str(self.kept / "home"))
            self.assertEqual(env["XDG_STATE_HOME"], str(self.kept / "state"))
            self.assertEqual(env["XDG_RUNTIME_DIR"], str(self.base / "run" / "runtime"))


if __name__ == "__main__":
    unittest.main()
