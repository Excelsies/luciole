"""The private headless Umbriel: its configuration, start and stop, queries and screenshots."""

import json
import re
import stat
import subprocess
import time
from collections.abc import Mapping
from pathlib import Path

from isolation import Profile, check_child_environment, client_environment, compositor_environment
from processes import kill_with_parent

OUTPUT_NAME = "HEADLESS-1"
SCALES = (1.0, 1.5, 2.0)
START_TIMEOUT = 15.0
QUERY_TIMEOUT = 5.0
SHOT_TIMEOUT = 15.0
# One word that cannot start with '-', so the command line client never reads it as an option.
WINDOW_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}")


class CompositorError(RuntimeError):
    pass


def parse_size(text: str) -> tuple[int, int]:
    match = re.fullmatch(r"([0-9]{2,5})x([0-9]{2,5})", text)
    if not match or not all(64 <= int(value) <= 8192 for value in match.groups()):
        raise ValueError(f"size must be WIDTHxHEIGHT between 64 and 8192, not '{text}'")
    return int(match[1]), int(match[2])


def config_text(width: int, height: int, scale: float) -> str:
    # Every window opens maximized to the output's edges with no title bar, border or shadow, so the window
    # rectangle the compositor reports is the whole output and screenshots show the app alone.
    return f"""\
[general]
show_cheatsheet = false
xwayland = false
autostart = []

[output.{OUTPUT_NAME}]
mode = "{width}x{height}"
scale = {scale}

[appearance]
border_width = 0
outer_border_width = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.app_id = ".*"
default_maximize_to_edges = true
title_bar = "off"
"""


def is_socket(path: Path) -> bool:
    try:
        return stat.S_ISSOCK(path.lstat().st_mode)
    except FileNotFoundError:
        return False


class PrivateCompositor:
    def __init__(self, binary: Path, profile: Profile, caller: Mapping[str, str], marker: str):
        self.binary = binary
        self.profile = profile
        self.caller = caller
        self.marker = marker
        self.process: subprocess.Popen | None = None
        self.socket = profile.runtime / "umbriel-wayland-0.sock"
        self.display = profile.runtime / "wayland-0"

    def client_env(self) -> dict[str, str]:
        env = client_environment(self.caller, self.profile, self.marker)
        check_child_environment(env, self.caller, self.profile)
        return env

    def start(self, config: Path, log: Path) -> None:
        env = compositor_environment(self.caller, self.profile, self.marker)
        check_child_environment(env, self.caller, self.profile)
        checked = subprocess.run([str(self.binary), "config", "validate", "-c", str(config)], env=env,
                                 capture_output=True, text=True, timeout=QUERY_TIMEOUT,
                                 preexec_fn=kill_with_parent())
        if checked.returncode != 0:
            raise CompositorError(f"umbriel refuses its configuration: {checked.stdout}{checked.stderr}".strip())
        with open(log, "wb") as output:
            self.process = subprocess.Popen([str(self.binary), "-c", str(config)], env=env, stdout=output,
                                            stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, cwd=self.profile.home,
                                            start_new_session=True, preexec_fn=kill_with_parent())
        deadline = time.monotonic() + START_TIMEOUT
        while not (is_socket(self.socket) and is_socket(self.display)):
            if self.process.poll() is not None:
                raise CompositorError(f"umbriel exited with {self.process.returncode} while starting; see {log}")
            if time.monotonic() > deadline:
                raise CompositorError(f"umbriel's sockets did not appear within {START_TIMEOUT:.0f} s; see {log}")
            time.sleep(0.02)

    def _call(self, *arguments: str) -> str:
        """Runs the compositor's command line client against the private compositor and returns what it printed."""
        try:
            result = subprocess.run([str(self.binary), *arguments], env=self.client_env(), capture_output=True,
                                    text=True, timeout=QUERY_TIMEOUT, preexec_fn=kill_with_parent())
        except subprocess.TimeoutExpired:
            raise CompositorError(f"umbriel {arguments[0]} gave no answer within {QUERY_TIMEOUT:.0f} s") from None
        if result.returncode != 0:
            raise CompositorError(f"umbriel {' '.join(arguments)} failed: "
                                  f"{result.stderr.strip() or result.stdout.strip()}")
        return result.stdout

    def query(self, command: str) -> object:
        output = self._call(command, "--json")
        try:
            return json.loads(output)
        except json.JSONDecodeError as error:
            raise CompositorError(f"umbriel {command} printed no JSON: {error}") from None

    def output(self) -> dict:
        for entry in self.query("outputs"):
            if entry["name"] == OUTPUT_NAME:
                return entry
        raise CompositorError(f"the compositor has no output {OUTPUT_NAME}")

    def wait_output(self, width: int, height: int, scale: float, timeout: float) -> dict:
        deadline = time.monotonic() + timeout
        while True:
            output = self.output()
            mode = next((m for m in output["modes"] if m["current"]), {})
            if (mode.get("width"), mode.get("height"), output["scale"]) == (width, height, scale):
                return output
            if time.monotonic() > deadline:
                raise CompositorError(f"{OUTPUT_NAME} did not take {width}x{height} at scale {scale}: it reports "
                                      f"{mode.get('width')}x{mode.get('height')} at scale {output['scale']}")
            time.sleep(0.05)

    def window_of(self, pid: int) -> dict | None:
        for entry in self.query("windows"):
            if entry.get("pid") == pid and entry.get("w", 0) > 0 and entry.get("h", 0) > 0:
                return entry
        return None

    def close_window(self, window_id: str) -> None:
        """Asks a window to close through the action a close key binding runs: its client gets a close request."""
        if not WINDOW_ID.fullmatch(window_id):
            raise CompositorError(f"umbriel reported the window id {window_id!r}, which is not an id")
        self._call("msg", "window-close", window_id)

    def screenshot(self, path: Path, image_type: str = "png") -> None:
        path.unlink(missing_ok=True)
        try:
            result = subprocess.run(["grim", "-t", image_type, str(path)], env=self.client_env(),
                                    capture_output=True, text=True, timeout=SHOT_TIMEOUT,
                                    preexec_fn=kill_with_parent())
        except subprocess.TimeoutExpired:
            raise CompositorError(f"grim took longer than {SHOT_TIMEOUT:.0f} s") from None
        if result.returncode != 0 or not path.is_file() or path.stat().st_size == 0:
            raise CompositorError(f"grim failed with {result.returncode}: {result.stderr.strip()}")

    def stop(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        self.process.terminate()
        try:
            self.process.wait(5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(5)
