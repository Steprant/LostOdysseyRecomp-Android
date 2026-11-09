"""tools/android/bottleneck-map.py (Steprant 2.0 WP-A) on synthetic Tester archives."""
import importlib.util
import io
import tempfile
import unittest
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("bottleneck_map", ROOT / "tools/android/bottleneck-map.py")
bottleneck = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bottleneck)


def archive(path, render, pass_ms, outside_ms=2.0, draws=800, cmdproc=12.0, cmdproc_max=15.0, what_if_share=10.0,
            discarded_mb=0.0, fit=None):
    t, rows = 0, []
    for i in range(600):
        t += 33_333_333
        rows.append(f"{t},33333333,0,0,{i},{draws}\n")
    width, height = render
    log = (f"[    1.000 t1] [info]  start\n"
           f"[   11.000 t2] [info]  gpu passes: frames=300 lists=600 untimed_lists=0 per_frame: passes=55.0 empty=4.0 "
           f"clear_start=20.0 draws={draws}.0 pass_ms={pass_ms:.2f} outside_ms={outside_ms:.2f} load_mb=300.0 store_mb=400.0 "
           f"barriers=90.0 copies=15.0 dispatches=0.0 draws_outside=0.0 present: passes=2.0 ms=1.10 load_mb=0.0 store_mb=12.0 "
           f"top: 1x1 c passes=1.0 empty=0.0 draws=1.0 ms=1.00 load_mb=0.0 store_mb=0.0; "
           f"cmdproc_per_frame: cpu_ms={cmdproc:.2f} record_ms={cmdproc * 0.6:.2f} parse_ms={cmdproc * 0.4:.2f} "
           f"max_cpu_ms={cmdproc_max:.2f} swaps=300\n"
           f"[   11.000 t2] [info]  frame graph what-if: frames=300 per_frame: traffic_mb=700.0 dead_store_mb=60.0 "
           f"merge_passes=3.0 merge_mb=10.0 savable_mb=70.0 savable_share={what_if_share:.1f}% copies_to_pass_end=7.0 "
           f"copies_early=5.0 passes=55.0 diag_discarded_stores=4.0 diag_discarded_mb={discarded_mb:.1f}\n"
           f"[   12.000 t2] [info]  Sgsr2: frame=9 outcome=submitted input={width}x{height} output=2670x1200 quality=0 serial=1 reason=none\n")
    if fit:
        log += (f"[   13.000 t2] [info]  draw classes (session, untimed: list start/end only): frames=9 per_frame: draws={draws} "
                f"ms=10.00 us_per_draw_avg=12.0; fit: us_per_draw={fit[0]:.2f} fixed_ms={fit[1]:.2f} r2=0.97 samples=9;\n")
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("frame_timing.csv", "monotonic_ns,interval_ns,present_wall_ns,present_thread_cpu_ns,renderer_frame,draws\n" + "".join(rows))
        z.writestr("gpu.csv", f"monotonic_ns,completed_batch_total_ns,completed_batch_count\n0,0,0\n{t},{int(t * 0.6)},10\n")
        z.writestr("runtime.log", log)


class BottleneckMapTest(unittest.TestCase):
    def test_table_and_decisions(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            # 576 lines: 1.47 MP, 900: 1.80 MP; 1 ms per MP plus a big fixed part.
            archive(d / "a.zip", (1282, 576), 14.0 + 1.0 * 1.282 * 0.576 - 2.0)
            archive(d / "b.zip", (2003, 900), 14.0 + 1.0 * 2.003 * 0.900 - 2.0)
            archive(d / "c.zip", (2003, 900), 14.0 + 1.0 * 2.003 * 0.900 - 2.0 - 0.8, discarded_mb=60.0)
            archive(d / "e.zip", (2003, 900), 15.0, fit=(6.5, 4.0))
            archive(d / "f.zip", (1282, 576), 12.0, cmdproc=13.0, cmdproc_max=18.5)
            sessions = [(label, bottleneck.compare.analyse(path)) for label, path in (
                ("city-576", d / "a.zip"), ("city-900", d / "b.zip"), ("city-900-deadstores", d / "c.zip"),
                ("city-900-untimed", d / "e.zip"), ("city-60fps-576", d / "f.zip"))]
            table = bottleneck.build(sessions)
            city = table["city"]
            self.assertEqual(city["render"], "2003x900")
            self.assertAlmostEqual(city["ms_per_mpix"], 1.0, delta=0.02)  # pass_ms has two decimals
            self.assertAlmostEqual(city["dead_store_saving_ms"], 0.8, delta=0.011)
            self.assertAlmostEqual(city["us_per_draw"], 6.5)
            self.assertAlmostEqual(city["draw_ms"], 5.2)
            self.assertAlmostEqual(city["cmdproc_max_ms_60fps"], 18.5)
            decisions = {name: chosen for name, chosen, _ in bottleneck.decide(table)}
            self.assertFalse(decisions["WP1 frame graph"], "10% savable and 0.8 ms measured: no frame graph")
            self.assertTrue(decisions["WP2 grouped draws"], "6.5 us per draw, 5.2 ms per frame")
            self.assertTrue(decisions["WP-C two command threads"], "18.5 ms at 60 FPS")
            self.assertFalse(decisions["WP4 faster SGSR2/SMAA, base choice"])
            output = io.StringIO()
            with redirect_stdout(output):
                bottleneck.main([f"city-576={d / 'a.zip'}", f"city-900={d / 'b.zip'}"])
            self.assertIn("Where the milliseconds go", output.getvalue())
            self.assertIn("Decision D8", output.getvalue())


if __name__ == "__main__":
    unittest.main()
