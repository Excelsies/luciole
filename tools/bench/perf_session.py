"""A window_check session that runs a scenario and samples the app at checkpoints.

It reuses window_check's compositor, sandbox, input helper, memory watchdog and steps; only the step loop and the
app launch (a start folder inside the bound fixtures folder, and the profile mode wrappers) differ.
"""

import os
import signal
import subprocess
import sys
import time
from collections import deque
from pathlib import Path

WINDOW_CHECK = Path(__file__).resolve().parents[1] / "window_check"
sys.path.insert(0, str(WINDOW_CHECK))

import session as wc_session  # noqa: E402
from app import ASSETS  # noqa: E402
from isolation import check_child_environment  # noqa: E402
from perf_proc import (CLK_TCK, SampleError, cpu_ticks, sample_memory, switch_delta, thread_counters,  # noqa: E402
                       thread_names)
from processes import descendants, kill_with_parent  # noqa: E402
from sandbox import sandbox_command  # noqa: E402
from step_actions import StepFailed, perform  # noqa: E402
from steps import parse_step  # noqa: E402

SETTLE_WINDOW = 1.0
# One tick is 10 ms: no more than one tick of CPU in a settle window counts as quiet.
SETTLE_TICKS = 1
SETTLE_TIMEOUT = 30.0
POLL = 0.1
# A burst of screenshots is bounded by time and by count (a PPM of 1920x1080 is 6 MB).
BURST_MAX_FRAMES = 400
BURST_MAX_SECONDS = 20.0
MAX_UNTIL_SECONDS = 600.0
HEAP_SIGNAL = signal.SIGUSR1
HEAP_DUMP_TIMEOUT = 30.0
PROFILE_FOLDER = "profile-out"
PERF_FREQUENCY = "999"
APP_ENV = {"LUCIOLE_STARTUP_TRACE": "1", "GST_PLUGIN_FEATURE_RANK": "interaudiosink:MAX"}
# The tcmalloc heap profiler and the folder it writes to, inside the sandbox.
HEAP_LIBRARY = "/usr/lib/libtcmalloc_and_profiler.so"

# Set by perf_one before the run; the forked worker inherits them.
CONFIG: dict = {"scenario": None, "start": None, "mode": "none", "shots": False, "callgraph": "fp"}
LAUNCH: dict = {"ns": None}


def profile_folder(run_folder: Path) -> Path:
    return run_folder / PROFILE_FOLDER


def wrapped_command(binary: Path, start: Path, run_folder: Path, mode: str) -> list[str]:
    """The command inside the sandbox: the binary alone, or with the heap profiler preloaded."""
    command = [str(binary), str(start)]
    out = profile_folder(run_folder)
    if mode == "heap":
        return ["/usr/bin/env", f"LD_PRELOAD={HEAP_LIBRARY}", f"HEAPPROFILE={out / 'heap'}",
                f"HEAPPROFILESIGNAL={int(HEAP_SIGNAL)}", "HEAP_PROFILE_ALLOCATION_INTERVAL=0",
                "HEAP_PROFILE_INUSE_INTERVAL=0", "HEAP_PROFILE_TIME_INTERVAL=0", *command]
    if mode in ("none", "perf"):
        return command
    raise ValueError(f"unknown profile mode {mode}")


def start_app(binary, folder, env, profile, hidden, log):
    """Replaces window_check's start_app: opens the scenario's start folder, which lies inside the bound `folder`."""
    run_folder = profile.root
    profile_folder(run_folder).mkdir(parents=True, exist_ok=True)
    inner = sandbox_command(wrapped_command(binary, CONFIG["start"], run_folder, CONFIG["mode"]),
                            [*profile.writable(), folder], [binary, ASSETS], hidden)
    # A scope without swap: the kernel would otherwise page the app out on this machine, which hides its memory.
    # systemd-run needs the caller's session to reach the user manager, so the private environment goes through env.
    command = ["systemd-run", "--user", "--scope", "--quiet", "--collect", "-p", "MemorySwapMax=0", "--",
               "/usr/bin/env", "-i", *[f"{key}={value}" for key, value in env.items()], *inner]
    if CONFIG["mode"] == "perf":
        # perf cannot open events inside the sandbox's user namespace, so it records the whole chain from outside;
        # the events are inherited by the sandbox and the app.
        command = ["perf", "record", "-q", "-o", str(profile_folder(run_folder) / "perf.data"), "-F", PERF_FREQUENCY,
                   "-e", "cpu-clock:u", "--user-callchains", "--call-graph", CONFIG["callgraph"], "--", *command]
    LAUNCH["ns"] = time.monotonic_ns()
    with open(log, "wb") as output:
        return subprocess.Popen(command, env=dict(os.environ), stdout=output, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, cwd=profile.home, start_new_session=True,
                                preexec_fn=kill_with_parent())


class BenchSession(wc_session.Session):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.bench: dict = {"checkpoints": [], "mode": CONFIG["mode"], "actions": 0}
        self.result["bench"] = self.bench
        self.previous = {"ns": None, "cpu_s": 0.0}
        self.marks: dict[str, float] = {}

    def app_environment(self) -> dict[str, str]:
        env = super().app_environment() | APP_ENV
        check_child_environment(env, self.caller, self.app_profile)
        return env

    def worker_reads(self, read) -> list:
        """`read(pid)` for every live child; a child that ended since the listing is skipped, since its CPU time
        is already in the app's reaped total. Any other failure is an error."""
        found = []
        for pid in descendants(self.app_pid):
            try:
                found.append(read(pid))
            except SampleError:
                if os.path.exists(f"/proc/{pid}"):
                    raise
        return found

    def ticks(self) -> dict:
        """CPU ticks of the live tree and of the children the app has already reaped."""
        own, reaped = cpu_ticks(self.app_pid)
        workers = sum(sum(item) for item in self.worker_reads(cpu_ticks))
        return {"app": own, "workers_live": workers, "reaped": reaped, "total": own + workers + reaped}

    def settle(self, timeout: float) -> dict:
        """Waits until the tree used at most SETTLE_TICKS of CPU over SETTLE_WINDOW seconds, or the timeout."""
        begun = time.monotonic()
        history: deque = deque()
        while True:
            now = time.monotonic()
            ticks = self.ticks()
            history.append((now, ticks))
            base = None
            for stamp, earlier in history:
                if stamp <= now - SETTLE_WINDOW:
                    base = (stamp, earlier)
            while len(history) > 1 and history[1][0] <= now - SETTLE_WINDOW:
                history.popleft()
            if base is not None and ticks["total"] - base[1]["total"] <= SETTLE_TICKS:
                return {"settled": True, "waited_s": now - begun, "busy_end_t": base[0],
                        "cpu_at_busy_end_s": base[1]["total"] / CLK_TCK}
            if now - begun > timeout:
                return {"settled": False, "waited_s": now - begun, "busy_end_t": None, "cpu_at_busy_end_s": None}
            self.pause(POLL)

    def hold(self, seconds: float) -> dict:
        """Runs a window of `seconds` without waiting for quiet, as playback needs, and reports its CPU use."""
        before = self.ticks()
        began = time.monotonic()
        self.pause(seconds)
        after = self.ticks()
        elapsed = time.monotonic() - began
        cpu = {key: (after[key] - before[key]) / CLK_TCK for key in ("app", "workers_live", "total")}
        return {"seconds": elapsed, "cpu_s": cpu, "cpu_percent_of_one_core": cpu["total"] / elapsed * 100}

    def memory(self) -> tuple[dict, list[str]]:
        errors: list[str] = []
        exited: list[str] = []
        app = sample_memory(self.app_pid)
        workers = []
        for pid in descendants(self.app_pid):
            try:
                workers.append(sample_memory(pid))
            except SampleError as error:
                # A worker that ended between the listing and the read has nothing left to sample.
                (errors if os.path.exists(f"/proc/{pid}") else exited).append(f"worker {pid}: {error}")
        totals = {key: sum(worker["rollup"][key] for worker in workers) for key in ("Rss", "Pss", "Private_Dirty",
                                                                                      "Private_Clean", "Anonymous")}
        totals["peak_rss_kib"] = sum(worker["peak_rss_kib"] for worker in workers)
        return {"app": app, "workers": {"count": len(workers), "sum_kib": totals, "processes": workers,
                            "exited_during_sampling": exited},
                "tree_pss_kib": app["rollup"]["Pss"] + totals["Pss"],
                "tree_rss_kib": app["rollup"]["Rss"] + totals["Rss"]}, errors

    def memory_summary(self) -> dict:
        """The app's memory after a quiet window, reduced to the totals and classes; release after idle time shows here."""
        memory, errors = self.memory()
        app = memory["app"]
        return {"rss_kib": app["rss_kib"], "peak_rss_kib": app["peak_rss_kib"], "rollup": app["rollup"],
                "classes": app["classes"], "workers_sum_kib": memory["workers"]["sum_kib"], "errors": errors}

    def shot(self, name: str) -> dict:
        """A screenshot for the playback position, with the time it was taken; the file is read after the run."""
        path = self.options.output / f"rt-{name}.png"
        self.compositor.screenshot(path)
        return {"file": path.name, "t": time.monotonic()}

    def mark(self, name: str) -> None:
        self.marks[name] = time.monotonic()

    def until(self, mark: str, seconds: float) -> None:
        """Waits until `seconds` after the mark; a time already past is not waited for."""
        if mark not in self.marks:
            raise StepFailed(f"until: no mark named {mark}")
        if not 0 <= seconds <= MAX_UNTIL_SECONDS:
            raise StepFailed(f"until: {seconds} s is outside 0 to {MAX_UNTIL_SECONDS:.0f}")
        remaining = self.marks[mark] + seconds - time.monotonic()
        if remaining > 0:
            self.pause(remaining)

    def burst(self, name: str, options: dict) -> None:
        """Screenshots one after another for `seconds` (PPM: no compression time), each stamped with the middle of
        the call that took it, then one settled screenshot after the app is quiet. The frames are read against the
        settled one after the run (perf_tiles); the time is counted from the mark."""
        if options["mark"] not in self.marks:
            raise StepFailed(f"burst {name}: no mark named {options['mark']}")
        seconds = min(float(options["seconds"]), BURST_MAX_SECONDS)
        if any(item["name"] == name for item in self.bench.setdefault("bursts", [])):
            raise StepFailed(f"burst {name} is named twice")
        frames = []
        begun = time.monotonic()
        while time.monotonic() - begun < seconds and len(frames) < BURST_MAX_FRAMES:
            self.watch()
            path = self.options.output / f"burst-{name}-{len(frames):03d}.ppm"
            before = time.monotonic()
            self.compositor.screenshot(path, "ppm")
            after = time.monotonic()
            frames.append({"file": path.name, "t": (before + after) / 2, "call_s": after - before})
        self.settle(SETTLE_TIMEOUT)
        settled = self.options.output / f"burst-{name}-settled.ppm"
        self.compositor.screenshot(settled, "ppm")
        self.bench["bursts"].append({"name": name, "t0": self.marks[options["mark"]], "frames": frames,
                                     "settled": {"file": settled.name, "t": time.monotonic()}})

    def counters(self) -> dict:
        """CPU ticks and the context switches of every live thread of the tree, keyed by pid and thread id."""
        threads = {self.app_pid: thread_counters(self.app_pid)}
        for pid in descendants(self.app_pid):
            try:
                threads[pid] = thread_counters(pid)
            except SampleError:
                if os.path.exists(f"/proc/{pid}"):
                    raise
        return {"ticks": self.ticks()["total"], "threads": threads}

    def quiet_window(self, seconds: float) -> dict:
        """CPU and wakeups of the tree over a fixed window in which the script does nothing."""
        before = self.counters()
        began = time.monotonic()
        self.pause(seconds)
        after = self.counters()
        elapsed = time.monotonic() - began
        total = {"voluntary": 0, "involuntary": 0, "threads_started": 0, "threads_ended": 0}
        app = {"voluntary": 0, "involuntary": 0}
        for pid in before["threads"].keys() & after["threads"].keys():
            delta = switch_delta(before["threads"][pid], after["threads"][pid])
            for key in total:
                total[key] += delta[key]
            if pid == self.app_pid:
                app = delta
        return {"seconds": elapsed, "cpu_ms": (after["ticks"] - before["ticks"]) * 1000 / CLK_TCK,
                "cpu_resolution_ms": 1000 / CLK_TCK, "voluntary_switches": total["voluntary"],
                "involuntary_switches": total["involuntary"], "app_voluntary_switches": app["voluntary"],
                "wakeups_per_s": total["voluntary"] / elapsed, "app_wakeups_per_s": app["voluntary"] / elapsed,
                "threads_started": total["threads_started"], "threads_ended": total["threads_ended"]}

    def heap_dump(self, name: str) -> list[str]:
        folder = profile_folder(self.run_folder)
        seen = {path.name for path in folder.glob("heap.*.heap")}
        os.kill(self.app_pid, HEAP_SIGNAL)
        deadline = time.monotonic() + HEAP_DUMP_TIMEOUT
        while time.monotonic() < deadline:
            fresh = sorted(path.name for path in folder.glob("heap.*.heap") if path.name not in seen)
            if fresh:
                # The profiler writes the file in one pass; give it a moment to finish.
                self.pause(0.3)
                return fresh
            self.pause(0.1)
        raise StepFailed(f"the heap profiler wrote no dump for checkpoint {name} within {HEAP_DUMP_TIMEOUT:.0f} s")

    def checkpoint(self, name: str, options: dict) -> None:
        if any(item["name"] == name for item in self.bench["checkpoints"]):
            raise StepFailed(f"checkpoint {name} is named twice")
        hold = None
        if options.get("realtime"):
            first = self.shot(f"{name}-a")
        if options.get("hold"):
            hold = self.hold(options["hold"])
        if options.get("realtime"):
            self.bench.setdefault("realtime", []).append(
                {"name": name, "duration_s": options["realtime"], "first": first, "second": self.shot(f"{name}-b")})
        if hold is not None or options.get("settle", True) is False:
            settle = {"settled": None, "waited_s": 0.0, "busy_end_t": None, "cpu_at_busy_end_s": None}
        else:
            settle = self.settle(options.get("settle_timeout", SETTLE_TIMEOUT))
        now_ns = time.monotonic_ns()
        ticks = self.ticks()
        threads = {"app": len(thread_counters(self.app_pid)),
                   "tree": len(thread_counters(self.app_pid)) + sum(self.worker_reads(lambda pid: len(thread_counters(pid))))}
        memory, errors = self.memory()
        record = {"name": name, "since_launch_s": (now_ns - LAUNCH["ns"]) / 1e9, "settle": settle,
                  "cpu_s": {key: value / CLK_TCK for key, value in ticks.items()},
                  "cpu_since_previous_s": ticks["total"] / CLK_TCK - self.previous["cpu_s"],
                  "wall_since_previous_s": None if self.previous["ns"] is None
                  else (now_ns - self.previous["ns"]) / 1e9,
                  "threads": threads, "thread_names": thread_names(self.app_pid), "memory": memory, "errors": errors}
        busy_end = settle.pop("busy_end_t")
        if busy_end is not None:
            record["busy_until_s"] = busy_end - LAUNCH["ns"] / 1e9
        if hold is not None:
            record["hold"] = hold
        if options.get("quiet"):
            record["quiet"] = self.quiet_window(options["quiet"])
            record["after_quiet"] = self.memory_summary()
        if CONFIG["mode"] == "heap":
            record["heap_dumps"] = self.heap_dump(name)
        if CONFIG["shots"]:
            self.compositor.screenshot(self.options.output / f"{name}.png")
        self.previous = {"ns": now_ns, "cpu_s": ticks["total"] / CLK_TCK}
        self.bench["checkpoints"].append(record)

    def action(self, item: tuple) -> None:
        kind = item[0]
        self.bench["actions"] += 1
        if kind == "do":
            perform(self, parse_step(item[1]), {})
        elif kind == "wait":
            self.pause(item[1])
        elif kind == "mark":
            self.mark(item[1])
        elif kind == "until":
            self.until(item[1], item[2])
        elif kind == "burst":
            self.burst(item[1], item[2])
        elif kind == "cp":
            self.checkpoint(item[1], item[2] if len(item) > 2 else {})
        elif kind == "repeat":
            for _ in range(item[1]):
                for inner in item[2]:
                    self.action(inner)
        else:
            raise StepFailed(f"unknown scenario action {kind}")

    def run_steps(self) -> None:
        self.bench["launch_monotonic_ns"] = LAUNCH["ns"]
        for index, item in enumerate(CONFIG["scenario"]["steps"], 1):
            try:
                self.watch()
                self.action(item)
            except StepFailed:
                raise
            except (SampleError, OSError) as error:
                raise StepFailed(f"scenario action {index} {item[:2]}: {error}") from None

    def _stop(self) -> None:
        super()._stop()
        source = profile_folder(self.run_folder)
        if CONFIG["mode"] != "none" and source.is_dir():
            target = self.options.output / "profile"
            target.mkdir(exist_ok=True)
            for path in source.iterdir():
                path.replace(target / path.name)


def install() -> None:
    """Points window_check's session module at the bench launch; call once in the process that runs the session."""
    wc_session.start_app = start_app
