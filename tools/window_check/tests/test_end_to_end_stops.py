"""Real runs stopped from outside: SIGKILL of the tool, SIGTERM, SIGINT to its process group and the time limit."""

import os
import shutil
import signal
import subprocess
import time
import unittest

from e2e_support import (
    alive,
    children_of,
    command,
    make_work_folder,
    private_mounts,
    private_processes,
    real_run,
    result_of,
    run_lock_is_free,
    stop,
    wait_for_app,
    wait_until_gone,
)


@real_run
class Stops(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work, cls.binary, cls.folder = make_work_folder()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def start(self, name: str, *steps: str, time_limit: int = 90) -> tuple[subprocess.Popen, object]:
        output = self.work / name
        # Its own session, so that a signal to its process group reaches nothing else.
        tool = subprocess.Popen(command(self.binary, self.folder, output, *steps, time_limit=time_limit),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                                start_new_session=True)
        self.addCleanup(stop, tool)
        return tool, output

    def assert_stopped_cleanly(self, tool: subprocess.Popen, output, reason: str, errors: str) -> None:
        result = result_of(output)
        self.assertEqual(tool.returncode, 1)
        self.assertFalse(result["ok"])
        self.assertIn(reason, result["error"])
        self.assertTrue(result["steps"], "no step was recorded")
        self.assertTrue(all("ok" in step for step in result["steps"]), result["steps"])
        self.assertFalse(result["steps"][-1]["ok"])
        self.assertTrue(result["leftover_check"]["ok"], result["leftover_check"])
        self.assertNotIn("Traceback", errors)
        self.assertEqual(private_processes(), [])
        self.assertEqual(private_mounts(), [])

    def test_sigterm_stops_the_run_and_records_it(self):
        tool, output = self.start("sigterm", "wait:60000")
        wait_for_app()
        tool.send_signal(signal.SIGTERM)
        _, errors = tool.communicate(timeout=60)
        self.assert_stopped_cleanly(tool, output, "SIGTERM", errors)

    def test_repeated_sigint_to_the_process_group_stops_the_run_once(self):
        tool, output = self.start("sigint", "wait:60000")
        wait_for_app()
        for _ in range(3):
            os.killpg(tool.pid, signal.SIGINT)
            time.sleep(0.05)
        _, errors = tool.communicate(timeout=60)
        self.assert_stopped_cleanly(tool, output, "SIGINT", errors)

    def test_the_time_limit_stops_the_run(self):
        started = time.monotonic()
        tool, output = self.start("time-limit", "wait:60000", time_limit=12)
        _, errors = tool.communicate(timeout=60)
        self.assertLess(time.monotonic() - started, 40)
        self.assert_stopped_cleanly(tool, output, "time limit of 12 s", errors)

    def test_killing_the_tool_leaves_nothing_running_and_frees_the_lock(self):
        tool, output = self.start("killed-tool", "wait:60000")
        wait_for_app()
        workers = children_of(tool.pid)
        self.assertEqual(len(workers), 1, "the tool has no single worker")
        tool.send_signal(signal.SIGKILL)
        tool.wait(10)
        self.assertEqual(wait_until_gone(lambda: [pid for pid in workers if alive(pid)], 30), [])
        self.assertEqual(wait_until_gone(private_processes), [])
        self.assertEqual(private_mounts(), [])
        self.assertTrue(run_lock_is_free())
        self.assertFalse(result_of(output)["ok"])


if __name__ == "__main__":
    unittest.main()
