"""Real runs: the private compositor and a copy of the debug build of Luciole, with no fakes."""

import os
import pwd
import shutil
import signal
import subprocess
import unittest

from e2e_support import (
    RUN_FOLDER,
    children_of,
    command,
    make_work_folder,
    private_mounts,
    private_named,
    private_processes,
    real_run,
    result_of,
    stop,
    wait_for_app,
    wait_until_gone,
)
from images import png_colors, png_pixel, png_size

# The default theme's fill of a selected row in List view.
SELECTED = (88, 85, 82)
# Window positions of the note-1.txt and note-2.txt rows in the fixture at 1400x900.
NOTE_1 = (450, 277)
NOTE_2 = (450, 307)
HELPER_NAME = "window-check-in"


@real_run
class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work, cls.binary, cls.folder = make_work_folder()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.work)

    def test_screenshot_click_and_key_in_a_real_window(self):
        output = self.work / "basic"
        run = subprocess.run(command(self.binary, self.folder, output, "settle", "shot:first",
                                     f"move:{NOTE_1[0]},{NOTE_1[1]}", "click:left", "settle", "shot:clicked",
                                     "key:Down", "settle", "shot:after-key"),
                             capture_output=True, text=True, timeout=180)
        result = result_of(output)
        self.assertEqual(run.returncode, 0, f"{result.get('error')}\n{run.stderr}")
        self.assertTrue(result["ok"])
        self.assertTrue(all(step["ok"] for step in result["steps"]))
        for name in ("first", "clicked", "after-key"):
            self.assertEqual(png_size(output / f"{name}.png"), (1400, 900))
            self.assertEqual(png_colors(output / f"{name}.png", limit=2), 2, f"{name}.png is one flat color")
        self.assertNotEqual(png_pixel(output / "first.png", *NOTE_1), SELECTED)
        self.assertEqual(png_pixel(output / "clicked.png", *NOTE_1), SELECTED, "the click selected nothing")
        self.assertEqual(png_pixel(output / "after-key.png", *NOTE_2), SELECTED, "Down did not move the selection")
        self.assertNotEqual(png_pixel(output / "after-key.png", *NOTE_1), SELECTED)

        self.assertEqual(result["output"]["mode"], [1400, 900])
        self.assertEqual(result["output"]["scale"], 1.0)
        self.assertEqual(result["output"]["logical_size"], {"width": 1400, "height": 900})
        window = {key: result["window"][key] for key in ("x", "y", "w", "h", "app_id")}
        self.assertEqual(window, {"x": 0, "y": 0, "w": 1400, "h": 900, "app_id": "dev.noctalia.luciole"})
        self.assertEqual(result["app"]["runtime_seen"], str(RUN_FOLDER / "runtime"))
        self.assertIn(pwd.getpwuid(os.getuid()).pw_dir, result["app"]["hidden_folders"])

        self.assertTrue(result["leftover_check"]["ok"], result["leftover_check"])
        self.assertTrue(result["supervisor"]["ok"], result["supervisor"])
        self.assertEqual(private_processes(), [])
        self.assertEqual(private_mounts(), [])
        self.assertTrue((output / "app.log").is_file() and (output / "compositor.log").is_file())

    def test_a_dead_input_helper_fails_the_next_step_with_its_number(self):
        output = self.work / "dead-helper"
        tool = subprocess.Popen(command(self.binary, self.folder, output, "wait:20000", "key:Down"),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            wait_for_app()
            helpers = private_named(HELPER_NAME)
            self.assertEqual(len(helpers), 1, "no input helper found")
            os.kill(helpers[0], signal.SIGKILL)
            _, errors = tool.communicate(timeout=60)
        finally:
            stop(tool)
        result = result_of(output)
        self.assertEqual(tool.returncode, 1)
        self.assertFalse(result["ok"])
        self.assertTrue(result["error"].startswith("step 2 'key:Down' failed"), result["error"])
        self.assertIn("input helper", result["error"])
        self.assertEqual([step.get("ok") for step in result["steps"]], [True, False])
        self.assertNotIn("Traceback", errors)
        self.assertEqual(wait_until_gone(private_processes), [])

    def test_a_killed_worker_leaves_a_failed_result(self):
        output = self.work / "killed-worker"
        tool = subprocess.Popen(command(self.binary, self.folder, output, "wait:60000"),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            wait_for_app()
            workers = children_of(tool.pid)
            self.assertEqual(len(workers), 1, "the tool has no single worker")
            os.kill(workers[0], signal.SIGKILL)
            _, errors = tool.communicate(timeout=60)
        finally:
            stop(tool)
        result = result_of(output)
        self.assertEqual(tool.returncode, 1)
        self.assertFalse(result["ok"])
        self.assertIn("killed by signal 9", result["error"])
        self.assertEqual(result["supervisor"]["exit_status"], -signal.SIGKILL)
        self.assertIn("killed by signal 9", errors)
        self.assertEqual(wait_until_gone(private_processes), [])
        self.assertEqual(private_mounts(), [])


if __name__ == "__main__":
    unittest.main()
