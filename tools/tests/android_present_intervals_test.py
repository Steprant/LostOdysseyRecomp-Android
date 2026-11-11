"""SurfaceFlinger collection and pacing statistics with synthetic timestamps."""

import importlib.util
from itertools import accumulate
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("android_present_intervals", ROOT / "tools/perf/android-present-intervals.py")
present = importlib.util.module_from_spec(spec)
spec.loader.exec_module(present)


class PresentIntervalsTest(unittest.TestCase):
    def test_parse_uses_signaled_actual_present(self):
        period, times = present.parse_latency(
            f"8333333\n5 105 4\n0 0 0\n3 70 2\n8 {present.PENDING_FENCE} 1\n8 105 1\n"
        )
        self.assertEqual((period, times), (8_333_333, [70, 105]))
        with self.assertRaisesRegex(ValueError, "no signaled"):
            present.parse_latency("8333333\n0 0 0\n")

    def test_overlapping_history_is_required(self):
        samples = [10, 20]
        present.append_snapshot(samples, [5, 10, 20, 30, 40])
        present.append_snapshot(samples, [20, 30, 40])
        self.assertEqual(samples, [10, 20, 30, 40])
        with self.assertRaisesRegex(ValueError, "lost overlap"):
            present.append_snapshot(samples, [50, 60])

    def test_percentile_and_strict_thresholds(self):
        milliseconds = [35] * 98 + [50, 55]
        times = list(accumulate([1] + [value * 1_000_000 for value in milliseconds]))
        result = present.summarize(times)
        self.assertEqual(result["p95_ms"], 35)
        self.assertEqual(result["p99_ms"], 50)
        self.assertEqual(result["percent_within_35_ms"], 98)
        self.assertEqual(result["intervals_over_50_ms"], 1)
        self.assertFalse(result["meets_interval_targets"])
        self.assertTrue(present.summarize([1, 35_000_001, 70_000_001])["meets_interval_targets"])
        minute = [33_000_000] * 1800 + [600_000_000]
        minute_result = present.summarize(list(accumulate([1] + minute)))
        self.assertEqual(minute_result["over_50_ms_per_minute"], 1)
        self.assertFalse(minute_result["meets_interval_targets"])
        with self.assertRaises(ValueError):
            present.summarize([1, 1, 35_000_001])

    def test_compare_reads_timestamps_instead_of_cached_deltas(self):
        with tempfile.TemporaryDirectory() as tmp:
            baseline, candidate = Path(tmp, "before.csv"), Path(tmp, "after.csv")
            baseline.write_text("actual_present_ns,interval_ns\n1,\n75000001,0\n", encoding="utf-8")
            candidate.write_text("actual_present_ns,interval_ns\n1,\n35000001,0\n", encoding="utf-8")
            result = present.compare(baseline, candidate)
            self.assertEqual(result["candidate_minus_baseline"]["p99_ms"], -40)
            self.assertEqual(result["candidate"]["intervals_over_50_ms"], 0)

    def test_capture_fails_if_no_complete_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp, "result.csv")
            args = SimpleNamespace(adb="adb", serial=None, layer="test", seconds=5, out=output)
            dumps = ["8333333\n0 100 0\n", "8333333\n0 200 0\n", "8333333\n0 200 0\n"]
            with patch.object(present.time, "monotonic", side_effect=[0, 0, 6]), \
                    patch.object(present.time, "sleep"), patch.object(present.subprocess, "run") as run:
                run.side_effect = [present.subprocess.CompletedProcess([], 0, dump) for dump in dumps]
                with self.assertRaisesRegex(ValueError, "stopped updating"):
                    present.capture(args)
            self.assertFalse(output.exists())

    def test_capture_collects_overlapping_windows(self):
        def dump(start, end):
            timestamps = [1 + i * 33_333_333 for i in range(start, end)]
            return "16666667\n" + "".join(f"0 {timestamp} 0\n" for timestamp in timestamps)

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp, "result.csv")
            args = SimpleNamespace(adb="adb", serial=None, layer="SurfaceView [game]", seconds=5, out=output)
            dumps = [dump(0, 60), dump(0, 75), dump(11, 135), dump(71, 195), dump(101, 225)]
            with patch.object(present.time, "monotonic", side_effect=[0, 0, 2, 2, 2, 4, 4, 4, 5]), \
                    patch.object(present.time, "sleep"), patch.object(present.subprocess, "run") as run:
                run.side_effect = [present.subprocess.CompletedProcess([], 0, text) for text in dumps]
                result = present.capture(args)
                self.assertIn("'SurfaceView [game]'", run.call_args.args[0][-1])
            self.assertEqual(result["displayed_intervals"], 150)
            self.assertTrue(result["meets_interval_targets"])
            self.assertEqual(len(output.read_text(encoding="utf-8").splitlines()), 152)


if __name__ == "__main__":
    unittest.main()
