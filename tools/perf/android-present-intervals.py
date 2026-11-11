#!/usr/bin/env python3
"""Capture or compare Android SurfaceFlinger timestamps for the game's visible layer."""

import argparse
import csv
import json
from pathlib import Path
import shlex
import subprocess
import time


POLL_SECONDS = 2.0
PENDING_FENCE = (1 << 63) - 1
TIME_COLUMN = "actual_present_ns"


def parse_latency(text):
    rows = [line.split() for line in text.splitlines() if line.strip()]
    if not rows or len(rows[0]) != 1 or not rows[0][0].isdecimal() or int(rows[0][0]) <= 0:
        raise ValueError("missing SurfaceFlinger refresh period")
    timestamps = set()
    for row in rows[1:]:
        if len(row) != 3:
            raise ValueError("unexpected SurfaceFlinger latency row")
        # Android FrameTracker prints desired, actual and ready times, in ns.
        _, actual, _ = map(int, row)
        if 0 < actual < PENDING_FENCE:
            timestamps.add(actual)
    if not timestamps:
        raise ValueError("no signaled present fences for this layer; check SurfaceFlinger --list")
    return int(rows[0][0]), sorted(timestamps)


def append_snapshot(timestamps, snapshot):
    last = timestamps[-1]
    if last not in snapshot:
        raise ValueError("SurfaceFlinger history lost overlap; the layer changed or the ring wrapped")
    timestamps.extend(t for t in snapshot if t > last)


def summarize(timestamps):
    intervals = [right - left for left, right in zip(timestamps, timestamps[1:])]
    if not intervals or timestamps[0] <= 0 or min(intervals) <= 0 or timestamps[-1] >= PENDING_FENCE:
        raise ValueError("need at least two strictly increasing actual present timestamps")
    duration = (timestamps[-1] - timestamps[0]) / 1e9
    ordered = sorted(intervals)
    count = len(ordered)
    within_35 = sum(ns <= 35_000_000 for ns in ordered)
    over_50 = sum(ns > 50_000_000 for ns in ordered)

    def percentile(p):
        return ordered[(p * count + 99) // 100 - 1] / 1e6

    return {
        "duration_s": duration,
        "displayed_intervals": count,
        "displayed_fps": count / duration,
        "p95_ms": percentile(95),
        "p99_ms": percentile(99),
        "max_ms": ordered[-1] / 1e6,
        "percent_within_35_ms": 100 * within_35 / count,
        "intervals_over_50_ms": over_50,
        "over_50_ms_per_minute": 60 * over_50 / duration,
        "meets_interval_targets": 100 * within_35 >= 99 * count and 60 * over_50 < duration,
    }


def capture(args):
    adb = [args.adb] + (["-s", args.serial] if args.serial else [])
    command = "dumpsys SurfaceFlinger --latency " + shlex.quote(args.layer)

    def snapshot():
        result = subprocess.run(adb + ["shell", command], check=True, capture_output=True, text=True, timeout=10)
        return parse_latency(result.stdout)

    first_period, first = snapshot()
    time.sleep(0.5)
    period, initial = snapshot()
    if initial[-1] <= first[-1]:
        raise ValueError("the selected layer is not actively presenting")
    periods = {first_period, period}
    timestamps = [initial[-1]]
    last_update = time.monotonic()
    end = last_update + args.seconds
    while True:
        time.sleep(max(0.0, min(POLL_SECONDS, end - time.monotonic())))
        period, rows = snapshot()
        periods.add(period)
        before = len(timestamps)
        append_snapshot(timestamps, rows)
        if time.monotonic() >= end:
            if before == len(timestamps) and (end - last_update) > POLL_SECONDS:
                raise ValueError("the layer stopped updating before the end of the capture")
            break
        if len(timestamps) > before:
            last_update = time.monotonic()
    stats = summarize(timestamps)
    if stats["duration_s"] < args.seconds - POLL_SECONDS:
        raise ValueError("present timestamps do not cover the capture window")
    with args.out.open("x", newline="", encoding="utf-8") as out:
        writer = csv.writer(out)
        writer.writerow([TIME_COLUMN, "interval_ns"])
        writer.writerow([timestamps[0], ""])
        writer.writerows((current, current - previous) for previous, current in zip(timestamps, timestamps[1:]))
    return {"layer": args.layer, "pacesetter_periods_ns": sorted(periods), **stats}


def compare(baseline, candidate):
    def from_csv(path):
        with path.open(newline="", encoding="utf-8") as source:
            rows = csv.DictReader(source)
            if not rows.fieldnames or TIME_COLUMN not in rows.fieldnames:
                raise ValueError(f"{path}: missing {TIME_COLUMN}")
            return summarize([int(row[TIME_COLUMN]) for row in rows])

    before, after = from_csv(baseline), from_csv(candidate)
    metrics = ("displayed_fps", "p95_ms", "p99_ms", "percent_within_35_ms", "over_50_ms_per_minute")
    return {"baseline": before, "candidate": after, "candidate_minus_baseline": {
        metric: after[metric] - before[metric] for metric in metrics
    }}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_subparsers(dest="mode", required=True)
    collect = modes.add_parser("capture", help="read the selected layer's rolling present history")
    collect.add_argument("--layer", required=True, help="exact layer name from dumpsys SurfaceFlinger --list")
    collect.add_argument("--seconds", type=int, default=300)
    collect.add_argument("--adb", default="adb")
    collect.add_argument("--serial")
    collect.add_argument("--out", type=Path, required=True)
    pair = modes.add_parser("compare", help="compare two captured CSVs from the same scene and output")
    pair.add_argument("baseline", type=Path)
    pair.add_argument("candidate", type=Path)
    args = parser.parse_args(argv)
    if args.mode == "capture" and args.seconds < 5:
        parser.error("capture requires at least five seconds")
    try:
        result = capture(args) if args.mode == "capture" else compare(args.baseline, args.candidate)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(1, f"error: {error}\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
