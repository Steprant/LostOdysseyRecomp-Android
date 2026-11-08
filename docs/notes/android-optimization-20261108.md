# Android optimization round, 8 November 2026

Branch `integration/v1.7-optimization-20261108`, from the 1.6 candidate
(`bbb4859`). Scope: make the existing game cheaper to run on Adreno 750
(Xiaomi 14). No new features and no visible change to the image; every step
keeps an A/B switch in the Tester until it is confirmed on the phone.

Starting point (phone logs of 7 November, 1780x800 -> 2670x1200, SGSR2
Quality + SMAA, 30 FPS): in battle and heavy field scenes each frame has
about 59 render passes, 98 barriers and 15 copies, and the passes' load and
store ops move about 420 MB in and 500 MB out per frame. These byte counts
follow the chosen Vulkan load/store ops, not measured bandwidth, but load
being about equal to store says almost every pass reloads and rewrites its
attachments, the expensive case on a tiled GPU.

## 1.1 Content census

Plume now follows, while lists are profiled (Android Tester with the
Advanced or Intensive profile, or `LO_PASS_PROFILE=1`), what became of each
render pass's stored attachment contents:

- `store_read`: the texture next moved to a read layout (sampled, copy or
  resolve source, read-only depth, present);
- `store_loaded`: the next pass on that texture loaded it;
- `store_cleared`: the next pass cleared it at load (or discarded it) with
  nothing having read it in between, so that store was not needed;
- `load_valid` / `load_undefined`: a pass loaded a texture that had or had
  not been written since it was created.

The ten-second `gpu passes` line gains `content_mb:` (MB per frame, colour and
depth apart) and the passes whose stores were most often cleared unread or
loaded by the next pass (`stored_then_cleared:`, `stored_then_loaded:`), by
framebuffer size, colour count and depth.

Caveats: a barrier to a read layout counts as a read even when nothing then
samples the texture, so `store_cleared` is a lower bound. Textures written by
lists recorded before profiling started carry no pending store. Tracking
takes a mutex per attachment and per barrier, only on profiled lists.

Tests: `tools/tests/pass_content_census_test.cpp` records three passes on a
real Vulkan device (lavapipe in CI, `SGSR2 Vulkan readbacks` workflow) and
checks each bucket; `pass_profile_test.cpp` checks the log line.

### Why passes end

The same line gains `pass_ends_per_frame:`: how many passes ended at a
framebuffer change, a barrier on one of the pass's attachments
(`barrier_attachment`), a barrier on some other texture (`barrier_other`) or a
buffer (`barrier_buffer`), a copy, a resolve, a dispatch, external commands, a
query reset or the end of the list. On a tiled GPU every pass that ends and
later resumes on the same target stores and reloads it; `barrier_other` and
`barrier_buffer` breaks are candidates for moving the barrier before the pass
(roadmap 1.4).

## 5.1 ARMv8.2-A build

`tools/android/build-runtime.sh` now builds the Android runtime with
`-march=armv8.2-a` (every arm64 Snapdragon since the 845; Cortex-A55/A75
and later). The point is LSE atomics inline instead of the outlined
runtime-dispatched helpers the arm64 baseline uses for every atomic
operation, which the recompiled guest code (reservations, critical sections)
and the renderer's queues execute constantly. `LO_ANDROID_TARGET_ARCH=armv8-a`
builds for older cores. Nothing changes in behaviour; on the phone the effect
shows as CPU time of the Guest Main and GPU CmdProc threads in `threads.csv`.

## 0.2 Comparing two Tester sessions

`tools/android/compare-tester.py before.zip after.zip [--from S --to S]`
prints, per session and as a percentage change: frame pacing (median, p95,
p99, max, frames over 50 and 100 ms), GPU occupancy from `gpu.csv`, battery
temperature, CPU of the main threads, and the averages of the ten-second
`gpu passes` lines (passes, barriers, copies, load/store MB, content census,
pass ends, draws per Xenos format) plus each session's last `draw classes`
line. The window options keep the same stretch of a repeated route.
Test: `tools/tests/compare_tester_test.py`.

## 6.1 gc render flush: not a target

The `gc render flush` lines come from the Issue #12 crash fix: before the
engine's garbage collection the guest render thread is drained (what later
UE3 versions do themselves). It runs at loads, waits for the guest render
thread, not for the GPU, and removing it brings back a crash. Left as is.

## 1.2a No load of undefined contents

A barrier out of an unknown layout transitions from `VK_IMAGE_LAYOUT_UNDEFINED`,
which discards the image's contents. If such a texture then becomes an
attachment before anything wrote it, its pass used to load it anyway:
bandwidth for garbage. Plume now marks the texture (`undefinedContent`) and
the next pass loads that attachment with `DONT_CARE` (a render pass variant
next to the clear variants; load and store ops do not affect compatibility,
so framebuffers and pipelines are shared). By the Vulkan rules the result is
the same undefined contents, so the image cannot change.

A/B: Tester row "Skip loading undefined attachments" (`discard_undefined`,
`LO_VK_DISCARD_UNDEFINED=0` loads again). In the census these loads leave
`load_undefined`.
