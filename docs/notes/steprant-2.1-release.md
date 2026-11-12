**Steprant Android 2.1 — release notes.** Branch `steprant/v2.1`, versionCode 2010000. 2.1 is the code tested on the phone as 2.0.8 ("Sync", 12 November 2026), released under its own number. The download link is on the [releases page](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/latest) once Steprant publishes it.

An independent Android edition maintained by **Steprant**, based on the official **Lost Odyssey Recomp v1.1.0** by **Frank / freefrank and the original community**. 2.1 installs over 1.8, 2.0 and every 2.0.x test build (same package and signing key) and keeps saves, settings and the shader cache.

All figures were measured on one device: Xiaomi 14, Snapdragon 8 Gen 3, Adreno 750, 2670×1200 panel, 30 FPS, with the in-game Tester. Results depend on device, driver, scene and settings.

## 1. The GPU driver: install Mr. Purple T30 first

**This is the biggest single gain of 2.1 on Snapdragon devices.** The driver page now recommends **Mr. Purple T30 (PurpleVK 26.3.0-devel)** on Adreno.

**How:** CTRL → **GPU driver** → **Download recommended: Mr. Purple T30** → restart when asked. The package comes from [MrPurple666/purple-turnip](https://github.com/MrPurple666/purple-turnip/releases) and is installed in the app's private storage; the system driver is untouched and can be chosen again from the same page.

**First start:** after any driver change the pipeline cache is rebuilt, which takes about **50 seconds** in the background (51 s measured). Expect stutter in that first minute, once. From the second start the cache is warm (about 1.2 s).

**Driver comparison** (11 November 2026, app 2.0.5, same settings in all sessions: 900p, SGSR2 + SMAA, shadows ×2, 8× AF; gameplay frames with at least 800 draws, no movies, no pauses; for the new drivers the cold-cache first minute is excluded):

| Driver | GPU ms per frame, median (p90) | vs Mesa 26.0 | Main scene, µs per draw | GPU frames over 33 ms |
|---|---|---|---|---|
| **Mr. Purple T30, PurpleVK 26.3.0-devel git-62ac221a33** | **15.9 (18.0)** | **−35%** | **~2.6 (−41%)** | **0.27% (−87%)** |
| Turnip Mesa 26.0.0-devel | 24.5 (28.6) | = | ~4.4 | 2.08% |
| Turnip Mesa 26.2.3 (whitebelyash) | 24.9 (27.5) | +2% | ~3.9 | — |
| Qualcomm system driver | 34.6 (40.8) | +41% | ~8.9 | — |

→ **−35% GPU time per frame** against Mesa 26.0. The main scene of about 2,700 draws costs 7.1 ms instead of 9.5–10.3 ms. Later sessions with 2.0.7 and 2.1 confirm 15.4–16.0 ms per frame (about 46–48% of the 33 ms frame at 30 FPS).

**What 2.1 changes around the driver:** Mesa 26.2/26.3 builds can block `vkQueuePresentKHR` for 5–17 ms; with three swap chain images a frame then missed its 33 ms slot and lasted 50 ms. 2.1 uses four images on Android: present calls over 10 ms went from 25% of frames (2.0.6 with PurpleVK) to 1.1% (2.0.7) and 0.8% (2.1). A system performance overlay (such as a game-turbo monitor) can make the compositor return images later; turn it off while playing.

Verified on Adreno 750 only. Other Adreno GPUs can try it; if the game fails to start, the driver page opens with the reason and the system driver is one tap away.

## 2. From 1.1 to 2.1 at a glance

Xiaomi 14, Snapdragon 8 Gen 3, Adreno 750, 30 FPS, in-game Tester. "First builds" are the Steprant 1.1–1.5 builds and the 1.7 test builds that preceded 1.8; each column is that version at its own defaults. Rows marked (1) come from different sessions and scenes and are indicative; section 5 names the sessions behind the numbers. Results depend on device, driver, scene and settings.

### GPU and driver

| What was measured | First builds | 1.8 | 2.0 | **2.1** | vs first builds | vs 1.8 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| GPU time per frame in gameplay | ~23 ms (21–26), native 2403×1080 | 16–17 ms, 1780×800 + SGSR2 | 17.3–18.4 ms, 2003×900 + SGSR2 | **15.4–16.0 ms**, 2003×900 + SGSR2 | **−32%** (1) | **−5%** with 27% more pixels (1) |
| GPU occupancy at 30 FPS | 62–81% | 60–65% | — | **46–48%** | **−34%** (1) | **−25%** (1) |
| GPU time, same scenes and settings: Mesa 26.0 → Mr. Purple T30 | — | — | 24.5 ms | **15.9 ms** | — | **−35%** |
| GPU frames over 33 ms, same comparison | — | — | 2.08% | **0.27%** | — | **−87%** |
| GPU time per draw in the main scene, same comparison | — | — | ~4.4 µs | **~2.6 µs** | — | **−41%** |

### Smoothness

| What was measured | First builds | 1.8 | 2.0 | **2.1** | vs first builds | vs 1.8 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| Frames over 50 ms per minute, normal play (city / field in 1.7 and 1.8) | 18.2 / 20.7 | 6.2 / 10.0 | 9.5 | **3.7** | **−80 to −82%** (1) | **−40 to −63%** (1) |
| Frames over 50 ms per minute, whole session without split-screen scenes | — | — | — | **6.7** | **−63 to −68%** (1) | — |
| 99th percentile frame time | 48.5 / 51.2 ms | 37.5 / 39.0 ms | 42.2 ms | **37.6 ms** (41.2 whole session) | **−22 to −27%** (1) | **0 to −4%** (1) |
| Split-screen scenes (two cameras) | — | — | ~12 FPS | **~20 FPS** | — | **+67%** from 2.0 (1) |
| Present calls over 10 ms with recent Turnip builds | — | — | 25% of frames (2.0.6) | **0.8%** | — | **−97%** from 2.0.6 |
| Hitches at menus, loading screens and fades | 40–300 ms each | none | none | **none** | **−100%** | = |
| Opening the settings menu over the game (first frame) | — | 113–137 ms | ≤ 250 ms | **9.4 ms** | — | **−93%** |

### Image

| What was measured | First builds | 1.8 | 2.0 | **2.1** | vs first builds | vs 1.8 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| Pixels rendered with the 900p row and SGSR2, phone | — | 1780×800 | 2003×900 | **2003×900** | — | **+27%** |
| Pixels rendered with the 900p row and SGSR2, 1080p TV | — | 1280×720 | 1600×900 | **1600×900** | — | **+56%** |
| Frames SGSR2 hands over without anti-aliasing | — | all of them | all of them | **none: SMAA on every one** | — | **−100%** |
| Hand-overs from SGSR2 to the spatial fallback per minute | — | — | 4.5 | **2.2** (measured on 2.0.3) | — | **−50%** from 2.0 (1) |
| Loading screens and movies on the phone | — | render size | render size | **output size, 2670×1200** | — | **+78% pixels** |
| Shadow casters with dynamic shadows on Low | 8 | 16 | 16 | **16** | **×2** | = |

### Rendering work

| What was measured | First builds | 1.8 | 2.0 | **2.1** | vs first builds | vs 1.8 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| Render passes per frame | 100% | ~60% | ~60% | **~60%** | **−40%** | = |
| Render passes split by an EDRAM resolve, city / field | 14.7 / 15.0 | 7.1 / 8.6 | kept | **kept** | **−52% / −43%** | — |
| Render-pass memory traffic per frame (loads + stores) | — | 796–872 MB (1.9) | 712–735 MB | **712–735 MB** | — | **−9 to −18%** |
| Stores to memory that nothing reads | all kept | all kept | 48–54 MB less per frame | **48–54 MB less per frame** | **−93% of such stores** | **−93%** |
| Render passes ended by a barrier outside the pass | — | 13.0 per frame (1.9) | 4.8 | **4.8** | — | **−63%** |
| Memory per pixel of 8-bit render targets | 8 bytes | 4 bytes | 4 bytes | **4 bytes** | **−50%** | = |

### Audio

| What was measured | First builds | 1.8 | 2.0 | **2.1** | vs first builds | vs 1.8 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| XMA audio decode errors | 12–14 thousand in a second | 56–150 per session | 48 in 90 minutes | **11 in 8.5 minutes** | **over −99%** | **−80 to −93%** (1) |
| Audio queue running empty during play | 74–91 per session | 0 | 68 | **0** (9 in the first seconds of startup) | **−100%** (1) | = |
| Longest audio callback stall | — | 705 ms | 322 ms | **84 ms** | — | **−88%** (1) |
| Audio behind the picture after a stall | stays behind | stays behind | stays behind | **measured and caught up** | **fixed** | **fixed** |

### Memory

| What was measured | First builds | 1.8 | 2.0 | **2.1** | vs first builds | vs 1.8 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| Peak GPU memory with phone ↔ TV switches | — | — | 2,592 MiB | **2,336 MiB** | — | **−10%** from 2.0 (1) |
| Peak app memory (PSS) with phone ↔ TV switches | — | — | 4.1 GB (two switches) | **3.78 GB** (three switches) | — | **−8%** from 2.0 (1) |

**In short:** against the first Steprant builds, 2.1 needs **about a third less GPU time** per frame while rendering through SGSR2 + SMAA, has **80% fewer long frames** in normal play and keeps audio in sync. Against 1.8, frame times in normal play are about the same while 2.1 renders **27% more pixels** and occupies **a quarter less GPU**; most of that headroom comes from the recommended driver (**−35%** on its own).

**Reading the table honestly:**
- The GPU row mixes render sizes: the first builds rendered at native 1080 (2403×1080), 1.8 at 1780×800, 2.0 and 2.1 at 2003×900, all at 30 FPS. 2.1 renders 31% fewer pixels than native 1080 and 27% more than 1.8. The clean like-for-like number is the driver A/B of section 1 (−35%).
- From 2.0.3 the Tester always records per-draw GPU timestamps, which add their own cost: the 2.0.x and 2.1 GPU times are, if anything, slightly high.
- Against 1.8, frame times in normal play are about the same, at 27% more pixels and about a quarter less GPU occupancy.
- 2.1 frame pacing: whole session of 12 November 17:03 without the split-screen scene, p99 41.2 ms and 6.7 frames over 50 ms per minute; normal play from 280 to 440 s, p99 37.6 ms and 3.7 per minute; a quiet stretch of the 2.0.7 session, 1.0 per minute. The remaining clusters are the game itself pausing (`cp_idle` of 50–100 ms at loads and scene changes).

## 3. Every improvement since 2.0, point by point

2.1 contains everything in the [2.0 release notes](steprant-2.0-release.md) (frame graph, exact render areas, settings menu without stall, fast-forward rate cap, mosaic and shadow fixes, save points that restore HP and MP, EXP up to ×10, SGSR2 Soft, crash diagnostics, profile-guided build) and the [1.8 release notes](steprant-1.8-release.md). On top of 2.0:

### GPU driver and presentation

1. **Mr. Purple T30 recommended on Adreno** (section 1): −35% GPU time per frame; the driver page names the verified driver.
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

## Credits

Lost Odyssey Recomp by **Frank / freefrank and the original community** (GPL-3.0). Qualcomm Snapdragon Game Super Resolution (BSD-3-Clause). XeGTAO (MIT). PurpleVK Turnip builds by **Mr. Purple** (Mesa, MIT), downloaded by the player from their own project. Steprant Android 2.1 by Steprant. This repository contains no game data, keys or private files.
