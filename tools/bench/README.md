# Benchmarks

`gui_bench.py` measures start-up, CPU and memory per view of one or more builds in a private headless Umbriel compositor (see its docstring). `perf_bench.py` below measures what the app does under input, per scenario and checkpoint, and compares two result sets.

## perf_bench.py

Named scenarios, run N times each against one binary, in the private compositor and sandbox of `tools/window_check` (its session, input helper, memory watchdog and isolation checks are reused, not copied). Python standard library only; `perf`, `addr2line`, `nm`, `gst-launch-1.0` (fixtures), `systemd-run` and `bwrap` are used when present.

```sh
python tools/bench/perf_bench.py fixtures                    # once; writes .cache/perf-fixtures (about 4 GB on disk)
python tools/bench/perf_bench.py list
python tools/bench/perf_bench.py run --binary BIN --label NAME --out DIR [--scenario a,b|all] [--runs 3] [--cpus 6-11]
python tools/bench/perf_bench.py compare BEFORE_DIR AFTER_DIR [--significant] [--scenario a,b] [--metric text]
python tools/bench/perf_bench.py summarize DIR               # rewrites DIR/summary.md and summary.json
python tools/bench/perf_bench.py profile --binary BIN --scenario S --mode perf|heap --out DIR [--callgraph dwarf,16384]
python tools/bench/perf_bench.py baseline --binary BIN [--stock STOCK] --label NAME --out DIR   # all scenarios x3, stock, all profiles
python -m unittest discover -s tools/bench/tests
```

Use a copy of the binary outside any `build-*` folder (window_check refuses those). The run takes the window_check lock, so one graphical program runs at a time; never start two harness runs together.

### What a run does

Each run starts the app cold with a fresh private profile at 1920x1080 (`pictures-warm` reuses one profile after a priming run), pinned to `--cpus` (default 6 to 11, the cores window_check builds on). The app runs in a systemd scope with `MemorySwapMax=0`: on a loaded machine the kernel otherwise pages the app out and its memory reads low. The window_check watchdog kills the tree at 4096 MiB resident.

The stock (upstream) binary has no Workbench view, so `baseline --stock` runs only the scenarios that use the list or grid view and the keyboard: `start-details`, `list-20k`, `list-20k-icons`, `pictures`, `pictures-warm`, `tabs`; the Workbench scenarios (`workbench-100k`, `previews`, `video`, `video-bink`, `text-big`, `churn`) are not run on it.

Scenarios live in `perf_scenarios.py` as data: `start-details`, `list-20k`, `list-20k-icons`, `workbench-100k`, `previews`, `pictures`, `pictures-rgba` (the same walk over `pictures-rgba-300`: the same pictures with an alpha channel of 255 everywhere), `pictures-warm`, `video`, `video-bink` (only with `LUCIOLE_BINK2_SAMPLE` set to a file on the fixtures' filesystem; it is hard linked under a neutral name and unlinked afterward), `text-big` (the 20 MB log and the 100,000 row CSV are previewed and scrolled; the editor refuses files over 1 MiB and tables over the 10,000 rows the preview shows, so editing is tried on both and then used on a 900 KB log and a 9,000 row CSV), `churn`, `tabs`, and the three that leave and return to the picture folder (below). The playback scenarios (`video`, `video-bink`, `video-4k`, `video-60`) play a clip for 10 s while the position is read from two screenshots of the seek bar (`realtime` in the run file: position advance against wall time, so lost time and a clip that does not keep real time show), pause it for 40 s, play it again for 10 s (`resumed`), pause, select a file that is no media and leave the folder; `video-end` plays a 3 s clip to its end and waits 30 s; `video-switch`, `video-tab-playing` and `video-tab-paused` leave a playing or paused clip by selecting another media file or opening a tab. Each `quiet` checkpoint also records the memory read after its quiet window (`after_quiet`) and every checkpoint the thread names. The test clips are 4:2:0 8-bit H.264 like ordinary video. Interactive scenarios take a settled checkpoint before the first click: a click in the first frames lands on a layout that is not final. Pointer positions (instrument and ribbon buttons, the preview picture, Enable editing, list rows) are fixed pixel coordinates at 1920x1080 and scale 1; `--shots` saves a screenshot at every checkpoint to confirm they still land after a layout change.

A checkpoint waits until the app and its children use at most one CPU tick (10 ms) in one second, for at most 30 s (`settle.settled` is false when it never got quiet: that is a finding, not a failure), then records:

- memory of the app and, apart, its child processes, from `/proc/PID/smaps_rollup` and `smaps`: Rss, Pss, Private_Dirty, Private_Clean, Anonymous, Swap, shared file-backed, peak Rss (`VmHWM`), and a breakdown by class (`heap`, `arena` for glibc arenas, `stack`, `anon_other`, `file`, `device`). Arenas and stacks are told from other anonymous memory by alignment and guard pages, so they are close, not exact. `file` is listed per library (top 15, code and data Pss). `device` holds `/dev/nvidia*`, `/dev/dri`, memfd and shm mappings, which are not the app's to shrink;
- CPU: ticks of the app, of live children and of reaped children, and the change since the previous checkpoint; wall time; `since_launch_s`; `busy_until_s` (when the last activity before the quiet second ended); thread counts;
- `quiet` (checkpoints with a window): CPU and voluntary context switches over a fixed window in which the script does nothing, as `wakeups_per_s` (threads that start or end inside the window are left out and counted);
- `hold` (playback): CPU percent of one core over a window without settling.

Start-up events come from `LUCIOLE_STARTUP_TRACE` (time since launch to `first-frame`, `content-presented` and the others). A sample that fails is listed under the checkpoint's `errors` and fails the run; a child that exits between listing and reading is listed as `exited_during_sampling`.

Pss of file-backed memory depends on how many other processes map the same libraries at that moment, so it wobbles by 10 to 15 MiB between identical runs; judge memory changes on `private_dirty`, `anonymous`, the class breakdown and `rss`, which stay within a few MiB.

CPU is read in ticks of 10 ms, so a quiet window of 10 s resolves 0.1 percent. Memory numbers come from one reading at each checkpoint, not a continuous peak; `peak_rss` (`VmHWM`) and the watchdog's `app_peak_rss_mib` cover peaks.

### Result files

`DIR/meta.json` (binary path, size and sha256, scenario hashes, repo head, processors), one `DIR/SCENARIO/run-N.json` per run (scenario hash, machine load before the run: load average, busy percent of the pinned processors, available memory, whether the run is `noisy`), `run-N.d/` (app log, compositor log, `result.json`), `DIR/summary.md` and `summary.json` (median, minimum, maximum and spread over the runs). `--rerun-noisy` runs a scenario again once when the spread of the last checkpoint's private dirty memory or CPU is over 10 percent.

### compare

Rows per scenario, checkpoint and metric that both sets measured, with medians, spread, delta, percent and a verdict: `noise` when the medians differ by no more than the larger run-to-run spread, `lower` or `higher` otherwise, `n=1` when a set has one run and no spread is known. Memory is in MiB, time in seconds or milliseconds as the metric name says; a lower number is better for every metric.

### Profile modes

For diagnosis only; both slow the app, so never compare their numbers across builds.

- `--mode perf`: `perf record -e cpu-clock:u --user-callchains` around the sandbox (perf cannot open events inside the sandbox's user namespace), then `perf-report.md` with the top 15 functions by self and inclusive samples, our code apart from libraries, and `perf-folded.txt` (folded stacks). The release binary keeps its symbol table but is built without frame pointers, so use `--callgraph dwarf,16384` for usable call chains; the report warns when chains are short. Inlined frames and source lines need a build with `-g`.
- `--mode heap`: the tcmalloc heap profiler preloaded (`libtcmalloc_and_profiler.so`), one dump per checkpoint (`SIGUSR1`), `heap-report.md` with the top 15 allocation sites by live bytes at each checkpoint and by total bytes allocated, a site being the innermost frame in the binary. Addresses are resolved with `addr2line` and `nm -D`; there is no `pprof` here.

## Fixtures

`perf_fixtures.py` writes `small`, `flat-20k`, `tree-100k` (about 100,000 files in 10,000 folders, depth 1 to 9, sparse files with log-normal apparent sizes), `pictures-300` (4000x3000 PNGs), `pictures-rgba-300` (the same with an alpha channel of 255), `kinds` (the seed kinds plus a folder, a 20 MB log, a 100,000 row CSV and a 1080p clip), `text-big` and `video` (a 30 s 1080p H.264 clip) under `.cache/perf-fixtures`, with a marker file in each and a `manifest.json`. A second `build` does nothing; `clean` removes marked folders only.

### Leaving and returning to the pictures; late thumbnails

`pictures-leave`, `pictures-return` and `pictures-scrollback` use a kept profile and a priming run, so the thumbnails are in the on-disk cache. Three more actions make them possible: `("mark", NAME)` takes the time, `("until", MARK, SECONDS)` waits until that long after the mark (the checkpoint then samples as usual), and `("burst", NAME, {"seconds": S, "mark": M})` takes screenshots one after another for S seconds as PPM (no compression, so grim returns about every 17 ms here), then one settled screenshot once the app is quiet.

- `pictures-leave` scrolls to the end, leaves for `small` and samples memory at 0, 35, 70, 130 and 360 s after the Return that leaves (checkpoints `left-0s` to `left-360s`; the first does not wait for a quiet CPU).
- `pictures-return` leaves the same way, returns after 70 s and again after 130 s (the clock restarts at the second leave): bursts `return-70s-top` and `return-130s-top` start at the Return that opens the folder (shown from its top); bursts `return-70s-end` and `return-130s-end` start at the End key that goes back to where it was left.
- `pictures-scrollback` scrolls to the end, waits 70 s (then 130 s), and goes back to the top with Home: bursts `scrollback-70s` and `scrollback-130s`.

`perf_tiles.py` reads a burst after the run: the tiles are the pictures found in the settled screenshot (runs of pixels brighter than the background, inside the grid area; the file names are too thin to count), and a frame's tile is unsettled when its pixels differ from the settled one by more than 6 per channel on average. Per burst the run file has `late`: `tiles`, `unsettled` per frame, `empty` per frame (tiles showing no picture at all, the generic icon or nothing, which tells a late thumbnail from the view before the action, as that one shows other pictures) with `last_empty_s`, `times_s` (middle of each grim call, from the mark), `first_all_settled_s`, `last_unsettled_s` and `interval_s`; the compare table has them as `late.*` metrics (`late.never_settled` is 1 when no frame matched). Frame 0 can still show the view before the key took effect, so a time of about 0.03 s is the input latency, not a late thumbnail. The PPM files are removed after they are read.
