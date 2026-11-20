"""No sound leaves the sandbox: the session's PipeWire and PulseAudio sockets and the sound devices are out of reach."""

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL))

from isolation import Profile, client_environment, hidden_folders  # noqa: E402
from sandbox import sandbox_command  # noqa: E402

PLAIN = {"PATH": "/usr/bin:/bin"}
# Connects to each unix socket path given and prints {path: "connected" or the error's name}.
PROBE = """\
import json, socket, sys
seen = {}
for path in sys.argv[1:]:
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(2)
    try:
        client.connect(path)
        seen[path] = "connected"
    except OSError as error:
        seen[path] = type(error).__name__
    finally:
        client.close()
print(json.dumps(seen))
"""


def session_sound_sockets() -> list[str]:
    """The sound server sockets of this session and of a system-wide server, where they exist."""
    runtimes = {Path(f"/run/user/{os.getuid()}")}
    if os.environ.get("XDG_RUNTIME_DIR"):
        runtimes.add(Path(os.environ["XDG_RUNTIME_DIR"]))
    candidates = [Path("/run/pulse/native"), Path("/var/run/pulse/native")]
    for runtime in runtimes:
        candidates += [runtime / "pipewire-0", runtime / "pipewire-0-manager", runtime / "pulse" / "native"]
    return sorted({str(path) for path in candidates if path.is_socket()})


def probe(paths: list[str], sandboxed: bool) -> dict[str, str]:
    command = [sys.executable, "-I", "-c", PROBE, *paths]
    if sandboxed:
        command = sandbox_command(command, [], [], hidden_folders(PLAIN))
    result = subprocess.run(command, env=PLAIN, capture_output=True, text=True, timeout=30)
    if result.returncode != 0:
        raise AssertionError(f"the probe failed: {result.stderr.strip()[-300:]}")
    return json.loads(result.stdout)


class SoundOutOfReach(unittest.TestCase):
    def test_no_sound_server_socket_of_the_session_is_reachable_inside(self):
        sockets = session_sound_sockets()
        if not sockets:
            self.skipTest("this session runs no PipeWire or PulseAudio server")
        outside = probe(sockets, sandboxed=False)
        # The same probe reaches them from outside, so a refusal inside comes from the sandbox.
        self.assertEqual(set(outside.values()), {"connected"}, outside)
        inside = probe(sockets, sandboxed=True)
        self.assertEqual(sorted(inside), sockets)
        self.assertNotIn("connected", inside.values(), inside)

    def test_the_sound_devices_are_empty_inside(self):
        devices = Path("/dev/snd")
        if not (devices.is_dir() and any(devices.iterdir())):
            self.skipTest("this machine has no sound devices")
        command = sandbox_command(["ls", "-A", "/dev/snd"], [], [], hidden_folders(PLAIN))
        result = subprocess.run(command, env=PLAIN, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.split(), [])

    def test_the_shared_memory_folder_where_jack_keeps_its_sockets_is_private_inside(self):
        planted = Path("/dev/shm") / f"window-check-probe-{os.getpid()}"
        self.addCleanup(lambda: planted.unlink(missing_ok=True))
        planted.write_text("")
        written = f"/dev/shm/window-check-inside-{os.getpid()}"
        self.addCleanup(lambda: Path(written).unlink(missing_ok=True))
        command = sandbox_command(["sh", "-c", f"ls -A /dev/shm && : > {written}"], [], [], hidden_folders(PLAIN))
        result = subprocess.run(command, env=PLAIN, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        # The sandbox sees none of the host's entries, and what it writes there stays inside.
        self.assertEqual(result.stdout.split(), [])
        self.assertFalse(Path(written).exists())

    def test_no_sound_server_address_of_the_caller_reaches_the_app(self):
        caller = PLAIN | {"PULSE_SERVER": "unix:/run/user/1000/pulse/native", "PIPEWIRE_REMOTE": "pipewire-0",
                          "PULSE_RUNTIME_PATH": "/run/user/1000/pulse", "PIPEWIRE_RUNTIME_DIR": "/run/user/1000",
                          "ALSA_CONFIG_PATH": "/home/owner/.asoundrc"}
        with tempfile.TemporaryDirectory() as folder:
            env = client_environment(caller, Profile(Path(folder)), "run-1")
        for name in caller.keys() - PLAIN.keys():
            self.assertNotIn(name, env)


if __name__ == "__main__":
    unittest.main()
