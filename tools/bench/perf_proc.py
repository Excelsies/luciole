"""Reads of /proc for the performance harness: memory classes from smaps, CPU ticks, wakeups and threads.

A read that fails raises SampleError. A checkpoint records the error; it never records a zero in its place.
"""

import os
import re
from pathlib import Path

CLK_TCK = os.sysconf("SC_CLK_TCK")
ARENA_ALIGN = 64 << 20
LIBRARY_LIMIT = 15
SMAPS_FIELDS = ("Size", "Rss", "Pss", "Shared_Clean", "Shared_Dirty", "Private_Clean", "Private_Dirty",
                "Anonymous", "Swap")
ROLLUP_FIELDS = ("Rss", "Pss", "Pss_Anon", "Pss_File", "Pss_Shmem", "Shared_Clean", "Shared_Dirty",
                 "Private_Clean", "Private_Dirty", "Anonymous", "Swap", "SwapPss")
# Mappings of the graphics driver and of buffers shared with the compositor: not ours to shrink.
DEVICE_PREFIXES = ("/dev/nvidia", "/dev/dri", "/dev/shm", "/memfd:", "/SYSV", "/dev/zero", "/dev/udmabuf")
CLASSES = ("heap", "arena", "stack", "anon_other", "file", "device", "special")
_HEADER = re.compile(r"^([0-9a-f]+)-([0-9a-f]+) (\S{4}) ([0-9a-f]+) (\S+) (\d+)\s*(.*)$")


class SampleError(RuntimeError):
    pass


def read_text(path: str) -> str:
    try:
        return Path(path).read_text()
    except OSError as error:
        raise SampleError(f"cannot read {path}: {error.strerror}") from None


def parse_kib_fields(text: str, wanted: tuple[str, ...]) -> dict[str, int]:
    """`Name:   123 kB` lines of smaps_rollup or status as {Name: 123}; every wanted name must be present."""
    found: dict[str, int] = {}
    for line in text.splitlines():
        name, _, rest = line.partition(":")
        if name in wanted and rest.split():
            found[name] = int(rest.split()[0])
    missing = [name for name in wanted if name not in found]
    if missing:
        raise SampleError(f"missing {', '.join(missing)}")
    return found


def parse_smaps(text: str) -> list[dict]:
    """One dict per mapping: start, end, perms, path and the SMAPS_FIELDS in KiB."""
    mappings: list[dict] = []
    for line in text.splitlines():
        header = _HEADER.match(line)
        if header:
            mappings.append({"start": int(header[1], 16), "end": int(header[2], 16), "perms": header[3],
                             "path": header[7], **{name: 0 for name in SMAPS_FIELDS}})
            continue
        name, _, rest = line.partition(":")
        if mappings and name in SMAPS_FIELDS and rest.split():
            mappings[-1][name] = int(rest.split()[0])
    return mappings


def classify(mapping: dict, before: dict | None, after: dict | None) -> str:
    """The class of a mapping; neighbors tell thread stacks and glibc arenas from other anonymous memory."""
    path = mapping["path"]
    if path == "[heap]":
        return "heap"
    if path == "[stack]":
        return "stack"
    if path.startswith("["):
        return "special"
    if path.startswith(DEVICE_PREFIXES):
        return "device"
    if path:
        return "file"
    size = mapping["end"] - mapping["start"]
    next_guard = after is not None and after["start"] == mapping["end"] and after["perms"] == "---p" \
        and not after["path"]
    if mapping["start"] % ARENA_ALIGN == 0 and (next_guard or size == ARENA_ALIGN):
        return "arena"
    previous_guard = before is not None and before["end"] == mapping["start"] and before["perms"] == "---p" \
        and not before["path"]
    if previous_guard and mapping["perms"] == "rw-p":
        return "stack"
    return "anon_other"


def _empty_totals() -> dict[str, int]:
    return {"count": 0, "rss": 0, "pss": 0, "private_dirty": 0, "private_clean": 0, "anonymous": 0, "shared": 0}


def _add(totals: dict[str, int], mapping: dict) -> None:
    totals["count"] += 1
    totals["rss"] += mapping["Rss"]
    totals["pss"] += mapping["Pss"]
    totals["private_dirty"] += mapping["Private_Dirty"]
    totals["private_clean"] += mapping["Private_Clean"]
    totals["anonymous"] += mapping["Anonymous"]
    totals["shared"] += mapping["Shared_Clean"] + mapping["Shared_Dirty"]


def breakdown(mappings: list[dict]) -> dict:
    """Totals per class, the LIBRARY_LIMIT file-backed libraries with the most Pss, and the device mappings by path."""
    classes = {name: _empty_totals() for name in CLASSES}
    libraries: dict[str, dict] = {}
    devices: dict[str, dict] = {}
    for index, mapping in enumerate(mappings):
        before = mappings[index - 1] if index else None
        after = mappings[index + 1] if index + 1 < len(mappings) else None
        kind = classify(mapping, before, after)
        _add(classes[kind], mapping)
        if kind == "file":
            entry = libraries.setdefault(os.path.basename(mapping["path"]) or mapping["path"],
                                         {**_empty_totals(), "code_pss": 0, "data_pss": 0})
            _add(entry, mapping)
            entry["code_pss" if "x" in mapping["perms"] else "data_pss"] += mapping["Pss"]
        elif kind == "device":
            _add(devices.setdefault(mapping["path"].removesuffix(" (deleted)"), _empty_totals()), mapping)
    ranked = sorted(libraries.items(), key=lambda item: -item[1]["pss"])
    top = [{"name": name, **entry} for name, entry in ranked[:LIBRARY_LIMIT]]
    rest = _empty_totals()
    for _, entry in ranked[LIBRARY_LIMIT:]:
        for key in rest:
            rest[key] += entry[key]
    return {"classes": classes, "libraries_top": top, "libraries_rest": rest,
            "devices": [{"path": path, **entry} for path, entry in sorted(devices.items())]}


def sample_memory(pid: int) -> dict:
    """Rollup, peak and class breakdown of one process, in KiB."""
    base = f"/proc/{pid}"
    status = parse_kib_fields(read_text(f"{base}/status"), ("VmRSS", "VmHWM"))
    rollup = parse_kib_fields(read_text(f"{base}/smaps_rollup"), ROLLUP_FIELDS)
    mappings = parse_smaps(read_text(f"{base}/smaps"))
    if not mappings:
        raise SampleError(f"{base}/smaps lists no mapping")
    detail = breakdown(mappings)
    detail["shared_file_kib"] = detail["classes"]["file"]["shared"]
    return {"pid": pid, "name": read_text(f"{base}/comm").strip(), "rss_kib": status["VmRSS"],
            "peak_rss_kib": status["VmHWM"], "rollup": rollup, **detail}


def cpu_ticks(pid: int) -> tuple[int, int]:
    """(own user+system ticks, ticks of children it has reaped) from /proc/PID/stat."""
    text = read_text(f"/proc/{pid}/stat")
    try:
        fields = text.rsplit(")", 1)[1].split()
        return int(fields[11]) + int(fields[12]), int(fields[13]) + int(fields[14])
    except (IndexError, ValueError):
        raise SampleError(f"cannot parse /proc/{pid}/stat") from None


def thread_counters(pid: int) -> dict:
    """Per live thread (voluntary, involuntary) context switches, keyed by thread id."""
    found: dict[int, tuple[int, int]] = {}
    try:
        names = os.listdir(f"/proc/{pid}/task")
    except OSError as error:
        raise SampleError(f"cannot list /proc/{pid}/task: {error.strerror}") from None
    for name in names:
        try:
            found[int(name)] = _switches(Path(f"/proc/{pid}/task/{name}/status").read_text())
        except OSError:
            # The thread ended between the listing and the read; the next sample no longer lists it.
            continue
    if not found:
        raise SampleError(f"/proc/{pid} has no readable thread")
    return found


def switch_delta(before: dict[int, tuple[int, int]], after: dict[int, tuple[int, int]]) -> dict:
    """Context switches of the threads present in both samples, and how many threads came and went between them."""
    common = before.keys() & after.keys()
    return {"voluntary": sum(after[tid][0] - before[tid][0] for tid in common),
            "involuntary": sum(after[tid][1] - before[tid][1] for tid in common),
            "threads_started": len(after.keys() - before.keys()), "threads_ended": len(before.keys() - after.keys())}


def _switches(text: str) -> tuple[int, int]:
    voluntary = involuntary = None
    for line in text.splitlines():
        if line.startswith("voluntary_ctxt_switches:"):
            voluntary = int(line.split()[1])
        elif line.startswith("nonvoluntary_ctxt_switches:"):
            involuntary = int(line.split()[1])
    if voluntary is None or involuntary is None:
        raise SampleError("thread status has no context switch counters")
    return voluntary, involuntary



def thread_names(pid: int) -> dict[str, int]:
    """How many live threads carry each name, from /proc/PID/task/*/comm."""
    found: dict[str, int] = {}
    try:
        tasks = os.listdir(f"/proc/{pid}/task")
    except OSError as error:
        raise SampleError(f"cannot list /proc/{pid}/task: {error.strerror}") from None
    for task in tasks:
        try:
            name = Path(f"/proc/{pid}/task/{task}/comm").read_text().strip()
        except OSError:
            continue
        found[name] = found.get(name, 0) + 1
    return found
