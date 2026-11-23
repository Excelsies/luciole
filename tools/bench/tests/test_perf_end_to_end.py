"""One real run of start-details through perf_bench.py: real compositor, real app, real /proc.

Set LUCIOLE_PERF_TEST_BINARY to a copy of a Luciole binary outside any build folder; without it the test is skipped.
"""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import perf_compare  # noqa: E402
import perf_fixtures  # noqa: E402

BINARY = os.environ.get("LUCIOLE_PERF_TEST_BINARY")
READY = (perf_fixtures.DEFAULT_ROOT / "manifest.json").is_file()


@unittest.skipUnless(BINARY and Path(BINARY).is_file(), "LUCIOLE_PERF_TEST_BINARY names no binary")
@unittest.skipUnless(READY, "the fixtures are not built: python tools/bench/perf_bench.py fixtures")
class EndToEnd(unittest.TestCase):
    def test_start_details_records_every_checkpoint_and_compares_with_itself_as_noise(self):
        with tempfile.TemporaryDirectory(dir=perf_fixtures.REPO / ".cache") as name:
            out = Path(name) / "set"
            command = [sys.executable, str(TOOLS / "perf_bench.py"), "run", "--binary", BINARY, "--label", "e2e",
                       "--out", str(out), "--scenario", "start-details", "--runs", "2"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=300)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            runs = [json.loads(path.read_text()) for path in sorted((out / "start-details").glob("run-*.json"))]
            self.assertEqual(len(runs), 2)
            for run in runs:
                self.assertTrue(run["ok"], run["error"])
                self.assertEqual([point["name"] for point in run["checkpoints"]], ["first-frame", "after-5s", "quiet"])
                quiet = run["checkpoints"][-1]
                self.assertGreater(quiet["memory"]["app"]["rollup"]["Pss"], 10 * 1024)
                self.assertGreaterEqual(quiet["quiet"]["wakeups_per_s"], 0)
                self.assertGreater(run["startup"]["first-frame"], 0)
                self.assertEqual(len(run["binary"]["sha256"]), 64)
            rows = perf_compare.compare(perf_compare.load_set(out), perf_compare.load_set(out))
            self.assertTrue(rows)
            self.assertEqual({row["verdict"] for row in rows} - {"noise", "same"}, set())
            self.assertTrue((out / "summary.md").is_file())


if __name__ == "__main__":
    unittest.main()
