"""The scenarios of the performance harness, as data. Changing anything here changes the scenario hash.

An action is one of
  ("do", WORD)            a window_check step such as "key:Down", "click:left", "type:{root}/small"
  ("wait", SECONDS)       a bounded pause
  ("cp", NAME, OPTIONS)   a checkpoint: wait for a quiet CPU, then sample memory, CPU and threads
  ("repeat", N, ACTIONS)  the actions N times (checkpoint names inside must not repeat)
  ("mark", NAME)          the time now, as a reference for "until" and "burst"
  ("until", MARK, SECONDS)  wait until SECONDS after the mark (bounded)
  ("burst", NAME, {"seconds": S, "mark": M})  screenshots as fast as possible for S seconds, then one settled
                          screenshot; the time of each is read against the mark, and the tiles that still show no
                          picture in each (perf_tiles)
Checkpoint options: settle=False (sample at once), settle_timeout=SECONDS, quiet=SECONDS (then a quiet window),
hold=SECONDS (no settle: run the window for that long and report its CPU, for playback).
{root} in a word is the fixtures folder. The coordinates are logical pixels at 1920x1080 and 100 percent scale.
"""

import hashlib
import json

SIZE = "1920x1080"
QUIET = 10
KINDS_WAIT = 0.4
# Workbench header buttons: instruments, ribbons.
INSTRUMENTS = {"nest": (480, 106), "iris": (505, 106), "atlas": (530, 106)}
RIBBONS = {"flame": (1804, 106), "flow": (1829, 106), "ladder": (1854, 106)}
# The preview column: the picture of a video, the Enable editing button, rows 1 and 2 of the list.
PREVIEW_PICTURE = (1620, 740)
PREVIEW_SCROLL = (1620, 700)
ENABLE_EDITING = (1381, 431)
ROW_ONE = (850, 411)
ROW_TWO = (850, 453)
ROW_THREE = (850, 495)
ROW_FOUR = (850, 537)


def settings(view: str) -> str:
    return f'[browser]\nview_mode = "{view}"\n'


def click(point: tuple[int, int]) -> list[tuple]:
    return [("do", f"move:{point[0]},{point[1]}"), ("do", "click:left")]


def step(name: str, start: str, view: str, steps: list[tuple], **extra) -> dict:
    return {"name": name, "start": start, "settings": settings(view), "steps": steps, "time_limit": 600,
            "memory_limit_mib": 4096, "profile": "fresh", "prime": None, "needs_env": None, **extra}


def start_details() -> dict:
    return step("start-details", "small", "list", [
        ("cp", "first-frame", {"settle": False}), ("wait", 5), ("cp", "after-5s", {"settle": False}),
        ("cp", "quiet", {"quiet": QUIET})], time_limit=120)


def list_20k(name: str, view: str) -> dict:
    return step(name, "flat-20k", view, [
        ("cp", "settled", {}), ("do", "key:End"), ("cp", "scrolled-to-end", {}),
        ("cp", "quiet", {"quiet": QUIET})], time_limit=300)


def workbench_100k() -> dict:
    steps: list[tuple] = [("cp", "measured", {"settle_timeout": 300})]
    for name, point in INSTRUMENTS.items():
        steps += [*click(point), ("cp", f"instrument-{name}", {})]
    for name, point in RIBBONS.items():
        steps += [*click(point), ("cp", f"ribbon-{name}", {})]
    steps += [("cp", "quiet", {"quiet": QUIET}), ("do", "key:ctrl+l"), ("do", "type:{root}/small"),
              ("do", "key:Return"), ("cp", "left-to-small", {"settle_timeout": 60})]
    return step("workbench-100k", "tree-100k", "workbench", steps, time_limit=900)


REST_SECONDS = (35, 70, 130)


def rest_samples() -> list[tuple]:
    """Memory at rest after the last use: previews go after 30 s unseen, so the quiet window before them is too early."""
    steps: list[tuple] = [("mark", "done")]
    for count in REST_SECONDS:
        steps += [("until", "done", count), ("cp", f"rest-{count}s", {})]
    return steps


def previews(entries: int, rest: bool = False) -> dict:
    steps: list[tuple] = [("cp", "loaded", {})]
    for index in range(1, entries + 1):
        steps += [("do", "key:Down"), ("wait", KINDS_WAIT), ("cp", f"entry-{index:02d}", {})]
    steps += [("do", "key:Home"), ("wait", KINDS_WAIT), ("cp", "folder-selected", {}),
              ("cp", "quiet", {"quiet": QUIET})]
    return step("previews-rest" if rest else "previews", "kinds", "workbench",
                steps + (rest_samples() if rest else []), time_limit=900)


def pictures(name: str, prime: str | None, folder: str = "pictures-300") -> dict:
    """The same folder twice with one profile: `prime` runs first, so the second run finds a warm thumbnail cache."""
    return step(name, folder, "grid", [
        ("cp", "thumbnails-in", {"settle_timeout": 300}), ("do", "key:End"), ("cp", "scrolled-to-end", {"settle_timeout": 300}),
        ("cp", "quiet", {"quiet": QUIET})], profile="kept" if prime else "fresh", prime=prime, time_limit=1200)


PICTURES = "{root}/pictures-300"
LEAVE_SECONDS = (0, 35, 70, 130, 360)
BURST_SECONDS = 6


def scroll_through() -> list[tuple]:
    """Pages down through the whole folder, so every picture's thumbnail is asked for, then to the very end."""
    return [("repeat", 8, [("do", "key:Page_Down"), ("wait", 0.5)]), ("do", "key:End")]


def leave_to_small(mark: str) -> list[tuple]:
    """Goes to the small folder; the mark is the instant before the Return that leaves."""
    return [("do", "key:ctrl+l"), ("do", "type:{root}/small"), ("mark", mark), ("do", "key:Return")]


def leave_samples(mark: str, seconds: tuple[int, ...]) -> list[tuple]:
    """A checkpoint at each of `seconds` after the mark; the one at once does not wait for a quiet CPU."""
    steps: list[tuple] = []
    for count in seconds:
        steps += [("until", mark, count), ("cp", f"left-{count}s", {"settle": False} if count == 0 else {})]
    return steps


def pictures_leave() -> dict:
    """Scrolls the picture folder to its end with the thumbnails already in the disk cache, leaves for another
    folder and samples memory at 0, 35, 70, 130 s and 6 minutes after leaving."""
    steps: list[tuple] = [("cp", "thumbnails-in", {"settle_timeout": 300}), *scroll_through(),
                          ("cp", "scrolled-to-end", {"settle_timeout": 300}), *leave_to_small("left"),
                          *leave_samples("left", LEAVE_SECONDS)]
    return step("pictures-leave", "pictures-300", "grid", steps, profile="kept", prime="pictures", time_limit=1500)


def pictures_return() -> dict:
    """Leaves the scrolled picture folder and comes back after 70 s, then again after 130 s: how late the tiles
    show their pictures, at the top where the folder opens and at the end where it was left."""
    steps: list[tuple] = [("cp", "thumbnails-in", {"settle_timeout": 300}), *scroll_through(),
                          ("cp", "scrolled-to-end", {"settle_timeout": 300}), *leave_to_small("left-a"),
                          *leave_samples("left-a", (0, 35)), ("do", "key:ctrl+l"), ("do", f"type:{PICTURES}"),
                          ("until", "left-a", 70), ("mark", "back-a"), ("do", "key:Return"),
                          ("burst", "return-70s-top", {"seconds": BURST_SECONDS, "mark": "back-a"}),
                          ("mark", "back-a-end"), ("do", "key:End"),
                          ("burst", "return-70s-end", {"seconds": BURST_SECONDS, "mark": "back-a-end"}),
                          ("cp", "returned-70s", {}), *leave_to_small("left-b"), ("until", "left-b", 35),
                          ("cp", "left2-35s", {}),
                          ("do", "key:ctrl+l"), ("do", f"type:{PICTURES}"), ("until", "left-b", 130),
                          ("mark", "back-b"), ("do", "key:Return"),
                          ("burst", "return-130s-top", {"seconds": BURST_SECONDS, "mark": "back-b"}),
                          ("mark", "back-b-end"), ("do", "key:End"),
                          ("burst", "return-130s-end", {"seconds": BURST_SECONDS, "mark": "back-b-end"}),
                          ("cp", "returned-130s", {}), ("cp", "quiet", {"quiet": QUIET})]
    return step("pictures-return", "pictures-300", "grid", steps, profile="kept", prime="pictures", time_limit=1500)


def pictures_scrollback() -> dict:
    """Scrolls to the end, stays there 70 s, scrolls back to the top; then again with 130 s: how late the tiles of
    the top show their pictures."""
    steps: list[tuple] = [("cp", "thumbnails-in", {"settle_timeout": 300})]
    for count in (70, 130):
        steps += [("mark", f"end-{count}"), *scroll_through(), ("cp", f"at-end-{count}", {"settle_timeout": 300}),
                  ("until", f"end-{count}", count), ("cp", f"end-{count}s-later", {}), ("mark", f"up-{count}"),
                  ("do", "key:Home"), ("burst", f"scrollback-{count}s", {"seconds": BURST_SECONDS, "mark": f"up-{count}"})]
    steps += [("cp", "quiet", {"quiet": QUIET})]
    return step("pictures-scrollback", "pictures-300", "grid", steps, profile="kept", prime="pictures",
                time_limit=1500)


def video(name: str, start: str, picks: int, needs_env: str | None, duration: int, away: list[tuple]) -> dict:
    """Plays a clip for 10 s while the position is read from two screenshots (`duration` is the clip length in
    seconds), pauses it for 40 s, plays it again for 10 s, pauses it, then `away` moves off it; the folder is left last."""
    steps: list[tuple] = [("cp", "loaded", {})] + [("do", "key:Down")] * picks
    # The pause after the click lets playback start, so the position read at the window's start is not zero.
    steps += [("cp", "poster", {}), *click(PREVIEW_PICTURE), ("wait", 1.5),
              ("cp", "playing-10s", {"hold": 10, "realtime": duration}), *click(PREVIEW_PICTURE),
              ("cp", "paused", {}), ("wait", 40), ("cp", "paused-40s", {}),
              # Play after the long pause: where the player stands again, and how much of the next 10 s it lost.
              *click(PREVIEW_PICTURE), ("cp", "resumed", {"hold": 10, "realtime": duration}),
              *click(PREVIEW_PICTURE), ("cp", "paused-again", {}), *away,
              ("do", "key:ctrl+l"), ("do", "type:{root}/small"), ("do", "key:Return"),
              ("cp", "left-folder", {"settle_timeout": 60, "quiet": QUIET})]
    return step(name, start, "workbench", steps, needs_env=needs_env, time_limit=300)


def video_leave(name: str, pause: bool, leave: list[tuple], checkpoint: str) -> dict:
    """Plays the 30 s clip, optionally pauses it, then `leave` moves off it; one case of leaving per scenario."""
    steps: list[tuple] = [("cp", "loaded", {})] + [("do", "key:Down")] * 4
    steps += [("cp", "poster", {}), *click(PREVIEW_PICTURE), ("wait", 3), ("cp", "playing", {"settle": False})]
    if pause:
        steps += [*click(PREVIEW_PICTURE), ("cp", "paused", {})]
    steps += [*leave, ("cp", checkpoint, {"settle_timeout": 60, "quiet": QUIET})]
    return step(name, "video", "workbench", steps, time_limit=300)


def video_hard(name: str, duration: int) -> dict:
    """One clip of the codec and rate in its name, played for 10 s with the position read, then left."""
    steps: list[tuple] = [("cp", "loaded", {}), ("do", "key:Down"), ("cp", "poster", {}), *click(PREVIEW_PICTURE),
                          ("wait", 1.5), ("cp", "playing-10s", {"hold": 10, "realtime": duration}),
                          *click(PREVIEW_PICTURE), ("cp", "paused", {}), ("do", "key:ctrl+l"),
                          ("do", "type:{root}/small"), ("do", "key:Return"),
                          ("cp", "left-folder", {"settle_timeout": 60})]
    return step(name, name, "workbench", steps, time_limit=300)


def video_end() -> dict:
    """The 3 s clip plays to its end; the player then sits at the end for 30 s."""
    steps: list[tuple] = [("cp", "loaded", {})] + [("do", "key:Down")] * 3
    steps += [("cp", "poster", {}), *click(PREVIEW_PICTURE), ("wait", 6), ("cp", "ended", {"quiet": QUIET}),
              ("wait", 30), ("cp", "ended-30s", {"quiet": QUIET})]
    return step("video-end", "video", "workbench", steps, time_limit=300)


def text_big() -> dict:
    """Rows sort by name: big.csv, big.log, edit.csv, edit.log. The editor takes files up to 1 MiB and tables up to 10,000 rows."""
    scroll = [("do", f"move:{PREVIEW_SCROLL[0]},{PREVIEW_SCROLL[1]}"), ("do", "scroll:40")]
    steps: list[tuple] = [("cp", "loaded", {}), *click(ROW_ONE), ("cp", "csv-100k-preview", {"settle_timeout": 60}),
                          *scroll, ("cp", "csv-100k-scrolled", {}), *click(ENABLE_EDITING),
                          ("cp", "csv-100k-edit-refused", {}), *click(ROW_TWO),
                          ("cp", "log-20mb-preview", {"settle_timeout": 60}), *scroll, ("cp", "log-20mb-scrolled", {}),
                          *click(ENABLE_EDITING), ("cp", "log-20mb-edit-refused", {}), *click(ROW_THREE),
                          ("cp", "csv-small-preview", {"settle_timeout": 60}), *click(ENABLE_EDITING),
                          ("cp", "csv-small-editing", {"settle_timeout": 60}), *click(ROW_FOUR),
                          ("cp", "log-small-preview", {"settle_timeout": 60}), *click(ENABLE_EDITING),
                          ("cp", "log-small-editing", {"settle_timeout": 60}), ("cp", "quiet", {"quiet": QUIET})]
    return step("text-big", "text-big", "workbench", steps, time_limit=600)


def churn(entries: int, rest: bool = False) -> dict:
    steps: list[tuple] = [("cp", "start", {})]
    for number in range(1, 6):
        for _ in range(entries):
            steps += [("do", "key:Down"), ("wait", KINDS_WAIT)]
        steps += [("cp", f"pass-{number}", {}), ("do", "key:Home")]
    steps += [("cp", "quiet", {"quiet": QUIET})]
    return step("churn-rest" if rest else "churn", "kinds", "workbench",
                steps + (rest_samples() if rest else []), time_limit=900)


def tabs() -> dict:
    steps: list[tuple] = [("cp", "one-tab", {})]
    for _ in range(4):
        steps += [("do", "key:ctrl+t"), ("do", "key:ctrl+l"), ("do", "type:{root}/flat-20k"), ("do", "key:Return"),
                  ("wait", 1)]
    steps += [("cp", "five-tabs", {"settle_timeout": 60})]
    steps += [("do", "key:ctrl+w")] * 4
    steps += [("cp", "one-tab-again", {"settle_timeout": 60}), ("cp", "quiet", {"quiet": QUIET})]
    return step("tabs", "flat-20k", "list", steps, time_limit=300)


def scenarios(manifest: dict) -> dict[str, dict]:
    """Every scenario by name; the entry counts come from the fixtures' manifest."""
    entries = manifest["kinds"]["entries"]
    # The keys go to the player once it has been clicked, so a click on the text file's row moves the selection.
    away = [*click(ROW_TWO), ("cp", "nonmedia-selected", {"settle_timeout": 60, "quiet": QUIET})]
    found = [start_details(), list_20k("list-20k", "list"), list_20k("list-20k-icons", "grid"),
             workbench_100k(), previews(entries), pictures("pictures", None), pictures("pictures-rgba", None, "pictures-rgba-300"),
             pictures("pictures-warm", "pictures"),
             pictures_leave(), pictures_return(), pictures_scrollback(),
             video("video", "video", 4, None, 30, away),
             video("video-bink", "bink", 1, "LUCIOLE_BINK2_SAMPLE", 110, []),
             video("video-4k", "video-4k", 1, None, 40, []), video("video-60", "video-60", 1, None, 40, []),
             *[video_hard(f"video-{codec}-4k{fps}", 30) for codec in ("hevc", "av1", "vp9") for fps in (60, 30)],
             video_hard("video-hevc10-4k60", 30),
             video_end(),
             video_leave("video-switch", False, click(ROW_ONE), "media-selected"),
             video_leave("video-tab-playing", False, [("do", "key:ctrl+t")], "tab-hidden"),
             video_leave("video-tab-paused", True, [("do", "key:ctrl+t")], "tab-hidden"),
             text_big(), churn(entries), tabs(), previews(entries, True), churn(entries, True)]
    return {item["name"]: item for item in found}


def definition_hash(scenario: dict) -> str:
    return hashlib.sha256(json.dumps(scenario, sort_keys=True).encode()).hexdigest()
