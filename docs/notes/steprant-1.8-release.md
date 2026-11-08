**[Download Steprant Android 1.8 APK](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/download/v1.8-steprant.20261108/LostOdyssey-Steprant-v1.8-20261108.apk)** · **[Android guide](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v1.8/docs/STEPRANT_ANDROID.md)** · **[Changelog](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v1.8/CHANGELOG.md)**

An independent Android edition maintained by **Steprant**, based on the official **Lost Odyssey Recomp v1.1.0** by **Frank / freefrank and the original community**. 1.8 collects everything since the first Steprant build and installs over 1.5, 1.6 and 1.7 test builds (same package and signing key).

All figures below were measured on one device: Xiaomi 14, Snapdragon 8 Gen 3, Adreno 750, Turnip Mesa, 2670×1200 panel, 30 FPS, with the in-game Tester. Each row compares sessions on the same maps and settings unless noted. Results depend on device, driver, scene and settings.

## Results at a glance

| What was measured | Before | 1.8 | Change |
|---|---|---|---|
| Frames over 50 ms per minute, city (900p, SGSR2 Quality) | 18.2 | 6.2 | **−66%** |
| Frames over 50 ms per minute, open field (same settings) | 20.7 | 10.0 | **−52%** |
| 99th percentile frame time, city | 48.5 ms | 37.5 ms | **−23%** |
| 99th percentile frame time, open field | 51.2 ms | 39.0 ms | **−24%** |
| GPU time per frame: 2403×1080 native → 1780×800 + SGSR2 (same session) | ~23 ms | 16–17 ms | **−26 to −30%** |
| GPU occupancy at 30 FPS: native 1080 → SGSR2 Quality (same session) | 81% | 66–74% | **−9 to −19%** |
| Render passes per frame (depth-only draws kept in the scene pass) | 100% | ~60% | **−40%** |
| GPU time inside render passes (same change) | 100% | ~92% | **−8%** |
| Render passes split by an EDRAM resolve, city | 14.7 | 7.1 | **−52%** |
| Render passes split by an EDRAM resolve, open field | 15.0 | 8.6 | **−43%** |
| Hitches at every menu, loading screen and fade (resolution switch) | 40–300 ms each | none: no switch | **−100%** |
| GPU occupancy in menus (1) | ~60% | 28–40% | **−33 to −53%** |
| XMA audio decode errors | bursts of 12–14 thousand in about a second, every session | 56 and 150 in whole sessions | **over −99%** |
| Audio queue running empty (1) | 74–91 per session | 0 | **−100%** |
| Shadow casters kept with dynamic shadows on Low | 8 | 16 | **×2** |
| Memory per pixel of 8-bit render targets (previously stored as 16-bit float) | 8 bytes | 4 bytes | **−50%** |

(1) Different sessions and scenes, before and after the change; indicative.

Not reduced in 1.8: GPU occupancy in gameplay at the new defaults stays at about **60–65%** at 30 FPS. The gains above are in smoothness, image quality and redundant work, not in total GPU load; that is the target of 2.0.

## Defaults on a fresh install

900p render resolution, **Anti-aliasing → SGSR2** at **Quality** (SMAA before the upscale), shadow resolution ×2, 30 FPS. Existing settings files are kept as they are.

## Every improvement, point by point

### Smoothness

1. **Resolve copies wait for the end of the render pass** (EDRAM emulation). A copy out of the active pass used to end it, store everything and reload it in the next pass; now it runs at the end of the pass whenever nothing in between touches the copied area. The copied pixels are identical. → **−43 to −52%** passes split this way; together with the change below, **−52 to −66% frames over 50 ms** and **−23 to −24%** on the 99th percentile.
2. **Render passes begin over the drawn area** instead of the whole target where the draws stay inside it; a pass that has to grow restarts once over the whole target (no repeated reloads).
3. **No resolution switch in menus, loading screens and fades** (also the boot title): the interface is drawn at display resolution over the enlarged background. → **every 40–300 ms hitch at those transitions removed**; menu GPU load **−33 to −53%**.
4. **Render targets reused** after a resolution switch and back (two sizes per kind, 512 MB budget): switches now happen only when you change settings.
5. **Display refresh follows the game frame rate** (the 120 Hz panel runs at the game's pace).
6. **A breakdown of every frame over 50 ms** in the Tester log (guest idle, GPU wait, present stage, pacing), to find the remaining ones.

### GPU work

7. **Qualcomm SGSR2 temporal upscaling** (Anti-aliasing → SGSR2, Quality/Balanced/Performance/Native AA): renders at 1/1.5–1/2 of the output and reconstructs it from jitter, depth and camera motion. → **−26 to −30% GPU time per frame** against native 2403×1080 in the same session.
8. **Depth-only draws stay in the scene's render pass** and run their vertex shaders without unused outputs. → **−40% render passes, −8% GPU time in passes**.
9. **Early depth rejection kept** (Adreno LRZ): pixel shaders keep early depth testing unless the game enables the alpha test. Existing shader packs stay valid.
10. **Compact render targets:** 8-bit and 10-bit targets live in 8-bit/10-bit memory instead of 16-bit float. → **−50% memory per pixel** on those targets; their pipelines are prepared at startup.
11. **Attachments with undefined contents are no longer loaded.**
12. **Earlier work retained:** redundant resolve barriers, covered clears, read-only depth draws, descriptor and pipeline lookups, shadow bookkeeping.

### Image quality

13. **SMAA in SGSR2 Quality:** edges are smoothed on the render-size image before the upscale; no extra sharpening pass. Scene textures keep output-resolution detail (mip bias).
14. **Shadows with SGSR2/TAA:** no grid of blocks in shadowed areas. Every shadow-projection shader that rebuilds its receiver gets the jitter correction; any other stays unjittered.
15. **Dynamic shadows on Low keep 16 casters on Android** instead of 8 (**×2**): NPC shadows no longer disappear.
16. **SSAO** no longer draws stripes on the sky and distant terrain.
17. **Qualcomm SGSR1** spatial upscaling in the Vulkan SDR presentation path; it stays enabled after a swapchain format change, and the overlay shows whether it is active and its sizes. Checked pixel by pixel against Qualcomm's algorithm: **maximum error 2/255**.
18. **576p and 900p** render resolutions; full-screen 2D layers cover the whole width on displays wider than 16:9; the title screen's mesh pattern is no longer drawn.

### Audio

19. **XMA error bursts removed:** a voice whose frames keep failing resynchronises on the next packet and then plays silence instead of retrying thousands of times a second. → from **12–14 thousand errors in about a second** to **56–150 in a whole session**.
20. **XMA decoder** on performance cores at audio priority; it no longer holds its lock for a whole voice.
21. **No AAudio reopen** on every return from the background; queue-reserve recovery and stall protection. → audio queue empty **0 times** in the measured sessions (74–91 before).
22. **64 ms audio buffer** on Android.

### CPU and build

23. The Android runtime is built for **ARMv8.2-A** (inline LSE atomics; Snapdragon 845 and later). No separate gain is claimed.

### Android, TV and extras

24. **TV / HDMI:** independent external output, 16:9 fitting, 720p/1080p requests, controller forwarding; phone blackout returns 10 s after the last touch.
25. **MediaCodec** H.264 replacement movies, with the original video on failure.
26. **EXP ×2 and SP ×2** quick-tool switches (LB+RB → Cheats → Quick tools).
27. Graphics preset **Mobile 30 FPS** (1080 internal, SGSR1, low shadows).
28. Cleaner settings: the development A/B rows are removed and the confirmed behaviour is built in.
29. New launcher icon; the edition is named on the title screen above the upstream credit.

### Diagnostics (Tester)

30. Bounded recordings with ZIP export: frame, CPU, thread, GPU, thermal and memory samples; per-render-pass GPU profile; GPU time by draw type and most expensive shaders; attachment load/store census; why render passes end; a breakdown of every frame over 50 ms. `tools/android/compare-tester.py` compares two recordings.

## Known limits

- GPU occupancy in gameplay is unchanged at about 60–65% at 30 FPS; 60 FPS at these settings is not reached yet.
- Some light effects look blocky at high resolution: the game draws them into small buffers.
- Some slow frames remain at scene changes and in the city (about 6–10 a minute).
- Tested on one device; HDMI/audio routing and A/V synchronisation on other devices are not verified.

## Installation and verification

Android 8.0+, ARM64, Vulkan. Package `io.github.freefrank.lostodyssey.steprant`, launcher **Lost Odyssey Steprant**, signing certificate SHA-256 `ecaadcdc715ff7e4aeaaf265ec11f1854d4039175dbc7e53fd4f86fd03af6f3f`. It coexists with the official app; saves and settings are separate. CI checks: native ARM64 build, JVM tests, lint, APK Signature Scheme v2, alignment, host regression tests and Vulkan (lavapipe) readbacks.

## Credits and game data

Thanks to **Frank / freefrank and the original community** for [Lost Odyssey Recomp](https://github.com/freefrank/LostOdysseyRecomp). This is an unofficial Android fork. The runtime remains GPL-3.0; dependencies keep their notices, including Qualcomm SGSR (BSD-3-Clause) and Intel XeGTAO (MIT). Supply supported game data from your own discs; no game data is included.
