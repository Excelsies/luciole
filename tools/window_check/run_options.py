"""The options of one run, and the checks on the options that reach into the app's private world."""

import re
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

from isolation import IsolationError

# Names --env may set for the app: GStreamer's, the app's own and GLib's debug output.
APP_PREFIXES = ("GST_", "LUCIOLE_")
APP_NAMES = ("G_MESSAGES_DEBUG",)
MAX_VALUE = 4096
_NAME = re.compile(r"[A-Z][A-Z0-9_]*")
# The file that marks a folder as a profile this tool made.
PROFILE_MARK = ".window-check-profile"
# Folders of the checkout a kept profile may lie in.
PROFILE_ROOTS = (".tmp", ".cache")


@dataclass
class Options:
    binary: Path
    folder: Path
    width: int
    height: int
    scale: float
    settings: Path | None
    output: Path
    time_limit: float
    memory_limit_mib: int
    app_env: dict[str, str] = field(default_factory=dict)
    profile: Path | None = None


def _allowed(name: str) -> bool:
    if not _NAME.fullmatch(name):
        return False
    return name in APP_NAMES or any(name.startswith(prefix) and len(name) > len(prefix) for prefix in APP_PREFIXES)


def app_environment(words: Iterable[str], reserved: Iterable[str]) -> dict[str, str]:
    """NAME=VALUE words as variables for the app alone; a name outside the allow list or set by the tool is refused."""
    reserved = set(reserved)
    found: dict[str, str] = {}
    for word in words:
        name, equals, value = word.partition("=")
        if not equals or not name:
            raise ValueError(f"--env takes NAME=VALUE, not '{word}'")
        if not _allowed(name):
            raise ValueError(f"--env refuses {name}: only GST_*, LUCIOLE_* and G_MESSAGES_DEBUG reach the app")
        if name in reserved:
            raise ValueError(f"--env refuses {name}: the tool sets {name} itself")
        if name in found:
            raise ValueError(f"--env names {name} twice")
        if len(value) > MAX_VALUE:
            raise ValueError(f"--env value of {name} is longer than {MAX_VALUE} characters")
        found[name] = value
    return found


def _is_mark(path: Path) -> bool:
    return path.is_file() and not path.is_symlink()


def guard_profile(profile: Path, repo: Path, apart: Iterable[Path]) -> Path:
    """A kept profile: inside the checkout's .tmp or .cache, apart from the run's other folders, and new or marked."""
    resolved = profile.resolve()
    roots = [(repo / name).resolve() for name in PROFILE_ROOTS]
    if not any(resolved.is_relative_to(root) and resolved != root for root in roots):
        raise IsolationError(f"refusing profile {resolved}: it must lie inside {' or '.join(map(str, roots))}")
    for other in apart:
        other = other.resolve()
        if resolved.is_relative_to(other) or other.is_relative_to(resolved):
            raise IsolationError(f"refusing profile {resolved}: it overlaps {other}")
    if resolved.exists() and not resolved.is_dir():
        raise IsolationError(f"refusing profile {resolved}: it is not a folder")
    mark = resolved / PROFILE_MARK
    if resolved.is_dir() and any(resolved.iterdir()) and not _is_mark(mark):
        raise IsolationError(f"refusing profile {resolved}: it holds files and no {PROFILE_MARK} from an earlier run")
    resolved.mkdir(parents=True, exist_ok=True)
    if not _is_mark(mark):
        mark.write_text("A profile kept between window_check runs.\n")
    return resolved
