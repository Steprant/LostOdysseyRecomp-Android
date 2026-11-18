**Steprant Android 2.5 — stable release.** [Download the Android 2.5 APK](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/download/v2.5-steprant.20261118-Release/LostOdyssey-Steprant-v2.5-Release-20261118.apk) · [Release notes in the repository](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v2.5/docs/notes/steprant-2.5-release.md). On Adreno, keep using the **Mr. Purple T30** GPU driver (section 3).

Branch `steprant/v2.5`, versionCode 2050000. The 2.5 build uses the code tested on the phone as 2.4.8 (18 November 2026).

**2.5 is the stable version of Steprant Android.** It was optimized as far as we could take it **without lowering visual quality**: same render resolution, same upscaler, same shadows, same effects, same draw order and the same shader operations as 2.1. The gains come from doing the same work with less waiting, less spinning and less overhead, not from drawing less. The one deliberate exception is a thermal safeguard for a hot phone (point 7 in section 2).

2.5 collects six days of work after 2.1, tested on the phone build by build: 2.1.1 to 2.1.6, 2.2, 2.2.1, 2.4 and 2.4.2 to 2.4.8. Section 2 lists every change.

An independent Android edition maintained by **Steprant**, based on the official **Lost Odyssey Recomp v1.1.0** by **Frank / freefrank and the original community**. 2.5 installs over 2.1 and every earlier Steprant build (same package and signing key) and keeps saves, settings and the shader cache.

All figures were measured on one device: Xiaomi 14, Snapdragon 8 Gen 3, Adreno 750, 2670×1200 panel, 900p with SGSR2 Quality, 30 FPS, Mr. Purple T30, with the in-game Tester. Results depend on device, driver, scene and settings.

## 1. Results at a glance: 2.1 → 2.5

| What was measured | 2.1 | 2.5 | Change |
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

(1) 2.1: the split-screen scenes with two cameras (2,500–3,600 draws per frame, about 20 FPS). 2.5: the station entrance with the ticket gate, crowded with NPCs (3,000–4,500 draws per frame). Same draw counts, different scenes. (2) Battery current × voltage, unplugged, screen included, Tester profiling bursts excluded. 2.1: the 25-minute session of 14 November (other 2.1 sessions: 6.1–6.5 W, which would make the change −27 to −31%). 2.5: about two hours of play on 18 November, which includes some hot cutscenes with the safeguard of point 7. Different scenes; every session from 2.2 on measured 4.4–5.1 W.

In the two-hour session the battery stayed between 39 and 42 °C after the first quarter of an hour, the CPUs were never throttled and the game did not crash.

### Also measured on the way (2.1.1 → 2.4.8)

| What was measured | Before | After | Change |
| :--- | ---: | ---: | ---: |
| Numara shipyard, same map: battery draw (2.1.4 → 2.1.5 poll back-off) | 6.65 W | 4.78 W | **−28%** |
| Numara shipyard, same map: CPU used by the game (2.1.4 → 2.1.5) | 188% | 141% | **−25%** |
| Game's main thread, CPU of one core (2.1.4 → 2.1.5) | 39% | 17% | **−56%** |
| Zero-timeout polls by the main thread, peak per second (2.1.4 → 2.1.5) | 350,000–370,000 | 7,900 | **−98%** |
| CPU of the thread waiting for the GPU (2.1.1-C → D) | 34% | 20% | **−41%** |
| Wake-ups of the thread waiting for the GPU, per second (2.1.1-C → D) | 4,900 | 717 | **−85%** |
| Frame-graph analysis per frame (2.1.1-C → D) | 1.2–1.7 ms | 0.23–0.69 ms | **about −68%** |
| Register save/restore call sites in the recompiled code (2.1.6 → 2.2) | 19,238 | 178 | **−99%** |
| Instructions / memory accesses / calls in the 46 hottest recompiled files (2.1.6 → 2.2, static) | 100% | 80% / 72% / 82% | **−20% / −28% / −18%** |
| CPU of the recompiled game code at equal draws (2.1.6 → 2.2, estimate) (3) | 81.6–94.3% | 76.5–86.2% | **−6 to −9%** |
| Commands kept in the frame-graph capture at the station, per frame (2.4.2, 2.4.8) | ~50,000 | ~17,400 | **−65%** |
| Station entrance, frame time at 3,000–3,499 / 3,500–3,999 / 4,000–4,499 draws (2.4.5 → 2.4.8) | 45.0 / 44.0 / 45.8 ms | 39.1 / 40.8 / 41.3 ms | **−13% / −7% / −10%** |
| Station entrance, frames over 50 ms per minute (2.4.5 → 2.4.8) | 37 | 16.8 | **−55%** |

(3) Regression over 10-second windows at 400 and 700 draws per frame, different sessions; consistent signals, not an A/B in the same scene.

## 2. Every improvement since 2.1, point by point

2.5 contains everything in the [2.1 release notes](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v2.5/docs/notes/steprant-2.1-release.md). On top of 2.1:

### Heat, CPU and battery

1. **The game's main thread no longer spins (2.1.5).** In towns it polled a kernel object with a zero timeout up to 370,000 times a second while the render thread waited for the GPU. After 64 failed polls in a row, the poll now waits up to 0.5 ms on the object itself and wakes the moment it is signalled; the game sees the same result as a poll made a moment later. It never engages while the thread holds a lock. → shipyard **−28% W, −25% CPU**.
2. **Threads that wait for the GPU sleep until the GPU publishes progress (2.1.1)** instead of polling, with a bounded sleep as a safety net. → **−41% CPU, −85% wake-ups** for that thread; multiple waits from about 3,000 to about 350 a second.
3. **The idle GPU command thread waits 2 ms instead of 0.5 ms (2.1.1)**; new work, occlusion queries and shutdown wake it at once.
4. **The XMA decoder sleeps until the game sends it work (2.1.1)** instead of polling every 0.25–1 ms.
5. **The stall sampler wakes every 20 ms instead of 5 ms** when the Tester is not recording (2.1.1).
6. **The game's audio workers sleep instead of spinning while they wait for each other (2.1.6).** The rendezvous of the audio engine is replaced by a host version with the same effect on memory and registers (checked byte for byte in CI): a short spin, then a low-power wait, then a real sleep that the next arrival wakes. In 2.4.5 the round is closed only when it is full, so a late worker can never lose its turn.
7. **Thermal safeguard in cutscenes (2.1.2–2.1.4).** When the battery reaches 41.5 °C, real-time cutscenes render the 3D scene at 720p instead of 900p before SGSR2, and go back below 40.5 °C. Output size, menus and gameplay are untouched. It is there so a long session does not reach the 45–46 °C where the phone throttles its CPUs.

### Crowded scenes and the GPU command thread

8. **Heavy frames go to the GPU in pieces (2.4.6–2.4.8).** After a frame with at least 1,800 draws, the frame is submitted in pieces of about 768 draws, cut where a render pass ends. The pieces stay in the same GPU slot and are submitted without a fence, so nothing waits in the middle of a frame: the GPU starts drawing while the rest of the frame is still being recorded. Same commands, same order, same picture. Normal play (under 1,800 draws) is untouched.
9. **Less Vulkan work per draw (2.4.2, 2.4.8).** Repeated descriptor-set, pipeline-layout, framebuffer, viewport, scissor and index-buffer binds are left out of the frame-graph capture before it is analysed and replayed. The shared index storage is bound once and each draw's slice is passed as `firstIndex`. After temporal jitter and post-processing, only the edited constant blocks are restored instead of the whole bank. Checked against randomized colour and depth readbacks in serial and parallel recording.
10. **The frame-graph replay is recorded in parallel (2.1.5)** on two helper threads ("GPU Record 1/2") when a list is large; the chunks are submitted in order and the last render pass closes inside them.
11. **Faster frame-graph analysis (2.1.1)**: about −68% per frame.

### Recompiled game code

12. **Registers become function locals (2.1.5, 2.2).** First CR, XER and the reservation (2.1.5), then the general, float and vector registers (2.2), wherever an analysis of all 62,851 functions proves it safe; the rest stay in the context. A strict check runs at every build and fails it if a register would be both local and in the context. → register save/restore call sites **19,238 → 178**.
13. **`memcpy`, `memset` and the stack probe run as native host code (2.2)**: checked against the recompiled versions with 40,000 and 27,000 cases at every build.
14. **Profile-guided build with a profile recorded on the phone** (16 November), with register promotion enabled.

### Audio

15. **Elastic audio reserve (2.1.1):** from 64 to 128 ms after an empty queue, a long callback or a frozen picture, then back down one 5.3 ms period at a time after 15 s without risk. A higher reserve chosen by the player is never lowered.
16. **The game's audio worker gets audio priority after a long callback (2.1.1)** and keeps it for 10 s.
17. **XMA frames assembled by bytes** instead of bit by bit (2.4.2).
18. **The original audio mix is back (2.1.5):** the voice slider works as upstream, the stereo fold-down keeps centre and rear at −3 dB, and the music-balance changes are gone (they made the music far too quiet in the dream scenes).
19. Audio pacing and resync, recovery after fast-forward, natural end of narration streams and audio preloaded at startup (2.1.2–2.1.4).

### Memory, stability and Android

20. **The game survives the background (2.1.1):** a foreground service keeps the process at service priority so Android and HyperOS do not close it as soon as you switch app; tapping its notification returns to the game.
21. **Fixed a crash after the battle with Mack** (a null animation object) (2.1.3).
22. **GPU memory in 64 MiB blocks on Android**, and the resolve readback buffer created only when it is first used (2.4.2, 2.4.5).
23. **Textures of areas left long ago are released** when the texture cache passes 512 MB, and reloaded from the game's memory if needed (2.4); the game's virtual memory asks the kernel for huge pages (2.4).
24. Buffered file reads and fewer pacing waits during loads (2.1.2–2.1.4).

### Interface

25. **Battery temperature in the performance overlay** (2.2.1): white up to 42 °C, orange to 45 °C, red above.
26. Button prompts follow the active controller; the settings menu is drawn on the raster worker (2.1.2–2.1.4).
27. From upstream: the quit prompt's Yes option is labelled as quitting, and the Chinese quit row no longer shows a missing glyph.

### Tester and diagnostics

28. **Heavy profiling only in the last 12 s of each minute**, so the rest of a recording measures the game as it is played; `summary.json` reports watts, battery °C per minute, CPU and throttling.
29. New data: CPU run-queue time per thread, memory by category, display refresh and power-saving state, and log lines for kernel calls, command recording phases, frame-graph replay, audio barrier, scene pieces and resolve copies. Fixed: slow-frame windows no longer fire on paused intervals.

## 3. GPU driver: keep Mr. Purple T30

**On Adreno, play 2.5 with Mr. Purple T30 (PurpleVK 26.3.0-devel)**, as with 2.1: it is the driver 2.5 was tuned, tested and measured with. CTRL → **GPU driver** → **Download recommended: Mr. Purple T30** → restart when asked. After any driver change the shader cache is rebuilt once, in about 50 seconds. The driver comparison is in the [2.1 release notes](https://github.com/Steprant/LostOdysseyRecomp-Android/blob/steprant/v2.5/docs/notes/steprant-2.1-release.md#3-gpu-driver-play-with-mr-purple-t30).

## 4. Known in 2.5

- **The most crowded scenes run at about 24–25 FPS**, not 30: the station entrance draws up to 4,500 objects per frame and the GPU command thread needs about 10 µs of CPU per draw. The GPU is busy about 60% of the time there, so the CPU sets the pace.
- **Hitches in pre-rendered movies:** 29 FPS on average, but about 27 frames over 50 ms per minute.
- **Short pauses at map changes**, while the game itself loads (the screen is fading, 3 draws per frame).
- About 50 s of shader preparation at the first start after a driver change.

## 5. Sessions behind the numbers

| Session | Build | What it gives |
|---|---|---|
| 12/11 00:01, 17:03 | 2.0.7, 2.0.8 = 2.1 | Split-screen scenes at ~20 FPS, GPU time per frame, XMA errors |
| 12/11 23:11, 23:25 | 2.1 | CPU cores and GPU occupancy in play |
| 14/11 10:08 | 2.1 | 25-minute session: battery draw, temperature, audio |
| 15/11 01:14 | 2.1.1-D | Threads waiting for the GPU, frame-graph analysis |
| 16/11 14:02, 15:59 | 2.1.4, 2.1.5 | Poll back-off at the Numara shipyard |
| 17/11 11:56 | 2.2 | Game-code CPU at equal draws |
| 18/11 10:48 | 2.4.5 | Station entrance before the crowded-scene work |
| 18/11 18:34 | 2.4.8 | Station entrance, per-draw work |
| 18/11 20:31 | 2.4.8 | About two hours of play: battery draw, temperature, audio |

## APK and build

`LostOdyssey-Steprant-v2.5-Release-20261118.apk`

SHA-256 `6034311d84416aad2052008b57b488ce66f2bfff5fa7f7cfd95375d2cd8264b4`

versionCode 2050000, signing certificate SHA-256 `ecaadcdc715ff7e4aeaaf265ec11f1854d4039175dbc7e53fd4f86fd03af6f3f`. Built from commit `bfdd1f1af4e85f1782e4013d0524646ca5da48bb` with the profile-guided build, from the same code as the tested 2.4.8 (commit `f2fb2670ce149b5403092828c71105f2c9a00800`). [Successful Android build](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/44393075790).

## Credits

Lost Odyssey Recomp by **Frank / freefrank and the original community** (GPL-3.0). Qualcomm Snapdragon Game Super Resolution (BSD-3-Clause). XeGTAO (MIT). PurpleVK Turnip builds by **Mr. Purple** (Mesa, MIT), downloaded by the player from their own project. Steprant Android 2.5 by Steprant. This repository contains no game data, keys or private files.
