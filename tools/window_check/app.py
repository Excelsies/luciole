"""Luciole and its private session bus, each started inside the sandbox."""

import os
import signal
import subprocess
from collections.abc import Mapping
from pathlib import Path

from build import REPO
from isolation import Profile
from processes import descendants, kill_with_parent, signal_all
from sandbox import sandbox_command

ASSETS = REPO / "lib" / "assets"
# Variables the tool gives the app beyond the private client environment.
APP_VARIABLES = {"LUCIOLE_ASSETS_DIR": str(ASSETS)}
TAIL_BYTES = 64 << 10


def start_bus(env: Mapping[str, str], profile: Profile, hidden: list[Path]) -> subprocess.Popen:
    command = sandbox_command(["dbus-daemon", "--session", "--nofork", "--nopidfile", "--nosyslog",
                               f"--address={env['DBUS_SESSION_BUS_ADDRESS']}"], profile.writable(), [], hidden)
    return subprocess.Popen(command, env=dict(env), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            stdin=subprocess.DEVNULL, cwd=profile.home, start_new_session=True,
                            preexec_fn=kill_with_parent())


def start_app(binary: Path, folder: Path, env: Mapping[str, str], profile: Profile, hidden: list[Path],
              log: Path) -> subprocess.Popen:
    """bwrap running the binary on the folder; the app sees the folder, its profile and its assets only."""
    command = sandbox_command([str(binary), str(folder)], [*profile.writable(), folder], [binary, ASSETS], hidden)
    with open(log, "wb") as output:
        return subprocess.Popen(command, env=dict(env), stdout=output,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=profile.home,
                                start_new_session=True, preexec_fn=kill_with_parent())


def app_pid(sandbox: subprocess.Popen, binary: Path) -> int | None:
    """The pid of the descendant of bwrap that runs the binary, once it has started it."""
    for pid in descendants(sandbox.pid):
        try:
            if os.path.samefile(f"/proc/{pid}/exe", binary):
                return pid
        except OSError:
            continue
    return None


def log_tail(log: Path, count: int = 20) -> str:
    """The last `count` lines of the app's log, read from its last TAIL_BYTES only, since the log has no size cap."""
    with open(log, "rb") as stream:
        size = stream.seek(0, os.SEEK_END)
        stream.seek(max(0, size - TAIL_BYTES))
        lines = stream.read().decode(errors="replace").splitlines()
    if size > TAIL_BYTES and len(lines) > 1:
        # The first line read starts mid-line.
        lines = lines[1:]
    return "\n".join(lines[-count:])


def stop_app(sandbox: subprocess.Popen | None, pid: int | None) -> None:
    """SIGTERM to the app so it can exit on its own; bwrap then ends. SIGKILL to the tree after five seconds."""
    if sandbox is None or sandbox.poll() is not None:
        return
    signal_all([pid or sandbox.pid], signal.SIGTERM)
    try:
        sandbox.wait(5)
    except subprocess.TimeoutExpired:
        signal_all([sandbox.pid, *descendants(sandbox.pid)], signal.SIGKILL)
        sandbox.wait(5)
