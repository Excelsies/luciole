"""The memory watchdog: a process of its own that kills a process tree whose resident memory passes a limit."""

import json
import signal
import subprocess
import sys
import time
from collections.abc import Mapping
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from processes import descendants, is_running, kill_with_parent, signal_all, tree_rss_kib  # noqa: E402

INTERVAL = 0.1


def start_memory_watch(pid: int, limit_kib: int, report: Path, seconds: float,
                       env: Mapping[str, str]) -> subprocess.Popen:
    """Watches `pid` and its descendants for at most `seconds`; the report is written when the watcher ends."""
    command = [sys.executable, "-I", str(Path(__file__).resolve()), str(pid), str(limit_kib), str(report),
               str(seconds)]
    return subprocess.Popen(command, env=dict(env), stdin=subprocess.DEVNULL, start_new_session=True,
                            preexec_fn=kill_with_parent())


def _write(report: Path, peak: int, killed: int | None) -> None:
    partial = report.with_name(report.name + ".partial")
    partial.write_text(json.dumps({"peak_kib": peak, "killed_kib": killed}) + "\n")
    partial.replace(report)


def watch(pid: int, limit_kib: int, report: Path, seconds: float) -> None:
    """Samples until the process ends or `seconds` pass. The report is written after the first sample, which tells
    the caller the watcher runs and handles SIGTERM, and again at the end."""
    peak, killed, started = 0, None, False
    deadline = time.monotonic() + seconds
    try:
        while is_running(pid) and time.monotonic() < deadline:
            rss = tree_rss_kib(pid)
            peak = max(peak, rss)
            if rss > limit_kib:
                signal_all([pid, *descendants(pid)], signal.SIGKILL)
                killed = rss
                return
            if not started:
                _write(report, peak, None)
                started = True
            time.sleep(INTERVAL)
    finally:
        _write(report, peak, killed)


def main(argv: list[str]) -> int:
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    pid, limit_kib, report, seconds = int(argv[0]), int(argv[1]), Path(argv[2]), float(argv[3])
    watch(pid, limit_kib, report, seconds)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
