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

## 2.2 State commands per frame

The `gpu passes` line also counts, per frame, the native graphics state
commands Plume records: pipeline binds and the ones skipped as unchanged,
descriptor set binds, push constant writes, vertex and index buffer binds
(`state_per_frame:`). Together with the draw classes they show whether the
~12 µs per depth-only draw is state churn.

## 5.3 Present mode during loads: not a stall

During loads the runtime asks for immediate presentation. Turnip on the
Xiaomi 14 offers only FIFO, so Plume keeps FIFO, the swapchain is not
rebuilt and nothing waits; the "immediate mode unavailable" line is only
the report. Not a cause of the load stutters.

## 0.4 Slow-frame breakdown

The 8 November city session had dozens of 55-97 ms frames every ten seconds
while the GPU spent its usual ~21 ms per frame on them and the present
thread 0.3 ms; the GPU timeline and the big-core frequencies (cores 2-6 at
1.0-2.1 GHz, the prime core held at 902 MHz) say the time is lost on the CPU
side. While a Tester session records, every frame over 50 ms now logs one
`slow frame` line splitting the time since the previous present:

- `cp_idle`: the command processor had nothing to execute (the guest had not
  submitted yet);
- `cp_busy`: executing PM4 and recording, of which `gpu_fence_wait` (blocked on
  a GPU fence) and `wait_reg_mem` (PM4 WAIT_REG_MEM: the guest's command stream
  waiting on a register or memory value);
- `flush`, `pace_sleep`, `present`, and `previous_post` (post-present
  diagnostics of the previous frame).

The first 300 slow frames are logged, then one in ten.

## First census on the phone (8 November, 10:03-10:36)

Heavy field frames at 2403x1080 (SGSR1): ~760 MB stored and ~630 MB loaded
per frame. Where the stores went:

| | colour | depth |
|---|---|---|
| read (sampled, copied, resolved) | 298 MB | 44 MB |
| loaded again by the next pass on the target | 160 MB | 195 MB |
| cleared unread | 75 MB | 1 MB |

`load_undefined` was 0: the 1.2a change saves nothing on this game (the
A/B sessions 10:30 and 10:36 agree within noise). Depth stores nobody reads
are 1 MB, so 1.3 (depth store DONT_CARE) is dropped. The loads that follow a
store on the same target (355 MB) come from passes that end and resume:
26 per frame at a framebuffer change, 22 at a barrier on the pass's own
attachment (EDRAM resolves), 14.5 at a barrier on another texture. The
biggest are the scene colour and depth (2403x1104, ~110 MB each) and the
shadow map (1760x1792 depth, 65-74 MB, 3.7-4.1 passes of 24 MB each for
~25 draws).

## 1.4a Render area

Plume began every pass over the whole framebuffer, so a pass that draws
into one cascade of the shadow map or a sub-rectangle of an EDRAM target
still loaded and stored the whole attachment. A pass now begins over the
rectangle its first draw or clear needs (the scissor the renderer already
sets from the guest's), widened to the render pass granularity. A later
draw or clear outside it ends the pass and the next one covers the union,
so nothing is drawn outside the render area. Passes that start with a
clear, draws with several or unknown scissors, and any caller that does
not say what it needs use the whole framebuffer. An occlusion query's pass
is fitted before the query begins.

A/B: Tester row "Render passes limited to the scissor" (`render_area`,
`LO_VK_RENDER_AREA=0` uses the whole target). Profile byte counts and the
census follow the render area; `pass_ends_per_frame` gains `render_area`.
Lavapipe test: a corner clear fits the pass to the corner, a clear of the
opposite corner restarts it over both (both settings tested).

The `gpu passes` line also names the layout transitions behind the barriers
that end passes (`barrier_ends_per_frame:`, `own:` for the pass's own
attachments, `other:` for other textures), to decide which barriers can move
before their pass.

## 5.2 Performance hints reachable from the Tester

The Android Performance Hint session for the guest main and render threads
(`os::scheduling::PerformanceHints`) existed but nothing ever enabled it:
`SetPerformanceHints` was never called, so even `LO_ADPF=1` had no effect.
The runtime now calls it at startup (off by default), and the Tester row
"CPU performance hints (ADPF)" (`performance_hints`, `LO_ADPF=1`) turns it on,
for an A/B in the city where the 8 November session showed the prime core
held at 902 MHz during the stutter.

### 1.4a on the phone: the shadow map regressed, fixed

The 11:47 session (render area on) showed the shadow map (1760x1792 depth)
going from ~4 to 46 passes a frame and 471 MB of loads: its draws do not
stay inside the first scissor, and growing the area step by step restarted
the pass and reloaded the union each time. A pass that has to grow now
restarts once over the whole framebuffer, and that framebuffer's later
passes begin whole (`renderAreaGrew`): at worst one extra restart per
framebuffer, the gain kept where scissors are stable.

## P6: the city stutter is the present blocking

The 11:47 and 11:56 sessions (city) logged 115 and 160 `slow frame` lines.
In the typical one the command processor is idle for 0 ms and busy for
10-15 ms, the pacing sleep takes 3-20 ms, and then the present takes
40-55 ms (median 44-48 ms): the GPU fence wait is 0.2 ms, so the present
waits on the display. The host frame pacer keeps its own 30 FPS cadence on
the steady clock while FIFO presentation follows the panel; when the two
phases drift apart, a frame sleeps for the host cadence and then blocks
another display period. The other kind of slow frame (40 ms in PM4
WAIT_REG_MEM, 13-15 ms present) is the guest waiting for its vblank after
such a frame.

Now the time spent acquiring the swapchain image is measured
(`present_acquire` in the slow-frame line). When it exceeds 4 ms the display
held the host back, and the pacer counts its next period from the
present's return (`FramePacer::Reanchor`), so the host cadence follows the
display's phase instead of fighting it. Timing only: frames, their content
and the cap are unchanged. `LO_DISPLAY_PACED=0` restores the old behaviour.
ADPF (performance hints) did not change this: same pattern with it on.

## Settings clean-up: A/B rows removed

At Steprant's request the Android diagnostics page keeps only the Tester
rows (mode, profile, sample period, slow-frame detection, thermal,
recording, export). The device A/B rows and `ab_switches.ini` are gone; each
switch keeps the value the phone sessions confirmed, as the built-in
behaviour:

| was | now |
|---|---|
| position-only depth VS, pixel shaders without discard, depth-only draws without PS, viewport jitter, render-target parking, SMAA in SGSR2 Quality, interface promotion instead of a menu size switch | always on (confirmed or no regression) |
| Turnip render mode | driver default |
| skip loading undefined attachments, render area | on |
| ADPF performance hints | off (no effect on the city stutter) |

An `ab_switches.ini` left on a device is no longer read. The development
variables (`LO_VK_KEEP_VS_OUTPUTS`, `LO_ALPHATEST_SPEC`, `LO_KEEP_DEPTH_ONLY_PS`,
`LO_SR_VIEWPORT_JITTER`, `LO_PARK_TARGETS`, `LO_SGSR2_SMAA`,
`LO_SGSR2_NATIVE_MENUS`, `TU_DEBUG`, `LO_VK_DISCARD_UNDEFINED`,
`LO_VK_RENDER_AREA`, `LO_ADPF`, `LO_DISPLAY_PACED`) still override them on
development builds.
