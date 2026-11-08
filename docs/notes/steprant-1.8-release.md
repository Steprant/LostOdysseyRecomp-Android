**[Download Steprant Android 1.8 APK](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/download/v1.8-steprant.20261108/LostOdyssey-Steprant-v1.8-20261108.apk)** · **[Android guide](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v1.8/docs/STEPRANT_ANDROID.md)** · **[Changelog](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v1.8/CHANGELOG.md)**

An independent Android edition maintained by **Steprant**, based on the official **Lost Odyssey Recomp v1.1.0** by **Frank / freefrank and the original community**. 1.8 is this fork's app version; it installs over 1.5, 1.6 and 1.7 test builds (same package and signing key).

This release collects everything since the first Steprant build. Measurements were taken on one device (Xiaomi 14, Snapdragon 8 Gen 3, Adreno 750, Turnip Mesa, 2670×1200, 30 FPS); results depend on device, driver, scene and settings.

## New defaults on a fresh install

900p render resolution, **Anti-aliasing → SGSR2** at **Quality** (SMAA before the upscale), shadow resolution ×2, 30 FPS. Existing settings files are kept as they are.

## Graphics

- **Qualcomm SGSR2 temporal upscaling** (Anti-aliasing → SGSR2, with a quality row): renders at 1/1.5–1/2 of the output size and reconstructs it from jitter, depth and camera motion; scene textures keep output-resolution detail (mip bias). In **Quality**, SMAA smooths edges on the render-size image before the upscale. 576p and 900p render resolutions.
- **Shadows with SGSR2/TAA:** no grid of blocks in shadowed areas; every shadow-projection shader that rebuilds its receiver gets the jitter correction, any other stays unjittered.
- **Menus, loading screens and fades** no longer change the render resolution: their interface is drawn at the display resolution over the enlarged background (no hitch at every transition). The boot title screen is included.
- **Qualcomm SGSR1** spatial upscaling in the Vulkan SDR presentation path; it stays enabled after a swapchain format change, and the overlay shows whether it is active and its sizes.
- **Ambient occlusion:** SSAO no longer draws stripes on the sky and distant terrain.
- Full-screen 2D layers cover the whole width on displays wider than 16:9; the title screen's mesh pattern is no longer drawn.
- Graphics preset **Mobile 30 FPS** (1080 internal, SGSR1, low shadows).

## Rendering performance (Vulkan, Android)

- Compact 8/10-bit render targets in 8-bit memory, with their pipelines prepared at startup.
- Depth-only draws stay in the scene's render pass (about 40% fewer render passes, ~8% less pass time measured in 1.6) and run their vertex shaders without unused outputs.
- Pixel shaders keep early depth rejection unless the game enables the alpha test (Adreno LRZ); existing shader packs stay valid.
- Render targets are reused after a resolution switch and back.
- Render passes begin over the draws' scissor instead of the whole target where the draws stay inside it.
- **EDRAM resolve copies wait for the end of the render pass** instead of splitting it whenever nothing in between touches the copied area; the copied pixels are identical. About two pass splits a frame are saved in the measured scenes.
- Attachments whose contents are undefined are no longer loaded.
- Display refresh follows the game frame rate.
- Earlier work retained: redundant resolve barriers, covered clears, read-only depth draws, descriptor/pipeline lookups and shadow bookkeeping.

## Frame pacing

Compared with the 1.7 test builds on the same maps and settings (900p, SGSR2 Quality):

| | before | 1.8 |
|---|---|---|
| frames over 50 ms per minute, city | 18.2 | 6.2 |
| frames over 50 ms per minute, field | 20.7 | 10.0 |
| 99th percentile frame time, city | 48.5 ms | 37.5 ms |
| 99th percentile frame time, field | 51.2 ms | 39.0 ms |

GPU occupancy at 30 FPS is about 60–65% in these scenes; no measurable reduction is claimed for 1.8.

## CPU and build

- The Android runtime is built for ARMv8.2-A (inline LSE atomics; Snapdragon 845 and later).
- The XMA decoder runs on performance cores at audio priority and no longer holds its lock for a whole voice.

## Audio

- No AAudio reopen on every return from a long background pause; queue-reserve recovery and AAudio stall protection.
- An XMA voice whose frames keep failing resynchronises on the next packet and then plays silence instead of retrying thousands of times a second (decode failures went from bursts of 12–14 thousand a second to a few hundred per session).
- 64 ms audio buffer on Android.

## Android, TV and extras

- **TV / HDMI:** independent external output, 16:9 fitting, 720p/1080p requests, controller forwarding; phone blackout returns 10 s after the last touch.
- **MediaCodec** H.264 replacement movies, with the original video on failure.
- **EXP ×2 and SP ×2** quick-tool switches (LB+RB → Cheats → Quick tools).
- New launcher icon, and the edition named on the title screen above the upstream credit.

## Diagnostics (Tester)

Bounded recordings with ZIP export: frame, CPU, thread, GPU, thermal and memory samples; per-render-pass GPU profile, GPU time by draw type and most expensive shaders, attachment load/store census, why render passes end, and a breakdown of every frame over 50 ms. The device A/B rows used during development are removed; the confirmed behaviour is built in. `tools/android/compare-tester.py` compares two recordings.

## Known limits

- Some light effects look blocky at high resolution: the game draws them into small buffers.
- Some slow frames remain at scene changes and in the city.
- Gameplay was tested on one device; HDMI/audio routing and A/V synchronisation on other devices are not verified.

## Installation and verification

Android 8.0+, ARM64, Vulkan. Package `io.github.freefrank.lostodyssey.steprant`, launcher **Lost Odyssey Steprant**, signing certificate SHA-256 `ecaadcdc715ff7e4aeaaf265ec11f1854d4039175dbc7e53fd4f86fd03af6f3f`. It coexists with the official app; saves and settings are separate. CI checks: native ARM64 build, JVM tests, lint, APK Signature Scheme v2, alignment, host regression tests and Vulkan (lavapipe) readbacks.

## Credits and game data

Thanks to **Frank / freefrank and the original community** for [Lost Odyssey Recomp](https://github.com/freefrank/LostOdysseyRecomp). This is an unofficial Android fork. The runtime remains GPL-3.0; dependencies keep their notices, including Qualcomm SGSR (BSD-3-Clause) and Intel XeGTAO (MIT). Supply supported game data from your own discs; no game data is included.
