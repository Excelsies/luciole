"""What each step does in a running session: screenshots, pointer, keys, waits and the close request."""

import time
from collections.abc import Mapping

from images import png_size
from steps import Step


class StepFailed(RuntimeError):
    pass


def pointer_motion(window: Mapping, output: Mapping, x: float, y: float) -> tuple[tuple[float, float], str]:
    """The layout position of a window-relative point and the helper command that moves there."""
    origin, size = output["position"], output["logical_size"]
    layout = (window["x"] + x, window["y"] + y)
    inside_x, inside_y = layout[0] - origin["x"], layout[1] - origin["y"]
    if not (0 <= inside_x < size["width"] and 0 <= inside_y < size["height"]):
        raise StepFailed(f"position {x},{y} in the window is outside the {size['width']}x{size['height']} output")
    command = (f"motion {round(inside_x * 100)} {round(inside_y * 100)} "
               f"{size['width'] * 100} {size['height'] * 100}")
    return layout, command


def _settle(session, tries: int) -> str:
    previous = None
    frame = session.profile.root / "settle.ppm"
    for attempt in range(tries + 1):
        session.compositor.screenshot(frame, "ppm")
        current = frame.read_bytes()
        if current == previous:
            return f"two identical frames after {attempt + 1} screenshots"
        previous = current
        session.pause(0.15)
    raise StepFailed(f"the window kept changing over {tries + 1} screenshots")


def _close_window(session, seconds: float, record: dict) -> str:
    """Sends the close request a close key binding sends, then waits up to `seconds` for the app to exit."""
    window = session.window()
    record["window_id"] = window["id"]
    session.close_requested = True
    session.compositor.close_window(window["id"])
    sent = time.monotonic()
    deadline = sent + seconds
    # pause() fails the step when the app exits with an error after the request.
    while session.app.poll() is None and time.monotonic() < deadline:
        session.pause(min(0.05, max(0.0, deadline - time.monotonic())))
    code = session.app.poll()
    if code is None:
        return f"close request sent to window {window['id']}; the app still runs after {seconds:.1f} s"
    return f"close request sent to window {window['id']}; the app exited after {time.monotonic() - sent:.1f} s"


def perform(session, step: Step, record: dict) -> str:
    """Carries out one step in the session and returns what it did; a failure raises."""
    helper = session.helper
    if step.kind == "shot":
        path = session.options.output / f"{step.name}.png"
        session.compositor.screenshot(path)
        record["file"], record["pixels"] = str(path), list(png_size(path))
        return f"{path.name}, {record['pixels'][0]}x{record['pixels'][1]} pixels"
    if step.kind == "move":
        record["window"] = {key: value for key, value in session.window().items() if key in ("x", "y", "w", "h")}
        record["layout_position"], command = pointer_motion(record["window"], session.output, step.x, step.y)
        helper.send(command)
        return f"pointer at {record['layout_position'][0]},{record['layout_position'][1]} in the layout"
    if step.kind in ("down", "up", "click"):
        for pressed in {"down": (1,), "up": (0,), "click": (1, 0)}[step.kind]:
            helper.send(f"button {step.button} {pressed}")
        return "sent"
    if step.kind in ("scroll", "hscroll"):
        helper.send(f"wheel {step.axis[0]} {step.notches}")
        return "sent"
    if step.kind in ("key", "keydown", "keyup"):
        action = {"key": "tap", "keydown": "press", "keyup": "release"}[step.kind]
        helper.send(f"key {action} {','.join(step.modifiers) or '-'} {step.key}")
        return "sent"
    if step.kind == "type":
        helper.send("type " + " ".join(f"{ord(character):x}" for character in step.text))
        return "sent"
    if step.kind == "wait":
        session.pause(step.milliseconds / 1000)
        return "waited"
    if step.kind == "close-window":
        return _close_window(session, step.milliseconds / 1000, record)
    return _settle(session, step.tries)
