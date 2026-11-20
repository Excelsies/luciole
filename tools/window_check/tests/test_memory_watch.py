"""The memory watcher: a separate process that kills an over-limit tree without being polled."""

import json
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from memory_watch import start_memory_watch  # noqa: E402

PLAIN = {"PATH": "/usr/bin:/bin"}
# Holds about 200 MiB resident, then sleeps.
HOG = "import time\nheld = b'x' * (200 << 20)\nprint('ready', flush=True)\ntime.sleep(60)\n"


def wait_for(condition, seconds: float) -> bool:
    deadline = time.monotonic() + seconds
    while not condition() and time.monotonic() < deadline:
        time.sleep(0.05)
    return bool(condition())


class MemoryWatch(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.report = Path(self.folder.name) / "memory.json"

    def tearDown(self):
        self.folder.cleanup()

    def start(self, code: str) -> subprocess.Popen:
        process = subprocess.Popen([sys.executable, "-c", code], env=PLAIN, stdout=subprocess.PIPE, text=True)
        self.addCleanup(lambda: (process.kill(), process.wait(10), process.stdout.close()))
        return process

    def test_a_tree_over_the_limit_is_killed_without_polling(self):
        hog = self.start(HOG)
        self.assertEqual(hog.stdout.readline().strip(), "ready")
        watcher = start_memory_watch(hog.pid, 64 * 1024, self.report, 30, PLAIN)
        self.addCleanup(lambda: (watcher.kill(), watcher.wait(10)))
        self.assertTrue(wait_for(lambda: hog.poll() is not None, 5), "the over-limit process is still running")
        self.assertEqual(hog.returncode, -signal.SIGKILL)
        self.assertTrue(wait_for(self.report.is_file, 5), "no report")
        report = json.loads(self.report.read_text())
        self.assertGreater(report["killed_kib"], 64 * 1024)
        self.assertGreaterEqual(report["peak_kib"], report["killed_kib"])

    def test_a_tree_under_the_limit_is_left_alone_and_its_peak_reported(self):
        small = self.start("import time\nprint('ready', flush=True)\ntime.sleep(1)\n")
        self.assertEqual(small.stdout.readline().strip(), "ready")
        watcher = start_memory_watch(small.pid, 1024 * 1024, self.report, 30, PLAIN)
        self.addCleanup(lambda: (watcher.kill(), watcher.wait(10)))
        self.assertEqual(small.wait(10), 0)
        self.assertEqual(watcher.wait(10), 0)
        report = json.loads(self.report.read_text())
        self.assertIsNone(report["killed_kib"])
        self.assertGreater(report["peak_kib"], 0)

    def test_the_first_sample_is_reported_while_the_process_runs(self):
        small = self.start("import time\nprint('ready', flush=True)\ntime.sleep(60)\n")
        self.assertEqual(small.stdout.readline().strip(), "ready")
        watcher = start_memory_watch(small.pid, 1024 * 1024, self.report, 30, PLAIN)
        self.addCleanup(lambda: (watcher.kill(), watcher.wait(10)))
        # However slowly the watcher starts, its report appears once it watches.
        self.assertTrue(wait_for(self.report.is_file, 30), "no report while the process runs")
        self.assertIsNone(small.poll())
        self.assertIsNone(watcher.poll())
        self.assertGreater(json.loads(self.report.read_text())["peak_kib"], 0)

    def test_stopping_the_watcher_writes_its_report(self):
        small = self.start("import time\nprint('ready', flush=True)\ntime.sleep(60)\n")
        self.assertEqual(small.stdout.readline().strip(), "ready")
        watcher = start_memory_watch(small.pid, 1024 * 1024, self.report, 30, PLAIN)
        self.assertTrue(wait_for(self.report.is_file, 30), "the watcher never started watching")
        self.report.unlink()
        watcher.terminate()
        self.assertEqual(watcher.wait(10), 0)
        self.assertIsNone(small.poll())
        self.assertIsNone(json.loads(self.report.read_text())["killed_kib"])


if __name__ == "__main__":
    unittest.main()
