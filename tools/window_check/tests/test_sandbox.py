"""The app's file system view: the real home and session folders hidden, only the run's folders bound back."""

import os
import pwd
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL))

from isolation import IsolationError, hidden_folders  # noqa: E402
from processes import descendants, marked, signal_all  # noqa: E402
from sandbox import BWRAP, not_hidden_in, sandbox_command  # noqa: E402

HOME = Path(pwd.getpwuid(os.getuid()).pw_dir).resolve()
PLAIN = {"PATH": "/usr/bin:/bin"}


def listing(folder: Path, writable=(), readable=()) -> list[str]:
    """Names in a folder as a sandboxed child sees them."""
    command = sandbox_command(["ls", "-A", str(folder)], list(writable), list(readable), hidden_folders(PLAIN))
    result = subprocess.run(command, env=PLAIN, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        raise AssertionError(f"the sandbox failed: {result.stderr}")
    return result.stdout.split()


class HiddenFolders(unittest.TestCase):
    def test_the_account_home_and_the_session_runtime_are_hidden(self):
        hidden = hidden_folders({"XDG_RUNTIME_DIR": f"/run/user/{os.getuid()}"})
        self.assertIn(HOME, hidden)
        if Path(f"/run/user/{os.getuid()}").is_dir():
            self.assertIn(Path(f"/run/user/{os.getuid()}").resolve(), hidden)

    def test_the_real_home_is_empty_inside(self):
        # Only the count is reported, so a failure never prints the home folder's names.
        self.assertEqual(len(listing(HOME)), 0, "the sandboxed child sees entries in the real home folder")

    def test_the_session_runtime_is_empty_inside(self):
        runtime = Path(f"/run/user/{os.getuid()}")
        if not runtime.is_dir():
            self.skipTest(f"{runtime} does not exist here")
        self.assertEqual(len(listing(runtime)), 0, "the sandboxed child sees the session's runtime folder")

    def test_a_bound_folder_under_home_shows_only_its_own_path(self):
        names = listing(HOME, readable=[TOOL])
        first = TOOL.relative_to(HOME).parts[0]
        self.assertTrue(names == [first], f"the home folder shows {len(names)} entries, not only the bound path")
        self.assertIn("window_check.py", listing(TOOL, readable=[TOOL]))

    def test_a_writable_folder_is_shared_with_the_caller(self):
        with tempfile.TemporaryDirectory(dir=TOOL.parents[1] / ".cache") as folder:
            target = Path(folder) / "written"
            command = sandbox_command(["touch", str(target)], [Path(folder)], [], hidden_folders(PLAIN))
            result = subprocess.run(command, env=PLAIN, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(target.is_file())

    def test_a_read_only_folder_refuses_writes(self):
        command = sandbox_command(["touch", str(TOOL / "written")], [], [TOOL], hidden_folders(PLAIN))
        result = subprocess.run(command, env=PLAIN, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((TOOL / "written").exists())


class ProcessNamespace(unittest.TestCase):
    def test_the_child_sees_only_the_sandbox_processes(self):
        pids = [name for name in listing(Path("/proc")) if name.isdigit()]
        self.assertLessEqual(len(pids), 3, f"the sandboxed child sees {len(pids)} processes")

    def test_stopping_bwrap_stops_a_process_that_left_the_command_session(self):
        name, value = "WINDOW_CHECK_SANDBOX_TEST", f"{os.getpid()}-{time.time_ns()}"
        command = sandbox_command(["sh", "-c", "setsid sleep 30 & exec sleep 30"], [], [], hidden_folders(PLAIN))
        sandbox = subprocess.Popen(command, env=PLAIN | {name: value})
        self.addCleanup(lambda: signal_all(marked(name, value), signal.SIGKILL))
        sleeps = lambda: [pid for pid in marked(name, value)  # noqa: E731
                          if Path(f"/proc/{pid}/comm").read_text().strip() == "sleep"]
        self.assertTrue(self.wait_for(lambda: len(sleeps()) == 2), "the sandboxed command did not start both sleeps")
        sandbox.kill()
        sandbox.wait(10)
        self.assertTrue(self.wait_for(lambda: not marked(name, value)), "a process started in the sandbox outlived it")

    def wait_for(self, condition, seconds: float = 5.0) -> bool:
        deadline = time.monotonic() + seconds
        while not condition() and time.monotonic() < deadline:
            time.sleep(0.05)
        return bool(condition())


class BindGuard(unittest.TestCase):
    def test_a_bind_that_would_uncover_a_hidden_folder_is_refused(self):
        hidden = hidden_folders(PLAIN)
        binds = [([Path("/tmp")], []), ([], [HOME]), ([], [Path("/")]), ([], [HOME.parent])]
        if Path(f"/run/user/{os.getuid()}").is_dir():
            binds.append(([Path("/run")], []))
        for writable, readable in binds:
            with self.assertRaises(IsolationError, msg=f"{writable} {readable}"):
                sandbox_command(["true"], writable, readable, hidden)

    def test_a_bind_inside_a_hidden_folder_is_accepted(self):
        self.assertIn(str(TOOL), sandbox_command(["true"], [], [TOOL], hidden_folders(PLAIN)))


class MountCheck(unittest.TestCase):
    def sleeper(self, command: list[str]) -> int:
        """The pid of `sleep` inside a sandbox, once bwrap has set up its mounts and started it."""
        sandbox = subprocess.Popen(command, env=PLAIN)
        self.addCleanup(lambda: (sandbox.kill(), sandbox.wait(10)))
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            for pid in descendants(sandbox.pid):
                if Path(f"/proc/{pid}/comm").read_text().strip() == "sleep":
                    return pid
            time.sleep(0.05)
        raise AssertionError("bwrap started no sleep")

    def test_a_sandboxed_process_has_every_hidden_folder_covered(self):
        hidden = hidden_folders(PLAIN)
        self.assertEqual(not_hidden_in(self.sleeper(sandbox_command(["sleep", "20"], [], [], hidden)), hidden), [])

    def test_a_hidden_folder_put_back_by_a_later_bind_is_reported(self):
        runtime = Path(f"/run/user/{os.getuid()}")
        if not runtime.is_dir():
            self.skipTest(f"{runtime} does not exist here")
        hidden = hidden_folders(PLAIN)
        # The bind sandbox_command refuses, built by hand: /run over the tmpfs on the runtime folder.
        command = [BWRAP, "--ro-bind", "/", "/", "--dev-bind", "/dev", "/dev"]
        for folder in hidden:
            command += ["--tmpfs", str(folder)]
        command += ["--ro-bind", "/run", "/run", "--unshare-net", "--die-with-parent", "--", "sleep", "20"]
        self.assertIn(runtime.resolve(), not_hidden_in(self.sleeper(command), hidden))

    def test_this_process_is_not_sandboxed_even_where_the_host_mounts_tmpfs(self):
        hidden = hidden_folders(PLAIN)
        self.assertEqual(not_hidden_in(os.getpid(), hidden), hidden)


if __name__ == "__main__":
    unittest.main()
