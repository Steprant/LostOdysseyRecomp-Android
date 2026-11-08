#!/usr/bin/env python3
"""Compare Android Tester diagnostics archives (roadmap 0.2).

Each argument is a Tester ZIP or an extracted folder. Prints one column per
session: frame pacing from frame_timing.csv, GPU occupancy from gpu.csv,
battery temperature from thermal.csv, thread CPU from threads.csv, and the
per-frame averages of the runtime log's ten-second "gpu passes" lines
(attachment load/store MB, passes, barriers, copies, content census) plus the
last "draw classes" line.

  python3 tools/android/compare-tester.py before.zip after.zip [--from 30 --to 90]

--from/--to keep a window of seconds from the first frame, so two runs of
the same route can be compared on the same scene. Only the standard library
is used; nothing is written.
"""
import argparse
import csv
import io
import re
import statistics
import sys
import zipfile
from pathlib import Path


class Source:
    """A Tester ZIP or folder, read by file name wherever the file sits."""

    def __init__(self, path):
        self.path = Path(path)
        self.zip = zipfile.ZipFile(self.path) if self.path.is_file() else None

    def read(self, name):
        if self.zip:
            for entry in self.zip.namelist():
                if entry == name or entry.endswith("/" + name):
                    return self.zip.read(entry).decode("utf-8", "replace")
            return None
        for candidate in sorted(self.path.rglob(name)):
            if candidate.is_file() and candidate.parent.name != "events":
                return candidate.read_text("utf-8", "replace")
        return None


def rows(text):
    return list(csv.DictReader(io.StringIO(text))) if text else []


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def percentile(values, p):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, max(0, round(p / 100.0 * (len(ordered) - 1))))
    return ordered[index]


GPU_PASSES = re.compile(r"^\[\s*([0-9.]+)[^\]]*\].*?gpu passes: .*? per_frame: (.*)$")
DRAW_CLASSES = re.compile(r"draw classes \(session.*")
LOG_TIME = re.compile(r"^\[\s*([0-9.]+)")


def pass_fields(text):
    """Numeric key=value pairs before ' top:', plus content_mb and pass ends."""
    head = text.split(" top:")[0]
    fields = {}
    for key, value in re.findall(r"(\w+)=([0-9.]+)", head.split(" present:")[0]):
        fields[key] = float(value)
    present = re.search(r"present: passes=([0-9.]+) ms=([0-9.]+)", text)
    if present:
        fields["present_ms"] = float(present.group(2))
    content = re.search(r"content_mb: color (.*?); depth (.*?);", text)
    if content:
        for kind, part in (("color", content.group(1)), ("depth", content.group(2))):
            for key, value in re.findall(r"(\w+)=([0-9.]+)", part):
                fields[f"{kind}_{key}"] = float(value)
    formats = re.search(r"format_draws_per_frame:(.*)$", text)
    if formats:
        for key, value in re.findall(r"(\w+)=([0-9.]+)", formats.group(1)):
            fields[f"draws_{key}"] = float(value)
    state = re.search(r"state_per_frame:(.*?);", text)
    if state:
        for key, value in re.findall(r"(\w+)=([0-9.]+)", state.group(1)):
            fields[f"state_{key}"] = float(value)
    ends = re.search(r"pass_ends_per_frame:(.*?);", text)
    if ends:
        for key, value in re.findall(r"(\w+)=([0-9.]+)", ends.group(1)):
            fields[f"end_{key}"] = float(value)
    return fields


def analyse(path, start=None, end=None):
    source = Source(path)
    out = {"session": Path(path).name}

    frames = rows(source.read("frame_timing.csv"))
    first = number(frames[0]["monotonic_ns"]) if frames else None

    def in_window(ns):
        if first is None or ns is None:
            return True
        seconds = (ns - first) / 1e9
        return (start is None or seconds >= start) and (end is None or seconds <= end)

    intervals = [number(r["interval_ns"]) / 1e6 for r in frames
                 if number(r.get("interval_ns")) and in_window(number(r["monotonic_ns"]))]
    if intervals:
        out["frames"] = len(intervals)
        out["seconds"] = sum(intervals) / 1000.0
        out["frame_median_ms"] = statistics.median(intervals)
        out["frame_p95_ms"] = percentile(intervals, 95)
        out["frame_p99_ms"] = percentile(intervals, 99)
        out["frame_max_ms"] = max(intervals)
        out["frames_over_50ms"] = sum(1 for v in intervals if v > 50)
        out["frames_over_100ms"] = sum(1 for v in intervals if v > 100)
        draws = [number(r.get("draws")) for r in frames if in_window(number(r["monotonic_ns"]))]
        draws = [d for d in draws if d]
        if draws:
            out["draws_median"] = statistics.median(draws)

    gpu = [r for r in rows(source.read("gpu.csv")) if in_window(number(r["monotonic_ns"]))]
    samples = [(number(r["monotonic_ns"]), number(r["completed_batch_total_ns"])) for r in gpu]
    samples = [s for s in samples if s[0] is not None and s[1] is not None]
    if len(samples) >= 2:
        wall = samples[-1][0] - samples[0][0]
        busy = samples[-1][1] - samples[0][1]
        if wall > 0:
            out["gpu_busy_pct"] = 100.0 * busy / wall

    thermal = [number(r.get("battery_temperature_c")) for r in rows(source.read("thermal.csv"))
               if in_window(number(r["monotonic_ns"]))]
    thermal = [t for t in thermal if t is not None]
    if thermal:
        out["battery_c_max"] = max(thermal)
        out["battery_c_last"] = thermal[-1]

    threads = {}
    for r in rows(source.read("threads.csv")):
        value = number(r.get("thread_cpu_percent_one_core"))
        if value is not None and in_window(number(r["monotonic_ns"])):
            threads.setdefault(r.get("name", "?"), []).append(value)
    for name, values in threads.items():
        if any(key in name for key in ("Guest Main", "GPU CmdProc", "Present", "XMA", "Audio")):
            out[f"cpu% {name}"] = statistics.mean(values)

    log = source.read("runtime.log") or ""
    log_start = None
    passes = []
    draw_classes = None
    for line in log.splitlines():
        stamp = LOG_TIME.match(line)
        if stamp and log_start is None:
            log_start = float(stamp.group(1))
        match = GPU_PASSES.match(line)
        if match:
            seconds = float(match.group(1)) - (log_start or 0.0)
            # The log clock and the CSV clock differ; the window applies to
            # seconds since the log's first line, close enough at 10 s windows.
            if (start is None or seconds >= start) and (end is None or seconds <= end):
                passes.append(pass_fields(match.group(2)))
        found = DRAW_CLASSES.search(line)
        if found:
            draw_classes = found.group(0)
    if passes:
        out["pass_windows"] = len(passes)
        keys = sorted({k for p in passes for k in p})
        for key in keys:
            values = [p[key] for p in passes if key in p]
            out[f"pass {key}"] = statistics.mean(values)
    out["draw_classes"] = draw_classes
    return out


def fmt(value):
    if value is None:
        return "-"
    if isinstance(value, float):
        return f"{value:.2f}" if abs(value) < 100 else f"{value:.0f}"
    return str(value)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("sessions", nargs="+", help="Tester ZIPs or extracted folders")
    parser.add_argument("--from", dest="start", type=float, help="window start, seconds from the first frame")
    parser.add_argument("--to", dest="end", type=float, help="window end, seconds from the first frame")
    args = parser.parse_args(argv)
    results = [analyse(path, args.start, args.end) for path in args.sessions]
    keys = []
    for result in results:
        for key in result:
            if key not in keys and key != "draw_classes":
                keys.append(key)
    width = max(len(k) for k in keys) + 2
    for key in keys:
        cells = [fmt(r.get(key)) for r in results]
        line = key.ljust(width) + "  ".join(c.rjust(14) for c in cells)
        if len(results) == 2:
            a, b = results[0].get(key), results[1].get(key)
            if isinstance(a, (int, float)) and isinstance(b, (int, float)) and a:
                line += f"  {100.0 * (b - a) / abs(a):+7.1f}%"
        print(line)
    for result in results:
        if result.get("draw_classes"):
            print(f"\n{result['session']}: {result['draw_classes']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
