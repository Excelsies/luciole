"""The step syntax of window_check: one command-line word per step, KIND or KIND:ARGUMENT."""

import re
from dataclasses import dataclass

# Evdev button codes (linux/input-event-codes.h).
BUTTONS = {"left": 0x110, "right": 0x111, "middle": 0x112}
MODIFIERS = ("ctrl", "shift", "alt", "super")
DEFAULT_SETTLE_TRIES = 20
MAX_SETTLE_TRIES = 100
MAX_WAIT_MS = 600_000
# How long close-window waits for the app to exit, unless told.
DEFAULT_CLOSE_WAIT_MS = 3000
MAX_CLOSE_WAIT_MS = 60_000

SYNTAX = """\
steps (in order, one word each):
  shot:NAME           screenshot to NAME.png in the output folder
  move:X,Y            pointer to X,Y logical pixels from the window's top-left corner
  down:B  up:B        press or release button B (left, middle, right)
  click:B             press and release button B
  scroll:N            N wheel notches down (negative: up)
  hscroll:N           N wheel notches right (negative: left)
  key:COMBO           press and release a key, for example ctrl+4, shift+Tab, Escape, Return, Left
  keydown:COMBO       press and hold; keyup:COMBO releases
  type:TEXT           type TEXT (quote it in the shell)
  wait:MS             wait MS milliseconds
  settle[:TRIES]      wait until two screenshots in a row are identical (default 20 tries)
  close-window[:MS]   ask the app's window to close, as a close key binding does, and wait up
                      to MS milliseconds for the app to exit (default 3000)
modifiers: ctrl, shift, alt, super. Keys: one character or an XKB key name."""

_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}")
_KEY = re.compile(r"[A-Za-z0-9_]+")
_INTEGER = re.compile(r"-?[0-9]+")


class StepError(ValueError):
    pass


@dataclass(frozen=True)
class Step:
    kind: str
    text_form: str
    name: str = ""
    x: float = 0.0
    y: float = 0.0
    button: int = 0
    axis: str = ""
    notches: int = 0
    modifiers: tuple[str, ...] = ()
    key: str = ""
    text: str = ""
    milliseconds: int = 0
    tries: int = 0


def _integer(argument: str, message: str) -> int:
    if not _INTEGER.fullmatch(argument):
        raise StepError(message)
    return int(argument)


def _position(word: str, argument: str) -> Step:
    parts = argument.split(",")
    try:
        x, y = (float(part) for part in parts)
    except ValueError:
        raise StepError("move needs X,Y in logical pixels, for example move:120,40") from None
    if x < 0 or y < 0:
        raise StepError("move coordinates are not negative: they count from the window's top-left corner")
    return Step("move", word, x=x, y=y)


def _combo(kind: str, word: str, argument: str) -> Step:
    *modifiers, key = argument.split("+")
    if not (_KEY.fullmatch(key) or (len(key) == 1 and key.isprintable() and key != " ")):
        raise StepError(f"{kind} needs a key name: one character or an XKB name such as Return")
    for modifier in modifiers:
        if modifier not in MODIFIERS:
            raise StepError(f"unknown modifier '{modifier}': use {', '.join(MODIFIERS)}")
    if len(set(modifiers)) != len(modifiers):
        raise StepError(f"repeated modifier in '{argument}'")
    return Step(kind, word, modifiers=tuple(modifiers), key=key)


def _close_window(word: str, argument: str) -> Step:
    milliseconds = DEFAULT_CLOSE_WAIT_MS
    if word != "close-window":
        milliseconds = _integer(argument, "close-window waits a whole number of milliseconds")
    if milliseconds < 0:
        raise StepError("close-window waits a whole number of milliseconds")
    if milliseconds > MAX_CLOSE_WAIT_MS:
        raise StepError(f"close-window waits at most {MAX_CLOSE_WAIT_MS} ms")
    return Step("close-window", word, milliseconds=milliseconds)


def parse_step(word: str) -> Step:
    kind, _, argument = word.partition(":")
    if kind == "shot":
        if not _NAME.fullmatch(argument):
            raise StepError("screenshot name must be letters, digits, '-' or '_' (up to 64)")
        return Step("shot", word, name=argument)
    if kind == "move":
        return _position(word, argument)
    if kind in ("down", "up", "click"):
        if argument not in BUTTONS:
            raise StepError("button must be one of left, middle, right")
        return Step(kind, word, button=BUTTONS[argument])
    if kind in ("scroll", "hscroll"):
        notches = _integer(argument, f"{kind} needs a nonzero whole number of notches")
        if notches == 0:
            raise StepError(f"{kind} needs a nonzero whole number of notches")
        return Step(kind, word, axis="vertical" if kind == "scroll" else "horizontal", notches=notches)
    if kind in ("key", "keydown", "keyup"):
        return _combo(kind, word, argument)
    if kind == "type":
        if not argument:
            raise StepError("type needs text, for example type:notes")
        return Step("type", word, text=argument)
    if kind == "wait":
        milliseconds = _integer(argument, "wait needs a whole number of milliseconds")
        if milliseconds < 0:
            raise StepError("wait needs a whole number of milliseconds")
        if milliseconds > MAX_WAIT_MS:
            raise StepError(f"wait is at most {MAX_WAIT_MS} ms")
        return Step("wait", word, milliseconds=milliseconds)
    if kind == "settle":
        tries = DEFAULT_SETTLE_TRIES if not argument else _integer(argument, "settle tries must be a number")
        if not 1 <= tries <= MAX_SETTLE_TRIES:
            raise StepError(f"settle tries must be between 1 and {MAX_SETTLE_TRIES}")
        return Step("settle", word, tries=tries)
    if kind == "close-window":
        return _close_window(word, argument)
    raise StepError(f"unknown step '{kind}'")


def parse_steps(words: list[str]) -> list[Step]:
    if not words:
        raise StepError("no steps given")
    steps = []
    for index, word in enumerate(words, 1):
        try:
            steps.append(parse_step(word))
        except StepError as error:
            raise StepError(f"step {index} '{word}': {error}") from None
    return steps
