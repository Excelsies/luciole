"""Profile modes: perf record of one scenario with a folded function report, and tcmalloc heap profiles.

For diagnosis only: both modes slow the app, so their numbers are never compared between builds.
"""

import bisect
import collections
import contextlib
import json
import re
import subprocess
from pathlib import Path

TOP = 15
NAME_WIDTH = 110
# A call chain with fewer frames than this for most samples means the unwinder lost it.
SHALLOW_DEPTH = 3
SHALLOW_LIMIT = 0.5
_FRAME = re.compile(r"^\s+([0-9a-f]+)\s+(.*?)\s+\(([^()]*)\)\s*$")
_HEAP_LINE = re.compile(r"^\s*(\d+):\s+(\d+)\s+\[\s*(\d+):\s+(\d+)\]\s+@\s+(.*)$")
_MAP_LINE = re.compile(r"^([0-9a-f]+)-([0-9a-f]+)\s+(\S+)\s+([0-9a-f]+)\s+\S+\s+\d+\s+(.*)$")


class ProfileError(RuntimeError):
    pass


def short(name: str) -> str:
    return name if len(name) <= NAME_WIDTH else name[:NAME_WIDTH - 3] + "..."


_HEADER = re.compile(r"^(\S.*?)\s+(\d+)/(\d+)\s*:?\s*$")
# Processes the harness itself starts around the app; their samples are not the app's.
SHIM_COMMS = ("systemd-run", "env", "bwrap", "perf")


def parse_perf_script(text: str) -> list[tuple[str, list[tuple[str, str]]]]:
    """Samples as (comm, call chain), leaf first, each frame (symbol, dso); a header line starts each sample."""
    samples: list[tuple[str, list[tuple[str, str]]]] = []
    comm, current = "", None
    for line in text.splitlines():
        if not line.strip():
            if current is not None:
                samples.append((comm, current))
            current = None
            continue
        frame = _FRAME.match(line)
        header = None if frame else _HEADER.match(line)
        if frame is None and header is None:
            raise ProfileError(f"unrecognized perf script line: {line[:80]!r}")
        if header is not None:
            if current is not None:
                samples.append((comm, current))
            comm, current = header[1], []
        elif current is not None:
            current.append((re.sub(r"\+0x[0-9a-f]+$", "", frame[2]), frame[3]))
    if current is not None:
        samples.append((comm, current))
    return [(name, chain) for name, chain in samples if name not in SHIM_COMMS and chain]


def is_ours(dso: str, binary: Path) -> bool:
    return Path(dso).name == binary.name


def aggregate(samples: list[tuple[str, list[tuple[str, str]]]], binary: Path) -> dict:
    """Self counts (leaf frame) and inclusive counts (once per sample) per function, and folded stacks."""
    self_counts: collections.Counter = collections.Counter()
    inclusive: collections.Counter = collections.Counter()
    folded: collections.Counter = collections.Counter()
    shallow = 0
    for _, chain in samples:
        shallow += len(chain) < SHALLOW_DEPTH
        self_counts[chain[0]] += 1
        for frame in set(chain):
            inclusive[frame] += 1
        folded[";".join(symbol for symbol, _ in reversed(chain))] += 1
    total = sum(self_counts.values())
    ours_self = sum(count for (_, dso), count in self_counts.items() if is_ours(dso, binary))
    return {"total": total, "self": self_counts, "inclusive": inclusive, "folded": folded,
            "ours_self_samples": ours_self, "shallow_fraction": shallow / total if total else 0.0}


def top_table(counts: collections.Counter, total: int, binary: Path, ours: bool | None) -> str:
    rows = [(frame, count) for frame, count in counts.most_common()
            if ours is None or is_ours(frame[1], binary) == ours][:TOP]
    lines = ["| samples | percent | function | library |", "| ---: | ---: | --- | --- |"]
    for (symbol, dso), count in rows:
        lines.append(f"| {count} | {count / total * 100:.1f} | `{short(symbol)}` | {Path(dso).name} |")
    return "\n".join(lines)


def perf_markdown(info: dict, binary: Path, scenario: str) -> str:
    total = info["total"]
    if total == 0:
        raise ProfileError("perf recorded no samples")
    out = [f"# perf profile of {scenario}", "",
           f"{total} samples; {info['ours_self_samples'] / total * 100:.1f} percent of the self time is in "
           f"{binary.name} itself, the rest in libraries and the kernel.", ""]
    if info["shallow_fraction"] > SHALLOW_LIMIT:
        out += [f"WARNING: {info['shallow_fraction'] * 100:.0f} percent of the call chains have fewer than "
                f"{SHALLOW_DEPTH} frames; the frame-pointer unwinder lost them, so the inclusive numbers are low. "
                "Record again with --callgraph dwarf.", ""]
    for title, counts, ours in (("Self time, our code", info["self"], True),
                                ("Self time, libraries and kernel", info["self"], False),
                                ("Inclusive time, our code", info["inclusive"], True),
                                ("Inclusive time, libraries and kernel", info["inclusive"], False)):
        out += [f"## {title}", "", top_table(counts, total, binary, ours), ""]
    return "\n".join(out)


def perf_report(data: Path, binary: Path, scenario: str, out: Path) -> Path:
    """Runs perf script on the recording, writes perf-report.md and perf-folded.txt beside it."""
    result = subprocess.run(["perf", "script", "-i", str(data), "--no-inline", "-F", "comm,pid,tid,ip,sym,dso"],
                            capture_output=True, text=True, timeout=1800)
    if result.returncode != 0:
        raise ProfileError(f"perf script failed: {result.stderr.strip()[-300:]}")
    info = aggregate(parse_perf_script(result.stdout), binary)
    (out / "perf-report.md").write_text(perf_markdown(info, binary, scenario))
    (out / "perf-folded.txt").write_text("".join(f"{stack} {count}\n" for stack, count in info["folded"].most_common()))
    return out / "perf-report.md"


def parse_heap(text: str) -> tuple[list[dict], list[tuple[int, int, int, str]]]:
    """Records {inuse_objects, inuse_bytes, alloc_objects, alloc_bytes, stack} and the executable mappings."""
    records, maps, in_maps = [], [], False
    for line in text.splitlines():
        if line.startswith("MAPPED_LIBRARIES"):
            in_maps = True
            continue
        if in_maps:
            match = _MAP_LINE.match(line)
            if match and "x" in match[3]:
                maps.append((int(match[1], 16), int(match[2], 16), int(match[4], 16), match[5].strip()))
            continue
        match = _HEAP_LINE.match(line)
        if match:
            records.append({"inuse_objects": int(match[1]), "inuse_bytes": int(match[2]),
                            "alloc_objects": int(match[3]), "alloc_bytes": int(match[4]),
                            "stack": [int(item, 16) for item in match[5].split()]})
    if not records:
        raise ProfileError("the heap dump has no allocation records")
    return records, maps


class Symbols:
    """Function names for runtime addresses: addr2line for files with a symbol table, nm -D for the others."""

    def __init__(self, maps: list[tuple[int, int, int, str]]):
        self.maps = sorted(maps)
        self.starts = [item[0] for item in self.maps]
        self.cache: dict[int, tuple[str, str]] = {}
        self.dynamic: dict[str, tuple[list[int], list[str]]] = {}

    def locate(self, address: int) -> tuple[str, int] | None:
        index = bisect.bisect_right(self.starts, address) - 1
        if index < 0:
            return None
        start, end, offset, path = self.maps[index]
        # A PIE or library address is relative to the lowest mapping of its file.
        base = min(item[0] - item[2] for item in self.maps if item[3] == path)
        return (path, address - base) if address < end else None

    def resolve_all(self, addresses: set[int]) -> None:
        by_file: dict[str, list[tuple[int, int]]] = collections.defaultdict(list)
        for address in addresses:
            found = self.locate(address - 1)
            if found is None:
                self.cache[address] = ("?", "unknown")
            else:
                by_file[found[0]].append((address, found[1] + 1))
        for path, items in by_file.items():
            names = self.addr2line(path, [offset - 1 for _, offset in items])
            for (address, offset), name in zip(items, names):
                self.cache[address] = (name or self.nm_name(path, offset - 1) or f"{Path(path).name}+0x{offset - 1:x}",
                                       path)

    def addr2line(self, path: str, offsets: list[int]) -> list[str | None]:
        result = subprocess.run(["addr2line", "-f", "-C", "-e", path, *[f"0x{item:x}" for item in offsets]],
                                capture_output=True, text=True, timeout=300)
        lines = result.stdout.splitlines()
        # Two lines per address: the function, then the file and line.
        names = [lines[index] for index in range(0, len(lines), 2)]
        if result.returncode != 0 or len(names) != len(offsets):
            return [None] * len(offsets)
        return [None if name.startswith("??") else name for name in names]

    def nm_name(self, path: str, offset: int) -> str | None:
        if path not in self.dynamic:
            result = subprocess.run(["nm", "-D", "-n", "-C", "--defined-only", path], capture_output=True, text=True,
                                    timeout=120)
            rows = [line.split(None, 2) for line in result.stdout.splitlines() if len(line.split(None, 2)) == 3]
            self.dynamic[path] = ([int(row[0], 16) for row in rows], [row[2] for row in rows])
        starts, names = self.dynamic[path]
        index = bisect.bisect_right(starts, offset) - 1
        return names[index] if index >= 0 else None


def site(stack: list[int], symbols: Symbols, binary: Path) -> str:
    """The allocation site: the innermost frame in the binary. A stack with none (a driver that keeps no frame
    pointers, for example) is named by its innermost frame outside the profiler."""
    frames = [symbols.cache.get(address, ("?", "unknown")) for address in stack]
    for name, path in frames:
        if Path(path).name == binary.name:
            return name
    outer = [frame for frame in frames if "tcmalloc" not in Path(frame[1]).name] or frames or [("?", "unknown")]
    return f"{outer[0][0]} [{Path(outer[0][1]).name}, no frame in {binary.name}]"


def heap_sites(records: list[dict], symbols: Symbols, binary: Path) -> dict[str, dict]:
    totals: dict[str, dict] = {}
    for record in records:
        entry = totals.setdefault(site(record["stack"], symbols, binary),
                                  {"inuse_bytes": 0, "inuse_objects": 0, "alloc_bytes": 0, "alloc_objects": 0})
        for key in entry:
            entry[key] += record[key]
    return totals


def heap_table(sites: dict[str, dict], key: str) -> str:
    total = sum(item[key] for item in sites.values()) or 1
    objects = key.replace("bytes", "objects")
    lines = [f"| {key.replace('_', ' ')} | percent | objects | site |", "| ---: | ---: | ---: | --- |"]
    for name, item in sorted(sites.items(), key=lambda pair: -pair[1][key])[:TOP]:
        lines.append(f"| {item[key]:,} | {item[key] / total * 100:.1f} | {item[objects]:,} | `{short(name)}` |")
    return "\n".join(lines)


def heap_markdown(dumps: list[tuple[str, Path]], binary: Path, scenario: str) -> str:
    out = [f"# heap profile of {scenario}", "", "tcmalloc replaces glibc malloc in this mode: the live bytes differ "
           "from the real build's; the ranking of sites is what to read.", ""]
    last: dict[str, dict] = {}
    for name, path in dumps:
        records, maps = parse_heap(path.read_text())
        symbols = Symbols(maps)
        symbols.resolve_all({address for record in records for address in record["stack"]})
        last = heap_sites(records, symbols, binary)
        live = sum(item["inuse_bytes"] for item in last.values())
        out += [f"## Live bytes at checkpoint {name} ({live / (1 << 20):.1f} MiB in {len(records)} stacks)", "",
                heap_table(last, "inuse_bytes"), ""]
    out += ["## Total bytes allocated up to the last checkpoint", "", heap_table(last, "alloc_bytes"), ""]
    return "\n".join(out)


def sample_link(scenario: dict, bench, root: Path):
    """The context a run needs: the linked sample for a scenario that names one, nothing for the others."""
    needed = scenario.get("needs_env")
    if not needed:
        return contextlib.nullcontext()
    if not bench.os.environ.get(needed):
        raise ValueError(f"{scenario['name']} needs {needed} set")
    return bench.BinkLink(root)


def profile_command(args, bench) -> int:
    """perf or heap profile of one scenario into args.out; `bench` is the perf_bench module (run_session)."""
    args.root = args.root.resolve()
    args.binary = args.binary.resolve()
    args.cpu_set = bench.parse_cpus(args.cpus)
    bench.os.sched_setaffinity(0, args.cpu_set)
    manifest = json.loads((args.root / "manifest.json").read_text())
    scenario = bench.scenarios(manifest).get(args.scenario)
    if scenario is None:
        raise ValueError(f"unknown scenario {args.scenario}")
    out = args.out.resolve() / f"{args.scenario}-{args.mode}"
    out.mkdir(parents=True, exist_ok=True)
    session = out / "session.d"
    bench.shutil.rmtree(session, ignore_errors=True)
    bench.SCRATCH.mkdir(parents=True, exist_ok=True)
    try:
        with sample_link(scenario, bench, args.root):
            code = bench.run_session(args, scenario, session, None, args.mode, args.callgraph)
    finally:
        bench.shutil.rmtree(bench.SCRATCH / str(bench.os.getpid()), ignore_errors=True)
    result = json.loads((session / "result.json").read_text()) if (session / "result.json").is_file() else {}
    if code != 0 or not result.get("ok"):
        raise RuntimeError(f"the profiled run failed: {result.get('error')}; see {session}")
    profile = session / "profile"
    if args.mode == "perf":
        print(f"report: {perf_report(profile / 'perf.data', args.binary, args.scenario, out)}")
        return 0
    dumps = []
    for point in result["bench"]["checkpoints"]:
        for name in point.get("heap_dumps", []):
            dumps.append((point["name"], profile / name))
    (out / "heap-report.md").write_text(heap_markdown(dumps, args.binary, args.scenario))
    print(f"report: {out / 'heap-report.md'}")
    return 0
