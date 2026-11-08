#!/usr/bin/env python3
"""tools/android/compare-tester.py on a synthetic Tester archive."""
import importlib.util
import io
import tempfile
import zipfile
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("compare_tester", ROOT / "tools/android/compare-tester.py")
compare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compare)


def archive(path, frame_ms, gpu_busy):
    header = "monotonic_ns,interval_ns,present_wall_ns,present_thread_cpu_ns,renderer_frame,draws\n"
    rows, t = [], 0
    for i, ms in enumerate(frame_ms):
        t += int(ms * 1e6)
        rows.append(f"{t},{int(ms * 1e6)},0,0,{i},500\n")
    gpu = "monotonic_ns,completed_batch_total_ns,completed_batch_count\n0,0,0\n" + f"{t},{int(t * gpu_busy)},10\n"
    log = ("[    1.000 t1] [info]  start\n"
           "[   11.000 t2] [info]  gpu passes: frames=300 lists=600 untimed_lists=0 per_frame: passes=50.0 empty=4.0 "
           "clear_start=20.0 draws=500.0 pass_ms=15.00 outside_ms=2.00 load_mb=400.0 store_mb=500.0 barriers=90.0 "
           "copies=15.0 dispatches=0.0 draws_outside=0.0 present: passes=2.0 ms=1.10 load_mb=24.4 store_mb=24.4 "
           "content_mb: color store_read=100.0 store_loaded=200.0 store_cleared=10.0 load_valid=300.0 load_undefined=5.0; "
           "depth store_read=20.0 store_loaded=50.0 store_cleared=60.0 load_valid=80.0 load_undefined=0.0; "
           "stored_then_cleared: 1282x589 cd depth=60.0; stored_then_loaded: 1282x589 cd color=200.0; "
           "pass_ends_per_frame: framebuffer=30.0 barrier_other=12.0; top: 1282x589 cd passes=10.0 empty=1.0 draws=100.0 "
           "ms=2.30 load_mb=31.6 store_mb=43.1; format_draws_per_frame: 8888=100.0 depth_only=200.0 depth_only_keeps_color=1\n")
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("session/frame_timing.csv", header + "".join(rows))
        z.writestr("session/gpu.csv", gpu)
        z.writestr("session/runtime.log", log)


with tempfile.TemporaryDirectory() as tmp:
    a, b = Path(tmp) / "a.zip", Path(tmp) / "b.zip"
    archive(a, [33.3] * 99 + [80.0], 0.7)
    archive(b, [33.3] * 100, 0.5)
    result = compare.analyse(a)
    assert result["frames"] == 100 and result["frames_over_50ms"] == 1, result
    assert abs(result["gpu_busy_pct"] - 70.0) < 0.5, result
    assert result["pass load_mb"] == 400.0 and result["pass store_mb"] == 500.0, result
    assert result["pass depth_store_cleared"] == 60.0 and result["pass color_store_loaded"] == 200.0, result
    assert result["pass end_barrier_other"] == 12.0 and result["pass draws_depth_only"] == 200.0, result
    assert result["pass present_ms"] == 1.1, result
    output = io.StringIO()
    with redirect_stdout(output):
        compare.main([str(a), str(b)])
    text = output.getvalue()
    assert "gpu_busy_pct" in text and "-28.6%" in text, text
print("PASS compare-tester: pacing, GPU occupancy, pass line, content census, pass ends")
