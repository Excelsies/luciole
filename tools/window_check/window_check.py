"""Check Luciole in a real window inside a private headless Umbriel compositor, away from the desktop session."""

import argparse
import fcntl
import json
import os
import signal
import sys
import time
import traceback
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from build import HARNESS, REPO, BuildError, CompositorMissing, build_compositor, require_compositor  # noqa: E402
from compositor import SCALES, parse_size  # noqa: E402
from app import APP_VARIABLES  # noqa: E402
from isolation import (MARKER, IsolationError, Profile, client_environment, guard_binary, guard_folder,  # noqa: E402
                       guard_output, mounts_under)
from processes import become_subreaper, die_with_parent, marked, stop_descendants  # noqa: E402
from run_options import Options, app_environment, guard_profile  # noqa: E402
from session import RUN_FOLDER, Session, TimeLimit, write_result  # noqa: E402
from steps import SYNTAX, StepError, parse_steps  # noqa: E402

LOCK = HARNESS / "run.lock"
# The worker stops itself at the time limit; the parent waits this much longer before killing it.
GRACE = 30.0
STOP_SIGNALS = (signal.SIGINT, signal.SIGTERM)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="window_check.py", description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("build-compositor", help="clone, build and install the pinned Umbriel into .cache/harness")
    run = commands.add_parser("run", help="open a folder in Luciole inside the private compositor and run steps",
                              epilog=SYNTAX, formatter_class=argparse.RawDescriptionHelpFormatter)
    run.add_argument("--binary", type=Path, required=True, help="a copy of the Luciole binary; a build-* folder of "
                     "the checkout is refused")
    run.add_argument("--folder", type=Path, required=True, help="the folder to open; never the home folder, a "
                     "parent of a hidden folder, or a folder in the session's runtime folder")
    run.add_argument("--output", type=Path, required=True, help="a new or empty folder for result.json, logs and "
                     "screenshots, outside --folder")
    run.add_argument("--size", default="1400x900", help="output mode in pixels (default 1400x900)")
    run.add_argument("--scale", type=float, default=1.0, choices=SCALES, help="output scale (default 1)")
    run.add_argument("--settings", type=Path, help="becomes the app's private luciole/config.toml")
    run.add_argument("--time-limit", type=float, default=120.0, help="seconds for the whole run (default 120)")
    run.add_argument("--memory-limit", type=int, default=3072, help="app resident memory limit in MiB "
                     "(default 3072)")
    run.add_argument("--env", action="append", default=[], metavar="NAME=VALUE", help="a variable for the app "
                     "alone: GST_*, LUCIOLE_* or G_MESSAGES_DEBUG; repeatable")
    run.add_argument("--profile", type=Path, help="keep the app's HOME and XDG folders here between runs: a new "
                     "folder or one an earlier run left, inside the checkout's .tmp or .cache")
    run.add_argument("steps", nargs="+", help="steps in order; see below")
    return parser


def _options(args: argparse.Namespace) -> Options:
    width, height = parse_size(args.size)
    if args.settings is not None and not args.settings.is_file():
        raise ValueError(f"settings file {args.settings} does not exist")
    if not 1 <= args.time_limit <= 3600 or args.memory_limit < 64:
        raise ValueError("the time limit is 1 to 3600 s and the memory limit at least 64 MiB")
    # Every name the tool itself gives the app, whatever the caller's environment holds.
    reserved = set(client_environment({}, Profile(RUN_FOLDER), "")) | set(APP_VARIABLES)
    app_env = app_environment(args.env, reserved)
    try:
        folder = guard_folder(args.folder, os.environ)
        binary = guard_binary(args.binary, REPO)
        output = guard_output(args.output, folder, RUN_FOLDER, os.environ)
        profile = None
        if args.profile is not None:
            profile = guard_profile(args.profile, REPO, [folder, output, RUN_FOLDER, binary])
        return Options(binary, folder, width, height, args.scale, args.settings.resolve() if args.settings else None,
                       output, args.time_limit, args.memory_limit, app_env, profile)
    except IsolationError as error:
        # A refused folder, binary or output is an invalid option.
        raise ValueError(str(error)) from None


def _raise(exception: type[BaseException], message: str):
    def handler(*_):
        # Only the first stop signal counts; the run is already stopping after it.
        for number in (*STOP_SIGNALS, signal.SIGALRM):
            signal.signal(number, signal.SIG_IGN)
        raise exception(message)
    return handler


def _worker(options: Options, steps, umbriel: Path, parent: int, marker: str) -> int:
    # The subreaper attribute is not inherited across fork, so the worker sets its own.
    become_subreaper()
    die_with_parent(signal.SIGTERM, parent)
    signal.signal(signal.SIGALRM, _raise(TimeLimit, f"time limit of {options.time_limit:.0f} s reached"))
    signal.signal(signal.SIGTERM, _raise(KeyboardInterrupt, "stopped by SIGTERM"))
    signal.signal(signal.SIGINT, _raise(KeyboardInterrupt, "stopped by SIGINT"))
    signal.alarm(max(1, round(options.time_limit)))
    return Session(options, steps, dict(os.environ), umbriel, marker).execute()


def _wait_worker(worker: int, options: Options) -> tuple[int, str | None]:
    def forward(received, _):
        try:
            os.kill(worker, received)
        except ProcessLookupError:
            pass

    for number in STOP_SIGNALS:
        signal.signal(number, forward)
    deadline = time.monotonic() + options.time_limit + GRACE
    error = None
    while True:
        pid, raw = os.waitpid(worker, os.WNOHANG)
        if pid == worker:
            break
        if time.monotonic() > deadline:
            os.kill(worker, signal.SIGKILL)
            # SIGKILL cannot be caught, so this wait ends promptly.
            _, raw = os.waitpid(worker, 0)
            error = "the run did not stop at its time limit and was killed"
            break
        time.sleep(0.1)
    for number in STOP_SIGNALS:
        signal.signal(number, signal.SIG_IGN)
    status = os.waitstatus_to_exitcode(raw)
    if error is None and status < 0:
        error = f"the run was killed by signal {-status}"
    return status, error


def _finish(options: Options, status: int, error: str | None, marker: str) -> int:
    """Completes result.json with the supervisor's own checks, whatever state the worker left it in."""
    late = stop_descendants()
    alive = marked(MARKER, marker)
    mounts = [mount for folder in Profile(RUN_FOLDER, options.profile).writable() for mount in mounts_under(folder)]
    path = options.output / "result.json"
    try:
        result = json.loads(path.read_text())
    except (OSError, ValueError):
        result = {"ok": False, "error": None, "steps": []}
        error = error or f"the run ended with code {status} without writing its result"
    if error is None and late:
        error = f"processes {late} outlived the run and were killed"
    if error is None and (alive or mounts):
        error = f"left behind after the run: processes {alive}, mounts {mounts}"
    result["supervisor"] = {"ok": error is None, "exit_status": status, "error": error, "killed_at_end": late,
                            "still_alive": alive, "mounts": mounts}
    if error is not None:
        result["ok"] = False
        result["error"] = result.get("error") or error
    if not result["ok"]:
        print(f"error: {result['error']}", file=sys.stderr)
    write_result(options.output, result)
    print(path.read_text(), end="")
    return 0 if status == 0 and result["ok"] else 1


def run(args: argparse.Namespace) -> int:
    steps = parse_steps(args.steps)
    options = _options(args)
    umbriel = require_compositor()
    HARNESS.mkdir(parents=True, exist_ok=True)
    lock = open(LOCK, "w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise IsolationError(f"another window_check run holds {LOCK}") from None
    become_subreaper()
    parent = os.getpid()
    marker = f"{parent}-{time.time_ns()}"
    worker = os.fork()
    if worker == 0:
        code = 1
        try:
            # Its own process group: a terminal's Ctrl-C reaches the supervisor alone, which forwards it once.
            os.setpgid(0, 0)
            code = _worker(options, steps, umbriel, parent, marker)
        except BaseException:
            traceback.print_exc()
        finally:
            sys.stdout.flush()
            os._exit(code)
    try:
        os.setpgid(worker, worker)
    except OSError:
        # The worker already moved itself, or has already exited.
        pass
    status, error = _wait_worker(worker, options)
    return _finish(options, status, error, marker)


def main(argv: list[str]) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "build-compositor":
            build_compositor()
            return 0
        return run(args)
    except (BuildError, CompositorMissing, IsolationError, StepError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2 if isinstance(error, (StepError, ValueError)) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
