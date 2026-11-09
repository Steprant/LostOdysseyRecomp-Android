#!/usr/bin/env python3
"""Steprant 2.0 WP-A: "where the milliseconds go" from Tester sessions.

Each argument is LABEL=ZIP, for example
  python3 tools/android/bottleneck-map.py city-576=a.zip city-900=b.zip \\
      city-900-deadstores=c.zip city-900-untimed=d.zip city-60fps-576=e.zip

Conventions in the label (any order, separated by '-'):
  the scene first (city, field, battle, tv, ...); "deadstores" for a session
  with the developer switch "Discard dead stores (test only)" on; "untimed"
  for one with "Per-draw timestamps" off; "60fps" for a 60 FPS session.

Per scene it reports: GPU ms per frame and how it splits (passes, outside
passes, upscaler), the pixel slope (ms per render megapixel across sessions of
different SGSR2 bases), traffic and what a frame graph could save (what-if),
the measured cost of dead stores (deadstores session against its twin), the
real cost per draw (least-squares fit of the untimed session), the GPU command
thread's CPU split (record against parse) and the 60 FPS CPU budget. Then the
decision table of Super Roadmap 2 (D8), with the rule each row applied.
Standard library only; imports tools/android/compare-tester.py.
"""
import argparse
import importlib.util
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("compare_tester", HERE / "compare-tester.py")
compare = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(compare)


def gpu_ms(result):
    """GPU time per frame: passes plus outside passes, from the pass profile."""
    pass_ms = result.get("pass pass_ms")
    outside = result.get("pass outside_ms")
    if pass_ms is None:
        return None
    return pass_ms + (outside or 0.0)


def slope(points):
    """Least squares slope and intercept of (x, y) points; None below two."""
    if len(points) < 2:
        return None, None
    n = float(len(points))
    sx = sum(p[0] for p in points); sy = sum(p[1] for p in points)
    sxx = sum(p[0] * p[0] for p in points); sxy = sum(p[0] * p[1] for p in points)
    den = sxx - sx * sx / n
    if den <= 0:
        return None, None
    b = (sxy - sx * sy / n) / den
    return b, (sy - b * sx) / n


def build(sessions):
    scenes = {}
    for label, result in sessions:
        parts = label.split("-")
        scene = scenes.setdefault(parts[0], {"plain": [], "deadstores": [], "untimed": [], "60fps": []})
        kind = "deadstores" if "deadstores" in parts else "untimed" if "untimed" in parts else \
            "60fps" if "60fps" in parts else "plain"
        scene[kind].append((label, result))
    table = {}
    for name, groups in scenes.items():
        row = {}
        plain = groups["plain"]
        reference = plain[-1][1] if plain else (groups["deadstores"] or groups["untimed"] or groups["60fps"])[0][1]
        ms = gpu_ms(reference)
        row["gpu_ms"] = ms
        row["pass_ms"] = reference.get("pass pass_ms")
        row["outside_ms"] = reference.get("pass outside_ms")
        row["gpu_busy_pct"] = reference.get("gpu_busy_pct")
        row["render"] = reference.get("sgsr2_input")
        # Pixels: ms per render megapixel over sessions with different bases.
        points = [(r["render_mpix"], gpu_ms(r)) for _, r in plain if r.get("render_mpix") and gpu_ms(r) is not None]
        per_mpix, fixed = slope(points)
        row["ms_per_mpix"] = per_mpix
        row["pixel_ms"] = per_mpix * reference["render_mpix"] if per_mpix is not None and reference.get("render_mpix") else None
        # Traffic and the frame graph what-if.
        row["traffic_mb"] = reference.get("what-if traffic_mb")
        row["what_if_savable_mb"] = reference.get("what-if savable_mb")
        row["what_if_share_pct"] = reference.get("what-if savable_share")
        row["what_if_merge_passes"] = reference.get("what-if merge_passes")
        # Measured dead-store cost: plain against the deadstores twin.
        if groups["deadstores"] and ms is not None:
            twin = groups["deadstores"][-1][1]
            if gpu_ms(twin) is not None:
                row["dead_store_saving_ms"] = ms - gpu_ms(twin)
                row["dead_store_discarded_mb"] = twin.get("what-if diag_discarded_mb")
        # Real cost per draw (untimed fit), and draw time per frame.
        source = groups["untimed"][-1][1] if groups["untimed"] else reference
        row["us_per_draw"] = source.get("draw_fit_us_per_draw")
        row["draw_fit_r2"] = source.get("draw_fit_r2")
        draws = reference.get("pass draws") or reference.get("draws_median")
        row["draws"] = draws
        row["draw_ms"] = row["us_per_draw"] * draws / 1000.0 if row["us_per_draw"] is not None and draws else None
        # CPU: the GPU command thread (30 and 60 FPS).
        row["cmdproc_cpu_ms"] = reference.get("pass cmdproc_cpu_ms")
        row["cmdproc_record_ms"] = reference.get("pass cmdproc_record_ms")
        row["cmdproc_parse_ms"] = reference.get("pass cmdproc_parse_ms")
        if groups["60fps"]:
            fast = groups["60fps"][-1][1]
            row["cmdproc_cpu_ms_60fps"] = fast.get("pass cmdproc_cpu_ms")
            row["cmdproc_max_ms_60fps"] = fast.get("pass cmdproc_max_cpu_ms")
        row["frames_over_50ms_per_min"] = (60.0 * reference["frames_over_50ms"] / reference["seconds"]) \
            if reference.get("seconds") else None
        table[name] = row
    return table


def decide(table):
    """The WP-A decision table (Super Roadmap 2, Part 3) on the worst scene."""
    def worst(key):
        values = [row[key] for row in table.values() if row.get(key) is not None]
        return max(values) if values else None
    decisions = []
    traffic_share = worst("what_if_share_pct")
    dead_saving = worst("dead_store_saving_ms")
    merge = worst("what_if_merge_passes")
    wp1 = (dead_saving is not None and dead_saving >= 3.0) or \
          (traffic_share is not None and traffic_share >= 25.0 and (dead_saving is None or dead_saving >= 1.0)) or \
          (merge is not None and merge >= 8.0)
    decisions.append(("WP1 frame graph", wp1,
                      f"what-if share {fmt(traffic_share)}% (>= 25%), merge passes {fmt(merge)} (>= 8), "
                      f"measured dead-store saving {fmt(dead_saving)} ms (>= 3 ms; gate: traffic must cost real GPU time)"))
    us_per_draw = worst("us_per_draw")
    draw_ms = worst("draw_ms")
    decisions.append(("WP2 grouped draws", bool(us_per_draw is not None and us_per_draw >= 5.0 and draw_ms and draw_ms >= 3.0),
                      f"real cost per draw {fmt(us_per_draw)} us (>= 5) and {fmt(draw_ms)} ms per frame (>= 3)"))
    cpu60 = worst("cmdproc_max_ms_60fps") or worst("cmdproc_cpu_ms_60fps")
    decisions.append(("WP-C two command threads", bool(cpu60 is not None and cpu60 > 14.0),
                      f"GPU command thread at 60 FPS {fmt(cpu60)} ms per frame (> 14)"))
    pixel_ms = worst("pixel_ms")
    gpu = worst("gpu_ms")
    pixels_dominant = bool(pixel_ms is not None and gpu and pixel_ms >= 0.5 * gpu)
    decisions.append(("WP4 faster SGSR2/SMAA, base choice", pixels_dominant,
                      f"pixel-dependent {fmt(pixel_ms)} of {fmt(gpu)} ms (>= half)"))
    if not any(d[1] for d in decisions):
        decisions.append(("Only P3 and P4 (fluidity, image)", True, "none of the above"))
    return decisions


def fmt(value):
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:.2f}"
    return str(value)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sessions", nargs="+", help="LABEL=TESTER_ZIP")
    parser.add_argument("--from", dest="start", type=float)
    parser.add_argument("--to", dest="end", type=float)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)
    sessions = []
    for item in args.sessions:
        if "=" not in item:
            parser.error(f"{item}: expected LABEL=ZIP")
        label, path = item.split("=", 1)
        sessions.append((label, compare.analyse(path, args.start, args.end)))
    table = build(sessions)
    decisions = decide(table)
    if args.json:
        print(json.dumps({"scenes": table, "decisions": [list(d) for d in decisions]}, indent=2))
        return 0
    keys = ["render", "gpu_ms", "pass_ms", "outside_ms", "gpu_busy_pct", "ms_per_mpix", "pixel_ms", "traffic_mb",
            "what_if_savable_mb", "what_if_share_pct", "what_if_merge_passes", "dead_store_saving_ms",
            "draws", "us_per_draw", "draw_fit_r2", "draw_ms", "cmdproc_cpu_ms", "cmdproc_record_ms", "cmdproc_parse_ms",
            "cmdproc_cpu_ms_60fps", "cmdproc_max_ms_60fps", "frames_over_50ms_per_min"]
    names = list(table)
    width = max(len(k) for k in keys) + 2
    print("Where the milliseconds go (per frame)")
    print(" " * width + "".join(f"{n:>14}" for n in names))
    for key in keys:
        print(f"{key:<{width}}" + "".join(f"{fmt(table[n].get(key)):>14}" for n in names))
    print("\nDecision D8 (Super Roadmap 2, WP-A table):")
    for name, chosen, rule in decisions:
        print(f"  [{'x' if chosen else ' '}] {name}: {rule}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
