"""Exit codes of the command line for options it refuses before starting anything."""

import os
import pwd
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1]
PLAIN = {"PATH": "/usr/bin:/bin"}


def run(*arguments: str) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(TOOL / "window_check.py"), "run", *arguments, "key:Escape"],
                          env=PLAIN, capture_output=True, text=True, timeout=60)


class RefusedOptions(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name)
        self.fixture = self.root / "fixture"
        self.fixture.mkdir()
        self.binary = self.root / "luciole"
        self.binary.write_text("")
        self.binary.chmod(0o755)

    def assert_invalid(self, result: subprocess.CompletedProcess, text: str) -> None:
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn(text, result.stderr)

    def test_the_home_folder_is_an_invalid_option(self):
        home = pwd.getpwuid(os.getuid()).pw_dir
        result = run("--binary", str(self.binary), "--folder", home, "--output", str(self.root / "out"))
        self.assert_invalid(result, "refusing")

    def test_a_missing_binary_is_an_invalid_option(self):
        result = run("--binary", str(self.root / "missing"), "--folder", str(self.fixture),
                     "--output", str(self.root / "out"))
        self.assert_invalid(result, "not an executable file")

    def test_an_output_inside_the_folder_is_an_invalid_option(self):
        result = run("--binary", str(self.binary), "--folder", str(self.fixture),
                     "--output", str(self.fixture / "out"))
        self.assert_invalid(result, "refusing output")

    def plain_run(self, *options: str) -> subprocess.CompletedProcess:
        return run("--binary", str(self.binary), "--folder", str(self.fixture), "--output", str(self.root / "out"),
                   *options)

    def test_an_env_name_outside_the_allow_list_is_an_invalid_option(self):
        for word in ("HOME=/tmp", "LD_PRELOAD=/tmp/x.so", "XDG_RUNTIME_DIR=/run/user/1000", "WAYLAND_DISPLAY=w-1",
                     "DBUS_SESSION_BUS_ADDRESS=unix:path=/x", "DISPLAY=:0", "PATH=/tmp"):
            self.assert_invalid(self.plain_run("--env", word), "--env refuses")

    def test_an_env_name_the_tool_sets_is_an_invalid_option(self):
        self.assert_invalid(self.plain_run("--env", "GST_DEBUG=2", "--env", "LUCIOLE_ASSETS_DIR=/x"),
                            "the tool sets LUCIOLE_ASSETS_DIR")

    def test_a_profile_outside_the_checkout_is_an_invalid_option(self):
        self.assert_invalid(self.plain_run("--profile", str(self.root / "profile")), "must lie inside")
        self.assertFalse((self.root / "profile").exists())

    def test_a_profile_in_the_run_folder_is_an_invalid_option(self):
        run_folder = TOOL.parents[1] / ".cache" / "harness" / "run"
        self.assert_invalid(self.plain_run("--profile", str(run_folder / "profile")), "overlaps")


if __name__ == "__main__":
    unittest.main()
