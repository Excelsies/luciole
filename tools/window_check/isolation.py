"""What a child of window_check may see: private folders, a private runtime and an allow-listed environment."""

import os
import pwd
import stat
import subprocess
import sys
import time
from collections.abc import Mapping
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bench"))

from gui_bench import make_profile  # noqa: E402
from processes import environment_of  # noqa: E402

# The only values copied from the caller's environment.
ALLOWED = ("PATH", "LANG", "LC_ALL", "TZ")
# Values that would connect a child to the caller's desktop session.
SESSION_NAMES = ("WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE",
                 "DBUS_SESSION_BUS_ADDRESS", "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "XDG_SESSION_TYPE",
                 "SWAYSOCK", "NIRI_SOCKET", "UMBRIEL_SOCKET", "XDG_RUNTIME_DIR", "HOME")
WAYLAND_DISPLAY = "wayland-0"
LONGEST_SOCKET = "umbriel-wayland-0.sock"
# sockaddr_un.sun_path holds 108 bytes including the terminating NUL.
SUN_PATH_MAX = 107
MARKER = "WINDOW_CHECK_RUN"
# Folders that commonly hold the owner's files or the desktop session's sockets.
SHARED_FOLDERS = (Path("/tmp"), Path("/mnt"), Path("/media"), Path("/run/media"))
# Sound card devices, and the shared memory folder where a JACK server keeps its sockets and other programs their
# buffers: with them hidden and the session's sound sockets in its runtime folder, nothing played inside the sandbox
# reaches a speaker.
DEVICE_FOLDERS = (Path("/dev/snd"), Path("/dev/shm"))
ENVIRONMENT_TIMEOUT = 2.0


class IsolationError(RuntimeError):
    pass


def real_folder(path: Path) -> Path:
    """Creates a folder unless it exists; a link or a file in its place is refused, so writes land where they seem."""
    if path.is_symlink() or (path.exists() and not path.is_dir()):
        raise IsolationError(f"refusing {path}: it is not a plain folder")
    path.mkdir(exist_ok=True)
    return path


class Profile:
    """Private HOME, XDG folders and runtime folder of one run, under one root or, when kept, beside it."""

    def __init__(self, root: Path, kept: Path | None = None):
        self.root = root
        self.kept = kept
        # A kept folder holds HOME and the XDG folders between runs; the runtime folder is always new.
        base = root if kept is None else kept
        self.runtime = root / "runtime"
        self.home = base / "home"
        self.config = base / "config"
        self.data = base / "data"
        self.state = base / "state"
        self.cache = base / "cache"

    def writable(self) -> list[Path]:
        """The folders the app and its bus write in."""
        return [self.root] if self.kept is None else [self.root, self.kept]

    def unkept(self) -> "Profile":
        """The run's own folders under the root, for the children outside the sandbox: the app can leave links in a
        kept folder, which such a child would follow."""
        return Profile(self.root)

    def create(self, caller: Mapping[str, str]) -> None:
        """Recreates the run's own folders and runtime folder, and creates the kept folders that are missing."""
        unmount_under(self.root)
        make_profile(self.root, None)
        if self.kept is not None:
            unmount_under(self.kept)
            for folder in (self.home, self.config, self.data, self.state, self.cache):
                real_folder(folder)
        self.runtime.mkdir(mode=0o700)
        self.runtime.chmod(0o700)
        check_runtime(self.runtime, caller)

    def folders(self) -> dict[str, str]:
        return {
            "HOME": str(self.home),
            "XDG_CONFIG_HOME": str(self.config),
            "XDG_DATA_HOME": str(self.data),
            "XDG_STATE_HOME": str(self.state),
            "XDG_CACHE_HOME": str(self.cache),
            "XDG_RUNTIME_DIR": str(self.runtime),
        }


def plain_environment(caller: Mapping[str, str]) -> dict[str, str]:
    """The allow-listed values alone, for helper commands that need nothing private."""
    return {name: caller[name] for name in ALLOWED if name in caller}


def _base(caller: Mapping[str, str], profile: Profile, marker: str) -> dict[str, str]:
    env = plain_environment(caller)
    env.setdefault("PATH", "/usr/bin:/bin")
    env |= profile.folders()
    env["XDG_DATA_DIRS"] = "/usr/local/share:/usr/share"
    env["XDG_CONFIG_DIRS"] = "/etc/xdg"
    # A system bus address that leads nowhere keeps children off the system bus too.
    env["DBUS_SYSTEM_BUS_ADDRESS"] = f"unix:path={profile.runtime / 'no-system-bus'}"
    env[MARKER] = marker
    return env


def compositor_environment(caller: Mapping[str, str], profile: Profile, marker: str) -> dict[str, str]:
    return _base(caller, profile, marker) | {
        "WLR_BACKENDS": "headless",
        "WLR_HEADLESS_OUTPUTS": "1",
        "WLR_LIBINPUT_NO_DEVICES": "1",
    }


def client_environment(caller: Mapping[str, str], profile: Profile, marker: str) -> dict[str, str]:
    return _base(caller, profile, marker) | {
        "WAYLAND_DISPLAY": WAYLAND_DISPLAY,
        "DBUS_SESSION_BUS_ADDRESS": f"unix:path={profile.runtime / 'bus'}",
        "UMBRIEL_SOCKET": str(profile.runtime / LONGEST_SOCKET),
        # gvfsd, which the private bus can start, otherwise mounts FUSE in the runtime folder.
        "GVFS_DISABLE_FUSE": "1",
    }


def _display_socket(env: Mapping[str, str]) -> Path | None:
    # WAYLAND_DISPLAY is relative to XDG_RUNTIME_DIR unless it is absolute.
    if "WAYLAND_DISPLAY" not in env:
        return None
    return Path(env.get("XDG_RUNTIME_DIR", "")) / env["WAYLAND_DISPLAY"]


def check_child_environment(env: Mapping[str, str], caller: Mapping[str, str], profile: Profile) -> None:
    """Refuses an environment that names the caller's session or leaves the private folders."""
    for name in SESSION_NAMES:
        if name != "WAYLAND_DISPLAY" and name in env and name in caller and env[name] == caller[name]:
            raise IsolationError(f"child environment carries the caller's {name}")
    if _display_socket(env) is not None and _display_socket(env) == _display_socket(caller):
        raise IsolationError("child environment names the caller's Wayland socket")
    for name in ("DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "XDG_CURRENT_DESKTOP", "WAYLAND_SOCKET"):
        if name in env:
            raise IsolationError(f"child environment sets {name}")
    for name, value in profile.folders().items():
        if env.get(name) != value:
            raise IsolationError(f"child {name} is {env.get(name)!r}, not the private {value}")
    if env.get("WAYLAND_DISPLAY", WAYLAND_DISPLAY) != WAYLAND_DISPLAY:
        raise IsolationError(f"child WAYLAND_DISPLAY is {env['WAYLAND_DISPLAY']!r}")
    bus = env.get("DBUS_SESSION_BUS_ADDRESS")
    if bus is not None and bus != f"unix:path={profile.runtime / 'bus'}":
        raise IsolationError(f"child session bus {bus!r} is not the private one")


def check_process_environment(pid: int, caller: Mapping[str, str], profile: Profile) -> dict[str, str]:
    """Reads a running process's real environment and refuses it unless it is the private one."""
    # Just after exec the kernel shows an empty environment until the new one is in place.
    deadline = time.monotonic() + ENVIRONMENT_TIMEOUT
    seen = environment_of(pid)
    while not seen and time.monotonic() < deadline:
        time.sleep(0.01)
        seen = environment_of(pid)
    check_child_environment(seen, caller, profile)
    if seen.get("WAYLAND_DISPLAY") != WAYLAND_DISPLAY:
        raise IsolationError(f"process {pid} has WAYLAND_DISPLAY {seen.get('WAYLAND_DISPLAY')!r}")
    return seen


def homes() -> set[Path]:
    """The home folder from $HOME and from the account database, which $HOME cannot change."""
    return {Path.home().resolve(), Path(pwd.getpwuid(os.getuid()).pw_dir).resolve()}


def session_runtimes(caller: Mapping[str, str]) -> set[Path]:
    """The desktop session's runtime folders: the account's /run/user/UID and the caller's XDG_RUNTIME_DIR."""
    runtimes = {Path(f"/run/user/{os.getuid()}").resolve()}
    if caller.get("XDG_RUNTIME_DIR"):
        runtimes.add(Path(caller["XDG_RUNTIME_DIR"]).resolve())
    return runtimes


def hidden_folders(caller: Mapping[str, str]) -> list[Path]:
    """The owner's home, the session's runtime folders and the shared folders, each as it exists here."""
    folders = homes() | session_runtimes(caller) | set(SHARED_FOLDERS) | set(DEVICE_FOLDERS)
    return sorted({folder.resolve() for folder in folders if folder.is_dir()})


def guard_folder(folder: Path, caller: Mapping[str, str]) -> Path:
    """A folder bound into the sandbox: never a hidden folder or a parent of one, never in the session runtime."""
    resolved = folder.resolve()
    for hidden in set(hidden_folders(caller)) | homes():
        if resolved == hidden or resolved in hidden.parents:
            raise IsolationError(f"refusing {resolved}: it is {hidden} or contains it, which the app must not see")
    for runtime in session_runtimes(caller):
        if resolved.is_relative_to(runtime):
            raise IsolationError(f"refusing {resolved}: it is inside the session's runtime folder {runtime}")
    if not resolved.is_dir():
        raise IsolationError(f"{resolved} is not a folder")
    return resolved


def guard_binary(binary: Path, repo: Path) -> Path:
    """The app binary: an executable copy, never a shared build folder another build may rewrite."""
    resolved = binary.resolve()
    if not (resolved.is_file() and os.access(resolved, os.X_OK)):
        raise IsolationError(f"{resolved} is not an executable file")
    if resolved.is_relative_to(repo) and resolved.relative_to(repo).parts[0].startswith("build"):
        raise IsolationError(f"refusing {resolved}: copy it first, for example "
                             f"flock .tmp/native/build.lock cp {resolved.relative_to(repo)} DEST/")
    return resolved


def guard_output(output: Path, folder: Path, run_folder: Path, caller: Mapping[str, str]) -> Path:
    """The results folder: new or empty, outside the folder being checked and outside the run folder."""
    resolved = output.resolve()
    for container in (folder.resolve(), run_folder.resolve()):
        if resolved.is_relative_to(container):
            raise IsolationError(f"refusing output {resolved}: it is inside {container}")
    resolved.mkdir(parents=True, exist_ok=True)
    if any(resolved.iterdir()):
        raise IsolationError(f"output folder {resolved} is not empty; give a new or empty folder")
    return guard_folder(resolved, caller)


def socket_path_fits(runtime: Path) -> bool:
    return len(os.fsencode(str(runtime / LONGEST_SOCKET))) <= SUN_PATH_MAX


def check_runtime(runtime: Path, caller: Mapping[str, str]) -> None:
    info = runtime.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid():
        raise IsolationError(f"{runtime} is not a folder owned by this user")
    if stat.S_IMODE(info.st_mode) != 0o700:
        raise IsolationError(f"{runtime} has mode {stat.S_IMODE(info.st_mode):04o}, not 0700")
    if runtime.resolve() in session_runtimes(caller):
        raise IsolationError(f"{runtime} is the session's runtime folder")
    if not socket_path_fits(runtime):
        raise IsolationError(f"{runtime / LONGEST_SOCKET} does not fit the 108-byte socket path limit")


def mounts_under(folder: Path) -> list[str]:
    found = []
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        point = line.split()[4].replace("\\040", " ")
        if Path(point).is_relative_to(folder):
            found.append(point)
    return found


def unmount_under(folder: Path) -> list[str]:
    """Lazily unmounts FUSE mounts a service left in the private folders; returns what it removed."""
    found = mounts_under(folder)
    for point in sorted(found, reverse=True):
        try:
            result = subprocess.run(["fusermount3", "-u", "-z", point], capture_output=True, text=True,
                                    timeout=10, env=plain_environment(os.environ))
        except (OSError, subprocess.TimeoutExpired) as error:
            raise IsolationError(f"cannot unmount {point}: {error}") from None
        if result.returncode != 0:
            raise IsolationError(f"cannot unmount {point}: {result.stderr.strip()}")
    return found
