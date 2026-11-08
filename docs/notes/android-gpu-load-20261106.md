# Android GPU load: measurements, compact targets, ADPF, display rate — 2026-11-06

Branch `perf/android-gpu-load-20261106` (base `integration/v1.1.0-steprant-all`
at `077ac142`), merged with the audio and SGSR1 test branches into
`integration/v1.6-candidate-20261106`.

## What the 2026-11-04 traces say

| Xiaomi 14, 1.5 build | value |
|---|---|
| Renderer GPU time per frame, 30 FPS, internal 1080 | 21–26 ms (62–79% busy) |
| Same, 60 FPS attempts | 15–18 ms per frame, 92–98% busy |
| Frames slower than 43 ms (33 min session) | 624, median 68.8 ms (one missed vsync) |
| GPU time of those frames | median 50 ms, p90 74 ms; median 1084 draws |
| Slow frames with inline pipeline creation / blocked present | 2 / 0 |
| Android thermal status | 0 throughout |

30 FPS drops are GPU-bound heavy scenes, not CPU, shader compilation or
thermal throttling. Menus with ~120 draws still cost ~25 ms: the load is
per-pixel and fixed per-frame work, not geometry.

Source inspection: every colour render target was RGBA16F (8 bytes/pixel),
including the console's 8-bit and 10-bit fixed-point formats; depth is
D32_S8. Shadow resolution 2x allocates 2560x1440 shadow maps (console:
1280x720). Internal "1080" on the 2670x1200 panel renders 2403x1080
(+25% over 1920x1080). BC textures are native; render targets keep UBWC
(no storage/mutable flags).

## Changes

1. **Pass profile (diagnostics).** plume brackets every Vulkan render pass
   with timestamps outside the pass and counts draws, barriers, copies,
   dispatches and the bytes its load/store ops move. While the Tester records
   with Advanced/Intensive (or `LO_PASS_PROFILE=1`) the log gets a
   `gpu passes:` line every 10 s: per-frame passes, empty passes, pass and
   outside-pass GPU ms, load/store MB, the eight costliest targets, and
   `format_draws_per_frame` (guest draws per Xenos colour format). Lists are
   read when next begun, after their fence: no GPU wait.
2. **Compact render targets** (Tester → *Compact render targets*, saved as
   `compact_render_targets`, `LO_COMPACT_RT=0/1`; restart). With the HDR
   scene off, Xenos 8_8_8_8(_GAMMA) targets become RGBA8 and fixed
   2_10_10_10 become A2B10G10R10: the bits the console stores (the epilogue
   already clamps both classes to [0, 1]), half the bytes per pixel. 7e3,
   16-bit and 32-bit classes and depth are unchanged. Resolves from a compact
   8888 target to an 8888 texture become plain copies. First run with the
   option compiles the new pipeline variants on demand.
3. **ADPF power hints** (Tester → *Power hints (ADPF)*, `performance_hints`,
   `LO_ADPF=0/1`, `LO_ADPF_TARGET_PERCENT`; live). One APerformanceHint
   session for the guest main and render threads reports the busier thread's
   CPU time per frame against 50% of the frame period, with prefer power
   efficiency on API 35+. A `performance hints:` log line every 10 s.
4. **Match display refresh** (Tester, `display_rate_hint`, default on; live).
   `Surface.setFrameRate(fps, FIXED_SOURCE)` on the phone surface for the
   native target, so a 120 Hz panel can run 60/30 Hz. TV output keeps its
   60 Hz request.
5. **900p render resolution**: 1600x900 at 16:9, 2003x900 on the phone panel.
6. **SGSR1 kept on swapchain format change** (it fell back to the ordinary
   filter until restart).

All options except (4) and (6) are off by default until measured.

## Verification performed

- Lavapipe: profiled presentation pass (timestamps, byte counts, draw count);
  7/7 presentation readbacks; SGSR1 reference comparison (other branch).
- Host: pass-profile summary, ADPF policy, render resolution (14,055 checks),
  settings menu suite including every new Tester row, config round-trip.
- Production `renderer.cpp` and `video.cpp` compile in the asset-free boundary,
  also with the Android branches (`-D__ANDROID__`); Java/JNI contract test.
- CI "Steprant Android integration" (android, host, vulkan jobs) on the branch.

Not performed: ARM64 APK build (private input), device run, measured FPS,
GPU time, temperature or battery.

## Device protocol (Xiaomi 14)

Same save and scene, preferably a heavy one (the fire boss or a crowded
battle), 30 FPS, warm shader cache, Tester *Advanced*, 5 minutes each:

| Run | Render | Shadow | Compact targets | ADPF |
|---|---|---|---|---|
| A (baseline) | 1080 | 2x | off | off |
| B | 1080 | 2x | **on** | off |
| C | 1080 | **1x** | on | off |
| D | **900** | 1x | on | off |
| E | best of B–D | | on | **on** |

Compare per run: `gpu passes:` pass_ms/outside_ms/load+store MB, slow frames
and 1% low from the Tester, `performance hints:` work, battery temperature.
Restart after changing compact targets. Display refresh matching is on in
all runs; turn it off once to compare panel power if wanted.

## Device sessions of 2026-11-06 evening (1.6 test build `a81a116a`)

Three Tester recordings (22:16, 22:25, 22:35; the last two are one run),
Turnip (Mesa 26.0.0-devel) on the Xiaomi 14, 30 FPS, FXAA, SGSR1, shadows 2x.

- Steady scenes hold 30.0 FPS (p95 34 ms). GPU busy 21–25 ms per frame
  (65–75% at 30 FPS). Slow intervals sit in scene changes with 4,000+ draw
  frames; inline pipeline compiles are near zero and the thermal status
  stays 0 (battery up to 44.8 °C).
- Main scene per frame: about 110 render passes and 1,800–1,900 draws;
  ~25 colour+depth passes and ~25 depth-only passes on the same target, in
  alternation; 73–80 barriers, 10–13 copies. Colour formats by draws:
  7e3 ≈ 450 + 7e3_as_16 ≈ 285 against 8888 ≈ 95, so compact targets change
  little here.
- Resolution sensitivity is low. A depth-only draw costs ≈ 3.7 µs at
  2003×920 and at 2403×1104, a scene draw ≈ 8.0 µs and ≈ 8.9 µs: GPU time
  follows draws (vertex and per-draw shader work), not pixels.
- Presentation: SGSR1 runs as a compute dispatch at 1080 → 2670×1200 and as
  a graphics pass at 900p, about 2–2.5 ms either way; FXAA at 2403×1080
  about 1.3 ms.
- Audio: no queue starvation during play in either XMA lock mode, no clock
  resets, the audio thread never waits for the XMA decoder.

### Consequences in the build

1. **Uniform-buffer shader constants** (Tester *Shader constants in UBO*,
   restart). Pointer-form SPIR-V reads every `XeConst(n)`, the sampler
   indices and the vertex-fetch offsets with `vk::RawBufferLoad` from device
   addresses — on Adreno, a global memory load each, which fits the
   per-draw, resolution-independent cost above. The UBO form (the Switch
   pack's) reads the same `cbuffer`s from set 5; Turnip can upload
   statically indexed ranges into the constant registers and serves the
   rest from the constant cache. No published pack has this form: the first
   start runs the full shader preparation on the phone (translation + DXC,
   progress screen, several minutes) and later starts load the local
   startup bundle. Turnip keeps its dynamic-offset descriptor set on Adreno
   7xx+ (`gpu::adreno::Generation`); otherwise the renderer keeps the
   pointer form.
2. **One pass for depth-only draws** (live): keeps the scene colour target
   bound for depth-only draws, removing ~45 pass changes per frame. An
   older note measured Turnip sysmem ~9% slower; to be measured again with
   the pass profile (`depth_only_keeps_color=` in the format line).
3. **Turnip render mode** (restart): Automatic / Sysmem / GMEM.
4. The title screen's full-screen mesh layer is no longer drawn
   (`LO_SCREEN_PATTERN=1` restores it).

### Next device protocol

Same save and spot, 30 FPS, Tester *Intensive*, 3 minutes standing still +
2 minutes moving, for each of:

| Run | Shader constants | Depth-only pass | Turnip mode |
|---|---|---|---|
| A | pointer | off | Automatic |
| B | pointer | **on** | Automatic |
| C | **UBO** (after its first preparation) | off | Automatic |
| D | UBO | best of A/B | Automatic |
| E | best | best | **Sysmem**, then **GMEM** |

Compare `gpu passes:` pass_ms + outside_ms and per-target ms, the Tester
GPU busy and 1% low.

## A/B results of 2026-11-07 and what the build keeps

Six recordings (08:43–10:13), Xiaomi 14, Turnip, 1080 internal, 30 FPS
unless noted. Medians over gameplay windows (> 1,200 draws per frame):

| Run | Constants | Depth-only pass | Turnip | Passes | Pass ms | GPU ms/frame |
|---|---|---|---|---:|---:|---:|
| 08:43 | pointer | separate | auto | 104 | 19.4 | 24.2 |
| 09:12 | UBO | separate | auto | 102 | 19.3 | 24.2 (13% more draws) |
| 09:33 | UBO | kept | GMEM | 67 | 26.6 | 31.4 |
| 10:03 | UBO | kept | sysmem | 64 | 17.9 | 23.1 |
| 10:13 | UBO | kept | auto | 74 | 17.8 | 22.1 |

Per scene draw: pointer 8.6 µs, UBO 7.9 µs. In the 10:13 run 720p
(1602×736) costs ~12.7 ms against ~22 ms at 1080p in comparable scenes.

Kept: depth-only draws in the scene pass (Android default), compact 8-bit
targets (always on Android), display refresh matching, 64 ms audio buffer.
Removed: UBO constants (~7% for a long first-start compile and a ~900 MB
bundle), Turnip render mode (GMEM much slower), ADPF hints (no effect on a
GPU-bound frame), per-voice XMA locks (audio clean without them).

The remaining large lever is rendering fewer pixels: SGSR1 already
reconstructs 720p/900p to the 2670×1200 panel, but it is spatial only. A
temporal upscaler (Qualcomm SGSR2, BSD-3) fed with the renderer's jitter,
depth and motion vectors would let 720p look close to native.

## SGSR2 (experimental), 2026-11-07

Anti-aliasing → **SGSR2** with an *SGSR2 quality* row (Quality 1.5×,
Balanced 1.7×, Performance 2.0×, Native AA). It runs on the renderer's
temporal SR route, like FSR: jittered scene colour, reversed R32 depth,
backward render-pixel motion and motion invalidity are copied at the scene
boundary, and Qualcomm's 2-pass compute SGSR2 (BSD-3, adapted to HLSL in
`gpu/shader/sgsr2_source.h`) writes an RGBA16F output that the RGB composite
returns to the game's target before the UI.

Adaptations to the reference: reversed Z; the renderer's motion vectors
instead of a velocity texture or a clip-to-previous-clip matrix; motion
invalidity forces the depth clip; a reset frame never reads the previous
history (it can be uninitialised or a discarded frame's, and NaN × 0 is
NaN); SDR input skips the reference's `rgb / (max + 1)` tone map (HDR keeps
it, with an exact inverse); the output is clipped to [0, 1] for SDR.

Motion is camera-only by default (`LO_SGSR2_OBJECT_MV=1` adds the object
replay) because the replay's vertex work would cost more than SGSR2 saves;
the colour clamp handles moving characters. `LO_SGSR2_JITTER_SIGN=-1` flips
the jitter handed to SGSR2 to check the convention on a device.

`tools/tests/sgsr2` runs the production controller and shaders on lavapipe
(CI: *SGSR2 Vulkan readbacks*), 64×36 → 128×72:

| Check | Result |
|---|---|
| flat colour after reset / accumulation | worst error 0.0015 |
| jittered static scene, RMSE R/G/B vs ideal 128×72 | 0.006 / 0.067 / 0.026 (bilinear 0.000 / 0.169 / 0.057) |
| same with the jitter sign flipped | 0.023 / 0.291 / 0.032 |
| pan with backward vectors vs reversed vectors | 0.009 / 0.224 / 0.035 vs 0.124 / 0.263 / 0.161 |
| HDR flat colour (2.0, 0.5, 0.25) | 0.5% |
| slots, resize, discard and shutdown lifecycle | pass |

The green channel is a sinusoid above the render Nyquist limit: SGSR2
recovers it on a still camera and, as expected, not during a pan. Not yet
measured on the Adreno 750.

### First device session (2026-11-07) and changes

Xiaomi 14, 2670×1200 output, GPU busy at 30 FPS: native 81%, Quality
(1780×800) 66–74%, Balanced (1571×706) 60–66%, Performance (1335×600) 63%.
Findings and changes:

- **Flat, soft textures**: scene textures were sampled at the mip of the
  render resolution. Scene-texture samplers now take `log2(render/output)`
  LOD bias in quarter steps (−0.5 at 1.5×, −1.0 at 2×) while a temporal SR
  consumer is planned (`LO_SR_MIP_BIAS=0` off, `=<lod>` override). Only
  samplers `sampling::Eligible` already selects for anisotropy (depth-tested
  scene geometry); UI and render-target fetches are untouched.
- **Soft output**: a contrast-adaptive sharpen (one cross lobe limited by
  the neighbourhood range, RCAS-style, written for this port) runs after the
  accumulation, strength 0.5 (`LO_SGSR2_SHARPNESS=0..1`).
- **One-frame pops**: 61 single-frame fallbacks (`UnknownColorEncoding`)
  came from a lost SDR write-ordinal proof after a tile transfer into the
  scene; each reset the history and showed one SGSR1 frame. SGSR2 now
  accepts the RGBA8 scene whenever view, frame, extent and swizzle match.
- **Menus**: frames without an eligible 3D scene (menus, transitions;
  `NoEligibleScene`) are not upscaled by SGSR2: they render at the SGSR2
  input size and SGSR1 enlarges them. Keeping SGSR1 on is therefore useful.
  Rendering those frames at the output size needs a plan switch and is not
  done yet.
- **576p**: new render resolution row. With SGSR2 it renders 576 lines
  (1282×576 on this panel, about 2.1×) whatever the quality row says; other
  rows keep the SGSR2 quality ratio.

### Second round (2026-11-07): menus, NPC shadows, aliasing

- **Menus at the output size**: after 20 frames without a 3D scene for SGSR2
  (`NoEligibleScene`: menus, pause, transitions) the CPU plan asks for
  SGSR2's native-AA size, the output size, so the 2D screens are no longer
  enlarged from the SGSR2 input. The first frame SGSR2 submits again (a 3D
  scene is back) returns to the chosen quality; a 90-frame cooldown keeps a
  scene that flickers between eligible and not from reallocating every few
  frames. `gpu/sgsr2_idle_policy.h`, covered by the frame plan test;
  `LO_SGSR2_NATIVE_MENUS=0` turns it off.
- **NPC shadows**: the session ran Dynamic shadows = Low, which keeps the
  shadows of only eight casters, the largest on screen; in Uhra's squares a
  dozen NPCs traded those places and the smaller ones' shadows vanished and
  came back. Low keeps sixteen casters on Android. On keeps them all.
- **Aliasing**: SGSR2 now also rejects history where the previous frame's
  depth shows a nearer surface everywhere around the reprojected position
  (a disocclusion test the 2-pass reference lacks; it helps moving
  characters, which have no motion vectors of their own). On the fixture's
  1.3-pixel railings the reference's depth-edge clip resolved them best, so
  it stays (`LO_SGSR2_EDGE_CLIP` can weight it). Sharpening drops to 0.3,
  since sharpened edges show stair steps more. Every 300 frames the log
  prints `SGSR2 jitter:` with the jittered, unjittered and unknown-camera
  scene draws: unjittered draws are never anti-aliased by SGSR2.

### Third round (2026-11-07): blocks in shadows, transition ghosting

Device session with Dynamic shadows On showed a blocky, brick-like pattern
over shadowed walls and floors at the SGSR2 input sizes (not at the output
size). The `SGSR2 jitter:` lines show every known scene draw jittered but
one or two draws a frame in the scene viewport with no known camera
constant, so unjittered: the class of bug fixed for cutscene shadow volumes
in PR #197 (a depth-testing pass half a pixel off the jittered depth flips
its stencil or shadow test per triangle or texel). Under SR those
depth-testing unknown draws now take the frame jitter as a viewport
translation, which moves clip space exactly as the projection offset does
(`LO_SR_VIEWPORT_JITTER=0` off); each such pair is logged once, and the
periodic line counts them (`viewport_jittered=`). Resolved render targets
(shadow maps, scene copies) are also excluded from anisotropic filtering and
the SR mip bias.

Transitions: the menu policy returned to the quality size on the first 3D
frame, often in the middle of a fade-in. It now enters after 30 scene-less
frames and returns after 20 consecutive upscaled frames.

## Early depth rejection restored (2026-11-07)

The profiles say the main scene pass is pixel-bound, not draw-bound: from
1780×800 to 1282×576 (0.52× the pixels) its time per draw falls 1.7×
(about 3.2 ms per megapixel, 1.8 µs per draw). 852 of its ~1,530 draws a
frame are depth-only: the game lays depth down first, so the colour pass
should shade only visible pixels. It did not. Every translated pixel shader
carries the Xenos alpha test as a dynamic branch ending in `discard`, also
when the game has the test off; a shader that can discard makes Turnip (and
tile GPUs in general) give up early Z for the draw and stop writing LRZ, so
every covered fragment was shaded before its depth test.

The alpha test is now pipeline state on native Vulkan
(`pipeline_cache::kFlagAlphaTest` in the key). Plume gives each fragment
module whose only `OpKill` is that test (exactly one `OpKill`, no
terminate/demote) a second module with the instruction turned into
`OpUnreachable` (one word each, patched in place), and pipelines whose draws
have the alpha test off use it (`RenderGraphicsPipelineDesc::
pixelShaderWithoutKill`), on the monolithic and the fast-link path alike.
The translated HLSL and the shader packs are unchanged (an earlier
specialization-constant version invalidated the 235 MB pack and was
replaced). Shaders with Xenos kill instructions have two `OpKill`s and keep
their module. Old pipeline recipes (flags 0) prewarm the kill-free variant.
`LO_ALPHATEST_SPEC=0` keeps the discard everywhere. `tools/tests/
kill_free_module_test.cpp` checks module selection and identical output on
lavapipe.

### Depth-only draws without their no-op pixel shader

852 draws a frame write no colour (depth pre-pass, shadow maps, stencil
volumes, occlusion boxes). Their pipelines kept the game's pixel shader,
which then ran for every fragment that passed the depth test with no effect
at all (no colour write, and with the alpha test off its only possible
effect, the discard, cannot happen). On native Vulkan such pipelines now
have no fragment shader when the shader neither exports depth nor has a
second kill (`PixelShaderIsNoOp`), so the hardware takes its depth-only
path. Occlusion-query draws with such a shader are counted by the fragment
counter instead of a host query, like draws without one.
`LO_KEEP_DEPTH_ONLY_PS=1` keeps the shaders.

### Depth-only vertex work, menu switch cost, softer Quality (2026-11-07 evening)

- **Position-only vertex modules.** Without a fragment stage a driver links
  no consumer for the vertex shader's varyings and keeps computing them
  (Turnip trims only its binning variant, which sysmem rendering does not
  use). Plume now gives every vertex module with non-built-in outputs a
  copy whose stores to those outputs are dropped (`OpStore` removed; any
  load, copy or call argument of such an output leaves the module without
  the copy), and pipelines without pixel and geometry shaders use it, on
  the monolithic and the fast-link path. The driver then removes the vertex
  fetches, constant reads and ALU that fed only the varyings of the ~650–850
  depth-only draws a frame. The shader pack is unchanged.
  `LO_VK_KEEP_VS_OUTPUTS=1` turns it off; `kill_free_module_test.cpp`
  checks the variant and identical depth on lavapipe; the stripped module
  of a test shader with arrays, clip distance and point size passes
  `spirv-val`.
- **Menu switch.** The idle policy enters the output size after 8
  scene-less frames (was 30, a full second of soft text at 30 FPS) and
  returns after 12 upscaled frames (was 20, rendered at the output size).
  The switch itself no longer rebuilds everything: the renderer keeps the
  previous size's render targets (`parkedTargets`, ≤ 512 MB, taken back for
  the same key and host size under a new allocation serial;
  `LO_PARK_TARGETS=0` frees them) and SGSR2 keeps its compute pipelines
  across `ReleaseFeatureAfterGpuDrain`, making only its images. The 17:16
  trace had a 40–75 ms frame at every return to the quality size.
- **Softer Quality.** SGSR2's post-sharpen now follows the Sharpness row
  (shown for SGSR2, Off by default; it was a fixed 30%), and Quality widens
  the upsampling kernel (`kernelScale` 0.8 on the Lanczos bias; Balanced and
  Performance keep the reference). `LO_SGSR2_SHARPNESS` and
  `LO_SGSR2_KERNEL_SCALE` override both.
- **Diagnostic.** Each unjittered draw in the jittered scene is logged once
  (`renderer: unjittered scene draw`): depth state, raster, sampled
  textures with their resolved host size and scale, `scene_depth` for the
  full scene depth and `stale` for a surface not written this frame, and
  pixel constants c0–c5. It identifies the screen-space pass behind the
  blocky shadows so its ray can be compensated like
  `IsShadowProjectionPair`.

## G1: blocks in shadowed areas under jitter (2026-11-07, night)

- **Evidence.** 18:45 session: SGSR2 Native AA (jitter, 1:1) shows a regular
  grid of blocks on shadowed floors and walls; SGSR1 720p (no jitter, 1.67×)
  does not. The blocks therefore follow the jitter, and grow as the input
  resolution drops.
- **Cause.** VS `99c2b4b0960a9ccd` has camera slot 0 for every pixel shader
  (`PositionVPSlot`), so its clip position is always jittered. The pixel
  shader samples the jittered scene depth at that position (correct) and
  rebuilds the receiver as `linearDepth * (ndcX*c2 + ndcY*c3 + c4) + c5`.
  Only the twelve reviewed shaders of `IsShadowProjectionPair` had the
  jitter removed from that ray (`c4 -= ndcX*c2 + ndcY*c3`); any other
  pixel shader under 99c2 rebuilt a receiver offset by the jitter and
  compared it with the shadow map off by whole texels: shadow acne in
  blocks of shadow-map texels.
- **Change.** The translator now records, as metadata that never alters
  the HLSL (so the shader pack and the cache keys are untouched), the
  components of c0..c15 each pixel shader reads by direct reference
  (`TranslatedShader::lowConstantLanesRead`). A portable-pack record has
  no such field, so the renderer translates a 99c2 pixel shader once, on
  its first draw, to read it. `temporal::ClassifyShadowProjection` then
  gives each 99c2 draw a contract:
  - *listed* (the twelve reviewed shaders): corrected as before, on the
    lanes they read (all four if analysis finds none);
  - *verified*: an unlisted shader that reads c2, c3 and c4 in the same
    lanes, also reads c5 and uses no relative constants: corrected exactly
    like the listed ones, only on those lanes, and still only when it
    samples the full scene depth of this frame;
  - *unverified*: anything else stays unjittered (rejection
    `shadow_unverified`). This is the safe side: an unjittered projection
    rebuilds a consistent receiver and only its depth lookup moves by the
    sub-pixel jitter, while an uncorrected jittered one rebuilds a wrong
    receiver on every pixel.
  A VS 99c2 draw without a pixel shader has no ray and keeps its jitter.
- **NaN check.** The finite-constant check on the pixel side now covers
  only the components that enter the correction (c2, c3 and c4 on the
  corrected lanes). A NaN in a lane the shader never reads no longer
  rejects the draw.
- **Diagnostics.** Each pixel shader seen with VS 99c2 is logged once per
  outcome: `renderer: shadow projection ps=… listed= contract= applied=
  compensated= lanes= rejection= reads c2= c3= c4= c5= known= relative=
  sampled_depth= … c0..c5`. The periodic `SGSR2 jitter:` line gains
  `shadow_compensated=` and `shadow_uncompensated=` (draws of that frame).
- **Verification.** Host only: `temporal_jitter_test` (a verified unlisted
  shader matches the listed correction bit for bit; unread NaN/inf lanes
  are skipped and left unchanged; a NaN in a read lane still rejects;
  missing c5, mismatched lanes, relative constants or no analysis stay
  unjittered; no-PS keeps jitter) and `position_flow_test` (lane mask of a
  synthetic pixel shader, relative reads flagged and not listed); the real
  `renderer.cpp` compiles in the motion-replay boundary. Not yet seen on the
  phone.
- **Device check.** Uhra, Residential Area, the stairs and walls on the
  right: no blocks at 576p, 1780×800 (900p + Quality) and Native AA; the
  zones of the twelve listed shaders unchanged. The log should list every
  99c2 shader of the area with its contract; any `unverified` one is a
  candidate for review.

## M2: render-target parking keeps both sizes (2026-11-07, night)

- **Evidence.** In the 17:16–18:45 traces the `resize parked … reused …`
  lines show 2–5 reused targets out of 14–24 at each return.
- **Cause.** The park held one target per key. At the output size,
  `GetRenderTarget` found the parked quality-size target of the same key,
  saw the wrong size and retired it; at the next switch the output-size
  target took its place. Only keys never drawn at the other size survived.
- **Change.** `render_target_parking.h`: up to two allocations per key, one
  per size. Parking a target replaces the parked one of the same size, else
  takes a free slot, else the older slot. A lookup takes back only a fitting
  allocation and leaves the other size parked for the next switch. The
  512 MB budget now evicts the oldest generations first instead of freeing
  everything. The log reads `resize parked N render targets (M MB, E evicted
  over the budget); the last size reused R of T new targets`.
- **Verification.** Host: `render_target_parking_test` (quality → output →
  quality → output reuses every target from the second switch on; a third
  size drops the oldest; budget eviction order); the real `renderer.cpp`
  compiles in the motion-replay boundary. Device: after M1 the switch should
  no longer happen in menus; when it does (SGSR2 quality change, the Tester
  fallback switch), `reused R of T` should be close to T.

## M1: menus and transitions keep one render size (2026-11-07, night)

- **Evidence.** The 7/11 traces log an `internal resolution` change at
  nearly every menu, loading screen and fade: the SGSR2 idle policy moved
  the plan to 2670×1200 after 8 scene-less frames and back after 12 3D ones.
  Each change cost a 40–300 ms frame (drain, allocations, M2), and menus
  rendered entirely at 2670×1200 cost about 20 ms of GPU.
- **Structural change.** The render size no longer changes. In a frame
  without a 3D scene the renderer promotes only the *final* target — the
  EDRAM target the frontbuffer is resolved from, learned at each present
  (`renderer: frontbuffer target …`) — to the output size, at the first
  draw on it that samples ordinary textures (fonts, windows, icons). What
  the target held (the background: the paused scene and its blur are
  resolved surfaces, and untextured fills, drawn at the render size) is
  enlarged once with the bilinear promotion resample; the interface is then
  rasterised at the output size, and the frontbuffer resolve and the
  presentation take it 1:1 (SGSR1 stays inactive on those frames). It reuses
  the scene-copy promotion of the SGSR2 path without the upscaler
  (`sceneCopyPromotion.idleUi`), so the next frame gives the target back the
  same way (lazy resample). Every other target, including the background
  passes, stays at the render size.
- **Conditions** (`idle_ui_promotion.h`): SGSR2 plan rendering below the
  output, previous presented frame without a 3D scene, no depth-writing
  draw yet in this frame, no depth on the draw, one promotion per frame.
  A promotion undone in its own frame (a depth draw on the final target)
  pauses the mechanism for 60 frames and logs it.
- **Fallback.** The old switch (`Sgsr2IdlePolicy`) now runs only with
  `LO_SGSR2_NATIVE_MENUS=1`, exposed on the Android diagnostics page
  ("Menus: render-size switch (old)"); when on, the promotion is off. Its
  entry threshold is 45 frames (~1.5 s at 30 FPS) instead of 8, so loading
  screens and fades no longer trigger it; the return threshold stays 12.
  `LO_SGSR2_UI_PROMOTION=0` disables the promotion alone (development).
- **Cost.** Per scene-less frame: one bilinear pass at the output size and
  one lazy pass back at the render size, plus the interface draws at the
  output size. The background passes no longer run at the output size.
- **Verification.** Host: `idle_ui_promotion_test` (conditions and idle
  counter), `frame_plan_test` (the fallback policy with 45/12), the real
  `renderer.cpp` compiles in the motion-replay boundary. Not run on a GPU
  or phone. Device acceptance: no `internal resolution` line when opening
  and closing a menu or during a loading screen; sharp menu text; no frame
  above 50 ms from a switch; the log shows `interface promoted to
  2670x1200` in menus.

## P5: A/B switches on the Android diagnostics page (2026-11-07, night)

Players cannot set `LO_*` variables on a phone, so every device A/B now has
a row on Settings → System → Android diagnostics, under Export. The rows
are saved at once in `ab_switches.ini` beside `settings.ini`; at the next
start (`main.cpp`, right after the crash handler, before the renderer, Plume
or the Vulkan driver read anything) each non-default choice is written as
the development variable the code already reads. A variable already in the
environment is left alone (development override). The startup log has one
line, `A/B switches: key=value … (set VAR=…)/(env VAR=…)`.

| Row | Default | Off/other writes |
|---|---|---|
| Position-only depth vertex shaders | On | `LO_VK_KEEP_VS_OUTPUTS=1` |
| Pixel shaders without discard | On | `LO_ALPHATEST_SPEC=0` |
| Depth-only draws without pixel shader | On | `LO_KEEP_DEPTH_ONLY_PS=1` |
| Viewport jitter for unknown draws | On | `LO_SR_VIEWPORT_JITTER=0` |
| Keep render targets across resizes | On | `LO_PARK_TARGETS=0` |
| SMAA in SGSR2 Quality | On | `LO_SGSR2_SMAA=0` (G3) |
| Menus: render-size switch (old) | Off | On writes `LO_SGSR2_NATIVE_MENUS=1` (M1 fallback) |
| Turnip render mode | Auto | `TU_DEBUG=sysmem` / `TU_DEBUG=gmem` |

All apply after restarting (the page says so when a row changes). The
Turnip mode is read by Mesa when libadrenotools loads the driver; it
replaces the Adreno 650 `syncdraw` default only when chosen. Verification:
`ab_switches_test` (defaults set nothing, persisted values reload, the
environment wins and is logged, range check) and the Android settings
fixture (`settings_interaction_android`), which compiles the Android page.

## G3: SGSR2 Quality with SMAA, no sharpening (2026-11-07, night)

Decisions from Stefano: SMAA goes into Quality, the SGSR2 sharpness row and
pass go away.

- **Sharpening removed.** The Sharpness row is FSR-only again (hidden for
  SGSR2), the renderer passes no sharpening options to SGSR2, and the
  controller no longer builds or dispatches the sharpen pipeline: the
  upscale pass writes the output itself (`sharpen = 0`; the shader source is
  unchanged, its sharpen entry point is simply unused).
- **SMAA 1x on SGSR2's input.** In Quality, with an RGBA8 SDR colour input
  (the 8-bit render targets of Android), the controller runs SMAA HIGH at
  the render size before the convert pass, inside SGSR2's isolated command
  list: the colour's render region is copied into SMAA's first RGBA8 stage
  (`SmaaPipeline::DrawCopied`, instead of Presentation's crop pass), then
  edge detection, blending weights and neighborhood blending, with a
  full-screen-triangle vertex shader compiled through
  `xenos::CompileCachedHlsl`. The convert pass reads the anti-aliased
  copy; colour history, YCoCg and the clamp box all start from it. Balanced,
  Performance, HDR or non-RGBA8 input run without SMAA. A failure to build
  it logs once and leaves SGSR2 running without it.
- **Kernel.** With SMAA, Quality keeps the reference upsampling kernel
  (`kernelScale` 1.0): the input edges are already smoothed and the wider
  kernel would only blur texture detail further. Without SMAA (switch off)
  Quality keeps 0.8. `LO_SGSR2_KERNEL_SCALE` still overrides. Screenshots
  from the phone should confirm or move this.
- **Switch.** "SMAA in SGSR2 Quality" on the Android diagnostics page
  (`LO_SGSR2_SMAA=0` turns it off). The log says once
  `SGSR2: SMAA on input active|inactive`.
- **Lavapipe (`sgsr2_test`).** Staircase edge, one frame without history:
  edge RMSE against the area-sampled ideal 0.1086 without SMAA, 0.0874 with
  (−20%); output finite and in [0, 1]; a flat colour unchanged (≤0.006).
  On the accumulated detail scene SMAA costs some above-Nyquist detail:
  green (5-pixel sinusoid) RMSE 0.0668 → 0.0872, red gradient 0.0059 →
  0.0046, blue hard edge 0.0259 → 0.0327 — the softer look requested, at a
  cost in fine texture. No validation-layer message.
- **Cost.** Not measured on the phone. SMAA HIGH at 1780×800 is a copy and
  three full-screen passes on 1.4 MP (about 1 ms expected on Adreno 750).
  The `gpu passes:` profile covers the renderer's render passes, not
  SGSR2's isolated list; P4's per-class profile adds an SGSR2 bracket.

## P4: per-class draw profile (2026-11-07, night)

Goal: see where the ~10 µs per guest draw go before any further
optimisation (P1: 1,400–2,000 draws, GPU time follows the draw count).

- **What.** While the pass profile is on (Tester Advanced/Intensive, or
  `LO_PASS_PROFILE=1`) the renderer samples one frame about every 300
  frames. On that frame each command list gets a 4,097-entry timestamp
  pool: index 0 at its start, then one bottom-of-pipe timestamp after every
  guest draw. A draw is charged the time since the previous timestamp of
  its list. Classes (`draw_class_profile.h`): `depth_only` (no colour
  written), `shadow` (VS 99c2), `opaque`, `blend` (depth bound), and for
  draws without depth `fullscreen` (a full-target quad, i.e.
  post-processing) or `interface`. The SR path adds an `upscaler` bracket:
  a timestamp when the continuation list opens charges SGSR2/FSR's isolated
  list (with SMAA, G3) and the prefix tail to it.
- **Log.** Each new sample writes the cumulative session line
  `draw classes (session, …): frames= per_frame: draws= ms= us_per_draw=
  untimed=; depth_only=…ms/N(…us) … upscaler=…; top: VS/PS class ms/N; …`
  with the ten most expensive VS/PS pairs. The last such line in a
  `runtime.log` is the session summary.
- **Caveats** (also in the line's label): timestamps between draws can
  change how Turnip schedules that one frame (it may render it in sysmem),
  and overlapping draws are charged where they retire. The numbers rank
  classes and shaders; they are not exact per-draw costs. Unused queries
  are written at the end of each list so the pool always reads back
  complete.
- **Verification.** Host: `draw_class_profile_test` (classification,
  accumulation, invalid/backwards timestamps, upscaler bracket, line
  format); the real `renderer.cpp` compiles. No device data yet.

## U1: which base SGSR2 uses, in the menu (2026-11-07, night)

With SGSR2 the Render resolution row does not set the render size: the
SGSR2 quality row does (display size / 1.5, 1.7 or 2; Native AA at the
display size), and only 576p changes it, capping it at 576 lines (with
900p, Quality still renders 1780×800 on the 2670×1200 panel). The help of
both rows now says so when SGSR2 is selected (the quality help also notes
that Quality includes SMAA), with Spanish, French, German and Italian
translations in `translations_eu.h` and Traditional Chinese at the call
site. Verified by the settings menu fixtures (15/15).

## Low-priority items after the night round (2026-11-07): G2, G4, G5, P6

- **G2, fog/atmosphere (`6318b7b358aa8b45`/`c55d2c256efff0d7`).** The only
  unjittered draw left in the jittered scene: a full-screen pass that reads
  the scene depth with blend 0x10601. Its pixel centres sit up to half a
  pixel off the jittered depth, so at depth edges the fog factor can
  flicker from frame to frame; flat areas are unaffected. Two options,
  neither applied without a device look: (a) the viewport-jitter route used
  for unknown scene draws (it would leave a sub-pixel uncovered column and
  row at the screen edge), (b) offsetting its depth fetch by the frame
  jitter in a reviewed pair, like the shadow ray. Needs an A/B screenshot
  at a fogged depth edge first.
- **G4, aliasing on railings and foliage; G5, ghosting in transitions.**
  To be rechecked on the phone with this build: G3's SMAA changes the
  input edges in Quality, and M1 removes the render-size switches that
  produced most transition ghosting.
- **P6, 50–75 ms intervals in menus with ~20 ms of GPU.** The 7/11 ZIPs are
  not part of this session, so the thread analysis of `frame_timing.csv`
  is still open. M1 removes the menu-time resizes (one candidate cause);
  the next Tester recording of a menu, with the draw-class profile (P4),
  separates GPU from CPU/present stalls.

## First device run of the night build (23:01 and 23:13 traces)

- **M1 never fired.** No `interface promoted` line; the render size no
  longer changed in menus (only on setting changes), but menus stayed at
  the render size (1282×576 at 576p). Cause: the promotion required a frame
  without depth-writing draws, and menu frames still issue them (57
  depth-only draws per frame in the draw-class profile). Fix: a scene frame
  is now one that SGSR2 upscaled; a menu is any presented frame SGSR2 did
  not upscale. A promotion undone by a depth draw on the final target in
  its own frame now blocks further promotions until SGSR2 runs again, so a
  menu that draws depth there stays at one size instead of alternating.
- **P4 classification.** `fullscreen=0` in every line: the full-screen test
  compared the scissor with the EDRAM target, which is taller than the
  screen. It now compares the viewport with the render size (≥80% of its
  area, ≤6 indices).
- **What the profile already says** (98 sampled frames, indicative):
  gameplay frames ~24 ms when sampled, 718 draws; depth-only 363 draws
  at ~17 µs (6.2 ms), opaque 188 at ~21 µs (4.0 ms), blend 107 at ~36 µs
  (3.8 ms), large untextured/2D and post passes 52 at ~128 µs (6.7 ms),
  upscaler bracket (SGSR2 + SMAA at 1780×800 → 2670×1200, plus the prefix
  tail) ~3.1–3.3 ms. One pass, `8bbd4da701845d16/cda578aef1724fdc`, costs
  1.3–1.8 ms per gameplay frame and 5.5 ms in menus: the next thing to
  look at.
- **G1.** Every VS 99c2 shader seen (`d55a…`, `67b1…`, `a195…`) is a
  listed one and compensated; `shadow_uncompensated=0` throughout. A few
  draws right after loads carry all-NaN c2–c5 (rejected, unjittered); a
  candidate for the brief black patch seen at 576p, not confirmed.
- **A1.** No `xma decode burst` line; cumulative XMA failures 313 (23:01)
  and 1,005 (23:13) against 12–14 thousand per second before.
- **Open: short white dashed lines on the ground in battle.** Most likely
  temporal: tiny particles that move without their own motion vectors
  leave a dotted trail in SGSR2's history. To confirm with SGSR1 (no
  history) in the same battle; the fix would be a reactive mask for
  blended draws, which SGSR2 does not have yet.

### M1 follow-up (8 November): title screen stayed at the render size

On the phone the boot title screen stayed at 1282x576. The renderer promoted
it at frame 60, but the plan changed in that same frame (the boot switches
plans twice in its first 70 frames), the promotion was undone and promotions
stayed paused "until the next SGSR2 frame", which the title screen never
has. An undo caused by a plan change (`plan_epoch`) no longer pauses; any
other undo pauses only within its plan geometry epoch
(`idle_ui::Pause`/`Paused`), and the log says when promotions resume.
