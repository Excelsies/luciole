"""Isolation rules: child environments, the folder guard, the runtime folder guard and socket path length."""

import os
import pwd
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOL))

from isolation import (  # noqa: E402
    IsolationError,
    Profile,
    check_child_environment,
    check_runtime,
    client_environment,
    compositor_environment,
    guard_binary,
    guard_folder,
    guard_output,
    plain_environment,
    socket_path_fits,
)

SESSION = {
    "PATH": "/usr/bin:/bin",
    "LANG": "en_US.UTF-8",
    "HOME": "/home/owner",
    "WAYLAND_DISPLAY": "wayland-1",
    "DISPLAY": ":0",
    "HYPRLAND_INSTANCE_SIGNATURE": "abc_123",
    "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
    "XDG_CURRENT_DESKTOP": "Hyprland",
    "XDG_RUNTIME_DIR": "/run/user/1000",
    "XDG_CONFIG_HOME": "/home/owner/.config",
    "SSH_AUTH_SOCK": "/run/user/1000/ssh",
}
LEAKS = ("WAYLAND_DISPLAY", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "DBUS_SESSION_BUS_ADDRESS",
         "XDG_CURRENT_DESKTOP")


class ChildEnvironment(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="wc-env-"))
        self.profile = Profile(self.root)

    def tearDown(self):
        for path in sorted(self.root.rglob("*"), reverse=True):
            path.rmdir()
        self.root.rmdir()

    def test_compositor_gets_none_of_the_session_variables(self):
        env = compositor_environment(SESSION, self.profile, "run-1")
        for name in LEAKS:
            self.assertNotIn(name, env)
        self.assertNotIn("SSH_AUTH_SOCK", env)
        self.assertEqual(env["WLR_BACKENDS"], "headless")

    def test_client_sees_only_the_private_display_and_bus(self):
        env = client_environment(SESSION, self.profile, "run-1")
        for name in ("DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE", "XDG_CURRENT_DESKTOP"):
            self.assertNotIn(name, env)
        self.assertEqual(env["WAYLAND_DISPLAY"], "wayland-0")
        self.assertEqual(env["XDG_RUNTIME_DIR"], str(self.root / "runtime"))
        self.assertEqual(env["DBUS_SESSION_BUS_ADDRESS"], f"unix:path={self.root / 'runtime' / 'bus'}")
        for value in env.values():
            self.assertNotIn("/run/user/1000", value)
            self.assertNotIn("/home/owner", value)

    def test_the_private_bus_starts_no_fuse_mounts(self):
        self.assertEqual(client_environment(SESSION, self.profile, "run-1")["GVFS_DISABLE_FUSE"], "1")

    def test_home_and_every_xdg_folder_are_private(self):
        env = client_environment(SESSION, self.profile, "run-1")
        for name in ("HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME",
                     "XDG_RUNTIME_DIR"):
            self.assertTrue(Path(env[name]).is_relative_to(self.root), name)

    def test_allowed_session_values_pass_through(self):
        env = client_environment(SESSION, self.profile, "run-1")
        self.assertEqual((env["PATH"], env["LANG"]), ("/usr/bin:/bin", "en_US.UTF-8"))
        self.assertEqual(env["WINDOW_CHECK_RUN"], "run-1")

    def test_tool_helpers_get_only_the_allow_list(self):
        self.assertEqual(plain_environment(SESSION), {"PATH": "/usr/bin:/bin", "LANG": "en_US.UTF-8"})

    def test_check_refuses_a_leaked_session_value(self):
        env = client_environment(SESSION, self.profile, "run-1")
        check_child_environment(env, SESSION, self.profile)
        for name in LEAKS:
            with self.assertRaises(IsolationError):
                check_child_environment(env | {name: SESSION[name]}, SESSION, self.profile)
        with self.assertRaises(IsolationError):
            check_child_environment(env | {"HOME": "/home/owner"}, SESSION, self.profile)

    def test_any_session_value_equal_to_the_callers_is_refused(self):
        caller = SESSION | {"SWAYSOCK": "/run/user/1000/sway-ipc.sock"}
        env = client_environment(caller, self.profile, "run-1")
        with self.assertRaises(IsolationError):
            check_child_environment(env | {"SWAYSOCK": caller["SWAYSOCK"]}, caller, self.profile)

    def test_a_caller_whose_display_is_the_childs_socket_is_refused(self):
        caller = SESSION | {"WAYLAND_DISPLAY": str(self.profile.runtime / "wayland-0")}
        with self.assertRaises(IsolationError):
            check_child_environment(client_environment(caller, self.profile, "run-1"), caller, self.profile)

    def test_a_caller_on_wayland_0_can_still_start_children(self):
        caller = SESSION | {"WAYLAND_DISPLAY": "wayland-0"}
        check_child_environment(client_environment(caller, self.profile, "run-1"), caller, self.profile)

    def test_the_callers_display_socket_is_refused_by_path(self):
        env = client_environment(SESSION, self.profile, "run-1")
        with self.assertRaises(IsolationError):
            check_child_environment(env | {"WAYLAND_DISPLAY": "/run/user/1000/wayland-1"}, SESSION, self.profile)


class FolderGuard(unittest.TestCase):
    def test_home_and_its_parents_are_refused(self):
        home = Path.home()
        for folder in (home, home.parent, Path("/")):
            with self.assertRaises(IsolationError):
                guard_folder(folder, {})

    def test_home_through_a_relative_spelling_is_refused(self):
        with self.assertRaises(IsolationError):
            guard_folder(Path.home() / "missing-child" / "..", {})

    def test_the_session_runtime_and_the_parents_of_hidden_folders_are_refused(self):
        for folder in (Path(f"/run/user/{os.getuid()}"), Path("/run/user"), Path("/run"), Path("/tmp"),
                       Path("/mnt")):
            if folder.is_dir():
                with self.assertRaises(IsolationError, msg=str(folder)):
                    guard_folder(folder, {})

    def test_a_folder_inside_the_callers_runtime_folder_is_refused(self):
        with tempfile.TemporaryDirectory() as runtime:
            inside = Path(runtime) / "sockets"
            inside.mkdir()
            with self.assertRaises(IsolationError):
                guard_folder(inside, {"XDG_RUNTIME_DIR": runtime})
            self.assertEqual(guard_folder(inside, {}), inside.resolve())

    def test_strict_children_of_hidden_folders_are_accepted(self):
        with tempfile.TemporaryDirectory(dir="/tmp") as folder:
            self.assertEqual(guard_folder(Path(folder), {}), Path(folder).resolve())
        self.assertEqual(guard_folder(TOOL, {}), TOOL)

    def test_the_account_home_is_refused_when_home_points_elsewhere(self):
        account_home = pwd.getpwuid(os.getuid()).pw_dir
        code = ("import sys; from pathlib import Path; from isolation import guard_folder, IsolationError\n"
                "try:\n    guard_folder(Path(sys.argv[1]), {})\nexcept IsolationError:\n    sys.exit(3)\n")
        with tempfile.TemporaryDirectory() as elsewhere:
            env = {"PATH": "/usr/bin:/bin", "HOME": elsewhere, "PYTHONPATH": str(TOOL)}
            for folder in (account_home, str(Path(account_home).parent)):
                result = subprocess.run([sys.executable, "-c", code, folder], env=env, capture_output=True,
                                        text=True, timeout=30)
                self.assertEqual(result.returncode, 3, f"accepted {folder}: {result.stderr}")

    def test_a_fixture_folder_is_accepted(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(guard_folder(Path(folder), {}), Path(folder).resolve())

    def test_a_missing_folder_is_refused(self):
        with self.assertRaises(IsolationError):
            guard_folder(Path(tempfile.gettempdir()) / "window-check-missing-folder", {})


class BinaryAndOutputGuards(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="wc-guard-"))
        self.repo = self.root / "repo"
        (self.repo / "build-debug").mkdir(parents=True)
        self.folder = self.root / "fixture"
        self.folder.mkdir()

    def tearDown(self):
        for path in sorted(self.root.rglob("*"), reverse=True):
            path.unlink() if path.is_file() else path.rmdir()
        self.root.rmdir()

    def executable(self, path: Path) -> Path:
        path.write_text("")
        path.chmod(0o755)
        return path

    def test_a_shared_build_is_refused(self):
        binary = self.executable(self.repo / "build-debug" / "luciole")
        with self.assertRaises(IsolationError) as caught:
            guard_binary(binary, self.repo)
        self.assertIn("build.lock", str(caught.exception))

    def test_a_copy_is_accepted(self):
        binary = self.executable(self.root / "luciole")
        self.assertEqual(guard_binary(binary, self.repo), binary.resolve())

    def test_a_missing_binary_is_refused(self):
        with self.assertRaises(IsolationError):
            guard_binary(self.root / "missing", self.repo)

    def test_an_output_inside_the_folder_is_refused(self):
        for output in (self.folder, self.folder / "out"):
            with self.assertRaises(IsolationError, msg=str(output)):
                guard_output(output, self.folder, self.root / "run", {})

    def test_an_output_inside_the_run_folder_is_refused(self):
        with self.assertRaises(IsolationError):
            guard_output(self.root / "run" / "out", self.folder, self.root / "run", {})

    def test_an_output_holding_files_is_refused(self):
        output = self.root / "out"
        output.mkdir()
        (output / "result.json").write_text("{}")
        with self.assertRaises(IsolationError) as caught:
            guard_output(output, self.folder, self.root / "run", {})
        self.assertIn("not empty", str(caught.exception))

    def test_a_new_output_is_created(self):
        output = guard_output(self.root / "new" / "out", self.folder, self.root / "run", {})
        self.assertTrue(output.is_dir())


class RuntimeGuard(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="wc-rt-"))
        self.runtime = self.root / "runtime"
        self.runtime.mkdir(mode=0o700)

    def tearDown(self):
        self.runtime.chmod(0o700)
        self.runtime.rmdir()
        self.root.rmdir()

    def test_private_runtime_is_accepted(self):
        check_runtime(self.runtime, SESSION)

    def test_open_permissions_are_refused(self):
        self.runtime.chmod(0o755)
        with self.assertRaises(IsolationError) as caught:
            check_runtime(self.runtime, SESSION)
        self.assertIn("0700", str(caught.exception))

    def test_the_callers_runtime_is_refused(self):
        with self.assertRaises(IsolationError):
            check_runtime(self.runtime, SESSION | {"XDG_RUNTIME_DIR": str(self.runtime)})

    def test_the_users_system_runtime_is_refused_even_when_the_caller_names_none(self):
        system_runtime = Path(f"/run/user/{os.getuid()}")
        if not system_runtime.is_dir():
            self.skipTest(f"{system_runtime} does not exist here")
        with self.assertRaises(IsolationError):
            check_runtime(system_runtime, {})


class SocketPathLength(unittest.TestCase):
    def test_limit_is_107_bytes_plus_the_terminator(self):
        name = "umbriel-wayland-0.sock"
        fits = Path("/" + "a" * (107 - len(name) - 2))
        self.assertEqual(len(str(fits / name)), 107)
        self.assertTrue(socket_path_fits(fits))
        self.assertFalse(socket_path_fits(Path(str(fits) + "b")))

    def test_check_runtime_refuses_a_long_path(self):
        with tempfile.TemporaryDirectory() as folder:
            deep = Path(folder) / ("d" * 100)
            deep.mkdir(mode=0o700)
            with self.assertRaises(IsolationError) as caught:
                check_runtime(deep, SESSION)
            self.assertIn("108", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
