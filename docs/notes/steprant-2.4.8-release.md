**Steprant Android 2.4.8 — stable release.** [Download the Android 2.4.8 APK](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/download/v2.4.8-steprant.20261118-Stable/LostOdyssey-Steprant-v2.4.8-Stable-20261118.apk) · [Release notes in the repository](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v2.4.8/docs/notes/steprant-2.4.8-release.md). On Adreno, keep using the **Mr. Purple T30** GPU driver (section 3).

Branch `steprant/v2.4.8`, versionCode 2040008.

**2.4.8 is the stable version of Steprant Android.** It was optimized as far as we could take it **without lowering visual quality**: same render resolution, same upscaler, same shadows, same effects, same draw order and the same shader operations as 2.1. Every gain below comes from doing the same work with less waiting and less overhead, never from drawing less.

An independent Android edition maintained by **Steprant**, based on the official **Lost Odyssey Recomp v1.1.0** by **Frank / freefrank and the original community**. 2.4.8 installs over 2.1 and every earlier Steprant build (same package and signing key) and keeps saves, settings and the shader cache.

All figures were measured on one device: Xiaomi 14, Snapdragon 8 Gen 3, Adreno 750, 2670×1200 panel, 900p with SGSR2 Quality, 30 FPS, Mr. Purple T30, with the in-game Tester. Results depend on device, driver, scene and settings.

## 1. Results at a glance: 2.1 → 2.4.8

| What was measured | 2.1 | 2.4.8 | Change |
| :--- | ---: | ---: | ---: |
| Frame rate in crowded scenes (3,000–4,000 draws per frame) (1) | ~20 FPS | 24.5–25.6 FPS | **+23 to +28%** |
| Frame time in crowded scenes (1) | ~50 ms | 39.1–40.8 ms | **−18 to −22%** |
| Frames over 50 ms per minute in crowded scenes (1) | ~240–260 | ~10 | **−96%** |
| Battery draw while playing on the phone screen (2) | 5.75 W | 4.47 W | **−22%** |
| CPU used by the game | 1.8–2.0 cores | 1.6 cores | **−11 to −20%** |
| GPU occupancy in normal play (GPU time per frame) | 46–48% (15.4–16.0 ms) | 43% (~14.4 ms) | **−6 to −10%** |
| Audio queue running empty during play | 2.5 per minute | 0 | **−100%** |
| Audio callback stalls of 50 ms or more | 16 in 25 minutes | 0 in 20 minutes | **−100%** |
| XMA audio decode errors | 11 in 8.5 minutes | 0 in 546,000 decodes | **−100%** |

(1) 2.1: the split-screen scenes with two cameras (2,500–3,600 draws per frame, about 20 FPS). 2.4.8: the station entrance with the ticket gate, crowded with NPCs (3,000–4,500 draws per frame). Same draw counts, different scenes. (2) Battery current × voltage, unplugged, screen included, Tester profiling bursts excluded. 2.1: the 25-minute session of 14 November (other 2.1 sessions: 6.1–6.5 W, which would make the change −27 to −31%). 2.4.8: about two hours of play on 18 November. Different scenes; every session from 2.2 on measured 4.4–5.1 W.

### Unchanged, said plainly

| What was measured | 2.1 | 2.4.8 | Change |
| :--- | ---: | ---: | ---: |
| Frames over 50 ms per minute in normal play, movies excluded | 3.8 | 3.8 | **0%** |
| 99th percentile frame time in normal play | 40.7 ms | 40.5 ms | **−0.5%** |
| Average frame rate in normal play | 30.2 FPS | 29.8 FPS | **−1%** |
| Peak battery temperature | 44.3 °C in a 25-minute session | 43.6 °C in a 108-minute session | **−0.7 °C** |
| Peak GPU memory | 2,336 MiB | 2,544 MiB (two-hour session) | **+9%** |

Normal play was already locked at 30 FPS in 2.1, so there was nothing left to gain there: the work went into the scenes that fell below it, into battery draw and into audio. In the two-hour 2.4.8 session the battery stayed between 39 and 42 °C after the first quarter of an hour and the CPUs were never throttled.

### Also measured during 2.4

| What was measured | Before | 2.4.8 | Change |
| :--- | ---: | ---: | ---: |
| Station entrance, frame time at 3,000–3,499 / 3,500–3,999 / 4,000–4,499 draws (2.4.5 → 2.4.8) | 45.0 / 44.0 / 45.8 ms | 39.1 / 40.8 / 41.3 ms | **−13% / −7% / −10%** |
| Station entrance, frames over 50 ms per minute (2.4.5 → 2.4.8) | 37 | 16.8 | **−55%** |
| Commands kept in the frame-graph capture at the station, per frame: repeated binds and state left out (2.4.2, 2.4.8) | ~50,000 | ~17,400 | **−65%** |
| CPU of the recompiled game code at equal draws (2.1.6 → 2.2, estimate) (3) | 81.6–94.3% | 76.5–86.2% | **−6 to −9%** |

(3) Regression over 10-second windows at 400 and 700 draws per frame, different sessions; consistent signals, not an A/B in the same scene.

## 2. What changed since 2.1

2.4.8 contains everything in the [2.1 release notes](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v2.4.8/docs/notes/steprant-2.1-release.md). On top of 2.1:

### Crowded scenes

1. **Heavy frames go to the GPU in pieces (2.4.6–2.4.8).** After a frame with at least 1,800 draws, the frame is submitted in pieces of about 768 draws, cut where a render pass ends. The pieces stay in the same GPU slot and are submitted without a fence, so nothing waits in the middle of a frame: the GPU starts drawing while the rest of the frame is still being recorded. Same commands, same order, same picture. Normal play (under 1,800 draws) is untouched.
2. **Less Vulkan work per draw (2.4.2, 2.4.8).** Repeated descriptor-set, pipeline-layout, framebuffer, viewport, scissor and index-buffer binds are left out of the frame-graph capture before it is analysed and replayed. The shared index storage is bound once and each draw's slice is passed as `firstIndex`. After temporal jitter and post-processing, only the edited constant blocks are restored instead of the whole bank. Checked against randomized colour and depth readbacks in serial and parallel recording.

### Recompiled game code and CPU

3. **Faster recompiled code (2.2).** General, float and vector registers become function locals where that is provably safe; `memcpy`, `memset` and the stack probe run as native host code (checked byte for byte). A strict check fails the build if a register would be both local and in the context.
4. **The game's main thread no longer spins (2.1.5).** Repeated zero-timeout polls back off into short, bounded waits on the same object, which wake immediately when it is signalled.
5. **Profile-guided build** of the runtime and the recompiled code, with register promotion enabled.

### Audio

6. **Audio workers sleep instead of spinning while they wait for each other (2.1.6)**, and the barrier between them closes only a full round, so a late worker can never lose its turn (2.4.5).
7. **XMA frames assembled by bytes** instead of bit by bit (2.4.2); no decode errors in the stable sessions.
8. Audio pacing and resync, recovery after fast-forward, natural end of narration streams, a better dialogue mix for companions and audio preloaded at startup (2.1.2–2.1.4).

### Memory, stability and interface

9. **GPU memory in 64 MiB blocks on Android**, and the resolve readback buffer is created only when it is first used (2.4.2, 2.4.5).
10. **Fixed a crash after the battle with Mack** (a null animation object) (2.1.3).
11. Buffered file reads and fewer pacing waits during loads; the settings menu is drawn on the raster worker; button prompts follow the active controller (2.1.2–2.1.4).
12. **Battery temperature in the performance overlay** (2.2.1).
13. From upstream: the quit prompt's Yes option is labelled as quitting, and the Chinese quit row no longer shows a missing glyph.

## 3. GPU driver: keep Mr. Purple T30

**On Adreno, play 2.4.8 with Mr. Purple T30 (PurpleVK 26.3.0-devel)**, as with 2.1: it is the driver 2.4.8 was tuned, tested and measured with. CTRL → **GPU driver** → **Download recommended: Mr. Purple T30** → restart when asked. After any driver change the shader cache is rebuilt once, in about 50 seconds. The driver comparison is in the [2.1 release notes](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v2.4.8/docs/notes/steprant-2.1-release.md#3-gpu-driver-play-with-mr-purple-t30).

## 4. Known in 2.4.8

- **The most crowded scenes run at about 24–25 FPS**, not 30: the station entrance draws up to 4,500 objects per frame and the GPU command thread needs about 10 µs of CPU per draw. The GPU is busy about 60% of the time there, so the CPU sets the pace.
- **Hitches in pre-rendered movies:** 29 FPS on average, but about 27 frames over 50 ms per minute.
- **Short pauses at map changes**, while the game itself loads (the screen is fading, 3 draws per frame).
- Peak GPU memory is not lower than in 2.1.
- About 50 s of shader preparation at the first start after a driver change.

## 5. Sessions behind the numbers

| Session | Build | What it gives |
|---|---|---|
| 12/11 00:01, 17:03 | 2.0.7, 2.0.8 = 2.1 | Split-screen scenes at ~20 FPS, GPU time per frame, XMA errors |
| 12/11 23:11, 23:25 | 2.1 | CPU cores and GPU occupancy in play |
| 14/11 10:08 | 2.1 | 25-minute session: frame pacing, battery draw, temperature, audio |
| 17/11 11:56 | 2.2 | Game-code CPU at equal draws |
| 18/11 10:48 | 2.4.5 | Station entrance before the crowded-scene work |
| 18/11 18:34 | 2.4.8 | Station entrance, per-draw work |
| 18/11 20:31 | 2.4.8 | About two hours of play: frame pacing, battery draw, temperature, audio, memory |

## APK and build

`LostOdyssey-Steprant-v2.4.8-Stable-20261118.apk`

SHA-256 `13a037d45d6edb334703f3a1fad5d8a499c217fd7eaa8c1f22e8574bb88c34b8`

versionCode 2040008, signing certificate SHA-256 `ecaadcdc715ff7e4aeaaf265ec11f1854d4039175dbc7e53fd4f86fd03af6f3f`. Built from commit `f2fb2670ce149b5403092828c71105f2c9a00800` with the profile-guided build. [Successful Android build](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/44313973105).

## Credits

Lost Odyssey Recomp by **Frank / freefrank and the original community** (GPL-3.0). Qualcomm Snapdragon Game Super Resolution (BSD-3-Clause). XeGTAO (MIT). PurpleVK Turnip builds by **Mr. Purple** (Mesa, MIT), downloaded by the player from their own project. Steprant Android 2.4.8 by Steprant. This repository contains no game data, keys or private files.
