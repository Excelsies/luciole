#!/usr/bin/env python3
"""Builds the fixtures of the performance harness under .cache/perf-fixtures, deterministically and with bounded
memory. A finished fixture carries a marker file, so a second run builds nothing; `clean` removes only marked folders.

    python tools/bench/perf_fixtures.py build [--only NAME,NAME] [--root DIR]
    python tools/bench/perf_fixtures.py clean [--root DIR]
"""

import argparse
import json
import math
import os
import random
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DEFAULT_ROOT = REPO / ".cache" / "perf-fixtures"
KINDS_SEED = REPO / ".tmp" / "native" / "rw5" / "fx" / "kinds"
MARKER = ".perf-fixture"
NAMES = ("small", "flat-20k", "tree-100k", "pictures-300", "pictures-rgba-300", "kinds", "text-big", "video", "video-4k", "video-60",
         "video-hevc-4k60", "video-hevc-4k30", "video-av1-4k60", "video-av1-4k30", "video-vp9-4k60", "video-vp9-4k30",
         "video-hevc10-4k60")
PICTURE_SIZE = (4000, 3000)
PICTURE_COUNT = 300
FLAT_COUNT = 20_000
TREE_FOLDERS = 10_000
TREE_FILES = 100_000
TOP_FOLDERS = 12
MAX_DEPTH = 9
MAX_PICKS = 8
LOG_BYTES = 20 << 20
CSV_ROWS = 100_000
EDITABLE_BYTES = 900_000
EDITABLE_ROWS = 9_000
VIDEO_SECONDS = {"video": 30, "kinds": 3, "short": 3, "video-4k": 40, "video-60": 40}
# What the video fixture holds besides its clip: a short clip (playback reaches its end) and a file that is no media.
VIDEO_FILES = ("other.ogg", "plain.txt", "short-3s.mp4", "video-30s.mp4")
WORDS = ("report", "notes", "draft", "invoice", "summary", "backup", "config", "export", "archive", "plan",
         "budget", "meeting", "photo", "scan", "data", "result", "chart", "review", "design", "build")
EXTENSIONS = ("txt", "md", "json", "csv", "log", "cpp", "h", "py", "xml", "yaml", "toml", "ini", "sh", "zip",
              "tar", "bin", "dat", "cfg", "html", "css")
YEAR = 365 * 24 * 3600
EPOCH = 1_700_000_000


class FixtureError(RuntimeError):
    pass


def write_marked(folder: Path, name: str) -> None:
    (folder / MARKER).write_text(f"luciole performance fixture {name}\n")


def begin(root: Path, name: str) -> Path | None:
    """The new folder to fill, or None when it is finished already; a folder with no marker is refused."""
    target = root / name
    if (target / MARKER).is_file():
        return None
    if target.exists():
        raise FixtureError(f"{target} exists without {MARKER}; remove it or run clean")
    target.mkdir(parents=True)
    return target


def scatter(path: Path, rng: random.Random) -> None:
    os.utime(path, (EPOCH - rng.randrange(YEAR), EPOCH - rng.randrange(YEAR)))


def build_small(root: Path) -> None:
    target = begin(root, "small")
    if target is None:
        return
    rng = random.Random(11)
    for index in range(12):
        path = target / f"{WORDS[index]}-{index:02d}.{EXTENSIONS[index]}"
        path.write_bytes(rng.randbytes(rng.randrange(40, 3000)))
        scatter(path, rng)
    for name in ("docs", "src"):
        (target / name).mkdir()
        (target / name / "readme.txt").write_text("small fixture\n")
    write_marked(target, "small")


def build_flat(root: Path) -> None:
    target = begin(root, "flat-20k")
    if target is None:
        return
    rng = random.Random(22)
    for index in range(FLAT_COUNT):
        path = target / f"{rng.choice(WORDS)}-{index:05d}.{rng.choice(EXTENSIONS)}"
        path.write_bytes(rng.randbytes(rng.randrange(0, 2000)))
        scatter(path, rng)
    write_marked(target, "flat-20k")


def tree_plan(rng: random.Random) -> tuple[list[tuple[int, int]], list[int]]:
    """Folder parents and files per folder. A folder picks a parent among the recent folders, so depth stays
    within MAX_DEPTH; file counts are heavy-tailed with a mean near TREE_FILES / TREE_FOLDERS."""
    parents = [(-1, 0)] * TOP_FOLDERS
    for index in range(TOP_FOLDERS, TREE_FOLDERS):
        parent = rng.randrange(TOP_FOLDERS)
        for _ in range(MAX_PICKS):
            picks = [rng.randrange(max(0, index - 40), index) if rng.random() < 0.5 else rng.randrange(index)
                     for _ in range(2)]
            pick = min(picks, key=lambda candidate: parents[candidate][1])
            if parents[pick][1] < MAX_DEPTH - 1:
                parent = pick
                break
        parents.append((parent, parents[parent][1] + 1))
    weights = [rng.paretovariate(1.6) for _ in range(TREE_FOLDERS)]
    scale = TREE_FILES / sum(weights)
    return parents, [max(0, round(weight * scale)) for weight in weights]


def build_tree(root: Path) -> None:
    target = begin(root, "tree-100k")
    if target is None:
        return
    rng = random.Random(33)
    parents, counts = tree_plan(rng)
    paths: list[Path] = []
    for index, (parent, _) in enumerate(parents):
        folder = target / f"{rng.choice(WORDS)}-{index:05d}" if parent < 0 else paths[parent] / \
            f"{rng.choice(WORDS)}-{index:05d}"
        folder.mkdir()
        paths.append(folder)
    for folder, count in zip(paths, counts):
        for number in range(count):
            path = folder / f"{rng.choice(WORDS)}-{number:04d}.{rng.choice(EXTENSIONS)}"
            # Apparent sizes are what Workbench measures: log-normal, a median of 6 KiB and a few very large files.
            size = min(int(math.exp(rng.gauss(math.log(6000), 2.2))), 4 << 30)
            with open(path, "wb") as stream:
                stream.truncate(size)
            scatter(path, rng)
    write_marked(target, "tree-100k")


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def write_png(path: Path, width: int, height: int, seed: int, alpha: bool = False) -> None:
    """An RGB picture of smooth gradients with one bit of noise per byte: compressible, and not trivially so. With
    `alpha` the same colors come with a fourth channel that is 255 everywhere."""
    rng = random.Random(seed)
    channels = 4 if alpha else 3
    row_bytes = width * 3
    phase = rng.randrange(256)
    pattern = bytes(((index * 5) // 13 + phase) & 255 for index in range(row_bytes * 2))
    mask = int.from_bytes(b"\x01" * row_bytes, "big")
    compressor = zlib.compressobj(3)
    with open(path, "wb") as stream:
        stream.write(b"\x89PNG\r\n\x1a\n")
        stream.write(png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6 if alpha else 2, 0, 0, 0)))
        pending = bytearray()
        for row in range(height):
            start = (row * 3) % row_bytes
            noise = int.from_bytes(rng.randbytes(row_bytes), "big") & mask
            line = (int.from_bytes(pattern[start:start + row_bytes], "big") ^ noise).to_bytes(row_bytes, "big")
            if alpha:
                line = b"".join(line[x * 3:x * 3 + 3] + b"\xff" for x in range(width))
            pending += compressor.compress(b"\x00" + line)
            if len(pending) >= 1 << 20:
                stream.write(png_chunk(b"IDAT", bytes(pending)))
                pending.clear()
        pending += compressor.flush()
        stream.write(png_chunk(b"IDAT", bytes(pending)))
        stream.write(png_chunk(b"IEND", b""))


def build_pictures(root: Path) -> None:
    target = begin(root, "pictures-300")
    if target is None:
        return
    for index in range(PICTURE_COUNT):
        write_png(target / f"picture-{index:03d}.png", *PICTURE_SIZE, seed=1000 + index)
    write_marked(target, "pictures-300")


def build_pictures_rgba(root: Path) -> None:
    target = begin(root, "pictures-rgba-300")
    if target is None:
        return
    for index in range(PICTURE_COUNT):
        write_png(target / f"picture-{index:03d}.png", *PICTURE_SIZE, seed=1000 + index, alpha=True)
    write_marked(target, "pictures-rgba-300")


def write_log(path: Path, size: int = LOG_BYTES) -> None:
    rng = random.Random(44)
    levels = ("INFO", "INFO", "INFO", "WARN", "DEBUG", "ERROR")
    written = 0
    with open(path, "w") as stream:
        number = 0
        while written < size:
            line = (f"2026-11-{1 + number % 28:02d}T{number % 24:02d}:{number % 60:02d}:{number % 57:02d}Z "
                    f"{rng.choice(levels)} worker-{rng.randrange(32)} request {rng.getrandbits(48):012x} "
                    f"finished in {rng.randrange(1, 9000)} ms status={rng.choice((200, 200, 204, 404, 500))}\n")
            stream.write(line)
            written += len(line)
            number += 1


def write_csv(path: Path, rows: int = CSV_ROWS) -> None:
    rng = random.Random(55)
    with open(path, "w") as stream:
        stream.write("id,name,city,score,amount,created\n")
        for number in range(rows):
            stream.write(f"{number},{rng.choice(WORDS)}-{rng.randrange(9999)},city-{rng.randrange(400)},"
                         f"{rng.random() * 100:.3f},{rng.randrange(10**6) / 100:.2f},2026-{1 + number % 12:02d}-"
                         f"{1 + number % 28:02d}\n")


def make_video(path: Path, seconds: int, width: int = 1920, height: int = 1080, fps: int = 30) -> None:
    """An H.264 clip of moving test patterns (1080p30 by default), 4:2:0 at 8 bits as ordinary video is; gst-launch
    must have the encoder, or the build fails loudly. The bitrate follows the pixel rate, so a 4K clip is as dense
    as the 1080p one."""
    bitrate = 6000 * width * height * fps // (1920 * 1080 * 30)
    command = ["gst-launch-1.0", "-q", "-e", "videotestsrc", f"num-buffers={seconds * fps}", "pattern=ball",
               "!", f"video/x-raw,format=I420,width={width},height={height},framerate={fps}/1", "!", "x264enc",
               "speed-preset=ultrafast", f"bitrate={bitrate}", f"key-int-max={fps}", "!", "h264parse", "!", "mp4mux",
               "!", "filesink",
               f"location={path}"]
    result = subprocess.run(command, capture_output=True, text=True, timeout=300)
    if result.returncode != 0 or not path.is_file() or path.stat().st_size == 0:
        raise FixtureError(f"gst-launch-1.0 made no video: {result.stderr.strip()[-400:]}")


# Encoders for the hard clips: ffmpeg arguments and the container of each codec.
CODECS = {"hevc": (["-c:v", "libx265", "-preset", "veryfast", "-x265-params", "log-level=error"], "mp4"),
          "av1": (["-c:v", "libsvtav1", "-preset", "10"], "mp4"),
          "vp9": (["-c:v", "libvpx-vp9", "-deadline", "realtime", "-cpu-used", "6", "-row-mt", "1"], "webm"),
          # 10-bit HEVC at twice the rate and grain: the hardest stream the decoder rule is checked on.
          "hevc10": (["-c:v", "libx265", "-preset", "veryfast", "-x265-params", "log-level=error"], "mp4")}
HARD_SECONDS = 30
HARD_MBIT_4K60 = 50


def make_hard_video(path: Path, codec: str, seconds: int, width: int, height: int, fps: int) -> None:
    """A clip of moving test shapes with film grain on top, so it stays detailed after encoding: about 50 Mbit/s at
    4K60, scaled to the pixel rate. ffmpeg must have the encoder, or the build fails loudly."""
    arguments, _ = CODECS[codec]
    heavy = codec == "hevc10"
    rate = max(1, round(HARD_MBIT_4K60 * (2 if heavy else 1) * width * height * fps / (3840 * 2160 * 60)))
    source = f"testsrc2=s={width}x{height}:r={fps}:d={seconds},noise=alls={14 if heavy else 10}:allf=t"
    command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", source, *arguments,
               "-b:v", f"{rate}M", "-pix_fmt", "yuv420p10le" if heavy else "yuv420p", "-an", str(path)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=1800)
    if result.returncode != 0 or not path.is_file() or path.stat().st_size == 0:
        raise FixtureError(f"ffmpeg made no {codec} video: {result.stderr.strip()[-400:]}")


def hard_builder(name: str, codec: str, fps: int):
    def build(root: Path) -> None:
        target = begin(root, name)
        if target is not None:
            make_hard_video(target / f"clip.{CODECS[codec][1]}", codec, HARD_SECONDS, 3840, 2160, fps)
            write_marked(target, name)
    return build


def build_kinds(root: Path) -> None:
    target = begin(root, "kinds")
    if target is None:
        return
    if not KINDS_SEED.is_dir():
        raise FixtureError(f"the seed folder {KINDS_SEED} is missing")
    for source in sorted(KINDS_SEED.iterdir()):
        shutil.copy2(source, target / source.name)
    # Sorts first with folders first or with names alone.
    (target / "0-folder").mkdir()
    for name in ("a.txt", "b.md", "c.json"):
        (target / "0-folder" / name).write_text("fixture\n")
    write_log(target / "big.log")
    write_csv(target / "big.csv")
    make_video(target / "video-3s.mp4", VIDEO_SECONDS["kinds"])
    write_marked(target, "kinds")


def build_text_big(root: Path) -> None:
    target = begin(root, "text-big")
    if target is None:
        return
    write_log(target / "big.log")
    write_csv(target / "big.csv")
    # The editor refuses files over 1 MiB and tables over the 10,000 rows the preview shows, so these two open.
    write_log(target / "edit.log", EDITABLE_BYTES)
    write_csv(target / "edit.csv", EDITABLE_ROWS)
    write_marked(target, "text-big")


def build_video(root: Path) -> None:
    target = begin(root, "video")
    if target is None:
        return
    make_video(target / "video-30s.mp4", VIDEO_SECONDS["video"])
    make_video(target / "short-3s.mp4", VIDEO_SECONDS["short"])
    shutil.copy2(KINDS_SEED / "quiet.ogg", target / "other.ogg")
    (target / "plain.txt").write_text("fixture\n")
    write_marked(target, "video")


def build_video_4k(root: Path) -> None:
    target = begin(root, "video-4k")
    if target is not None:
        make_video(target / "clip-4k.mp4", VIDEO_SECONDS["video-4k"], 3840, 2160, 30)
        write_marked(target, "video-4k")


def build_video_60(root: Path) -> None:
    target = begin(root, "video-60")
    if target is not None:
        make_video(target / "clip-60.mp4", VIDEO_SECONDS["video-60"], 1920, 1080, 60)
        write_marked(target, "video-60")


BUILDERS = {"small": build_small, "flat-20k": build_flat, "tree-100k": build_tree, "pictures-300": build_pictures,
            "pictures-rgba-300": build_pictures_rgba,
            "kinds": build_kinds, "text-big": build_text_big, "video": build_video,
            "video-4k": build_video_4k, "video-60": build_video_60,
            **{f"video-{codec}-4k{fps}": hard_builder(f"video-{codec}-4k{fps}", codec, fps)
               for codec in ("hevc", "av1", "vp9") for fps in (60, 30)},
            "video-hevc10-4k60": hard_builder("video-hevc10-4k60", "hevc10", 60)}


def count_entries(folder: Path) -> dict:
    entries = [item for item in folder.iterdir() if item.name != MARKER]
    return {"entries": len(entries), "folders": sum(item.is_dir() for item in entries)}


def build(root: Path, only: list[str]) -> dict:
    """Builds the named fixtures (all by default), writes root/manifest.json and returns it."""
    unknown = [name for name in only if name not in BUILDERS]
    if unknown:
        raise FixtureError(f"unknown fixture {', '.join(unknown)}; the names are {', '.join(NAMES)}")
    root.mkdir(parents=True, exist_ok=True)
    for name in only or NAMES:
        BUILDERS[name](root)
        print(f"fixture {name}: ready", file=sys.stderr)
    manifest = {name: count_entries(root / name) for name in NAMES if (root / name / MARKER).is_file()}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    return manifest


def clean(root: Path, only: list[str] | None = None) -> list[str]:
    removed = []
    for name in only or NAMES:
        if (root / name / MARKER).is_file():
            shutil.rmtree(root / name)
            removed.append(name)
    return removed


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("build", "clean"))
    parser.add_argument("--only", default="", help="comma separated fixture names")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    args = parser.parse_args(argv)
    try:
        if args.command == "clean":
            print("removed:", ", ".join(clean(args.root, [n for n in args.only.split(",") if n])) or "nothing")
        else:
            print(json.dumps(build(args.root, [name for name in args.only.split(",") if name]), indent=1))
    except FixtureError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
