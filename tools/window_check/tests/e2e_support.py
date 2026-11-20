"""Shared parts of the end-to-end tests: the binary copy, the fixture folder and the leftover checks."""

import fcntl
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL))

from build import HARNESS, REPO, UMBRIEL_COMMIT, built_commit  # noqa: E402
from session import RUN_FOLDER  # noqa: E402

DEBUG_BINARY = REPO / "build-debug" / "luciole"
BUILD_LOCK = REPO / ".tmp" / "native" / "build.lock"
RUN_LOCK = HARNESS / "run.lock"
LOCK_WAIT = 300.0
RUNTIME_ENTRY = f"XDG_RUNTIME_DIR={RUN_FOLDER / 'runtime'}".encode() + b"\0"

requires_real_run = [
    unittest.skipUnless(built_commit() == UMBRIEL_COMMIT, "the private compositor is not built: run "
                        "python tools/window_check/window_check.py build-compositor"),
    unittest.skipUnless(DEBUG_BINARY.is_file(), f"no Luciole binary at {DEBUG_BINARY}"),
]


def real_run(cls):
    for decorator in requires_real_run:
        cls = decorator(cls)
    return cls


def copy_binary(destination: Path) -> None:
    """Copies the shared debug build behind its lock, or skips when another build holds it too long."""
    BUILD_LOCK.parent.mkdir(parents=True, exist_ok=True)

    def give_up(*_):
        raise TimeoutError

    # A blocking wait queues with the other builds, which polling would not; the alarm bounds it.
    previous = signal.signal(signal.SIGALRM, give_up)
    try:
        with open(BUILD_LOCK, "a") as lock:
            signal.alarm(int(LOCK_WAIT))
            try:
                fcntl.flock(lock, fcntl.LOCK_EX)
            except TimeoutError:
                raise unittest.SkipTest(f"{BUILD_LOCK} stayed locked for {LOCK_WAIT:.0f} s") from None
            finally:
                signal.alarm(0)
            shutil.copy2(DEBUG_BINARY, destination)
    finally:
        signal.signal(signal.SIGALRM, previous)


def make_work_folder() -> tuple[Path, Path, Path]:
    """A work folder under the harness with a binary copy and a small generated fixture."""
    HARNESS.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="e2e-", dir=HARNESS))
    binary = work / "luciole"
    try:
        copy_binary(binary)
    except BaseException:
        shutil.rmtree(work)
        raise
    folder = work / "fixture"
    for name in ("Documents", "Music", "Projects"):
        (folder / name).mkdir(parents=True)
        (folder / name / "readme.txt").write_text(f"{name}\n")
    for index in range(5):
        (folder / f"note-{index}.txt").write_text("line\n" * (index + 1))
    return work, binary, folder


def wait_for_run_lock() -> None:
    """Waits while another run, of another user of the tool, holds the run lock; skips after LOCK_WAIT seconds."""
    deadline = time.monotonic() + LOCK_WAIT
    while not run_lock_is_free():
        if time.monotonic() > deadline:
            raise unittest.SkipTest(f"{RUN_LOCK} stayed held for {LOCK_WAIT:.0f} s")
        time.sleep(1)


def command(binary: Path, folder: Path, output: Path, *steps: str, time_limit: int = 90) -> list[str]:
    """The tool's command line for a run, once no other run holds the lock; a run that starts in the moment between
    the check and the start can still be refused."""
    wait_for_run_lock()
    return [sys.executable, str(TOOL / "window_check.py"), "run", "--binary", str(binary), "--folder", str(folder),
            "--output", str(output), "--time-limit", str(time_limit), *steps]


def result_of(output: Path) -> dict:
    return json.loads((output / "result.json").read_text())


def private_processes() -> list[int]:
    """Live processes, wherever they sit in the tree, whose environment names the private runtime folder."""
    found = []
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            if RUNTIME_ENTRY in Path(f"/proc/{entry.name}/environ").read_bytes():
                state = Path(f"/proc/{entry.name}/stat").read_text().rsplit(")", 1)[1].split()[0]
                if state != "Z":
                    found.append(int(entry.name))
        except OSError:
            continue
    return found


def private_named(name: str) -> list[int]:
    found = []
    for pid in private_processes():
        try:
            if Path(f"/proc/{pid}/comm").read_text().strip() == name:
                found.append(pid)
        except OSError:
            continue
    return found


def private_mounts() -> list[str]:
    lines = Path("/proc/self/mountinfo").read_text().splitlines()
    return [line.split()[4] for line in lines if line.split()[4].startswith(str(RUN_FOLDER))]


def wait_for_app(seconds: float = 60.0) -> None:
    """Waits until the app runs, then a moment more for its window and the services it starts."""
    deadline = time.monotonic() + seconds
    while not private_named("luciole") and time.monotonic() < deadline:
        time.sleep(0.2)
    if not private_named("luciole"):
        raise AssertionError("the app never started")
    time.sleep(3)


def wait_until_gone(pids_of, seconds: float = 15.0) -> list[int]:
    deadline = time.monotonic() + seconds
    while pids_of() and time.monotonic() < deadline:
        time.sleep(0.2)
    return pids_of()


def alive(pid: int) -> bool:
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except OSError:
        return False
    return state != "Z"


def children_of(pid: int) -> list[int]:
    text = Path(f"/proc/{pid}/task/{pid}/children").read_text()
    return [int(word) for word in text.split()]


def run_lock_is_free() -> bool:
    with open(RUN_LOCK, "a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return False
        fcntl.flock(lock, fcntl.LOCK_UN)
        return True


def stop(process: subprocess.Popen) -> None:
    if process.poll() is None:
        process.kill()
        process.wait(10)
    if process.stderr is not None:
        process.stderr.close()
