"""The process tree walk, on process tables given as data."""

import json
import resource
import subprocess
import sys
import unittest
from pathlib import Path

TOOL = Path(__file__).resolve().parents[1]


def walk(table: dict, root: int, include_zombies: bool = False) -> list[int]:
    """tree_of in a child process with a time and memory cap, so a walk that never ends fails instead of hanging."""
    code = (f"import json, sys; sys.path.insert(0, {str(TOOL)!r}); from processes import tree_of\n"
            f"print(json.dumps(sorted(tree_of({table!r}, {root}, {include_zombies}))))\n")

    def cap() -> None:
        resource.setrlimit(resource.RLIMIT_AS, (1 << 30, 1 << 30))

    try:
        result = subprocess.run([sys.executable, "-I", "-c", code], capture_output=True, text=True, timeout=10,
                                env={"PATH": "/usr/bin:/bin"}, preexec_fn=cap)
    except subprocess.TimeoutExpired:
        raise AssertionError("the walk did not end within 10 s") from None
    if result.returncode != 0:
        raise AssertionError(f"the walk failed: {result.stderr.strip()[-300:]}")
    return json.loads(result.stdout)


class TreeWalk(unittest.TestCase):
    def test_children_and_grandchildren_are_found_and_zombies_left_out(self):
        table = {2: (1, "S"), 3: (2, "Z"), 4: (3, "S"), 5: (1, "R"), 6: (9, "S")}
        self.assertEqual(walk(table, 1), [2, 4, 5])
        self.assertEqual(walk(table, 1, include_zombies=True), [2, 3, 4, 5])

    def test_a_cycle_from_a_reused_pid_ends(self):
        # A snapshot can name a reused pid as both an ancestor and a descendant.
        self.assertEqual(walk({2: (1, "S"), 3: (2, "S"), 1: (3, "S")}, 1), [2, 3])
        self.assertEqual(walk({2: (3, "S"), 3: (2, "S")}, 2), [3])


if __name__ == "__main__":
    unittest.main()
