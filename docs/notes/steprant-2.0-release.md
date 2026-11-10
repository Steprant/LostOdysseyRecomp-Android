**Steprant Android 2.0 — draft, not published.** Branch `steprant/v2.0`. The download link is added when Steprant publishes the release.

An independent Android edition maintained by **Steprant**, based on the official **Lost Odyssey Recomp v1.1.0** by **Frank / freefrank and the original community**. 2.0 installs over 1.8 and the 1.9 test builds (same package and signing key) and keeps saves, settings and the shader pack.

All figures below were measured on one device: Xiaomi 14, Snapdragon 8 Gen 3, Adreno 750, Turnip Mesa, 30 FPS, SGSR2 Quality at 900p on a 1080p output, with the in-game Tester (sessions of 10 November 2026). "Before" is the 1.9 Measures build, the last one without the 2.0 render-pass work; "2.0" is the FrameGraph build, whose rendering 2.0 keeps. GPU and traffic rows compare 10-second windows with the same number of draws per frame (600–1400), so that different scenes weigh alike. Results depend on device, driver, scene and settings.

## Results at a glance

| What was measured | Before | 2.0 | Change |
|---|---|---|---|
| GPU time per frame, gameplay windows with matching draw counts | 18.8–19.1 ms | 17.3–18.4 ms | **−4 to −9%** (median about −7%) |
| Memory traffic of render passes per frame (loads + stores) | 796–872 MB | 712–735 MB | **−9 to −18%** |
| Stores to memory removed because nothing reads them before the next full clear | 0 | 48–54 MB per frame | **93% of such stores** |
| Render passes ended by a barrier on a texture outside the pass | 13.0 per frame | 4.8 per frame | **−63%** |
| EDRAM resolve copies that wait for the end of the pass | 6.6 per frame | 13.6 per frame | **×2** |
| CPU time of the GPU command thread | 17.1 ms per frame | 17.4 ms per frame | unchanged (the frame graph's capture and replay cost about what direct recording did) |
| Battery power, whole sessions | 4.58 W | 4.57 W | not measurably different in these sessions |

Not measured yet: the profile-guided build (2.0 is compiled with a profile of the game's own code, recorded on the phone), 60 FPS per scene, 120-minute sessions. They are listed under "Still open".

## Every improvement, point by point

### GPU work and memory traffic

1. **Frame graph.** The GPU used to record every frame without knowing what came next, so it wrote every intermediate image back to memory. 2.0 keeps each frame's commands, replays them unchanged, and at the start of each render pass looks ahead: an attachment that nothing reads before a full clear replaces it is not written back (`DONT_CARE`). Anything the look-ahead cannot prove keeps the write. → **48–54 MB per frame** less written, **−7%** GPU time.
2. **Exact render areas.** A render pass now begins over the union of everything it will draw and clear, known from the look-ahead, instead of the first draw's area followed by a restart over the whole target. No restart reloads the area and no target falls back to whole-target passes for good.
3. **Copies between sampled textures wait for the end of the pass**, and so does a texture getting ready to be the next target. → passes ended this way **−63%**.
4. **Fixed:** the destination of a deferred resolve that had been sampled before stayed in a copy layout with no barrier before it was sampled again.

### Smoothness

5. **Settings menu opens without a stall:** its panels are drawn by one persistent worker thread; the first frame waits at most 250 ms, then the opening cascade starts.
6. **Fast-forward in battles and events** runs at the largest whole rate that keeps a game frame under 83 ms (2× at 30 FPS), for game time and audio alike; no more animations that never end or characters that stop until the button is released. Free field walking keeps the chosen rate.
7. **Fixed:** a mosaic of blocks on walls and rocks after changing the render resolution (old tile memory showed where a new render target had undefined contents).
8. **Fixed (suspected cause of the black spot after loads):** a shadow projection the game feeds with non-finite values on the first frame after a load is skipped.

### Gameplay

9. **Official save points restore the party's HP and MP.**
10. **Quick tools:** Battle EXP ×2/×3/×4/×10 (each award stays capped as in the game), encounter rate 0.75×.

### Image

11. **SGSR2 filter Soft by default** (no halos, a wider footprint); Standard stays available.
12. **SGSR2:** no trails of particles or alpha-tested edges that move without motion vectors; SMAA in Quality on every kind of scene target; render size from the resolution row, also on TV.

### Stability and diagnostics

13. **Crashes can be diagnosed after a restart:** the Tester ZIP carries the reasons Android 11+ keeps for the last process exits, the system's trace of the newest crashes, the end of the previous session's log and its native stderr.
14. **No false "GPU progress stalled"** while the app is in the background (the share sheet of the Tester export).
15. **Low memory:** when Android asks for memory, the allocator returns its free pages (all of them while the game is hidden).
16. **Tester measurements:** Vulkan capabilities, battery current, voltage and power, the GPU command thread's CPU split, where the game is during stalls and slow audio callbacks, per-draw timestamps on or off for a session, and the scene before SGSR2 in frame captures.
17. **Android settings show only what works on the phone or TV.**

### Build

18. **Profile-guided build:** the runtime and the recompiled game code are compiled with a profile recorded by an instrumented build on the phone (the profile is derived from the game and kept in the private build inputs, never in this repository). The CI builds the instrumented APK with `pgo=generate` and the profiled one with `pgo=use`.

## Architecture 2.0

- **Frame graph in Plume** (`LO_VK_FRAME_GRAPH`, on by default on Android): commands kept in an arena and replayed in order at the end of the list, before external commands, or past a budget; one pass over the list links the commands that touch each texture and the end of each framebuffer binding; store operations and render areas are chosen from the future. `LO_VK_FRAME_GRAPH=0` records directly.
- **Deferred work at the end of the pass:** resolves, copies between sampled textures and transitions of the next targets.
- **Safety net:** lavapipe tests compare random command sequences with a CPU model, and 400 random frames with draws (blending, sampling, depth, scissors) with direct recording, with every discarded store poisoned; both run under the Vulkan validation layer, and deliberately wrong rules fail them.
- **Diagnostics built for the Tester:** M-1 … M-8 measurements and the frame graph counters (`discarded_stores`, `discarded_mb`) in the log.

## 60 FPS

2.0 targets stability and efficiency at 30 FPS. 60 FPS per scene has not been measured on the phone yet.

## Still open

- 120-minute sessions on phone and TV, and the 1.8 reference session on the fixed route.
- Split-screen scenes (two cameras in one frame) drop to about 12 FPS: the game draws the scene twice, and the GPU command thread needs about 80 ms for such a frame.
- Status ailments at save points (only HP and MP are restored).
- The profile-guided build's gain, which needs a session.

## Credits

Lost Odyssey Recomp by **Frank / freefrank and the original community** (GPL-3.0). Qualcomm Snapdragon Game Super Resolution (BSD-3-Clause). XeGTAO (MIT). Steprant Android 2.0 by Steprant. This repository contains no game data, keys or private files.
