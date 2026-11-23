#!/usr/bin/env python3
"""Performance and memory harness for Luciole: named scenarios in a private headless compositor, N runs each.

    python tools/bench/perf_bench.py fixtures                      build the fixtures (first use)
    python tools/bench/perf_bench.py list                          the scenario names
    python tools/bench/perf_bench.py run --binary B --label L --out DIR [--scenario a,b|all] [--runs 3]
    python tools/bench/perf_bench.py compare BEFORE_DIR AFTER_DIR [--significant] [--scenario a,b]
    python tools/bench/perf_bench.py summarize DIR
    python tools/bench/perf_bench.py profile --binary B --scenario S --mode perf|heap --out DIR
    python tools/bench/perf_bench.py baseline --binary B [--stock S] --label L --out DIR   everything above, once

See tools/bench/README.md. Runs are sequential and pinned to --cpus; the app and the compositor run in window_check's
sandbox with a private profile.
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import perf_compare  # noqa: E402
import perf_fixtures  # noqa: E402
import perf_realtime  # noqa: E402
import perf_tiles  # noqa: E402
from perf_scenarios import definition_hash, scenarios  # noqa: E402
from gui_bench import parse_trace  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
SCRATCH = REPO / ".cache" / "perf-scratch"
DEFAULT_CPUS = "6-11"
NOISY_CPU_PERCENT = 10.0
MIN_MEMORY_MIB = 2500
QUIET_MEMORY_MIB = 4096
SPREAD_LIMIT_PERCENT = 10.0
MAX_STARTUP_MS = 120_000
SCHEMA = 1


def parse_cpus(text: str) -> set[int]:
    cpus: set[int] = set()
    for part in text.split(","):
        low, _, high = part.partition("-")
        cpus |= set(range(int(low), int(high or low) + 1))
    if not cpus or max(cpus) >= os.cpu_count():
        raise ValueError(f"--cpus {text} names a processor this machine does not have")
    return cpus


def cpu_times(cpus: set[int]) -> tuple[int, int]:
    """(busy, total) jiffies of the given processors from /proc/stat."""
    busy = total = 0
    for line in Path("/proc/stat").read_text().splitlines():
        name = line.split()[0]
        if name.startswith("cpu") and name[3:].isdigit() and int(name[3:]) in cpus:
            values = [int(item) for item in line.split()[1:]]
            idle = values[3] + values[4]
            busy += sum(values) - idle
            total += sum(values)
    return busy, total


def meminfo() -> dict[str, int]:
    found = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        name, _, rest = line.partition(":")
        found[name] = int(rest.split()[0])
    return found


def machine_state(cpus: set[int], wait_s: float) -> dict:
    """The load around the pinned processors, so a noisy run can be recognized; waits a bounded time for quiet."""
    deadline = time.monotonic() + wait_s
    while True:
        before = cpu_times(cpus)
        time.sleep(1.0)
        after = cpu_times(cpus)
        busy = (after[0] - before[0]) / max(1, after[1] - before[1]) * 100
        available = meminfo()["MemAvailable"] // 1024
        if (busy < NOISY_CPU_PERCENT and available >= QUIET_MEMORY_MIB) or time.monotonic() > deadline:
            break
    info = meminfo()
    reasons = []
    if busy >= NOISY_CPU_PERCENT:
        reasons.append(f"pinned processors {busy:.0f} percent busy before the run")
    if available < QUIET_MEMORY_MIB:
        reasons.append(f"only {available} MiB available")
    return {"loadavg": list(os.getloadavg()), "pinned_cpu_busy_percent": busy, "mem_available_mib": available,
            "swap_used_mib": (info["SwapTotal"] - info["SwapFree"]) // 1024,
            "procs_running": next(int(line.split()[1]) for line in Path("/proc/stat").read_text().splitlines()
                                  if line.startswith("procs_running")), "noisy": bool(reasons), "noise_reasons": reasons}


def binary_info(path: Path) -> dict:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return {"path": str(path), "size": path.stat().st_size, "sha256": digest.hexdigest()}


def git_head() -> str:
    result = subprocess.run(["git", "-C", str(REPO), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    return result.stdout.strip() if result.returncode == 0 else "unknown"


class BinkLink:
    """A hard link to the sample named by LUCIOLE_BINK2_SAMPLE, under a neutral name; removed on exit."""

    def __init__(self, root: Path):
        self.folder = root / "bink"
        self.source = Path(os.environ["LUCIOLE_BINK2_SAMPLE"])

    def __enter__(self):
        self.folder.mkdir(exist_ok=True)
        try:
            os.link(self.source, self.folder / f"sample{self.source.suffix}")
        except OSError as error:
            self.folder.rmdir()
            raise RuntimeError(f"cannot hard link the Bink 2 sample ({error.strerror}); it must lie on the "
                               f"same filesystem as the fixtures") from None
        return self

    def __exit__(self, *_):
        for item in self.folder.glob("*"):
            item.unlink()
        self.folder.rmdir()


def run_session(args, scenario: dict, output: Path, profile: Path | None, mode: str = "none",
                callgraph: str = "fp") -> int:
    """One perf_one.py process for one scenario; its stdout and stderr go to output.log beside the folder."""
    scratch = SCRATCH / str(os.getpid())
    scratch.mkdir(parents=True, exist_ok=True)
    settings = scratch / "config.toml"
    settings.write_text(scenario["settings"])
    command = [sys.executable, str(HERE / "perf_one.py"), "--binary", str(args.binary), "--root", str(args.root),
               "--scenario", scenario["name"], "--output", str(output), "--settings", str(settings), "--mode", mode,
               "--callgraph", callgraph]
    if profile is not None:
        command += ["--profile", str(profile)]
    if getattr(args, "shots", False):
        command.append("--shots")
    with open(output.with_name(output.name + ".log"), "wb") as log:
        try:
            return subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=scenario["time_limit"] + 180).returncode
        except subprocess.TimeoutExpired:
            return 124


def startup_events(output: Path, launch_ns: int | None) -> dict:
    if launch_ns is None or not (output / "app.log").is_file():
        return {}
    events = parse_trace(output / "app.log", launch_ns)
    bad = {name: value for name, value in events.items() if not 0 <= value <= MAX_STARTUP_MS}
    if bad:
        raise RuntimeError(f"startup trace times outside 0 to {MAX_STARTUP_MS} ms: {bad}")
    return events


def playback_lines(path: Path) -> list[dict]:
    """The picture counts of every finished playback, from the app's trace lines "playback delivered=N ...", in order."""
    if not path.is_file():
        return []
    found = []
    for line in path.read_text(errors="replace").splitlines():
        parts = line.split(" ")
        if len(parts) > 3 and parts[0] == "luciole-trace" and parts[2] == "playback":
            found.append({key: int(value) for key, value in (item.split("=") for item in parts[3:])})
    return found


def merge(args, scenario: dict, output: Path, index: int, state: dict, code: int, binary: dict) -> dict:
    """The run's JSON: window_check's result reduced to what the harness measured, plus the run's provenance."""
    run = {"schema": SCHEMA, "scenario": scenario["name"], "scenario_hash": definition_hash(scenario),
           "label": args.label, "run": index, "binary": binary, "machine": state, "cpus": args.cpus,
           "exit_code": code, "ok": False, "error": None, "checkpoints": [], "startup": {}, "app_peak_rss_mib": None}
    path = output / "result.json"
    if not path.is_file():
        run["error"] = f"no result.json (exit code {code}); see {output.with_name(output.name + '.log')}"
        return run
    result = json.loads(path.read_text())
    bench = result.get("bench", {})
    run |= {"ok": bool(result.get("ok")) and code == 0, "error": result.get("error"),
            "checkpoints": bench.get("checkpoints", []), "app_peak_rss_mib": result.get("app_peak_rss_mib"),
            "elapsed_s": result.get("elapsed_s"), "umbriel_commit": result.get("umbriel_commit"),
            "watchdog": result.get("watchdog"), "output_size": result.get("output")}
    try:
        run["startup"] = startup_events(output, bench.get("launch_monotonic_ns"))
    except RuntimeError as error:
        run["ok"], run["error"] = False, run["error"] or str(error)
    try:
        run["realtime"] = {entry["name"]: perf_realtime.playback(output, entry) for entry in bench.get("realtime", [])}
    except perf_realtime.RealtimeError as error:
        run["ok"], run["error"] = False, run["error"] or str(error)
    try:
        run["late"] = {entry["name"]: perf_tiles.late_tiles(output, entry) for entry in bench.get("bursts", [])}
    except (perf_tiles.TileError, OSError) as error:
        run["ok"], run["error"] = False, run["error"] or f"burst screenshots: {error}"
    for path in output.glob("*.ppm"):
        path.unlink()
    run["playback"] = playback_lines(output / "app.log")
    errors = [f"{point['name']}: {message}" for point in run["checkpoints"] for message in point["errors"]]
    if errors and run["ok"]:
        run["ok"], run["error"] = False, f"checkpoint samples failed: {errors[:3]}"
    return run


def one_run(args, scenario: dict, folder: Path, index: int, binary: dict) -> dict:
    """Primes with the scenario's `prime` when it has one, then measures; both use one kept profile."""
    output = folder / f"run-{index}.d"
    if output.exists():
        shutil.rmtree(output)
    free = meminfo()["MemAvailable"] // 1024
    if free < MIN_MEMORY_MIB:
        raise RuntimeError(f"only {free} MiB of memory is available; not starting a run (limit {MIN_MEMORY_MIB})")
    profile = None
    if scenario["profile"] == "kept":
        profile = SCRATCH / f"profile-{os.getpid()}-{scenario['name']}-{index}"
        shutil.rmtree(profile, ignore_errors=True)
    try:
        if scenario["prime"]:
            prime = scenarios(args.manifest)[scenario["prime"]]
            prime_out = folder / f"run-{index}.prime.d"
            shutil.rmtree(prime_out, ignore_errors=True)
            if run_session(args, prime, prime_out, profile) != 0:
                return {"schema": SCHEMA, "scenario": scenario["name"], "run": index, "ok": False,
                        "error": f"the priming run {scenario['prime']} failed; see {prime_out}", "checkpoints": []}
        state = machine_state(args.cpu_set, args.wait_quiet)
        code = run_session(args, scenario, output, profile)
    finally:
        if profile is not None:
            shutil.rmtree(profile, ignore_errors=True)
    run = merge(args, scenario, output, index, state, code, binary)
    (folder / f"run-{index}.json").write_text(json.dumps(run, indent=1) + "\n")
    return run


def spread_percent(runs: list[dict]) -> float:
    """The largest run-to-run spread, as a percent of the median, of the app's private dirty memory and the CPU at the last checkpoint."""
    good = [run for run in runs if run.get("ok") and run["checkpoints"]]
    if len(good) < 2:
        return 0.0
    worst = 0.0
    for key in (("memory", "app", "rollup", "Private_Dirty"), ("cpu_s", "total")):
        values = []
        for run in good:
            value = run["checkpoints"][-1]
            for part in key:
                value = value[part]
            values.append(float(value))
        median = sorted(values)[len(values) // 2]
        if median > 0:
            worst = max(worst, (max(values) - min(values)) / median * 100)
    return worst


def run_scenario(args, scenario: dict, out: Path, binary: dict) -> dict:
    folder = out / scenario["name"]
    folder.mkdir(parents=True, exist_ok=True)
    if scenario["needs_env"] and not os.environ.get(scenario["needs_env"]):
        return {"skipped": f"{scenario['needs_env']} is not set"}
    link = BinkLink(args.root) if scenario["needs_env"] == "LUCIOLE_BINK2_SAMPLE" else None
    note = {"reruns": 0}
    try:
        if link:
            link.__enter__()
        for attempt in range(1 + (1 if args.rerun_noisy else 0)):
            runs = []
            for index in range(args.runs):
                run = one_run(args, scenario, folder, index, binary)
                runs.append(run)
                print(f"{scenario['name']} run {index}: {'ok' if run['ok'] else 'FAILED ' + str(run['error'])}",
                      flush=True)
            note["spread_percent"] = spread_percent(runs)
            if note["spread_percent"] <= SPREAD_LIMIT_PERCENT or attempt == 1 or not args.rerun_noisy:
                break
            rejected = folder / f"rejected-{int(time.time())}"
            rejected.mkdir()
            for item in list(folder.glob("run-*")):
                item.rename(rejected / item.name)
            note["reruns"] += 1
    finally:
        if link:
            link.__exit__()
    return note


def run_command(args) -> int:
    args.cpu_set = parse_cpus(args.cpus)
    os.sched_setaffinity(0, args.cpu_set)
    args.root = args.root.resolve()
    args.manifest = json.loads((args.root / "manifest.json").read_text())
    found = scenarios(args.manifest)
    names = list(found) if args.scenario == "all" else args.scenario.split(",")
    unknown = [name for name in names if name not in found]
    if unknown:
        raise ValueError(f"unknown scenario {', '.join(unknown)}; the names are {', '.join(found)}")
    args.binary = args.binary.resolve()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = binary_info(args.binary)
    meta = {"schema": SCHEMA, "label": args.label, "binary": binary, "cpus": args.cpus, "runs": args.runs,
            "scenario_hashes": {name: definition_hash(found[name]) for name in names}, "repo_head": git_head(),
            "started": datetime.now(timezone.utc).isoformat(), "kernel": os.uname().release, "notes": {}}
    SCRATCH.mkdir(parents=True, exist_ok=True)
    try:
        for name in names:
            meta["notes"][name] = run_scenario(args, found[name], out, binary)
            (out / "meta.json").write_text(json.dumps(meta, indent=1) + "\n")
    finally:
        shutil.rmtree(SCRATCH / str(os.getpid()), ignore_errors=True)
    return summarize_folder(out)


def summarize_folder(out: Path) -> int:
    results = perf_compare.load_set(out)
    meta = json.loads((out / "meta.json").read_text()) if (out / "meta.json").is_file() else {}
    lines = [f"# {meta.get('label', out.name)}", ""]
    if meta:
        lines += [f"binary: {meta['binary']['path']} ({meta['binary']['size']} bytes, sha256 "
                  f"{meta['binary']['sha256'][:12]}), repo head {meta['repo_head']}, kernel {meta['kernel']}, "
                  f"processors {meta['cpus']}, {meta['runs']} runs per scenario", ""]
        for name, note in meta["notes"].items():
            flag = " OVER THE LIMIT" if note.get("spread_percent", 0) > SPREAD_LIMIT_PERCENT else ""
            lines.append(f"- {name}: " + (note.get("skipped") or
                                         f"spread {note.get('spread_percent', 0):.1f} percent, "
                                         f"{note.get('reruns', 0)} reruns{flag}"))
        lines.append("")
    noisy = [f"{name} run {run['run']}: {', '.join(run['machine']['noise_reasons'])}" for name, runs in results.items()
             for run in runs if run.get("machine", {}).get("noisy")]
    lines += ["Noisy runs: " + ("; ".join(noisy) if noisy else "none"), ""]
    failed = perf_compare.failures(results)
    lines += ["Failed runs: " + ("; ".join(failed) if failed else "none"), ""]
    lines.append(perf_compare.summary_markdown(results))
    (out / "summary.md").write_text("\n".join(lines))
    flat = {name: {f"{key[0]}/{key[1]}": value for key, value in perf_compare.summarize(runs).items()}
            for name, runs in results.items()}
    (out / "summary.json").write_text(json.dumps(flat, indent=1) + "\n")
    print(f"summary: {out / 'summary.md'}")
    return 1 if failed else 0


def compare_command(args) -> int:
    before, after = perf_compare.load_set(args.before), perf_compare.load_set(args.after)
    wanted = set(args.scenario.split(",")) if args.scenario else None
    rows = [row for row in perf_compare.compare(before, after) if wanted is None or row["scenario"] in wanted]
    metric = args.metric
    rows = [row for row in rows if metric is None or metric in row["metric"]]
    text = perf_compare.table(rows, args.significant)
    skipped = sorted(before.keys() ^ after.keys())
    if skipped:
        text += f"\nOnly in one set: {', '.join(skipped)}\n"
    for label, results in (("before", before), ("after", after)):
        failed = perf_compare.failures(results)
        if failed:
            text += f"\nFailed runs in {label}: {'; '.join(failed)}\n"
    if args.out:
        args.out.write_text(text)
    print(text)
    return 0


BASELINE_PERF = ("workbench-100k", "pictures", "video", "start-details")
BASELINE_HEAP = ("workbench-100k", "previews", "churn")
STOCK_SCENARIOS = "start-details,list-20k,list-20k-icons,pictures,pictures-warm,tabs"
DWARF = "dwarf,16384"


def baseline_command(args) -> int:
    """The full baseline in one command: every scenario on the binary, the non-Workbench ones on the stock binary, then the profiles."""
    import perf_profile
    out = args.out.resolve()
    common = {"cpus": args.cpus, "root": args.root, "wait_quiet": args.wait_quiet, "shots": False}
    failures: list[str] = []
    steps = [("scenarios", lambda: run_command(argparse.Namespace(
        binary=args.binary, label=args.label, out=out, scenario="all", runs=args.runs, rerun_noisy=True, **common)))]
    if args.stock:
        steps.append(("stock", lambda: run_command(argparse.Namespace(
            binary=args.stock, label=f"{args.label} stock", out=out / "stock", scenario=STOCK_SCENARIOS,
            runs=args.runs, rerun_noisy=True, **common))))
    for mode, names in (("perf", BASELINE_PERF), ("heap", BASELINE_HEAP)):
        for name in names:
            steps.append((f"{mode} {name}", lambda name=name, mode=mode: perf_profile.profile_command(
                argparse.Namespace(binary=args.binary, scenario=name, mode=mode, out=out / "profiles", cpus=args.cpus,
                                   root=args.root, callgraph=DWARF), sys.modules[__name__])))
    for title, step in steps:
        try:
            if step() not in (0, None):
                failures.append(f"{title}: reported failed runs")
        except (ValueError, RuntimeError, OSError) as error:
            failures.append(f"{title}: {error}")
        print(f"baseline step {title}: done", flush=True)
    print("failures: " + ("; ".join(failures) if failures else "none"))
    return 1 if failures else 0


def parser() -> argparse.ArgumentParser:
    top = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = top.add_subparsers(dest="command", required=True)
    commands.add_parser("list")
    commands.add_parser("fixtures").add_argument("--only", default="")
    run = commands.add_parser("run")
    run.add_argument("--binary", type=Path, required=True)
    run.add_argument("--label", required=True)
    run.add_argument("--out", type=Path, required=True)
    run.add_argument("--scenario", default="all")
    run.add_argument("--runs", type=int, default=3)
    run.add_argument("--cpus", default=DEFAULT_CPUS)
    run.add_argument("--root", type=Path, default=perf_fixtures.DEFAULT_ROOT)
    run.add_argument("--wait-quiet", type=float, default=120.0, help="seconds to wait for a quiet machine before a run")
    run.add_argument("--rerun-noisy", action="store_true", help="when the spread is over 10 percent, run again once")
    run.add_argument("--shots", action="store_true", help="a screenshot at every checkpoint")
    both = commands.add_parser("compare")
    both.add_argument("before", type=Path)
    both.add_argument("after", type=Path)
    both.add_argument("--significant", action="store_true", help="hide rows that are noise or unchanged")
    both.add_argument("--scenario")
    both.add_argument("--metric", help="keep metrics whose name contains this text")
    both.add_argument("--out", type=Path)
    commands.add_parser("summarize").add_argument("folder", type=Path)
    base = commands.add_parser("baseline", help="run, stock run and all profiles, as one command")
    base.add_argument("--binary", type=Path, required=True)
    base.add_argument("--stock", type=Path, help="the stock binary, measured on the scenarios that do not need the Workbench")
    base.add_argument("--label", required=True)
    base.add_argument("--out", type=Path, required=True)
    base.add_argument("--runs", type=int, default=3)
    base.add_argument("--cpus", default=DEFAULT_CPUS)
    base.add_argument("--root", type=Path, default=perf_fixtures.DEFAULT_ROOT)
    base.add_argument("--wait-quiet", type=float, default=120.0)
    prof = commands.add_parser("profile")
    prof.add_argument("--binary", type=Path, required=True)
    prof.add_argument("--scenario", required=True)
    prof.add_argument("--mode", choices=("perf", "heap"), required=True)
    prof.add_argument("--out", type=Path, required=True)
    prof.add_argument("--cpus", default=DEFAULT_CPUS)
    prof.add_argument("--root", type=Path, default=perf_fixtures.DEFAULT_ROOT)
    prof.add_argument("--callgraph", default="fp", help="perf call graph method: fp, or dwarf for binaries without frame pointers")
    return top


def main(argv: list[str]) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command == "list":
            manifest = json.loads((perf_fixtures.DEFAULT_ROOT / "manifest.json").read_text())
            print("\n".join(scenarios(manifest)))
            return 0
        if args.command == "fixtures":
            print(json.dumps(perf_fixtures.build(perf_fixtures.DEFAULT_ROOT, [n for n in args.only.split(",") if n]),
                             indent=1))
            return 0
        if args.command == "run":
            return run_command(args)
        if args.command == "compare":
            return compare_command(args)
        if args.command == "summarize":
            return summarize_folder(args.folder)
        if args.command == "baseline":
            return baseline_command(args)
        import perf_profile
        return perf_profile.profile_command(args, sys.modules[__name__])
    except (ValueError, RuntimeError, perf_compare.ResultError, perf_fixtures.FixtureError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
