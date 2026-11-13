**Steprant Android 2.1 — release notes.** [Download the Android 2.1 APK](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/download/v2.1-steprant.20261112-Release/LostOdyssey-Steprant-v2.1-Release-20261112.apk) · [GitHub release](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/tag/v2.1-steprant.20261112-Release). On Adreno, use the **Mr. Purple T30** GPU driver. See [section 3](#3-gpu-driver-play-with-mr-purple-t30).

Branch `steprant/v2.1`, versionCode 2010000. The 2.1 build uses the gameplay code tested as 2.0.8 (“Sync”, 12 November 2026).

An independent Android edition maintained by **Steprant**, based on the official **Lost Odyssey Recomp v1.1.0** by **Frank / freefrank and the original community**. 2.1 installs over 1.8, 2.0 and every 2.0.x test build (same package and signing key) and keeps saves, settings and the shader cache.

All figures were measured on one device: Xiaomi 14, Snapdragon 8 Gen 3, Adreno 750, 2670×1200 panel, 30 FPS, with the in-game Tester. Results depend on device, driver, scene and settings.

## 1. Results at a glance

Section 5 names the sessions behind the numbers.

The same measurements as the 1.8 table, now from 1.1 to 2.1. "1.1" is the "Before" of the 1.8 table: the first Steprant builds and the 1.7 test builds.

| What was measured | 1.1 | 1.8 | 2.1 | Change 1.1 → 2.1 |
| :--- | ---: | ---: | ---: | ---: |
| Frames over 50 ms per minute, city / open field | 18.2 / 20.7 | 6.2 / 10.0 | 3.7 in normal play (1) | **−80% / −82%** |
| 99th percentile frame time, city / open field | 48.5 / 51.2 ms | 37.5 / 39.0 ms | 37.6 ms in normal play (1) | **−22% / −27%** |
| GPU time per frame: native 2403×1080 → SGSR2 (1780×800 in 1.8, 2003×900 in 2.1) | ~23 ms | 16–17 ms | 15.4–16.0 ms (1) | **−30 to −33%** |
| GPU occupancy at 30 FPS: native 1080 → SGSR2 Quality | 81% | 66–74% | 46–48% (1) | **−41 to −43%** |
| Render passes per frame | 100% | ~60% | ~60% | **−40%** |
| GPU time inside render passes | 100% | ~92% | ~85% (2) | **−15%** |
| Render passes split by an EDRAM resolve, city / open field | 14.7 / 15.0 | 7.1 / 8.6 | 7.0 (1) | **−52% / −53%** |
| Hitches caused by resolution switches at menus, loading screens and fades | 40–300 ms each | none | none | **−100%** |
| GPU occupancy in menus (1) | ~60% | 28–40% | 11–24% | **−60 to −82%** |
| XMA audio decode errors | 12–14 thousand in a second | 56–150 per session | 11 in 8.5 minutes | **over −99%** |
| Audio queue running empty (1) | 74–91 per session | 0 | 0 in play (9 in the first seconds of startup) | **−100% in play** |
| Shadow casters with dynamic shadows on Low | 8 | 16 | 16 | **×2** |
| Memory per pixel of 8-bit render targets | 8 bytes | 4 bytes | 4 bytes | **−50%** |

(1) Different sessions and scenes, before and after; indicative. 2.1 GPU figures were measured with the recommended driver (section 3). (2) Estimate: the −8% of 1.8 followed by the 2.0 frame graph (−4 to −9% GPU time per frame); not measured as one number.

### Also measured in 2.1

| What was measured | Before | 2.1 | Change |
| :--- | ---: | ---: | ---: |
| Pixels rendered at 900p with SGSR2, phone / TV (1.8) | 1780×800 / 1280×720 | 2003×900 / 1600×900 | **+27% / +56%** |
| Frames shown without anti-aliasing when SGSR2 cannot upscale (1.8) | all of them | none (SMAA) | **−100%** |
| Opening the settings menu over the game (1.8) | 113–137 ms | 9.4 ms | **−93%** |
| Longest audio callback stall (1.8) (1) | 705 ms | 84 ms | **−88%** |
| Audio behind the picture after a stall (1.8) | stays behind | caught up | **fixed** |
| Stores to memory that nothing reads (1.8) | all kept | 48–54 MB less per frame | **−93%** |
| Split-screen scenes with two cameras (2.0) (1) | ~12 FPS | ~20 FPS | **+67%** |
| Peak GPU memory with phone ↔ TV switches (2.0) (1) | 2,592 MiB | 2,336 MiB | **−10%** |

**Reading the table honestly:**
- The GPU row mixes render sizes: the first builds rendered at native 1080 (2403×1080), 1.8 at 1780×800, 2.0 and 2.1 at 2003×900, all at 30 FPS. 2.1 renders 31% fewer pixels than native 1080 and 27% more than 1.8. The same-scene driver comparison is in section 3.
- From 2.0.3 the Tester always records per-draw GPU timestamps, which add their own cost: the 2.0.x and 2.1 GPU times are, if anything, slightly high.
- In the sampled gameplay, 2.1 renders 27% more pixels than 1.8 with similar frame times.
- 2.1 frame pacing: whole session of 12 November 17:03 without the split-screen scene, p99 41.2 ms and 6.7 frames over 50 ms per minute; normal play from 280 to 440 s, p99 37.6 ms and 3.7 per minute; a quiet stretch of the 2.0.7 session, 1.0 per minute. The remaining clusters are the game itself pausing (`cp_idle` of 50–100 ms at loads and scene changes).

### GPU load and battery use

On a Xiaomi 14 at 900p, SGSR2 Quality and 30 FPS with Mr. Purple T30, 2.1 used 15.4–16.0 ms of GPU time per frame in the sampled gameplay (46–48% of the frame budget). Battery draw and runtime still need direct measurement. The 1.8 release notes report about 60–65% GPU occupancy during gameplay at the 1.8 defaults, which used a lower internal render resolution. The two builds were recorded in different gameplay sessions.

## 2. Every improvement since 2.0, point by point

2.1 contains everything in the [2.0 release notes](steprant-2.0-release.md) (frame graph, exact render areas, settings menu without stall, fast-forward rate cap, mosaic and shadow fixes, save points that restore HP and MP, EXP up to ×10, SGSR2 Soft, crash diagnostics, profile-guided build) and the [1.8 release notes](steprant-1.8-release.md). On top of 2.0:

### GPU driver and presentation

1. **The driver page recommends Mr. Purple T30 on Adreno** and names the verified driver (section 3).
2. **Four swap chain images on Android** instead of three; the log line `video output:` reports the real image count (`images= requested=`). → present calls over 10 ms: **25% → 0.8%** of frames.
3. **Interrupt and vblank threads prefer the big cores** at a higher priority. Measured: interrupts wait 0.1 ms on average and almost never over 2 ms, so this changes little; it stays because it costs nothing.

### Image

4. **SMAA on every frame the upscaler skips**, in any cutscene: when SGSR2 cannot run (no scene camera, an incomplete scene), the frame goes through SMAA instead of plain spatial scaling. Seen in the children's cutscene, where 500–760 frames at startup had no scene camera.
5. **Scenes whose motion is not ready stay in SGSR2** (after loads and at cuts): zero motion and fresh history, same anti-aliasing and filter. SGSR1 hand-overs went from 39 in 8.6 minutes to 26 in 11.6 minutes (2.0.3), and with point 4 none of them lacks anti-aliasing.
6. **Loading screens and movies drawn at the output size** from their first draw (860 frames in one session), and no promote/undo loop in menus.

### Audio

7. **The game's audio catches up after a stall** instead of staying behind the picture: the runtime measures the lag (wall-clock frames against the game frames played and still queued) and drops what is late. The measurement starts again on pause, device reopen, reserve change and fast-forward.
8. **No resync while the picture is frozen** (loads, shader preparation, more than 100 ms without a present): audio and picture stop together there, so nothing is dropped.
9. **The game's audio worker gets audio priority while it mostly waits** (under 10% of a core), and a middle priority when it works hard, so it does not take cores from the game during loads.
→ the desync of the guard-with-flowers and Jansen scenes is gone; in the 2.1 session the audio queue ran empty only in the first seconds of startup (9 times), and the longest callback stall was 84 ms.

### Memory, menus, Tester

10. **Render targets parked at a phone ↔ TV switch are freed after 60 s** if no new size takes them back (132 MB parked at one switch; peak GPU memory 2,592 → 2,336 MiB).
11. **Settings menu over the game:** no reads of the upload buffer, unchanged images reused. → first frame **9.4 ms** (about 115 ms per frame in the 2.0.1 test builds).
12. **Update check at the title screen** on its own thread instead of at startup.
13. **Tester:** one profile (the most accurate), **Clear diagnostics** with the freed MB shown, exports with only the current run's frame captures and profiles (a stale capture had made every export 130 MB; now under 1 MB).
14. **Diagnostics:** `gpu sync:` lines measure where the GPU command thread waits for the game.

## 3. GPU driver: play with Mr. Purple T30

**On Adreno, play 2.1 with Mr. Purple T30 (PurpleVK 26.3.0-devel).** 2.1 is tuned, tested and measured with this driver; the system driver and older Turnip builds are not the supported setup. The driver page offers it as the recommended download.

**How:** CTRL → **GPU driver** → **Download recommended: Mr. Purple T30** → restart when asked. The package comes from [MrPurple666/purple-turnip](https://github.com/MrPurple666/purple-turnip/releases) and is installed in the app's private storage; the system driver is untouched and can be chosen again from the same page.

**First start:** after any driver change the pipeline cache is rebuilt, which takes about **50 seconds** in the background (51 s measured). Expect stutter in that first minute, once. From the second start the cache is warm (about 1.2 s).

**Driver comparison** (11 November 2026, app 2.0.5, same settings in all sessions: 900p, SGSR2 + SMAA, shadows ×2, 8× AF; gameplay frames with at least 800 draws, no movies, no pauses; for the new drivers the cold-cache first minute is excluded):

| Driver | GPU ms per frame, median (p90) | vs Mesa 26.0 | Main scene, µs per draw | GPU frames over 33 ms |
|---|---|---|---|---|
| **Mr. Purple T30, PurpleVK 26.3.0-devel git-62ac221a33** | **15.9 (18.0)** | **−35%** | **~2.6 (−41%)** | **0.27% (−87%)** |
| Turnip Mesa 26.0.0-devel | 24.5 (28.6) | = | ~4.4 | 2.08% |
| Turnip Mesa 26.2.3 (whitebelyash) | 24.9 (27.5) | +2% | ~3.9 | — |
| Qualcomm system driver | 34.6 (40.8) | +41% | ~8.9 | — |

→ about −35% GPU time per frame against Mesa 26.0 in these scenes. The main scene of about 2,700 draws costs 7.1 ms instead of 9.5–10.3 ms. Later sessions with 2.0.7 and 2.1 confirm 15.4–16.0 ms per frame (about 46–48% of the 33 ms frame at 30 FPS).

**What 2.1 changes around the driver:** Mesa 26.2/26.3 builds can block `vkQueuePresentKHR` for 5–17 ms; with three swap chain images a frame then missed its 33 ms slot and lasted 50 ms. 2.1 uses four images on Android: present calls over 10 ms went from 25% of frames (2.0.6 with PurpleVK) to 1.1% (2.0.7) and 0.8% (2.1). A system performance overlay (such as a game-turbo monitor) can make the compositor return images later; turn it off while playing.

Verified on Adreno 750 only. Other Adreno GPUs can try it; if the game fails to start, the driver page opens with the reason and the system driver is one tap away.

## 4. Known in 2.1

- **Split-screen scenes with two cameras run at about 20 FPS.** The game draws the scene twice (2,500–3,600 draws per frame) and the GPU command thread needs 32–38 ms of real work per frame, over the 33 ms budget. It is CPU work, not GPU: the driver does not change it.
- **Short hitches while the game streams textures** on entering a new area (4–9 ms per frame on the command thread).
- **Some cutscenes get SMAA instead of SGSR2** (frames without a usable scene camera): anti-aliased, not upscaled temporally.
- **About 50 s of shader preparation** at the first start after a driver change.
- **HDMI / TV output** has not been measured with the new driver yet.
- Status ailments at save points are not cured (only HP and MP).

## 5. Sessions behind the numbers

| Session | Build | What it gives |
|---|---|---|
| 4/11 logs | 1.5 | First-build GPU time and occupancy at native 1080 |
| 8/11 | 1.7 tests, 1.8 | The 1.8 table (city and open field); 1.8 TV session (settings menu, Uhra audio stall) |
| 10/11 | 2.0 | 2.0 table; 22:23, 90 minutes (XMA errors, audio stalls) |
| 11/11 13:33, 17:12, 18:39 | 2.0.2–2.0.4 | Settings menu, SGSR2 hand-overs, memory |
| 11/11 20:19, 20:48, 20:53, 20:57 | 2.0.5 | Driver comparison |
| 11/11 22:43, 22:46; 12/11 00:01 | 2.0.6, 2.0.7 | Audio catch-up, four swap chain images |
| 12/11 17:03 | 2.0.8 = 2.1 | 2.1 frame pacing, GPU time, audio, split-screen |

## APK and build

`LostOdyssey-Steprant-v2.1-Release-20261112.apk`

SHA-256 `5ea4f42175295972bc58584ec110861205a01594fb245d25d186fe9926958184`

Built from commit `90ef437e22cb3ec93eba72d73dcf665f1c0937df`. [Successful Android build](https://github.com/Steprant/LostOdysseyRecomp-Android/actions/runs/43275026759).

## Credits

Lost Odyssey Recomp by **Frank / freefrank and the original community** (GPL-3.0). Qualcomm Snapdragon Game Super Resolution (BSD-3-Clause). XeGTAO (MIT). PurpleVK Turnip builds by **Mr. Purple** (Mesa, MIT), downloaded by the player from their own project. Steprant Android 2.1 by Steprant. This repository contains no game data, keys or private files.
