"""Step syntax: what parses, what it parses to, and what is refused."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from steps import BUTTONS, Step, StepError, parse_step, parse_steps  # noqa: E402


class ValidSteps(unittest.TestCase):
    def test_screenshot(self):
        self.assertEqual(parse_step("shot:after-click_2"), Step("shot", "shot:after-click_2", name="after-click_2"))

    def test_move_takes_window_relative_logical_pixels(self):
        step = parse_step("move:120,48.5")
        self.assertEqual((step.kind, step.x, step.y), ("move", 120.0, 48.5))

    def test_buttons_map_to_evdev_codes(self):
        self.assertEqual(parse_step("click:left").button, 0x110)
        self.assertEqual(parse_step("down:right").button, 0x111)
        self.assertEqual(parse_step("up:middle").button, 0x112)
        self.assertEqual(BUTTONS, {"left": 0x110, "right": 0x111, "middle": 0x112})

    def test_scroll_counts_notches_on_each_axis(self):
        self.assertEqual((parse_step("scroll:3").axis, parse_step("scroll:3").notches), ("vertical", 3))
        self.assertEqual((parse_step("hscroll:-2").axis, parse_step("hscroll:-2").notches), ("horizontal", -2))

    def test_key_with_modifiers(self):
        step = parse_step("key:ctrl+shift+4")
        self.assertEqual((step.kind, step.modifiers, step.key), ("key", ("ctrl", "shift"), "4"))

    def test_named_keys_and_press_release(self):
        self.assertEqual(parse_step("key:Escape").key, "Escape")
        self.assertEqual(parse_step("keydown:Return").kind, "keydown")
        self.assertEqual(parse_step("keyup:alt+Left").modifiers, ("alt",))

    def test_type_keeps_text_verbatim(self):
        self.assertEqual(parse_step("type:hello: world").text, "hello: world")

    def test_wait_in_milliseconds(self):
        self.assertEqual(parse_step("wait:250").milliseconds, 250)

    def test_settle_default_and_explicit_tries(self):
        self.assertEqual(parse_step("settle").tries, 20)
        self.assertEqual(parse_step("settle:5").tries, 5)

    def test_close_window_waits_three_seconds_for_the_exit_unless_told(self):
        self.assertEqual(parse_step("close-window"), Step("close-window", "close-window", milliseconds=3000))
        self.assertEqual(parse_step("close-window:0").milliseconds, 0)
        self.assertEqual(parse_step("close-window:500").milliseconds, 500)

    def test_parse_steps_keeps_order(self):
        kinds = [step.kind for step in parse_steps(["shot:a", "move:1,2", "click:left", "shot:b"])]
        self.assertEqual(kinds, ["shot", "move", "click", "shot"])


class InvalidSteps(unittest.TestCase):
    def assertRefused(self, text, fragment):
        with self.assertRaises(StepError) as caught:
            parse_step(text)
        self.assertIn(fragment, str(caught.exception))

    def test_unknown_kind(self):
        self.assertRefused("jump:1", "unknown step 'jump'")

    def test_screenshot_name_must_be_a_plain_file_name(self):
        self.assertRefused("shot:../escape", "screenshot name")
        self.assertRefused("shot:", "screenshot name")

    def test_move_needs_two_numbers(self):
        self.assertRefused("move:5", "move needs X,Y")
        self.assertRefused("move:a,b", "move needs X,Y")
        self.assertRefused("move:-1,4", "not negative")

    def test_unknown_button(self):
        self.assertRefused("click:side", "button must be one of left, middle, right")

    def test_scroll_needs_a_nonzero_integer(self):
        self.assertRefused("scroll:0", "nonzero")
        self.assertRefused("hscroll:1.5", "nonzero")

    def test_bad_key_combinations(self):
        self.assertRefused("key:", "key name")
        self.assertRefused("key:ctrl+", "key name")
        self.assertRefused("key:hyper+a", "unknown modifier 'hyper'")
        self.assertRefused("key:ctrl+ctrl+a", "repeated modifier")
        self.assertRefused("key:Page Up", "key name")

    def test_type_needs_text(self):
        self.assertRefused("type:", "type needs text")

    def test_wait_bounds(self):
        self.assertRefused("wait:-5", "wait needs")
        self.assertRefused("wait:600001", "at most 600000")

    def test_settle_bounds(self):
        self.assertRefused("settle:0", "settle tries")
        self.assertRefused("settle:101", "settle tries")

    def test_close_window_bounds(self):
        self.assertRefused("close-window:-1", "close-window waits")
        self.assertRefused("close-window:1.5", "close-window waits")
        self.assertRefused("close-window:", "close-window waits")
        self.assertRefused("close-window:60001", "at most 60000")

    def test_no_steps_at_all(self):
        with self.assertRaises(StepError):
            parse_steps([])


if __name__ == "__main__":
    unittest.main()
