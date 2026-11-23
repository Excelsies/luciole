"""Reports of the profile modes, from literal perf script and tcmalloc heap dump text."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import perf_profile as pp  # noqa: E402

BINARY = Path("/opt/luciole-test")
SCRIPT = """\
luciole-test 100/100
\t    7f0000001000 decode (/opt/luciole-test)
\t    7f0000002000 render (/opt/luciole-test)
\t    7f0000003000 main (/opt/luciole-test)

luciole-test 100/101
\t    7f0000009000 memcpy (/usr/lib/libc.so.6)
\t    7f0000001000 decode (/opt/luciole-test)
\t    7f0000003000 main (/opt/luciole-test)

systemd-run 99/99
\t    7f0000005000 bus_wait (/usr/lib/libsystemd-shared.so)

luciole-test 100/100
\t    7f0000009000 memcpy (/usr/lib/libc.so.6)

"""


class PerfScript(unittest.TestCase):
    def setUp(self):
        self.samples = pp.parse_perf_script(SCRIPT)

    def test_frames_come_leaf_first_and_the_harness_shims_are_dropped(self):
        self.assertEqual(len(self.samples), 3)
        comm, chain = self.samples[0]
        self.assertEqual(comm, "luciole-test")
        self.assertEqual(chain[0], ("decode", "/opt/luciole-test"))
        self.assertNotIn("systemd-run", [name for name, _ in self.samples])

    def test_self_and_inclusive_counts_split_our_code_from_libraries(self):
        info = pp.aggregate(self.samples, BINARY)
        self.assertEqual(info["total"], 3)
        self.assertEqual(info["self"][("decode", "/opt/luciole-test")], 1)
        self.assertEqual(info["self"][("memcpy", "/usr/lib/libc.so.6")], 2)
        # decode and main are in two chains each; inclusive counts a function once per sample.
        self.assertEqual(info["inclusive"][("main", "/opt/luciole-test")], 2)
        self.assertEqual(info["inclusive"][("memcpy", "/usr/lib/libc.so.6")], 2)
        self.assertEqual(info["ours_self_samples"], 1)
        self.assertEqual(info["folded"]["main;render;decode"], 1)

    def test_short_chains_are_reported_as_a_lost_unwind_only_when_most_are_short(self):
        info = pp.aggregate(self.samples, BINARY)
        self.assertAlmostEqual(info["shallow_fraction"], 1 / 3)
        self.assertNotIn("WARNING", pp.perf_markdown(info, BINARY, "x"))
        short = pp.aggregate([("luciole-test", [("a", "/opt/luciole-test")])] * 3, BINARY)
        self.assertIn("WARNING", pp.perf_markdown(short, BINARY, "x"))

    def test_an_empty_recording_is_an_error(self):
        with self.assertRaises(pp.ProfileError):
            pp.perf_markdown(pp.aggregate([], BINARY), BINARY, "x")

    def test_a_line_that_is_neither_header_nor_frame_is_an_error(self):
        with self.assertRaises(pp.ProfileError):
            pp.parse_perf_script("???\n")


HEAP = """\
heap profile:      3:     3072 [     9:     9216] @ heapprofile
     1:     1024 [     4:     4096] @ 0x1100 0x1200 0x1300
     2:     2048 [     5:     5120] @ 0x1500 0x1300

MAPPED_LIBRARIES:
7f0000001000-7f0000009000 r-xp 00000000 08:01 12   /opt/luciole-test
7f000000a000-7f000000b000 rw-p 00008000 08:01 12   /opt/luciole-test
7f0000100000-7f0000200000 r-xp 00000000 08:01 13   /usr/lib/libstdc++.so.6
"""


class Heap(unittest.TestCase):
    def test_records_and_executable_mappings_are_parsed(self):
        records, maps = pp.parse_heap(HEAP)
        self.assertEqual(records[0], {"inuse_objects": 1, "inuse_bytes": 1024, "alloc_objects": 4,
                                      "alloc_bytes": 4096, "stack": [0x1100, 0x1200, 0x1300]})
        self.assertEqual([item[3] for item in maps], ["/opt/luciole-test", "/usr/lib/libstdc++.so.6"])

    def test_an_empty_dump_is_an_error(self):
        with self.assertRaises(pp.ProfileError):
            pp.parse_heap("MAPPED_LIBRARIES:\n")

    def test_sites_are_the_innermost_frame_in_the_binary(self):
        records, maps = pp.parse_heap(HEAP)
        symbols = pp.Symbols(maps)
        symbols.cache = {0x1100: ("operator new", "/usr/lib/libstdc++.so.6"), 0x1200: ("Loader::read", str(BINARY)),
                         0x1300: ("main", str(BINARY)), 0x1500: ("operator new", "/usr/lib/libstdc++.so.6")}
        sites = pp.heap_sites(records, symbols, BINARY)
        self.assertEqual(sites["Loader::read"]["inuse_bytes"], 1024)
        self.assertEqual(sites["main"]["inuse_bytes"], 2048)
        self.assertEqual(sites["main"]["alloc_bytes"], 5120)
        table = pp.heap_table(sites, "inuse_bytes")
        self.assertLess(table.index("`main`"), table.index("`Loader::read`"))

    def test_a_stack_with_no_frame_in_the_binary_names_the_library(self):
        symbols = pp.Symbols([])
        symbols.cache = {6: ("tcmalloc::hook", "/usr/lib/libtcmalloc_and_profiler.so.4"),
                         7: ("g_malloc", "/usr/lib/libglib-2.0.so")}
        self.assertEqual(pp.site([6, 7], symbols, BINARY), "g_malloc [libglib-2.0.so, no frame in luciole-test]")

    def test_addresses_map_to_file_offsets_relative_to_the_lowest_mapping(self):
        _, maps = pp.parse_heap(HEAP)
        symbols = pp.Symbols(maps)
        self.assertEqual(symbols.locate(0x7f0000001234), ("/opt/luciole-test", 0x234))
        self.assertIsNone(symbols.locate(0x7f0000009500))


if __name__ == "__main__":
    unittest.main()


class SampleLink(unittest.TestCase):
    """A scenario that needs an environment variable gets its sample linked in, or the profile refuses to start."""

    def test_a_scenario_that_needs_the_sample_is_refused_without_it(self):
        import os
        import perf_bench
        os.environ.pop("LUCIOLE_BINK2_SAMPLE", None)
        with self.assertRaises(ValueError):
            pp.sample_link({"name": "video-bink", "needs_env": "LUCIOLE_BINK2_SAMPLE"}, perf_bench, Path("/nowhere"))

    def test_the_sample_is_linked_for_the_run_and_removed_after(self):
        import os
        import tempfile
        import perf_bench
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            (root / "original.bk2").write_bytes(b"x")
            os.environ["LUCIOLE_BINK2_SAMPLE"] = str(root / "original.bk2")
            try:
                with pp.sample_link({"name": "video-bink", "needs_env": "LUCIOLE_BINK2_SAMPLE"}, perf_bench, root):
                    self.assertTrue((root / "bink" / "sample.bk2").exists())
                self.assertFalse((root / "bink").exists())
            finally:
                del os.environ["LUCIOLE_BINK2_SAMPLE"]

    def test_a_plain_scenario_needs_nothing(self):
        import perf_bench
        with pp.sample_link({"name": "churn", "needs_env": None}, perf_bench, Path("/nowhere")):
            pass
