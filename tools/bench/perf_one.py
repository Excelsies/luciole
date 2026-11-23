#!/usr/bin/env python3
"""Runs one scenario once in a window_check session and leaves result.json in the output folder.

Used by perf_bench.py, one process per run, so a run never shares state with another.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import perf_session  # noqa: E402  (also puts tools/window_check on the path)
import window_check  # noqa: E402
from perf_scenarios import SIZE, scenarios  # noqa: E402
from perf_session import BenchSession  # noqa: E402


def resolve(steps: list, root: Path) -> list:
    """The scenario actions with {root} replaced; nested action lists are resolved too."""
    out = []
    for item in steps:
        out.append(tuple(resolve(part, root) if isinstance(part, list) else
                         part.replace("{root}", str(root)) if isinstance(part, str) else part for part in item))
    return out


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True, help="the fixtures folder, with its manifest.json")
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--settings", type=Path, required=True)
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--mode", choices=("none", "perf", "heap"), default="none")
    parser.add_argument("--callgraph", default="fp")
    parser.add_argument("--shots", action="store_true")
    args = parser.parse_args(argv)
    manifest = json.loads((args.root / "manifest.json").read_text())
    found = scenarios(manifest)
    if args.scenario not in found:
        print(f"error: unknown scenario {args.scenario}; the names are {', '.join(found)}", file=sys.stderr)
        return 2
    scenario = dict(found[args.scenario])
    scenario["steps"] = resolve(scenario["steps"], args.root)
    perf_session.CONFIG |= {"scenario": scenario, "start": args.root / scenario["start"], "mode": args.mode,
                            "shots": args.shots, "callgraph": args.callgraph}
    perf_session.install()
    window_check.Session = BenchSession
    words = ["run", "--binary", str(args.binary), "--folder", str(args.root), "--output", str(args.output),
             "--size", SIZE, "--settings", str(args.settings), "--time-limit", str(scenario["time_limit"]),
             "--memory-limit", str(scenario["memory_limit_mib"])]
    if args.profile is not None:
        words += ["--profile", str(args.profile)]
    # window_check requires one step; the session ignores it and runs the scenario instead.
    return window_check.main([*words, "wait:0"])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
