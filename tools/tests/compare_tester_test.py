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
           "ms=2.30 load_mb=31.6 store_mb=43.1; format_draws_per_frame: 8888=100.0 depth_only=200.0 depth_only_keeps_color=1"
           " cmdproc_per_frame: cpu_ms=12.50 record_ms=8.00 parse_ms=4.50 max_cpu_ms=19.00 swaps=300\n"
           "[   11.000 t2] [info]  frame graph what-if: frames=300 per_frame: traffic_mb=900.0 dead_store_mb=70.0 merge_passes=6.0 "
           "merge_mb=120.0 savable_mb=190.0 savable_share=21.1% copies_to_pass_end=7.0 copies_early=5.0 passes=50.0 "
           "diag_discarded_stores=0.0 diag_discarded_mb=0.0\n"
           "[   12.000 t2] [info]  Sgsr2: frame=900 outcome=submitted input=2003x900 output=2670x1200 quality=0 serial=5 reason=none\n"
           "[   12.000 t2] [info]  SGSR2: smaa=on format=rgba8 requested=yes encoding=sdr filter=standard input=2003x900 output=2670x1200\n"
           "[   13.000 t2] [info]  draw classes (session, untimed: list start/end only): frames=4 per_frame: draws=800 ms=12.60 "
           "us_per_draw_avg=15.75; fit: us_per_draw=12.00 fixed_ms=3.00 r2=0.95 samples=4; depth_only=600\n")
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
    assert result["pass cmdproc_cpu_ms"] == 12.5 and result["pass cmdproc_parse_ms"] == 4.5, result
    assert result["what-if dead_store_mb"] == 70.0 and result["what-if merge_passes"] == 6.0, result
    assert result["sgsr2_input"] == "2003x900" and abs(result["render_mpix"] - 1.8027) < 1e-3, result
    assert result["sgsr2_smaa"] == "on (rgba8)", result
    assert result["draw_fit_us_per_draw"] == 12.0 and result["draw_fit_fixed_ms"] == 3.0, result
    output = io.StringIO()
    with redirect_stdout(output):
        compare.main([str(a), str(b)])
    text = output.getvalue()
    assert "gpu_busy_pct" in text and "-28.6%" in text, text
print("PASS compare-tester: pacing, GPU occupancy, pass line, content census, pass ends, what-if, cmdproc, SGSR2 base, draw fit")
