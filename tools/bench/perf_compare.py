"""Flattens run results into named numbers, summarizes them across runs and compares two result sets.

A result set is the output folder of `perf_bench.py run`: one folder per scenario holding run-N.json files.
"""

import json
import statistics
from pathlib import Path

KIB = 1024


class ResultError(RuntimeError):
    pass


def mib(kib: float) -> float:
    return kib / KIB


def memory_metrics(prefix: str, memory: dict) -> dict[str, float]:
    classes = memory["classes"]
    rollup = memory["rollup"]
    return {f"{prefix}.pss_mib": mib(rollup["Pss"]), f"{prefix}.rss_mib": mib(rollup["Rss"]),
            f"{prefix}.private_dirty_mib": mib(rollup["Private_Dirty"]),
            f"{prefix}.private_clean_mib": mib(rollup["Private_Clean"]),
            f"{prefix}.anonymous_mib": mib(rollup["Anonymous"]), f"{prefix}.swap_mib": mib(rollup["Swap"]),
            f"{prefix}.shared_file_mib": mib(memory["shared_file_kib"]),
            f"{prefix}.peak_rss_mib": mib(memory["peak_rss_kib"]),
            f"{prefix}.heap_mib": mib(classes["heap"]["rss"]), f"{prefix}.arena_mib": mib(classes["arena"]["rss"]),
            f"{prefix}.stack_mib": mib(classes["stack"]["rss"]),
            f"{prefix}.anon_other_mib": mib(classes["anon_other"]["rss"]),
            f"{prefix}.file_pss_mib": mib(classes["file"]["pss"]),
            f"{prefix}.device_rss_mib": mib(classes["device"]["rss"])}


def checkpoint_metrics(point: dict) -> dict[str, float]:
    memory = point["memory"]
    found = memory_metrics("app", memory["app"])
    workers = memory["workers"]
    found |= {"workers.count": float(workers["count"]), "workers.pss_mib": mib(workers["sum_kib"]["Pss"]),
              "workers.rss_mib": mib(workers["sum_kib"]["Rss"]),
              "workers.private_dirty_mib": mib(workers["sum_kib"]["Private_Dirty"]),
              "workers.peak_rss_mib": mib(workers["sum_kib"]["peak_rss_kib"]),
              "tree.pss_mib": mib(memory["tree_pss_kib"]), "tree.rss_mib": mib(memory["tree_rss_kib"]),
              "since_launch_s": point["since_launch_s"], "cpu_total_s": point["cpu_s"]["total"],
              "cpu_delta_s": point["cpu_since_previous_s"], "threads.app": float(point["threads"]["app"]),
              "threads.tree": float(point["threads"]["tree"])}
    if point["wall_since_previous_s"] is not None:
        found["wall_delta_s"] = point["wall_since_previous_s"]
    if "busy_until_s" in point:
        found["busy_until_s"] = point["busy_until_s"]
    quiet = point.get("quiet")
    if quiet:
        found |= {"quiet.cpu_ms": quiet["cpu_ms"], "quiet.wakeups_per_s": quiet["wakeups_per_s"],
                  "quiet.app_wakeups_per_s": quiet["app_wakeups_per_s"],
                  "quiet.threads_started": float(quiet["threads_started"])}
    after = point.get("after_quiet")
    if after:
        found |= {"after_quiet.private_dirty_mib": mib(after["rollup"]["Private_Dirty"]),
                  "after_quiet.anonymous_mib": mib(after["rollup"]["Anonymous"]),
                  "after_quiet.rss_mib": mib(after["rss_kib"]),
                  "after_quiet.workers_pss_mib": mib(after["workers_sum_kib"]["Pss"])}
    hold = point.get("hold")
    if hold:
        found["hold.cpu_percent"] = hold["cpu_percent_of_one_core"]
    return found


def flatten(run: dict) -> dict[tuple[str, str], float]:
    """{(checkpoint, metric): value} of one run; the startup events are the checkpoint `startup`."""
    found: dict[tuple[str, str], float] = {}
    for point in run["checkpoints"]:
        for metric, value in checkpoint_metrics(point).items():
            found[(point["name"], metric)] = value
    for event, milliseconds in run.get("startup", {}).items():
        found[("startup", f"{event}_ms")] = milliseconds
    found[("run", "peak_tree_rss_mib")] = run["app_peak_rss_mib"]
    if run.get("playback"):
        for key in ("delivered", "taken", "replaced", "missed"):
            found[("run", f"playback.{key}")] = float(sum(item[key] for item in run["playback"]))
    for name, play in run.get("realtime", {}).items():
        found[(name, "realtime.ratio")] = play["ratio"]
        found[(name, "realtime.advance_s")] = play["advance_s"]
    for name, late in run.get("late", {}).items():
        found[(name, "late.max_unsettled")] = float(max(late["unsettled"], default=0))
        found[(name, "late.frames")] = float(late["frames"])
        if "empty" in late:
            found[(name, "late.max_empty")] = float(max(late["empty"], default=0))
            found[(name, "late.empty_frames")] = float(sum(count > 0 for count in late["empty"]))
        found[(name, "late.never_settled")] = float(late["first_all_settled_s"] is None)
        for key in ("first_all_settled_s", "last_unsettled_s", "interval_s"):
            if late[key] is not None:
                found[(name, f"late.{key}")] = late[key]
    return found


def load_set(folder: Path) -> dict[str, list[dict]]:
    """Scenario name to its run results; a failed run stays in the list with ok false."""
    found: dict[str, list[dict]] = {}
    if not folder.is_dir():
        raise ResultError(f"{folder} is not a folder")
    for scenario in sorted(path for path in folder.iterdir() if path.is_dir()):
        runs = [json.loads(path.read_text()) for path in sorted(scenario.glob("run-*.json"))]
        if runs:
            found[scenario.name] = runs
    if not found:
        raise ResultError(f"{folder} holds no run-N.json under a scenario folder")
    return found


def stats(values: list[float]) -> dict:
    return {"n": len(values), "median": statistics.median(values), "min": min(values), "max": max(values),
            "spread": max(values) - min(values)}


def summarize(runs: list[dict]) -> dict[tuple[str, str], dict]:
    """Median, minimum, maximum and spread of every number across the successful runs of one scenario."""
    good = [flatten(run) for run in runs if run.get("ok")]
    keys = sorted({key for flat in good for key in flat})
    return {key: stats([flat[key] for flat in good if key in flat]) for key in keys}


def verdict(before: dict, after: dict) -> str:
    """noise when the medians differ by no more than the larger run-to-run spread; n=1 when a spread is unknown."""
    delta = after["median"] - before["median"]
    if delta == 0:
        return "same"
    if before["n"] < 2 or after["n"] < 2:
        return "n=1"
    if abs(delta) <= max(before["spread"], after["spread"]):
        return "noise"
    return "lower" if delta < 0 else "higher"


def compare(before: dict[str, list[dict]], after: dict[str, list[dict]]) -> list[dict]:
    """One row per scenario, checkpoint and metric that both sets measured."""
    rows = []
    for scenario in sorted(before.keys() & after.keys()):
        old, new = summarize(before[scenario]), summarize(after[scenario])
        for key in sorted(old.keys() & new.keys()):
            delta = new[key]["median"] - old[key]["median"]
            percent = None if old[key]["median"] == 0 else delta / old[key]["median"] * 100
            rows.append({"scenario": scenario, "checkpoint": key[0], "metric": key[1], "before": old[key],
                         "after": new[key], "delta": delta, "percent": percent,
                         "verdict": verdict(old[key], new[key])})
    return rows


def fmt(value: float) -> str:
    return f"{value:.3f}" if abs(value) < 10 else f"{value:.1f}"


def table(rows: list[dict], significant_only: bool = False) -> str:
    lines = ["| scenario | checkpoint | metric | before | after | delta | percent | verdict |",
             "| --- | --- | --- | ---: | ---: | ---: | ---: | --- |"]
    for row in rows:
        if significant_only and row["verdict"] in ("noise", "same"):
            continue
        percent = "-" if row["percent"] is None else f"{row['percent']:+.1f}%"
        lines.append(f"| {row['scenario']} | {row['checkpoint']} | {row['metric']} | {fmt(row['before']['median'])}"
                     f" (+-{fmt(row['before']['spread'])}) | {fmt(row['after']['median'])}"
                     f" (+-{fmt(row['after']['spread'])}) | {row['delta']:+.3f} | {percent} | {row['verdict']} |")
    return "\n".join(lines) + "\n"


def failures(results: dict[str, list[dict]]) -> list[str]:
    return [f"{name} run {index}: {run.get('error')}" for name, runs in results.items()
            for index, run in enumerate(runs) if not run.get("ok")]


SUMMARY_COLUMNS = ("app.private_dirty_mib", "app.anonymous_mib", "app.rss_mib", "app.pss_mib", "workers.pss_mib",
                   "cpu_delta_s", "since_launch_s", "threads.app")


def summary_markdown(results: dict[str, list[dict]]) -> str:
    """A table per scenario: the main numbers per checkpoint as median (min-max) over the successful runs."""
    out: list[str] = []
    for scenario, runs in results.items():
        good = [run for run in runs if run.get("ok")]
        out += [f"### {scenario}", "", f"runs: {len(good)} ok of {len(runs)}"]
        out += [f"- run {index} failed: {run.get('error')}" for index, run in enumerate(runs) if not run.get("ok")]
        if not good:
            out.append("")
            continue
        summary = summarize(runs)
        names = [point["name"] for point in good[0]["checkpoints"]]
        out += ["", "| checkpoint | " + " | ".join(SUMMARY_COLUMNS) + " | quiet cpu ms | wakeups/s |",
                "| --- | " + " | ".join("---:" for _ in range(len(SUMMARY_COLUMNS) + 2)) + " |"]
        for name in names:
            cells = []
            for column in (*SUMMARY_COLUMNS, "quiet.cpu_ms", "quiet.wakeups_per_s"):
                item = summary.get((name, column))
                cells.append("-" if item is None else f"{fmt(item['median'])} ({fmt(item['min'])}-{fmt(item['max'])})")
            out.append(f"| {name} | " + " | ".join(cells) + " |")
        startup = {key[1]: value for key, value in summary.items() if key[0] == "startup"}
        if startup:
            out += ["", "startup (ms since launch): " + ", ".join(f"{name.removesuffix('_ms')} {fmt(item['median'])}"
                                                                for name, item in startup.items())]
        out.append("")
    return "\n".join(out) + "\n"
