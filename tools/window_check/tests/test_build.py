"""The compositor build: the not-built message and a repeated build that does nothing."""

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL))

from build import (  # noqa: E402
    HARNESS,
    UMBRIEL_COMMIT,
    CompositorMissing,
    build_environment,
    built_commit,
    require_compositor,
)


class NotBuilt(unittest.TestCase):
    def test_missing_compositor_names_the_command_that_builds_it(self):
        with tempfile.TemporaryDirectory() as prefix:
            with self.assertRaises(CompositorMissing) as caught:
                require_compositor(Path(prefix))
        self.assertIn("window_check.py build-compositor", str(caught.exception))

    def test_pinned_commit_is_a_full_hash(self):
        self.assertRegex(UMBRIEL_COMMIT, r"^[0-9a-f]{40}$")


class BuildEnvironment(unittest.TestCase):
    CALLER = {"PATH": "/usr/bin:/bin", "LANG": "en_US.UTF-8", "HOME": "/home/owner", "WAYLAND_DISPLAY": "wayland-1",
              "DISPLAY": ":0", "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
              "SSH_AUTH_SOCK": "/run/user/1000/ssh", "XDG_CONFIG_HOME": "/home/owner/.config",
              "GIT_CONFIG_GLOBAL": "/home/owner/.gitconfig"}

    def test_build_children_see_no_session_and_no_user_git_configuration(self):
        env = build_environment(self.CALLER)
        self.assertEqual((env["PATH"], env["LANG"]), ("/usr/bin:/bin", "en_US.UTF-8"))
        self.assertTrue(Path(env["HOME"]).is_relative_to(HARNESS))
        self.assertEqual((env["GIT_CONFIG_NOSYSTEM"], env["GIT_CONFIG_GLOBAL"], env["GIT_TERMINAL_PROMPT"]),
                         ("1", "/dev/null", "0"))
        for value in env.values():
            self.assertNotIn("/home/owner", value)
            self.assertNotIn("/run/user/1000", value)
        self.assertEqual(set(env) - {"PATH", "LANG", "HOME", "GIT_CONFIG_NOSYSTEM", "GIT_CONFIG_GLOBAL",
                                     "GIT_TERMINAL_PROMPT"}, set())


class Rebuild(unittest.TestCase):
    def test_second_build_does_nothing_and_says_so(self):
        if built_commit() != UMBRIEL_COMMIT:
            self.skipTest("the compositor is not built yet: run window_check.py build-compositor")
        result = subprocess.run([sys.executable, str(TOOL / "window_check.py"), "build-compositor"],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"already built at {UMBRIEL_COMMIT}", result.stdout)


if __name__ == "__main__":
    unittest.main()
