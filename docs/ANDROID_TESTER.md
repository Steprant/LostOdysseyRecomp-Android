# Android tester diagnostics — Steprant 2.1.1

On Android, open **Settings → System → Android diagnostics** with the ordinary
settings controls (LB/RB change category). The separate LB+RB debug-overlay
chord opens the Quick Tools overlay. Switch Tester mode on, then select
**Recording → Start**. Stop the recording and select **Export diagnostics**;
Android's document picker asks where to save the ZIP. Export also works while
recording or after an interrupted process, if a trace was already written.
Tester mode starts off on every process start. Use **Clear diagnostics** to remove stored Tester sessions, frame captures and profiles. The menu shows the space freed.

## What Tester records

Recordings include per-render-pass GPU profiling,
attachment load/store counts, reasons render passes end, and state commands per
frame. About every ten seconds, a sampled frame is timed draw by draw, grouped
by draw type, with the ten most expensive shaders reported. Slow frames over
50 ms include a breakdown of guest idle, GPU wait, presentation stages and
pacing. These sampled timings do not give continuous hardware utilization.

Compare two exported recordings with `tools/android/compare-tester.py`.
Use the same scene, settings, driver and starting temperature. See the
[2.1 release notes](notes/steprant-2.1-release.md) for measured results.
Recording continues until you stop it or an I/O error occurs. Data and storage
are bounded as described below.

## Sampling

2.1 uses a single Intensive profile. It samples process data every 250 ms,
threads at most once per second and CPU frequencies and thermal headroom at
most once every ten seconds. It also records GPU queue timings, render-pass
profiles and slow-frame windows. The recording can affect performance.

### 2.1.1: light measurements always, heavy profiling in bursts

The heavy profiling (per-render-pass GPU timestamps, the GPU command thread's
per-draw timers and the sampled draw-by-draw frames) runs only in the last
12 seconds of every minute of a recording. `frame_timing.csv` marks those
frames in its last column, `profiling`. `summary.json` reports frame figures
both for all frames and `outside_profiling`, and its `power` block (mean
watts, battery temperature and its rate, thermal status, process CPU, the
lowest fastest-core frequency) leaves the bursts out, so power and CPU are
the game's rather than the Tester's. `LO_TESTER_CONTINUOUS_PROFILE=1` keeps
the profiling on for the whole recording.

New measurements, for the whole recording:

- `threads.csv`: per thread, time on a CPU, time runnable but waiting for a
  CPU (`sched_runqueue_wait_ns`, `runqueue_wait_ms_per_s`) and times
  scheduled in (`timeslices_per_s`, roughly its wakeups), from
  `/proc/self/task/*/schedstat`.
- `memory.csv`: PSS by category (graphics, native heap, Java heap, code,
  stack, other, system).
- `system.csv`, every ten seconds: display refresh rate (current and mode),
  screen brightness and its mode, power-saving mode, Android game mode and
  battery level.
- `runtime.log`: `io reads:` every ten seconds (count, KiB, transfer and
  lock-wait milliseconds, slowest read, reads over 20 ms); `guest samples:
  kind=periodic` once a minute (where the running guest threads are, sampled
  every 100 ms); and with each long audio callback, `stall samples:
  kind=audio_others`, every guest thread and the kernel wait it is in.

Slow-interval windows (`events/`) are again saved for slow intervals. Since
2.0 they were triggered by the line's last column, which had become
`game_paused`.

Per-core scaling frequencies are attempted from public, readable sysfs files;
Android may deny access. Missing values are blank or `null`. Battery temperature
is not GPU or CPU temperature. Headroom is Android's estimate, not a measured
temperature. CPU percentages are process/thread CPU time divided by elapsed
time for one logical core, so process CPU can exceed 100%.

`frame_timing.csv` uses monotonic nanoseconds. `interval_ns` is the interval
between accepted present calls, not a display scanout timestamp. The present
wall time and thread CPU time measure the host present call. Draw count is the
renderer frame most recently submitted. GPU columns are cumulative completed
timestamped queue intervals and completed-batch counts; they are not GPU load
percentage, isolated shader duration or an exact per-frame GPU time.
`summary.json` includes an estimated 1% low accepted-present rate, calculated
from the slowest one percent of retained intervals. This is not displayed FPS;
pauses and the finite rolling window affect it. The XMA
columns are cumulative decoder send/receive attempts, failures and elapsed
wall time since recording started, not thread CPU time. Movie fields are guest
frame update/PTS observations, not isolated video decoder time or A/V sync.
Audio's queue-empty counter is not a measured hardware underrun count. Frames
that were never submitted/presented cannot be inferred from these fields.
Pipeline counts and wall times cover inline `CreatePipeline` attempts only;
background pipeline jobs and cache hits are excluded. The two shader hashes
belong to the latest such attempt, not to every shader used by the frame.

The ZIP includes device/build metadata, selected non-identifying settings,
frame, CPU, thread, frequency, memory and thermal CSVs, summary and event information,
and up to 2 MiB from the newest runtime log. Frame captures and profiles in the
ZIP come from the current run of the app. GPU hardware utilization/frequency,
CPU/GPU sensor temperatures, A/V synchronization and display latency are not
implemented. Zero counts mean no recorded observations, not necessarily that
the underlying subsystem was available. The export may include no runtime log
if none exists. `memory.csv` samples process PSS (KiB) and native-heap
allocation (bytes) every ten seconds. These do not measure GPU VRAM. Recording
stops with an error if app-private usable space falls below 32 MiB, checked at
start and every ten seconds. The log can contain file paths despite limited path redaction:
review the archive before sharing it. Nothing is uploaded automatically.

The slow-interval detector uses 1.3 times the configured frame interval with a
25 ms floor; it identifies intervals, not their causes or dropped frames. Each
of up to three windows saves up to 30 seconds before and 10 seconds after a
trigger, subject to data availability, process termination and a 4 MiB window
cap. The background writer keeps up to four 2 MiB segments per CSV stream and
at most three private sessions. A bounded native queue drops samples instead of
blocking presentation, with its dropped count in `session.json`. Recent context
is also bounded to 16,384 samples. Files left by an interrupted process remain
exportable; a session marked `recording: true` may have ended abruptly.

## Device coverage and further validation

The 2.1 release notes include Tester measurements from one Xiaomi 14.
The implementation checks recorded below refer to the original 1.5 checkpoint.

The host settings fixture, native recorder tests and JVM trace tests do not run
the game, SDL lifecycle, an Android driver or a physical controller. The
complete ARM64 runtime and Steprant 1.5 APK built with authorized local inputs;
the APK was signed and statically checked. On Xiaomi 14, verify controller
navigation, start/stop/export, background/resume, a CGI, the intro boss and
recordings longer than two minutes. Compare repeated identical scenes with
the same build, graphics settings and temperature with diagnostic mode off and on.
Record frame-time percentiles, sustained temperature and power.
Those initial checks did not include a device run or overhead measurement;
they are separate from the later 1.8 sessions.

## Initial implementation checks — Steprant 1.5

- Standalone settings suite: 15 CTest cases passed, including the Android
  settings input fixture and the bounded native recorder.
- Production Android unit suite: 82 JVM tests passed, including trace export;
  lint completed. The standalone Java/JNI integration check passed.
- NDK 28.2, ARM64/API 26: the full generated-PPC runtime linked. The signed
  version 1.5 APK passed ZIP, ARM64 library, manifest, signature and alignment
  checks. These are build checks, not gameplay validation.
