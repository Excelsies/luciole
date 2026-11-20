"""The private compositor: its configuration, the output size option and the window ids it accepts."""

import sys
import tempfile
import tomllib
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from compositor import OUTPUT_NAME, CompositorError, PrivateCompositor, config_text, parse_size  # noqa: E402
from isolation import Profile  # noqa: E402


class Config(unittest.TestCase):
    def setUp(self):
        self.config = tomllib.loads(config_text(1400, 900, 1.5))

    def test_output_mode_and_scale(self):
        self.assertEqual(self.config["output"][OUTPUT_NAME], {"mode": "1400x900", "scale": 1.5})

    def test_nothing_else_starts_or_draws(self):
        general = self.config["general"]
        self.assertEqual((general["xwayland"], general["show_cheatsheet"], general["autostart"]), (False, False, []))

    def test_every_window_covers_the_output_without_decoration(self):
        rule = self.config["window_rule"][0]
        self.assertEqual(rule["match"]["app_id"], ".*")
        self.assertTrue(rule["default_maximize_to_edges"])
        self.assertEqual(rule["title_bar"], "off")


class WindowIds(unittest.TestCase):
    def compositor(self, client: str) -> PrivateCompositor:
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        return PrivateCompositor(Path(client), Profile(Path(folder.name)), {"PATH": "/usr/bin:/bin"}, "run-1")

    def test_an_id_the_client_would_read_as_an_option_or_several_words_is_refused(self):
        # A client that fails would raise a different error, so the refusal comes before it runs.
        compositor = self.compositor("/usr/bin/false")
        for window_id in ("-c", "--help", "a b", "", "x" * 129):
            with self.assertRaises(CompositorError, msg=window_id) as caught:
                compositor.close_window(window_id)
            self.assertIn("which is not an id", str(caught.exception))

    def test_an_id_as_the_compositor_reports_it_reaches_the_client(self):
        self.compositor("/usr/bin/true").close_window("db253ea1ff0da9131d08b4f4fd93b970")
        with self.assertRaises(CompositorError) as caught:
            self.compositor("/usr/bin/false").close_window("db253ea1ff0da9131d08b4f4fd93b970")
        self.assertIn("window-close db253ea1ff0da9131d08b4f4fd93b970 failed", str(caught.exception))


class Size(unittest.TestCase):
    def test_width_by_height(self):
        self.assertEqual(parse_size("1400x900"), (1400, 900))

    def test_refused_sizes(self):
        for text in ("1400", "0x900", "1400x", "99999x900", "-5x10", "a x b"):
            with self.assertRaises(ValueError, msg=text):
                parse_size(text)


if __name__ == "__main__":
    unittest.main()
