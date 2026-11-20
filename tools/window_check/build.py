"""Builds the private Umbriel compositor and the input helper into .cache/harness, never system-wide."""

import os
import shutil
import subprocess
import time
from collections.abc import Mapping
from pathlib import Path

from isolation import plain_environment

UMBRIEL_COMMIT = "1bf3bcec645774ab1e868a5838908d4e5f0afac3"
UMBRIEL_URL = "https://github.com/noctalia-dev/umbriel"

REPO = Path(__file__).resolve().parents[2]
HARNESS = REPO / ".cache" / "harness"
SOURCE = HARNESS / "src" / "umbriel"
BUILD = HARNESS / "build" / "umbriel"
PREFIX = HARNESS / "prefix"
BIN = HARNESS / "bin"
LOG = HARNESS / "build" / "umbriel-build.log"
BUILD_HOME = HARNESS / "build" / "home"
STAMP_NAME = "umbriel-commit"
HELPER_SOURCE = Path(__file__).resolve().with_name("input_helper.cpp")
HELPER = BIN / "window-check-input"
HELPER_PROTOCOLS = ("wlr-virtual-pointer-unstable-v1", "virtual-keyboard-unstable-v1")
# Cores 0 to 5 belong to other work on this machine.
CPU_SET = "6-11"
JOBS = "6"
BUILD_COMMAND = "python tools/window_check/window_check.py build-compositor"


class BuildError(RuntimeError):
    pass


class CompositorMissing(RuntimeError):
    pass


def build_environment(caller: Mapping[str, str]) -> dict[str, str]:
    """Build children see the allow list and a private HOME, and no user or system git configuration."""
    return plain_environment(caller) | {"HOME": str(BUILD_HOME), "GIT_CONFIG_NOSYSTEM": "1",
                                        "GIT_CONFIG_GLOBAL": "/dev/null", "GIT_TERMINAL_PROMPT": "0"}


def built_commit(prefix: Path = PREFIX) -> str | None:
    stamp = prefix / STAMP_NAME
    if not stamp.is_file() or not (prefix / "bin" / "umbriel").is_file():
        return None
    return stamp.read_text().strip()


def require_compositor(prefix: Path = PREFIX) -> Path:
    if built_commit(prefix) != UMBRIEL_COMMIT:
        raise CompositorMissing(f"the private compositor is not built at {UMBRIEL_COMMIT[:12]}; "
                                f"build it with: {BUILD_COMMAND}")
    if not HELPER.is_file() or HELPER.stat().st_mtime < HELPER_SOURCE.stat().st_mtime:
        raise CompositorMissing(f"the input helper is missing or older than its source; build it with: "
                                f"{BUILD_COMMAND}")
    return prefix / "bin" / "umbriel"


def _run(command: list[str], limit: int, cwd: Path | None = None) -> None:
    with open(LOG, "a") as log:
        log.write(f"\n$ {' '.join(command)}\n")
        log.flush()
        try:
            result = subprocess.run(["timeout", str(limit), *command], cwd=cwd, stdout=log, stderr=log,
                                    stdin=subprocess.DEVNULL, env=build_environment(os.environ))
        except FileNotFoundError as error:
            raise BuildError(f"{error.filename} is not installed") from None
    if result.returncode != 0:
        tail = "\n".join(LOG.read_text(errors="replace").splitlines()[-25:])
        raise BuildError(f"'{' '.join(command[:3])}' failed with {result.returncode}; "
                         f"end of {LOG}:\n{tail}")


def _git(*args: str, limit: int = 60) -> None:
    _run(["git", "-C", str(SOURCE), *args], limit)


def _has_commit() -> bool:
    result = subprocess.run(["git", "-C", str(SOURCE), "cat-file", "-e", f"{UMBRIEL_COMMIT}^{{commit}}"],
                            capture_output=True, timeout=30, env=build_environment(os.environ))
    return result.returncode == 0


def _fetch_source() -> None:
    partial = SOURCE.with_name("umbriel.partial")
    valid = (SOURCE / ".git").is_dir() and subprocess.run(
        ["git", "-C", str(SOURCE), "rev-parse", "--git-dir"], capture_output=True, timeout=30,
        env=build_environment(os.environ)).returncode == 0
    if not valid:
        # An interrupted clone left an unusable folder that only this tool writes.
        for leftover in (partial, SOURCE):
            if leftover.exists():
                shutil.rmtree(leftover)
        SOURCE.parent.mkdir(parents=True, exist_ok=True)
        print(f"cloning {UMBRIEL_URL}", flush=True)
        _run(["git", "clone", "--no-checkout", UMBRIEL_URL, str(partial)], 900)
        partial.rename(SOURCE)
    if not _has_commit():
        print("fetching the pinned commit", flush=True)
        _git("fetch", "origin", limit=900)
        if not _has_commit():
            raise BuildError(f"{UMBRIEL_URL} has no commit {UMBRIEL_COMMIT}")
    _git("-c", "advice.detachedHead=false", "checkout", "--detach", UMBRIEL_COMMIT)


def _build() -> None:
    pinned = ["taskset", "-c", CPU_SET]
    setup = ["meson", "setup", str(BUILD), str(SOURCE), f"--prefix={PREFIX}", "--buildtype=release",
             "--wrap-mode=nodownload", "-Dtests=disabled", "-Dharness=disabled", "-Dlocaled=disabled"]
    if (BUILD / "meson-private" / "coredata.dat").is_file():
        setup.append("--reconfigure")
    elif BUILD.exists():
        shutil.rmtree(BUILD)
    print("configuring", flush=True)
    _run([*pinned, *setup], 600)
    print(f"compiling on cores {CPU_SET} with {JOBS} jobs", flush=True)
    _run([*pinned, "ninja", "-C", str(BUILD), "-j", JOBS], 3600)
    print(f"installing into {PREFIX}", flush=True)
    _run([*pinned, "meson", "install", "--no-rebuild", "--quiet", "-C", str(BUILD)], 600)


def build_helper() -> None:
    """Compiles the virtual pointer and keyboard client against the pinned compositor's protocol files."""
    generated = HARNESS / "build" / "input-helper"
    generated.mkdir(parents=True, exist_ok=True)
    BIN.mkdir(parents=True, exist_ok=True)
    objects = []
    for protocol in HELPER_PROTOCOLS:
        xml = SOURCE / "protocols" / f"{protocol}.xml"
        _run(["wayland-scanner", "client-header", str(xml), str(generated / f"{protocol}-client-protocol.h")], 60)
        _run(["wayland-scanner", "private-code", str(xml), str(generated / f"{protocol}-protocol.c")], 60)
        objects.append(generated / f"{protocol}-protocol.o")
        _run(["cc", "-c", "-O2", str(generated / f"{protocol}-protocol.c"), "-o", str(objects[-1])], 120)
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "wayland-client", "xkbcommon"],
                           capture_output=True, text=True, timeout=30, env=build_environment(os.environ))
    if flags.returncode != 0:
        raise BuildError(f"pkg-config cannot find wayland-client and xkbcommon: {flags.stderr.strip()}")
    _run(["c++", "-std=c++23", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{generated}", str(HELPER_SOURCE),
          *map(str, objects), *flags.stdout.split(), "-o", str(HELPER)], 300)


def build_compositor() -> None:
    LOG.parent.mkdir(parents=True, exist_ok=True)
    BUILD_HOME.mkdir(parents=True, exist_ok=True)
    if built_commit() == UMBRIEL_COMMIT:
        print(f"umbriel already built at {UMBRIEL_COMMIT} in {PREFIX}; nothing to do")
        if not HELPER.is_file() or HELPER.stat().st_mtime < HELPER_SOURCE.stat().st_mtime:
            build_helper()
            print(f"input helper rebuilt: {HELPER}")
        return
    started = time.monotonic()
    LOG.write_text(f"umbriel {UMBRIEL_COMMIT}\n")
    _fetch_source()
    (PREFIX / STAMP_NAME).unlink(missing_ok=True)
    _build()
    build_helper()
    (PREFIX / STAMP_NAME).write_text(UMBRIEL_COMMIT + "\n")
    print(f"umbriel {UMBRIEL_COMMIT} built in {time.monotonic() - started:.0f} s; "
          f"installed in {PREFIX}; input helper {HELPER}; log {LOG}")
