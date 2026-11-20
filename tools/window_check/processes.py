"""Process lifetime: parent-death signals, the child subreaper, the process tree, memory and leftover checks."""

import ctypes
import os
import signal
import subprocess
import time
from collections.abc import Callable
from pathlib import Path

PR_SET_PDEATHSIG = 1
PR_SET_CHILD_SUBREAPER = 36
_libc = ctypes.CDLL(None, use_errno=True)


def _prctl(option: int, value: int) -> None:
    if _libc.prctl(option, value, 0, 0, 0) != 0:
        error = ctypes.get_errno()
        raise OSError(error, f"prctl({option}) failed: {os.strerror(error)}")


def become_subreaper() -> None:
    """Orphaned descendants are reparented to this process instead of leaving its tree."""
    _prctl(PR_SET_CHILD_SUBREAPER, 1)


def die_with_parent(signal_number: int, parent: int) -> None:
    _prctl(PR_SET_PDEATHSIG, signal_number)
    if os.getppid() != parent:
        os.kill(os.getpid(), signal_number)


def kill_with_parent() -> Callable[[], None]:
    """The preexec_fn of every child: it receives SIGKILL when the process that started it dies."""
    parent = os.getpid()

    def arm() -> None:
        _prctl(PR_SET_PDEATHSIG, signal.SIGKILL)
        # The parent died before the signal was set, so it will never come.
        if os.getppid() != parent:
            os._exit(1)

    return arm


def _stat(pid: int) -> tuple[int, str] | None:
    try:
        text = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    fields = text.rsplit(")", 1)[1].split()
    return int(fields[1]), fields[0]


def is_running(pid: int) -> bool:
    info = _stat(pid)
    return info is not None and info[1] != "Z"


def process_table() -> dict[int, tuple[int, str]]:
    """pid: (parent pid, state letter) for every process visible now."""
    table = {}
    for entry in os.scandir("/proc"):
        if entry.name.isdigit():
            info = _stat(int(entry.name))
            if info is not None:
                table[int(entry.name)] = info
    return table


def descendants(root: int, include_zombies: bool = False) -> list[int]:
    return tree_of(process_table(), root, include_zombies)


def tree_of(table: dict[int, tuple[int, str]], root: int, include_zombies: bool = False) -> list[int]:
    """The descendants of `root` in a process table, following parent links."""
    children: dict[int, list[int]] = {}
    for pid, (parent, _) in table.items():
        children.setdefault(parent, []).append(pid)
    # Each pid is expanded once, so the walk ends after at most len(table) expansions even on a cycle.
    found, pending, seen = [], list(children.get(root, [])), {root}
    while pending:
        pid = pending.pop()
        if pid in seen:
            continue
        seen.add(pid)
        if include_zombies or table[pid][1] != "Z":
            found.append(pid)
        pending.extend(children.get(pid, []))
    return found


def rss_kib(pid: int) -> int:
    try:
        for line in Path(f"/proc/{pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    except OSError:
        return 0
    return 0


def tree_rss_kib(pid: int) -> int:
    return rss_kib(pid) + sum(rss_kib(child) for child in descendants(pid))


def environment_of(pid: int) -> dict[str, str]:
    raw = Path(f"/proc/{pid}/environ").read_bytes()
    pairs = (item.decode(errors="replace").partition("=") for item in raw.split(b"\0") if item)
    return {name: value for name, _, value in pairs}


def marked(name: str, value: str) -> list[int]:
    """Live processes of this user whose environment carries name=value, wherever they were reparented."""
    needle = f"{name}={value}".encode() + b"\0"
    found = []
    for entry in os.scandir("/proc"):
        if not entry.name.isdigit():
            continue
        try:
            carries = needle in Path(f"/proc/{entry.name}/environ").read_bytes()
        except OSError:
            continue
        info = _stat(int(entry.name))
        if carries and info is not None and info[1] != "Z":
            found.append(int(entry.name))
    return found


def reap() -> None:
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if pid == 0:
            return


def signal_all(pids: list[int], signal_number: int) -> None:
    for pid in pids:
        try:
            os.kill(pid, signal_number)
        except ProcessLookupError:
            pass


def stop_process(process: subprocess.Popen | None, grace: float = 5.0) -> None:
    """SIGTERM, then SIGKILL after `grace` seconds."""
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(grace)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(grace)


def stop_descendants(grace: float = 3.0) -> list[int]:
    """Terminates every descendant of this process, then kills what is left. Returns the pids it had to kill."""
    me = os.getpid()
    signal_all(descendants(me), signal.SIGTERM)
    deadline = time.monotonic() + grace
    while descendants(me) and time.monotonic() < deadline:
        reap()
        time.sleep(0.05)
    stubborn = descendants(me)
    signal_all(stubborn, signal.SIGKILL)
    deadline = time.monotonic() + 2.0
    while descendants(me) and time.monotonic() < deadline:
        reap()
        time.sleep(0.02)
    reap()
    return stubborn
