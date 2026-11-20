"""The app's view of the file system: a read-only root with the owner's folders hidden under empty tmpfs mounts."""

import os
import re
from collections.abc import Iterable
from pathlib import Path

from isolation import IsolationError

BWRAP = "/usr/bin/bwrap"


def sandbox_command(command: list[str], writable: Iterable[Path], readable: Iterable[Path],
                    hidden: Iterable[Path]) -> list[str]:
    """bwrap arguments that run `command` with `hidden` emptied and only the given folders bound back."""
    hidden = sorted(Path(folder).resolve() for folder in hidden)
    # A pid namespace of its own: the app sees no host process, and the sandbox ends with its init.
    arguments = [BWRAP, "--unshare-pid", "--ro-bind", "/", "/", "--dev-bind", "/dev", "/dev", "--proc", "/proc"]
    for folder in hidden:
        arguments += ["--tmpfs", str(folder)]
    binds = [(Path(path).resolve(), "--bind") for path in writable]
    binds += [(Path(path).resolve(), "--ro-bind") for path in readable]
    for path, _ in binds:
        # Binds come after the tmpfs mounts, so such a bind would put the real folder back on top.
        uncovered = [folder for folder in hidden if path == folder or path in folder.parents]
        if uncovered:
            raise IsolationError(f"refusing to bind {path}: it would uncover {uncovered[0]}")
    # A parent is bound before its children so that it never covers them.
    for path, option in sorted(binds, key=lambda bind: len(bind[0].parts)):
        arguments += [option, str(path), str(path)]
    return [*arguments, "--unshare-net", "--die-with-parent", "--", *command]


def _unescape(field: str) -> str:
    return re.sub(r"\\([0-7]{3})", lambda match: chr(int(match[1], 8)), field)


def not_hidden_in(pid: int, hidden: Iterable[Path]) -> list[Path]:
    """The folders of `hidden` whose topmost mount in the process is not a tmpfs of its own at the folder itself."""
    mounts = []
    for line in Path(f"/proc/{pid}/mountinfo").read_text().splitlines():
        fields, _, rest = line.partition(" - ")
        words = fields.split()
        mounts.append((Path(_unescape(words[4])), words[2], rest.split()[0]))
    missing = []
    for folder in hidden:
        device = os.stat(folder).st_dev
        # mountinfo lists mounts in the order they were made, so the last one at or above the folder is on top.
        covering = [mount for mount in mounts if mount[0] == folder or mount[0] in folder.parents]
        if not covering:
            missing.append(folder)
            continue
        point, number, kind = covering[-1]
        if point != folder or kind != "tmpfs" or number == f"{os.major(device)}:{os.minor(device)}":
            missing.append(folder)
    return missing
