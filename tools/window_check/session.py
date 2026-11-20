"""One run: the private compositor, the input helper, a private session bus and the app, driven through steps."""

import json
import os
import shutil
import signal
import subprocess
import time
from collections.abc import Callable, Mapping
from pathlib import Path

from app import APP_VARIABLES, app_pid, log_tail, start_app, start_bus, stop_app
from build import HARNESS, UMBRIEL_COMMIT
from compositor import CompositorError, PrivateCompositor, config_text, is_socket
from input_device import InputHelper
from isolation import (MARKER, IsolationError, Profile, check_child_environment, check_process_environment,
                       client_environment, hidden_folders, mounts_under, real_folder, unmount_under)
from memory_watch import start_memory_watch
from processes import descendants, marked, reap, stop_descendants, stop_process
from sandbox import not_hidden_in
from run_options import Options
from step_actions import StepFailed, perform
from steps import Step

RUN_FOLDER = HARNESS / "run"
WINDOW_TIMEOUT = 30.0
APP_START_TIMEOUT = 10.0
POLL = 0.05
# The memory watcher outlives the time limit by this much at most.
WATCH_MARGIN = 60.0
WATCH_START_TIMEOUT = 30.0


class TimeLimit(RuntimeError):
    pass


def write_result(output: Path, result: dict) -> None:
    """Replaces result.json in one step, so a reader never sees a partial file."""
    partial = output / "result.json.partial"
    partial.write_text(json.dumps(result, indent=1) + "\n")
    partial.replace(output / "result.json")


class Session:
    def __init__(self, options: Options, steps: list[Step], caller: Mapping[str, str], umbriel: Path, marker: str,
                 run_folder: Path = RUN_FOLDER):
        self.options = options
        self.steps = steps
        self.caller = caller
        self.marker = marker
        self.run_folder = run_folder
        # The app and its bus use the kept folders, if any; every child outside the sandbox uses the run's own.
        self.app_profile = Profile(run_folder, options.profile)
        self.profile = self.app_profile.unkept()
        self.hidden = hidden_folders(caller)
        self.compositor = PrivateCompositor(umbriel, self.profile, caller, marker)
        self.helper: InputHelper | None = None
        self.bus: subprocess.Popen | None = None
        self.app: subprocess.Popen | None = None
        self.app_pid: int | None = None
        self.watcher: subprocess.Popen | None = None
        # Set by close-window: from then on a clean exit of the app is expected.
        self.close_requested = False
        self.memory_report = run_folder / "memory.json"
        self.started = time.monotonic()
        self.output: dict = {}
        self.result: dict = {"umbriel_commit": UMBRIEL_COMMIT, "binary": str(options.binary),
                             "folder": str(options.folder), "settings": str(options.settings or ""),
                             "run_folder": str(run_folder), "profile": str(options.profile or ""),
                             "app_environment_added": options.app_env, "memory_limit_mib": options.memory_limit_mib,
                             "time_limit_s": options.time_limit, "steps": []}

    def _memory(self) -> dict:
        if not self.memory_report.is_file():
            return {}
        return json.loads(self.memory_report.read_text())

    def watch(self) -> None:
        """Called from every wait: the app is alive and its memory watcher has not killed it."""
        if self.app is None:
            return
        code = self.app.poll()
        if code is not None and self.watcher is not None:
            # A watcher that killed the app writes its report as it exits.
            try:
                self.watcher.wait(2)
            except subprocess.TimeoutExpired:
                pass
        killed = self._memory().get("killed_kib")
        if killed is not None:
            self.result["watchdog"] = f"killed the app at {killed // 1024} MiB resident"
            raise StepFailed(f"watchdog: the app's resident memory reached {killed // 1024} MiB, over the "
                             f"{self.options.memory_limit_mib} MiB limit; the app was killed")
        if code is None or (self.close_requested and code == 0):
            return
        log = self.options.output / "app.log"
        if self.close_requested:
            raise StepFailed(f"luciole exited with code {code} after the close request; end of {log}:\n"
                             f"{log_tail(log)}")
        raise StepFailed(f"luciole exited early with code {code}; end of {log}:\n{log_tail(log)}")

    def app_exit(self) -> dict | None:
        """Whether the app exited by itself before the run stopped it, and with which code."""
        if self.app is None:
            return None
        code = self.app.poll()
        return {"close_requested": self.close_requested, "exited": code is not None, "code": code}

    def pause(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            self.watch()
            time.sleep(min(POLL, max(0.0, deadline - time.monotonic())))

    def _start(self) -> None:
        self.app_profile.create(self.caller)
        if self.options.settings is not None:
            target = real_folder(self.app_profile.config / "luciole") / "config.toml"
            # A kept profile may hold a link here; unlinking removes the link, never what it points to.
            target.unlink(missing_ok=True)
            shutil.copyfile(self.options.settings, target)
        config = self.profile.root / "umbriel.toml"
        config.write_text(config_text(self.options.width, self.options.height, self.options.scale))
        self.compositor.start(config, self.options.output / "compositor.log")
        self.output = self.compositor.wait_output(self.options.width, self.options.height, self.options.scale, 10)
        self.result["output"] = {key: self.output[key] for key in ("name", "scale", "position", "logical_size")}
        self.result["output"]["mode"] = [self.options.width, self.options.height]
        env = self.compositor.client_env()
        # A headless seat has no keyboard until a virtual one arrives, so the helper connects before the app.
        self.helper = InputHelper(env, self.watch)
        self.bus = start_bus(self.bus_environment(), self.app_profile, self.hidden)
        self._wait_for(lambda: is_socket(self.profile.runtime / "bus"), 5, "the private session bus did not start")
        self.launch_app(env)

    def _wait_for(self, ready: Callable[[], object], seconds: float, message: str):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            value = ready()
            if value:
                return value
            self.pause(0.1)
        raise StepFailed(f"{message} within {seconds:.0f} s")

    def launch_app(self, env: dict[str, str]) -> None:
        """Starts the app once the private socket exists, then checks its real environment and mounts. `env` is the
        environment of the memory watcher, which runs outside the sandbox."""
        if not is_socket(self.compositor.display):
            raise CompositorError("the private Wayland socket is missing; the app is not started")
        self.app = start_app(self.options.binary, self.options.folder, self.app_environment(), self.app_profile,
                             self.hidden, self.options.output / "app.log")
        self.watcher = start_memory_watch(self.app.pid, self.options.memory_limit_mib * 1024, self.memory_report,
                                          self.options.time_limit + WATCH_MARGIN, env)
        # Until its first report the watcher may still be starting, and a stop then would leave no peak.
        self._wait_for(self.memory_report.is_file, WATCH_START_TIMEOUT, "the memory watcher did not start")
        self.app_pid = self._wait_for(lambda: app_pid(self.app, self.options.binary), APP_START_TIMEOUT,
                                      "the app did not start inside the sandbox")
        try:
            seen = check_process_environment(self.app_pid, self.caller, self.app_profile)
            visible = not_hidden_in(self.app_pid, self.hidden)
        except OSError as error:
            # A process that already exited has no environment; report the exit instead.
            self.watch()
            raise IsolationError(f"cannot read the app's environment: {error}") from None
        if visible:
            raise IsolationError(f"the app can see {', '.join(map(str, visible))}")
        missing = [name for name, value in self.options.app_env.items() if seen.get(name) != value]
        if missing:
            raise IsolationError(f"the app's environment lacks the added {', '.join(missing)}")
        self.result["app"] = {"pid": self.app_pid, "sandbox_pid": self.app.pid,
                              "runtime_seen": seen["XDG_RUNTIME_DIR"],
                              "hidden_folders": [str(folder) for folder in self.hidden],
                              "environment_check": f"/proc/{self.app_pid}/environ has XDG_RUNTIME_DIR="
                                                   f"{seen['XDG_RUNTIME_DIR']}, WAYLAND_DISPLAY=wayland-0 and no "
                                                   f"session variables; /proc/{self.app_pid}/mountinfo has an "
                                                   f"empty tmpfs on every hidden folder"}
        window = self._wait_for(lambda: self.compositor.window_of(self.app_pid), WINDOW_TIMEOUT,
                                "the app opened no window")
        self.result["window"] = {key: window[key] for key in ("x", "y", "w", "h", "app_id", "title")}

    def bus_environment(self) -> dict[str, str]:
        """The private client environment on the app's folders: the bus's, and the base of the app's."""
        env = client_environment(self.caller, self.app_profile, self.marker)
        check_child_environment(env, self.caller, self.app_profile)
        return env

    def app_environment(self) -> dict[str, str]:
        """The app's environment: the bus's, its assets and the --env values, which replace nothing."""
        env = self.bus_environment() | APP_VARIABLES
        replaced = sorted(set(self.options.app_env) & set(env))
        if replaced:
            raise IsolationError(f"--env would replace {', '.join(replaced)}, which the tool sets")
        env |= self.options.app_env
        check_child_environment(env, self.caller, self.app_profile)
        return env

    def window(self) -> dict:
        window = self.compositor.window_of(self.app_pid)
        if window is None:
            raise StepFailed("the app has no window any more")
        return window

    def run_steps(self) -> None:
        for index, step in enumerate(self.steps, 1):
            begun = time.monotonic()
            record = {"index": index, "step": step.text_form, "at_s": round(begun - self.started, 3)}
            self.result["steps"].append(record)
            try:
                self.watch()
                record["detail"] = perform(self, step, record)
                self.watch()
            except BaseException as error:
                record["ok"], record["error"] = False, str(error) or type(error).__name__
                if isinstance(error, Exception):
                    raise StepFailed(f"step {index} '{step.text_form}' failed: {record['error']}") from None
                raise
            finally:
                # Measured from the unrounded start, so it is never negative.
                record["duration_s"] = round(time.monotonic() - begun, 3)
            record["ok"] = True

    def _stop(self) -> None:
        """Stops everything in stages; a failed stage is recorded and the later stages still run."""
        errors, killed, unmounted = [], [], []
        stages = (("app", lambda: stop_app(self.app, self.app_pid)),
                  ("memory watcher", lambda: stop_process(self.watcher)),
                  ("input helper", lambda: self.helper and self.helper.stop()),
                  ("session bus", lambda: stop_process(self.bus)), ("compositor", self.compositor.stop),
                  ("descendants", lambda: killed.extend(stop_descendants())), ("reap", reap),
                  ("unmount", lambda: unmounted.extend(mount for folder in self.app_profile.writable()
                                                       for mount in unmount_under(folder))))
        for name, action in stages:
            try:
                action()
            except Exception as error:
                errors.append(f"{name}: {error or type(error).__name__}")
        alive = sorted(set(descendants(os.getpid())) | set(marked(MARKER, self.marker)))
        mounts = [mount for folder in self.app_profile.writable() for mount in mounts_under(folder)]
        self.result["leftover_check"] = {"ok": not alive and not mounts and not errors, "still_alive": alive,
                                         "mounts": mounts, "killed_at_end": killed, "unmounted_at_end": unmounted,
                                         "errors": errors}

    def execute(self) -> int:
        error = None
        try:
            self._start()
            self.run_steps()
        except BaseException as caught:
            error = str(caught) or type(caught).__name__
        finally:
            signal.alarm(0)
            for number in (signal.SIGINT, signal.SIGTERM):
                signal.signal(number, signal.SIG_IGN)
            # Read before the stop, which ends the app whatever it was doing.
            self.result["app_exit"] = self.app_exit()
            self._stop()
        leftover = self.result["leftover_check"]
        if error is None and not leftover["ok"]:
            error = (f"left behind after the run: processes {leftover['still_alive']}, "
                     f"mounts {leftover['mounts']}, stop errors {leftover['errors']}")
        try:
            report = self._memory()
        except (OSError, ValueError) as caught:
            report, error = {}, error or f"cannot read the memory report: {caught}"
        if self.watcher is not None and "peak_kib" not in report:
            error = error or "the memory watcher left no report, so the peak memory is unknown"
        peak = report.get("peak_kib", 0)
        self.result |= {"ok": error is None, "error": error, "elapsed_s": round(time.monotonic() - self.started, 3),
                        "app_peak_rss_mib": round(peak / 1024, 1)}
        write_result(self.options.output, self.result)
        return 0 if error is None else 1
