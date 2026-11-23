"""Parsing and sampling of /proc for the performance harness."""

import os
import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from perf_proc import (SampleError, breakdown, classify, cpu_ticks, parse_kib_fields, parse_smaps,  # noqa: E402
                       sample_memory, switch_delta, thread_counters)

MIB = 1 << 20
SMAPS = """\
55d000000000-55d000100000 r-xp 00000000 08:01 12 /usr/bin/app
Size:               1024 kB
Rss:                 512 kB
Pss:                 400 kB
Shared_Clean:        100 kB
Shared_Dirty:          0 kB
Private_Clean:       412 kB
Private_Dirty:         0 kB
Anonymous:             0 kB
Swap:                  0 kB
55d000100000-55d000200000 rw-p 00100000 08:01 12 /usr/bin/app
Size:               1024 kB
Rss:                 100 kB
Pss:                 100 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:       100 kB
Anonymous:           100 kB
Swap:                  0 kB
55d001000000-55d001100000 rw-p 00000000 00:00 0 [heap]
Size:               1024 kB
Rss:                 300 kB
Pss:                 300 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:       300 kB
Anonymous:           300 kB
Swap:                  0 kB
7f0004000000-7f0004200000 rw-p 00000000 00:00 0
Size:               2048 kB
Rss:                2000 kB
Pss:                2000 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:      2000 kB
Anonymous:          2000 kB
Swap:                  0 kB
7f0004200000-7f0008000000 ---p 00000000 00:00 0
Size:              63488 kB
Rss:                   0 kB
Pss:                   0 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:         0 kB
Anonymous:             0 kB
Swap:                  0 kB
7f0010000000-7f0010001000 ---p 00000000 00:00 0
Size:                  4 kB
Rss:                   0 kB
Pss:                   0 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:         0 kB
Anonymous:             0 kB
Swap:                  0 kB
7f0010001000-7f0010801000 rw-p 00000000 00:00 0
Size:               8192 kB
Rss:                  64 kB
Pss:                  64 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:        64 kB
Anonymous:            64 kB
Swap:                  0 kB
7f0020000000-7f0020100000 rw-s 00000000 00:05 9 /memfd:wayland-shm (deleted)
Size:               1024 kB
Rss:                 800 kB
Pss:                 800 kB
Shared_Clean:          0 kB
Shared_Dirty:        800 kB
Private_Clean:         0 kB
Private_Dirty:         0 kB
Anonymous:             0 kB
Swap:                  0 kB
7f0030000000-7f0030100000 rw-p 00000000 00:00 0
Size:               1024 kB
Rss:                  10 kB
Pss:                  10 kB
Shared_Clean:          0 kB
Shared_Dirty:          0 kB
Private_Clean:         0 kB
Private_Dirty:        10 kB
Anonymous:            10 kB
Swap:                  0 kB
"""


class Smaps(unittest.TestCase):
    def setUp(self):
        self.mappings = parse_smaps(SMAPS)

    def test_parses_every_mapping_with_its_fields(self):
        self.assertEqual(len(self.mappings), 9)
        first = self.mappings[0]
        self.assertEqual((first["path"], first["perms"], first["Rss"], first["Pss"]), ("/usr/bin/app", "r-xp", 512, 400))
        self.assertEqual(self.mappings[2]["path"], "[heap]")
        self.assertEqual(self.mappings[3]["path"], "")

    def test_classes_are_assigned_by_path_and_neighbors(self):
        kinds = [classify(m, self.mappings[i - 1] if i else None,
                          self.mappings[i + 1] if i + 1 < len(self.mappings) else None)
                 for i, m in enumerate(self.mappings)]
        # file, file, heap, arena (aligned, guard after), two guards, stack (guard before), device, other
        self.assertEqual(kinds, ["file", "file", "heap", "arena", "anon_other", "anon_other", "stack", "device",
                                 "anon_other"])

    def test_breakdown_sums_classes_and_ranks_libraries(self):
        result = breakdown(self.mappings)
        classes = result["classes"]
        self.assertEqual(classes["heap"]["rss"], 300)
        self.assertEqual(classes["arena"]["rss"], 2000)
        self.assertEqual(classes["stack"]["rss"], 64)
        self.assertEqual(classes["device"]["rss"], 800)
        self.assertEqual(classes["file"]["rss"], 612)
        self.assertEqual(classes["file"]["shared"], 100)
        [library] = result["libraries_top"]
        self.assertEqual((library["name"], library["pss"], library["code_pss"], library["data_pss"]),
                         ("app", 500, 400, 100))
        [device] = result["devices"]
        self.assertEqual(device["path"], "/memfd:wayland-shm")

    def test_only_fifteen_libraries_are_listed_and_the_rest_summed(self):
        text = "".join(f"{0x1000 * i + 0x10000000:x}-{0x1000 * i + 0x10001000:x} r--p 0 08:01 1 /lib/lib{i}.so\n"
                       "Size: 4 kB\nRss: 4 kB\nPss: {}\nShared_Clean: 0 kB\nShared_Dirty: 0 kB\nPrivate_Clean: 4 kB\n"
                       "Private_Dirty: 0 kB\nAnonymous: 0 kB\nSwap: 0 kB\n".format(f"{i} kB") for i in range(20))
        result = breakdown(parse_smaps(text))
        self.assertEqual(len(result["libraries_top"]), 15)
        self.assertEqual(result["libraries_top"][0]["name"], "lib19.so")
        self.assertEqual(result["libraries_rest"]["pss"], sum(range(5)))


class Fields(unittest.TestCase):
    def test_missing_field_is_an_error(self):
        with self.assertRaises(SampleError):
            parse_kib_fields("Rss: 4 kB\n", ("Rss", "Pss"))

    def test_reads_named_fields(self):
        self.assertEqual(parse_kib_fields("Rss: 4 kB\nPss: 2 kB\nOther: 9 kB\n", ("Rss", "Pss")), {"Rss": 4, "Pss": 2})


class Live(unittest.TestCase):
    def test_own_process_memory_is_sampled(self):
        held = bytearray(20 * MIB)
        held[::4096] = b"x" * len(held[::4096])
        sample = sample_memory(os.getpid())
        self.assertGreaterEqual(sample["rollup"]["Rss"], 20 * 1024)
        self.assertGreaterEqual(sample["peak_rss_kib"], sample["rss_kib"])
        self.assertGreater(sample["classes"]["file"]["pss"], 0)
        self.assertEqual(sum(item["count"] for item in sample["classes"].values()),
                         len(parse_smaps(Path(f"/proc/{os.getpid()}/smaps").read_text())))

    def test_a_missing_process_is_an_error_not_zeros(self):
        with self.assertRaises(SampleError):
            sample_memory(2 ** 22 + 12345)

    def test_cpu_ticks_grow_with_work(self):
        before = sum(cpu_ticks(os.getpid()))
        end = os.times().user + 0.3
        while os.times().user < end:
            pass
        self.assertGreaterEqual(sum(cpu_ticks(os.getpid())) - before, 20)

    def test_threads_and_switch_delta(self):
        gate = threading.Event()
        thread = threading.Thread(target=gate.wait)
        thread.start()
        try:
            before = thread_counters(os.getpid())
            self.assertGreaterEqual(len(before), 2)
            gate.set()
            thread.join()
            after = thread_counters(os.getpid())
            delta = switch_delta(before, after)
            self.assertEqual(delta["threads_ended"], len(before) - len(before.keys() & after.keys()))
            self.assertGreaterEqual(delta["voluntary"], 0)
        finally:
            gate.set()
            thread.join()

    def test_switch_delta_counts_only_threads_in_both(self):
        before = {1: (10, 1), 2: (5, 0)}
        after = {1: (14, 3), 3: (7, 7)}
        self.assertEqual(switch_delta(before, after),
                         {"voluntary": 4, "involuntary": 2, "threads_started": 1, "threads_ended": 1})


if __name__ == "__main__":
    unittest.main()


class ThreadNames(unittest.TestCase):
    def test_threads_are_counted_by_the_name_they_gave_themselves(self):
        import ctypes
        gate = threading.Event()
        started = threading.Event()

        def named():
            ctypes.CDLL(None).prctl(15, b"probe-thread", 0, 0, 0)
            started.set()
            gate.wait()

        thread = threading.Thread(target=named)
        thread.start()
        try:
            started.wait(5)
            from perf_proc import thread_names
            self.assertEqual(thread_names(os.getpid())["probe-thread"], 1)
        finally:
            gate.set()
            thread.join()
