"""Launch guards of a run: no app before the private socket exists, and the app's real environment is checked."""

import json
import subprocess
import sys
import tempfile
import time
import tracemalloc
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app import log_tail  # noqa: E402
from compositor import CompositorError  # noqa: E402
from isolation import IsolationError, Profile, check_process_environment, client_environment  # noqa: E402
from session import Options, Session, StepFailed  # noqa: E402
from steps import parse_steps  # noqa: E402

CALLER = {"PATH": "/usr/bin:/bin", "XDG_RUNTIME_DIR": "/run/user/1000", "WAYLAND_DISPLAY": "wayland-1"}


class LaunchWithoutCompositor(unittest.TestCase):
    def test_the_app_is_not_started_when_the_private_socket_is_missing(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "out").mkdir()
            options = Options(Path("/usr/bin/true"), root, 1400, 900, 1.0, None, root / "out", 10, 3072)
            session = Session(options, [], CALLER, Path("/usr/bin/false"), "marker", root / "run")
            session.profile.create(CALLER)
            with self.assertRaises(CompositorError) as caught:
                session.launch_app(session.compositor.client_env())
            self.assertIn("socket", str(caught.exception))
            self.assertIsNone(session.app)


class StepRecords(unittest.TestCase):
    def test_a_step_that_fails_at_once_lasts_zero_not_minus_zero(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            options = Options(Path("/usr/bin/true"), root, 1400, 900, 1.0, None, root, 10, 3072)
            session = Session(options, parse_steps(["wait:0"]), CALLER, Path("/usr/bin/false"), "marker", root / "run")
            session.app = subprocess.Popen(["true"])
            session.app.wait(10)
            # The step starts 0.6 ms into the run, which a start time rounded to milliseconds puts after its end.
            session.started = time.monotonic() - 0.0006
            with self.assertRaises(StepFailed):
                session.run_steps()
            record = session.result["steps"][0]
            self.assertFalse(record["ok"])
            self.assertEqual(json.dumps(record["duration_s"]), "0.0")


class AppExit(unittest.TestCase):
    def session(self, command: list[str], close_requested: bool) -> Session:
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        root = Path(folder.name)
        (root / "app.log").write_text("last words\n")
        options = Options(Path("/usr/bin/true"), root, 1400, 900, 1.0, None, root, 10, 3072)
        session = Session(options, [], CALLER, Path("/usr/bin/false"), "marker", root / "run")
        session.app = subprocess.Popen(command)
        session.app.wait(10)
        session.close_requested = close_requested
        return session

    def test_an_exit_without_a_close_request_fails_the_run(self):
        session = self.session(["true"], close_requested=False)
        with self.assertRaises(StepFailed) as caught:
            session.watch()
        self.assertIn("exited early with code 0", str(caught.exception))
        self.assertEqual(session.app_exit(), {"close_requested": False, "exited": True, "code": 0})

    def test_a_clean_exit_after_the_close_request_is_expected(self):
        session = self.session(["true"], close_requested=True)
        session.watch()
        self.assertEqual(session.app_exit(), {"close_requested": True, "exited": True, "code": 0})

    def test_a_failed_exit_after_the_close_request_fails_the_run(self):
        session = self.session(["false"], close_requested=True)
        with self.assertRaises(StepFailed) as caught:
            session.watch()
        self.assertIn("exited with code 1 after the close request", str(caught.exception))
        self.assertIn("last words", str(caught.exception))

    def test_a_running_app_has_not_exited(self):
        session = self.session(["true"], close_requested=True)
        session.app = subprocess.Popen(["sleep", "30"])
        self.addCleanup(lambda: (session.app.kill(), session.app.wait(10)))
        session.watch()
        self.assertEqual(session.app_exit(), {"close_requested": True, "exited": False, "code": None})

    def test_a_run_that_never_started_the_app_records_no_exit(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            options = Options(Path("/usr/bin/true"), root, 1400, 900, 1.0, None, root, 10, 3072)
            session = Session(options, [], CALLER, Path("/usr/bin/false"), "marker", root / "run")
            self.assertIsNone(session.app_exit())


class AddedEnvironment(unittest.TestCase):
    def session(self, app_env: dict[str, str]) -> Session:
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        root = Path(folder.name)
        options = Options(Path("/usr/bin/true"), root, 1400, 900, 1.0, None, root, 10, 3072, app_env)
        return Session(options, [], CALLER, Path("/usr/bin/false"), "marker", root / "run")

    def test_added_variables_join_the_private_environment(self):
        session = self.session({"GST_DEBUG": "2"})
        env = session.app_environment()
        self.assertEqual(env["GST_DEBUG"], "2")
        self.assertEqual(env["XDG_RUNTIME_DIR"], str(session.profile.runtime))
        self.assertTrue(env["LUCIOLE_ASSETS_DIR"].endswith("lib/assets"))

    def test_an_added_variable_never_replaces_one_the_tool_sets(self):
        for name in ("LUCIOLE_ASSETS_DIR", "HOME", "WAYLAND_DISPLAY"):
            session = self.session({name: "/elsewhere"})
            with self.assertRaises(IsolationError, msg=name):
                session.app_environment()


class LogTail(unittest.TestCase):
    def write_log(self, text: str) -> Path:
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        log = Path(folder.name) / "app.log"
        log.write_text(text)
        return log

    def test_a_large_log_shows_its_last_lines_without_being_loaded(self):
        log = self.write_log("".join(f"line {index:06d} {'x' * 60}\n" for index in range(250_000)))
        tracemalloc.start()
        try:
            tail = log_tail(log, 3)
            _, peak = tracemalloc.get_traced_memory()
        finally:
            tracemalloc.stop()
        self.assertEqual([line.split()[1] for line in tail.splitlines()], ["249997", "249998", "249999"])
        self.assertLess(peak, 1 << 20, f"reading the tail of a {log.stat().st_size >> 20} MiB log took "
                                       f"{peak >> 20} MiB")

    def test_a_short_log_is_shown_from_its_first_line(self):
        self.assertEqual(log_tail(self.write_log("first\nsecond\n"), 20), "first\nsecond")


class ProcessEnvironment(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.profile = Profile(Path(self.folder.name))

    def tearDown(self):
        self.folder.cleanup()

    def sleeper(self, env: dict[str, str]) -> subprocess.Popen:
        process = subprocess.Popen(["sleep", "30"], env=env)
        self.addCleanup(lambda: (process.kill(), process.wait(10)))
        return process

    def test_a_private_process_passes_and_its_runtime_folder_is_returned(self):
        process = self.sleeper(client_environment(CALLER, self.profile, "run-1"))
        seen = check_process_environment(process.pid, CALLER, self.profile)
        self.assertEqual(seen["XDG_RUNTIME_DIR"], str(self.profile.runtime))

    def test_a_process_on_the_callers_runtime_folder_is_refused(self):
        env = client_environment(CALLER, self.profile, "run-1") | {"XDG_RUNTIME_DIR": "/run/user/1000"}
        process = self.sleeper(env)
        with self.assertRaises(IsolationError):
            check_process_environment(process.pid, CALLER, self.profile)

    def test_a_process_with_another_display_is_refused(self):
        env = client_environment(CALLER, self.profile, "run-1") | {"WAYLAND_DISPLAY": "wayland-7"}
        process = self.sleeper(env)
        with self.assertRaises(IsolationError):
            check_process_environment(process.pid, CALLER, self.profile)


if __name__ == "__main__":
    unittest.main()
