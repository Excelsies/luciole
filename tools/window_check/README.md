# window_check

`window_check` opens Luciole in a real window inside a private headless [Umbriel](https://github.com/noctalia-dev/umbriel) compositor, drives it with pointer and keyboard steps, and saves screenshots, logs and a result file. Nothing appears on the desktop, and nothing it starts talks to the desktop session.

It needs Python 3.12 or newer (standard library only), the build tools and libraries Umbriel needs (meson, ninja, wlroots 0.20, libeis and the others its `meson.build` lists), `grim`, `dbus-daemon`, `bwrap`, `taskset`, a C++23 compiler, `wayland-scanner` and `pkg-config`.

## Commands

```sh
python tools/window_check/window_check.py build-compositor
python tools/window_check/window_check.py run --binary BINARY --folder FOLDER --output OUTPUT [options] STEP...
python tools/window_check/window_check.py run --help
```

`build-compositor` clones Umbriel at the commit pinned in `build.py` into `.cache/harness/src/umbriel`, builds a release build in `.cache/harness/build/umbriel` on cores 6 to 11 with six jobs, and installs it into `.cache/harness/prefix`. It also compiles the input helper, `input_helper.cpp`, into `.cache/harness/bin`. Meson never downloads subprojects. Build commands get the allow-listed environment below, a private `HOME` under `.cache/harness/build/home` and no system or user git configuration. When the pinned commit is already installed, the command says so and does nothing; an interrupted build can be run again. The build log is `.cache/harness/build/umbriel-build.log`.

`run` starts the compositor, launches the binary on the folder, runs the steps in order and stops everything. Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `--binary` | required | A copy of the Luciole binary, for example `flock .tmp/native/build.lock cp build-debug/luciole DEST/`; a binary inside a `build*` folder of the checkout is refused. The binary needs nothing beside it. |
| `--folder` | required | The folder to open. A hidden folder (see Isolation), a parent of one, and anything inside the session's runtime folder are refused. |
| `--output` | required | Where `result.json`, `app.log`, `compositor.log` and the screenshots go. It must be new or empty, and outside the folder to open and the run folder. |
| `--size` | `1400x900` | The output mode in pixels. |
| `--scale` | `1` | The output scale: `1`, `1.5` or `2`. |
| `--settings` | none | A file that becomes the app's private `$XDG_CONFIG_HOME/luciole/config.toml`. |
| `--time-limit` | `120` | Seconds for the whole run. |
| `--memory-limit` | `3072` | MiB of resident memory for the app and its child processes. |
| `--env NAME=VALUE` | none | A variable for the app alone, for example `GST_DEBUG=2`. Repeatable. Only names that start with `GST_` or `LUCIOLE_`, and `G_MESSAGES_DEBUG`, are accepted; a name the tool sets itself, such as `LUCIOLE_ASSETS_DIR`, is refused. |
| `--profile` | none | A folder that keeps the app's `HOME`, configuration, data, state and cache between runs, so a choice saved in one run is there after a restart. See Kept profiles. |

The exit code is 0 only when every step succeeded, 1 when a run failed, and 2 for invalid steps or options, a refused folder, binary, output folder, profile or variable included. Every failure prints `error: ...` on stderr, a failed step with its number.

## Steps

Each step is one command-line word. Pointer positions are logical pixels from the app window's top-left corner.

| Step | Effect |
| --- | --- |
| `shot:NAME` | Screenshot of the output to `NAME.png`. A failed screenshot fails the run. |
| `move:X,Y` | Move the pointer. |
| `down:B`, `up:B`, `click:B` | Press, release, or press and release button `left`, `middle` or `right`. |
| `scroll:N`, `hscroll:N` | `N` wheel notches down or right; negative goes up or left. |
| `key:COMBO` | Press and release, for example `ctrl+4`, `shift+Tab`, `Escape`, `Return`, `Left`. |
| `keydown:COMBO`, `keyup:COMBO` | Press and hold, then release. `keydown:shift` holds a modifier alone. |
| `type:TEXT` | Type text with the US layout. Quote it in the shell. |
| `wait:MS` | Wait. |
| `settle[:TRIES]` | Wait until two screenshots in a row are identical, at most `TRIES` comparisons (default 20). |
| `close-window[:MS]` | Ask the app's window to close, as a user's close key binding does, then wait up to `MS` milliseconds (default 3000, at most 60000) for the app to exit. |

Modifiers are `ctrl`, `shift`, `alt` and `super`. A key is one character or an XKB key name.

Example, in List view:

```sh
printf 'schema_version = 1\n\n[browser]\nview_mode = "list"\n' > list.toml
python tools/window_check/window_check.py run --binary .tmp/luciole --folder .tmp/fixture \
    --settings list.toml --output .tmp/check-1 settle shot:start move:300,200 click:left settle shot:clicked
```

## How it works

The compositor is configured to open every window maximized to the output's edges with no title bar, border or shadow, so the window rectangle covers the output. The tool still reads the rectangle from `umbriel windows` before each pointer step and translates window coordinates to the output layout.

Input goes through the `zwlr_virtual_pointer_v1` and `zwp_virtual_keyboard_v1` protocols, which Umbriel offers to every client. The helper connects before the app, because a headless seat has no keyboard until a virtual one arrives.

`close-window` runs `umbriel msg window-close ID` with the window's id from `umbriel windows`. That is the action Umbriel's close key binding runs, so the app receives the same `xdg_toplevel.close` request. The run goes on after the step: a later screenshot shows what the app shows then, a question about unsaved changes for example, and a pointer step fails once the window is gone. The compositor animates a closing window, so `settle` before a screenshot. Once the close request is sent, an exit with code 0 is expected; any other exit code fails the run.

## Kept profiles

Without `--profile`, every run starts with empty private folders. With `--profile DIR`, the app's `HOME` is `DIR/home` and its XDG configuration, data, state and cache folders are `DIR/config`, `DIR/data`, `DIR/state` and `DIR/cache`, kept from one run to the next, while the runtime folder stays new for each run. Only the app and its private bus use these folders: the compositor, its command line client, `grim`, the input helper and the memory watcher run outside the sandbox and keep the run's own folders under `.cache/harness/run`, so a link the app leaves in `DIR` is never followed outside the sandbox. Inside the sandbox, `DIR` is bound writable at its own path, like the run folder.

`DIR` must lie inside the checkout's `.tmp` or `.cache` folder, apart from the folder to open, the output folder, the run folder and the binary. It must be new, empty, or a folder an earlier run left: the tool marks a profile with a `.window-check-profile` file and refuses a folder that holds files without one. A link or a file where one of the profile's folders belongs is refused, and `--settings` replaces `DIR/config/luciole/config.toml` without following a link. To check that a choice survives a restart, run once with `--settings` and the steps that change it, then again with the same `--profile` and no `--settings`; Luciole writes choices made in the window to `settings.toml` beside `config.toml`.

## Isolation

- Child environments are built from an allow list (`PATH`, `LANG`, `LC_ALL`, `TZ`) plus private values; nothing else is copied from the caller. Every environment is checked before a child starts. A child's `WAYLAND_DISPLAY` is compared with the caller's as a socket path, so a desktop that also names its socket `wayland-0` is no obstacle.
- `HOME` and every XDG folder are private folders under `.cache/harness/run`, recreated for each run; with `--profile`, the app's are kept in `DIR` instead (see Kept profiles). The runtime folder has mode 0700, differs from the caller's, and keeps the socket path under the 108-byte limit.
- The app gets the private Wayland display and a private D-Bus session bus. The system bus address points nowhere.
- `GVFS_DISABLE_FUSE=1` keeps services started through the private bus from mounting FUSE in the runtime folder. A mount found under the private folders at the start or end of a run is removed and reported.
- The app and the private session bus run inside `bwrap`: a read-only view of `/` in which the hidden folders are empty tmpfs mounts. The hidden folders are the home folder, the session's runtime folders (`/run/user/UID` and the caller's `XDG_RUNTIME_DIR`), `/tmp`, `/mnt`, `/media`, `/run/media`, the sound devices in `/dev/snd` and the shared memory folder `/dev/shm`. Only the folder to open, the run folder and, with `--profile`, `DIR` are bound back writable, and the binary and `lib/assets` read-only. A bind that equals or contains a hidden folder is refused, because it would put the real folder back on top of its tmpfs. The sandbox has no network and its own process namespace, so the app sees no process outside it, and stopping the sandbox stops everything started inside it. It dies with the tool. Services the private bus starts inherit the same view. So the app cannot list the owner's home folder, even when it walks up from the folder it opened.
- The app starts only after the private Wayland socket exists. The tool then reads `/proc/PID/environ` to confirm the app has the private runtime folder and display, and `/proc/PID/mountinfo` to confirm that the topmost mount on every hidden folder is a tmpfs of its own at that folder.
- Variables given with `--env` reach the app alone, never the bus, the compositor or a helper. They are checked like every other child environment, and the tool confirms in `/proc/PID/environ` that the app received each one.
- Nothing played inside the sandbox reaches a speaker. The session's PipeWire and PulseAudio sockets lie in its runtime folder, which is hidden; a JACK server's sockets lie in `/dev/shm`, which is a private tmpfs inside; the sound devices in `/dev/snd` are hidden; the sandbox has no network; and no sound server address passes the environment allow list. With no sound device, every audio sink GStreamer tries fails, and the app's media preview plays a video's picture without sound and refuses an audio file with "This file cannot be played: no sound device was found." `app.log` carries only the sound libraries' own complaints, such as `Failed to connect PipeWire event context`. `--env GST_DEBUG=2,autodetect:5` shows each sink it tried and why it failed.
- The folder to open is refused when it is a hidden folder or one of their parents (`/`, `/run` or `/home`, for example), or when it lies inside a session runtime folder. Folders inside the home folder, `/tmp` or `/mnt` are accepted. The home folder is both `$HOME` and the account's home folder, so changing `$HOME` does not lift the guard.

## Lifetime

- The tool forks a worker and both become child subreapers, so orphaned descendants, including services the private bus starts, stay in the tree. Every child gets a parent-death signal, and the worker gets one from the tool, so killing the tool with any signal, SIGKILL included, stops everything.
- The worker has a process group of its own, so a terminal's Ctrl-C reaches the tool alone, which passes it on once. The worker acts on the first SIGINT or SIGTERM and ignores later ones while it stops.
- Every exit path stops all processes: the end of the steps, a failed step, an error, the time limit, SIGINT and SIGTERM. Each stopping stage runs even when an earlier one fails. The run then checks that no descendant and no process carrying its environment marker is alive, and reports this as `leftover_check`.
- A watchdog process checks the resident memory of the app and its children every 100 ms, whatever the run is doing, and kills them when they pass the limit. All waits and polls are bounded.
- A lock file, `.cache/harness/run.lock`, keeps two runs from overlapping.

## Results

`OUTPUT/result.json` records the compositor commit, the output (mode, scale, logical size), the window rectangle and app id, every step with its start time, duration, outcome and details (pointer layout position, screenshot pixel size, the closed window's id), the variables `--env` added and the kept profile, the app's environment and mount checks, the leftover check and the overall error. `app_peak_rss_mib` is the highest resident memory of the app and its child processes that the watchdog saw. `app_exit` says whether the app exited by itself before the run stopped it, whether a close request came first, and its exit code. A step that fails or is interrupted carries `ok: false` and its error. The tool itself completes the file after the worker ends, under `supervisor`: when the worker was killed or wrote nothing, `result.json` still exists and says `ok: false`. `OUTPUT/app.log` holds the app's stdout and stderr, and `OUTPUT/compositor.log` the compositor's.

## Tests

```sh
python -m unittest discover -s tools/window_check/tests
```

The end-to-end tests run the real compositor and a copy of `build-debug/luciole`, one run at a time, and skip with a message when either is missing or when `.tmp/native/build.lock` or `.cache/harness/run.lock` stays held for five minutes.
