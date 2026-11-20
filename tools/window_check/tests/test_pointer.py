"""Window-relative pointer positions become absolute positions on the output layout."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from step_actions import StepFailed, pointer_motion  # noqa: E402

# A 1400x900 mode at scale 1.5 is 933x600 logical pixels.
OUTPUT = {"position": {"x": 0, "y": 0}, "logical_size": {"width": 933, "height": 600}}


class PointerMotion(unittest.TestCase):
    def test_window_at_the_origin(self):
        window = {"x": 0, "y": 0, "w": 933, "h": 600}
        self.assertEqual(pointer_motion(window, OUTPUT, 100, 50), ((100, 50), "motion 10000 5000 93300 60000"))

    def test_window_offset_and_fractional_position(self):
        window = {"x": 40, "y": 30, "w": 400, "h": 300}
        self.assertEqual(pointer_motion(window, OUTPUT, 10.25, 0.5), ((50.25, 30.5), "motion 5025 3050 93300 60000"))

    def test_output_offset_is_removed(self):
        output = {"position": {"x": 1920, "y": 0}, "logical_size": {"width": 800, "height": 600}}
        window = {"x": 1920, "y": 0, "w": 800, "h": 600}
        self.assertEqual(pointer_motion(window, output, 1, 2)[1], "motion 100 200 80000 60000")

    def test_position_off_the_output_is_refused(self):
        window = {"x": 0, "y": 0, "w": 933, "h": 600}
        with self.assertRaises(StepFailed) as caught:
            pointer_motion(window, OUTPUT, 933, 10)
        self.assertIn("outside the 933x600 output", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
