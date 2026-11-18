<div align="center">

<img src="assets/steprant-android-logo.webp" alt="Lost Odyssey Recomp Android — Steprant logo" width="360">

# Lost Odyssey Recomp — Steprant Android 2.5

**Lost Odyssey on Android with Qualcomm SGSR2 + SMAA, a Vulkan frame graph and ARM64 PGO.** Steprant 2.5 Stable improves frame pacing and XMA audio, adjusts resolution in hot real-time cutscenes and supports independent TV/HDMI output.

In Xiaomi 14 tests against 2.1, **FPS rose by up to 28% in crowded scenes**, **power draw fell by 22%** and **no audio dropouts were recorded**. [Progress by version](#progress-by-version)

[![Download Android 2.5 APK](https://img.shields.io/badge/Download-Android_2.5_APK-3DDC84?style=for-the-badge&logo=android&logoColor=white)](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/download/v2.5-steprant.20261118-Release/LostOdyssey-Steprant-v2.5-Release-20261118.apk)

**[Download APK 2.5](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/download/v2.5-steprant.20261118-Release/LostOdyssey-Steprant-v2.5-Release-20261118.apk) · [GitHub release](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/tag/v2.5-steprant.20261118-Release) · [Release notes](docs/notes/steprant-2.5-release.md) · [Android guide](docs/STEPRANT_ANDROID.md)**

![Android ARM64](https://img.shields.io/badge/Android-8.0%2B_ARM64-3DDC84?logo=android&logoColor=white)
![Vulkan](https://img.shields.io/badge/Graphics-Vulkan-AC162C)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)](LICENSE)

An independent Android edition maintained by **Steprant**, based on the official **v1.1.0** release of [Lost Odyssey Recomp](https://github.com/freefrank/LostOdysseyRecomp) by **Frank / freefrank and the original community**. Steprant 2.5 is this fork's version, separate from upstream's release numbering.

</div>

## Steprant Android 2.5 — stable

Steprant 2.5 is the stable release of the code tested as 2.4.8. Fresh installs default to **900p, SGSR2 Quality with SMAA, 2× shadows and 30 FPS**. The renderer keeps the gameplay image settings, draw order and shader operations used in 2.1. In real-time cutscenes, the thermal safeguard renders the 3D scene at 720p when the battery reaches 41.5 °C and restores the selected resolution below 40.5 °C. [2.5 release notes](docs/notes/steprant-2.5-release.md)

> [!IMPORTANT]
> **Snapdragon (Adreno): play 2.5 with the Mr. Purple T30 GPU driver (PurpleVK 26.3).** 2.5 is tuned, tested and measured with this driver; the system driver and older Turnip builds are not the supported setup.
>
> 1. In the game, open **CTRL → GPU driver**.
> 2. Tap **Download recommended: Mr. Purple T30** and restart the game.
> 3. The first start after the change prepares shaders for about 50 seconds, once; from the next start it is ready.

### Android techniques

- **Image quality.** Qualcomm SGSR2 reconstructs detail from jitter, depth and camera motion. SMAA smooths edges in Quality mode and covers cutscenes when SGSR2 cannot run. SGSR1 provides spatial upscaling.
- **Vulkan rendering.** Plume's frame graph records large command lists in parallel, submits heavy frames in ordered batches and skips redundant state changes and unread stores. It uses exact render areas, deferred EDRAM resolves, compact 8-bit and 10-bit targets and Adreno LRZ for early depth rejection.
- **Frame pacing.** A four-image swapchain, display refresh matching and off-thread menu rendering reduce presentation and menu stalls.
- **ARM64 and power.** XenonRecomp statically recompiles the PowerPC game code. ARMv8.2-A with LSE atomics, profile-guided optimization (PGO), register promotion and native `memcpy`/`memset` reduce CPU work. Event-driven CPU and GPU waits reduce polling.
- **Audio and video.** XMA decoding, elastic audio buffering and audio resync keep the original mix in sync. Android MediaCodec supports experimental H.264 replacement videos.
- **Android and TV.** Independent HDMI output supports 720p and 1080p mode requests. A foreground service helps keep the app available when switching away, and the Tester logs frame pacing, CPU and GPU load, battery draw and temperature. [TV guide](docs/ANDROID_TV_HDMI.md) · [Tester guide](docs/ANDROID_TESTER.md)

### Progress by version

Gameplay measurements come from a Xiaomi 14 (Snapdragon 8 Gen 3, Adreno 750). The 1.1 baseline includes early Steprant builds and 1.7 tests. A long frame takes more than 50 ms.

| Version | What changed | Measured progress |
| :--- | :--- | :--- |
| **1.1 baseline** | Early Android builds and 1.7 tests | **18.2** long frames/min in the city and **20.7** outdoors. |
| **[1.8](docs/notes/steprant-1.8-release.md)** | SGSR2 + SMAA, Adreno LRZ, compact render targets and depth-only draws kept in the scene pass | Long frames fell to **6.2/min in the city (−66%)** and **10.0/min outdoors (−52%)**. **40% fewer render passes** and **50% less memory per pixel** on 8-bit targets. |
| **[2.0](docs/notes/steprant-2.0-release.md)** | Vulkan frame graph, exact render areas and elimination of unread stores | **4–9% less GPU time per frame** and **9–18% less render-pass memory traffic** versus 1.9. |
| **[2.1](docs/notes/steprant-2.1-release.md)** | Four-image swapchain, SMAA fallback and audio resync | **3.7 long frames/min** in normal play, down **80–82%** from the early samples. GPU time per frame was **30–33% lower** with SGSR2 than in the early native 1080p setup. |
| **2.1.5** | The game thread uses signal-driven waits to cut polling | Numara shipyard drew **28% less battery power** and used **25% less CPU** versus 2.1.4 on the same map. |
| **2.2** | Register promotion and native memory routines | Register save/restore call sites fell from **19,238 to 178 (−99%)** in generated code versus 2.1.6. |
| **2.4.8** | Ordered submission of heavy frames in batches and fewer Vulkan state changes per draw | At the station, **7–13% shorter frame times** in matched draw-count ranges and **55% fewer long frames** versus 2.4.5. |
| **[2.5 Stable](docs/notes/steprant-2.5-release.md)** | The tested 2.4.8 optimizations in the stable release, including the Vulkan, CPU, thermal and audio work | Versus 2.1, **23–28% higher FPS** and **96% fewer long frames** in heavy scenes, **22% lower phone power draw** and **zero observed audio dropouts**. See the comparison below for test conditions. |

### Measured improvements: 2.1 → 2.5

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

(1) The heavy-scene samples compare overlapping draw counts from 2.1 split-screen scenes and the 2.5 station entrance. (2) Power draw includes the phone screen and was measured unplugged across different sessions and scenes. Other 2.1 sessions measured 6.1–6.5 W, a 27–31% drop to 4.47 W.


### What is new since 2.1

| Area | In 2.5 |
| :--- | :--- |
| **Crowded scenes** | Heavy frames go to the GPU in pieces within the same GPU slot, without waiting in the middle of a frame, so the GPU starts while the rest is still being recorded. Same commands, same order, same picture. |
| **Per-draw work** | Repeated binds and state (descriptor sets, layouts, framebuffer, viewport, scissor, index buffer) left out of the frame-graph capture (−65% commands at the station); shared index storage bound once; only edited constant blocks restored. |
| **Heat and CPU** | The game's main thread no longer spins on zero-timeout polls (Numara shipyard −28% W, −25% CPU); threads waiting for the GPU, the idle command thread and the XMA decoder sleep until there is work. |
| **Game code** | Registers as function locals where provably safe (save/restore call sites −99%), native `memcpy`/`memset`, profile-guided build with register promotion. |
| **Audio** | Audio workers sleep instead of spinning and never lose a turn; elastic audio reserve; XMA frames assembled by bytes; the original audio mix is back. |
| **Memory and stability** | The game survives the background (foreground service); GPU memory in 64 MiB blocks; readback buffer created on first use; old textures released; fixed a crash after the battle with Mack. |
| **Interface and Tester** | Battery temperature in the performance overlay; quit prompt fixes from upstream; Tester with watts, °C per minute and profiling only in short bursts. |

Gameplay extras include fast-forward, adjustable encounter rates and EXP/SP boosts, plus save points that restore party HP and MP.

**Known in 2.5:** the most crowded scenes run at about 24–25 FPS (CPU-bound GPU command thread, about 10 µs per draw); hitches in pre-rendered movies; short pauses at map changes while the game loads; about 50 s of shader preparation at the first start after a driver change.

**Install:** Android 8.0+, ARM64 and Vulkan. Launcher **Lost Odyssey Steprant**, package `io.github.freefrank.lostodyssey.steprant`. 2.5 installs over 2.1 and every earlier Steprant build (same package and signing key) and keeps saves, settings and the shader cache; it coexists with the official app.

[Installation](docs/STEPRANT_ANDROID.md#installation) · [All Steprant releases](https://github.com/Steprant/LostOdysseyRecomp-Android/releases) · [Changelog](CHANGELOG.md) · [2.1 release notes](docs/notes/steprant-2.1-release.md) · [2.0 release notes](docs/notes/steprant-2.0-release.md) · [1.8 release notes](docs/notes/steprant-1.8-release.md)

Supply supported game data from your own discs. Game assets, executables, private inputs and generated game sources are not distributed in this repository. The runtime remains [GPL-3.0](LICENSE); dependencies retain their licenses, including [Qualcomm SGSR](thirdparty/licenses/SnapdragonGSR.txt). GPU drivers downloaded from the driver page come from their own projects under their own licenses.

Thanks to **Frank / freefrank**, the upstream contributors and the original community for Lost Odyssey Recomp and its platform support, and to **Mr. Purple** for the PurpleVK Turnip builds. [Official project](https://github.com/freefrank/LostOdysseyRecomp) · [Official downloads](https://github.com/freefrank/LostOdysseyRecomp/releases/latest) · [Upstream Discord](https://discord.gg/z2yPct6z2w) · [Support Frank](https://ko-fi.com/dotslash).

> The reference below is retained from the original project. Desktop/platform features and download links in that section describe **upstream**. Use the **Steprant APK 2.5** download above for this Android edition.

## Contents

- [Start playing](#start-playing)
- [Features](#features)
- [Controls](#controls)
- [Debug menu](#debug-menu)
- [Files and folders](#files-and-folders)
- [Command-line options](#command-line-options)
- [Reporting a problem](#reporting-a-problem)
- [Screenshots](#screenshots)
- [Development](#development)
- [Sponsors](#sponsors)
- [Credits and game data](#credits-and-game-data)

## Start playing

For the official upstream packages, download your platform from the [latest upstream release](https://github.com/freefrank/LostOdysseyRecomp/releases/latest).

| Platform | Package | First launch |
| :--- | :--- | :--- |
| Windows x64 | `LostOdysseyRecomp-windows-x64-<version>.zip` | Extract the whole ZIP to a writable folder and run `LostOdysseyRecomp.exe`. Needs a CPU with AVX. |
| Linux x64 | `LostOdysseyRecomp-linux-x64-<version>.AppImage` | Make it executable with `chmod +x`, then run it. |
| Linux x64 | `LostOdysseyRecomp-linux-x64-<version>.flatpak` | Install the Freedesktop 26.08 runtime, then the bundle ([commands](docs/INSTALLING.md#flatpak)). |
| macOS arm64 | `LostOdysseyRecomp-macos-arm64-<version>.dmg` | Drag `LostOdysseyRecomp.app` to Applications. Needs an Apple Silicon Mac with macOS 15 or later. |
| Android arm64 | `LostOdysseyRecomp-android-arm64-<version>.apk` | Install the APK and open it once. Needs a 64-bit Android 8.0+ device with Vulkan. |

On Steam Deck, use **Settings → System → Add to Steam** with either Linux package to add the game and its library artwork ([steps](docs/INSTALLING.md#steam-deck)). Artwork is generated from your game files; if that fails, the game uses bundled fallback artwork.

1. **Import your game data.** The importer opens when no game is found. Select an extracted game folder, `default.xex`, an ISO or GOD data.
2. **Get the shaders.** On the first start the game offers precompiled shaders for your renderer; if you skip, it compiles them once.
3. **Add the other discs and DLC** in Settings → **System → Import discs & DLC**. With all four discs imported, the game changes discs on its own.

Disc 1 is required to start. Use one supported four-disc set (Asian multilingual or USA/Europe) and don't mix editions; the [installation guide](docs/INSTALLING.md) shows how to check yours.

### macOS

The app is not notarized, so macOS blocks the first launch: try to open it once, then choose **Open Anyway** in **System Settings → Privacy & Security**. To update, replace the app with the new one. [Step-by-step guide](docs/INSTALLING.md#macos).

### Android

The Steprant package is `io.github.freefrank.lostodyssey.steprant`. It can be installed alongside the official app; private saves and settings are separate. Export and import saves as needed. An update to this fork must use the same package and signing key.

- **Game data:** opening the official app creates `Android/data/io.github.freefrank.lostodyssey/files/game/`; the Steprant package uses `Android/data/io.github.freefrank.lostodyssey.steprant/files/game/`. Copy your extracted `disc1`–`disc4` into the appropriate folder over USB, or import disc images with **Game folder → Import disc images…**. **CTRL → Game folder** can point the game at another folder or an SD card.
- **Qualcomm devices:** the **GPU driver** page opens before the first start. Download a Turnip driver there.
- **Mali devices:** the **GPU driver** page also opens first. Download the recommended PanVK driver there; it needs a Mali-G57, G68, G77, G78 or Mali-G610 and newer GPU.
- **Controls:** the touch controls hide when a controller is connected. **CTRL** sets their size, opacity and layout. **Hide all touch controls** hides them completely; reveal the system bars or use Android Back to show CTRL for 4 seconds.
- **Updates:** a new APK installs over the old one and keeps your saves.
- **Saves:** **CTRL → Saves and logs** exports saves to a ZIP and imports ZIPs from a PC, Xenia or the RGH save converter. Its **Keep saves in the game folder** option uses `<game folder>/save/` for syncing with a PC; it is off by default and applies at the next app start.
- **Logs:** **CTRL → Saves and logs → Export logs** saves them to a ZIP for a bug report ([details](docs/INSTALLING.md#android-logs)).

[Step-by-step guide](docs/INSTALLING.md#android).

### Nintendo Switch

Ad0ky maintains an experimental homebrew build for Switch consoles running custom firmware. It is not part of the releases here: download it from [Ad0ky's releases](https://github.com/Ad0ky/LostOdysseyRecomp/releases), which also have the install steps.

### HDR

Turn on **HDR** in Graphics and save. **HDR peak brightness** opens a calibration page; **LB / RB** switch between the game scene and a test pattern. HDR is available on Windows, Linux and macOS.

## Features

| Feature | What you get |
| :--- | :--- |
| Import | Folder, XEX, ISO and GOD sources, DLC and disc replacement. Your original files are not changed. |
| Languages | Menus in English, Japanese, Korean, Traditional Chinese and Simplified Chinese. Game languages depend on your edition. |
| Display | Windowed or fullscreen, monitor and GPU choice, 16:9 and 21:9 resolutions (taller screens such as 16:10 are filled), [HDR](#hdr) and **Brightness / Gamma**. Cutscenes and the pause overlay use a centred 16:9 area. |
| Anti-aliasing and upscaling | FXAA, SMAA, TAA, DLSS, FSR 3.1, XeSS (Windows Direct3D 12) and MetalFX (macOS). |
| Graphics options | Shadow resolution 1×/2×/4×, SSAO/GTAO, anisotropic filtering, depth of field and bloom. |
| Frame rate | 30/60/90/120 FPS; VSync Off, On or FreeSync / G-SYNC Compatible VRR. **Performance overlay** shows the frame rate, or a bar with the upscaler, frame generation, CPU and GPU load. |
| Loading speed | Graphics → **Loading speed**: Original keeps the Xbox 360 loading pace; Fast (default) loads at a higher frame rate. Effects play at normal speed either way. |
| Frame generation | DLSS, FSR or XeSS on Windows Direct3D 12; DLSS on Windows Vulkan. It pauses while cinematic black bars are shown and resumes in gameplay. |
| DLSS 5 neural rendering | Windows with DLSS on an RTX GPU. It needs `nvngx_dlssnr.dll`, which the game does not include: put your own copy (tested with 310.8.0.0) next to `LostOdysseyRecomp.exe` and restart. |
| Shaders | Precompiled shaders downloaded on the first start, or compiled once and cached. |
| Audio | Stereo, 5.1 surround, or matrix surround for an AV receiver. Cutscene dialogue has its own volume. |
| Input | Controllers, keyboard and rumble; touch controls on Android. |
| Mods | Windows only: texture, model, menu and font replacements, cutscene videos, text translations and language packs (which can add letters the game's fonts lack), and PlayStation button prompts, installed by hand or with Mod Organizer 2 (its plugins are in the Windows ZIP). Other platforms may load mods but are not supported. See the [modding guide](docs/wiki/Modding.md). Ready-made mods, such as the 4x HD texture pack, are shared in the **#mods** forum on [Discord](https://discord.gg/z2yPct6z2w). |
| Achievements | The game's 36 Xbox 360 achievements, with an Xbox 360-style notification and sound when you unlock one. **Settings → System → Achievements** lists them with your unlock dates and turns the notification on, silent or off. An `achievement.wav` next to `settings.ini` replaces the sound. |
| Discord status | Windows, Linux and macOS: Discord Rich Presence shows your location, disc and activity. **Settings → System → Discord status** is on by default; changes take effect immediately. If status is missing, check the runtime log for connection and activity diagnostics. |
| Debug menu | Render captures, Save Anywhere, encounter switches, teleport, fast-forward and cheats. See [Debug menu](#debug-menu). |

## Controls

Controllers and the keyboard work together for player 1. If your controller is not recognized, see the [input reference](docs/notes/controller-input.md).

| Game action | Keyboard |
| :--- | :--- |
| Start / Back | Enter / Backspace |
| A / B / X / Y | Z / X / A / S |
| D-pad / left stick | Arrow keys / I, J, K, L |
| Left / right shoulder | Q / W |
| Left / right trigger | E / R |
| Title menu: Settings (Y) | S |
| Title menu: Quit game (Back / View) | Backspace |
| Debug Menu | F1 |

These are the default keys. **Settings → Gameplay → Controls** changes the keys and the controller buttons, and sets the button prompts and the vibration strength.

Quitting from the title menu asks for confirmation, with **No** selected by default.

Press **Back** to zoom the minimap; hold it for about half a second to hide the minimap. For Ring actions, use the controller's **right trigger** or **R**.

## Debug menu

Press **F1**, or **LB+RB** on a controller (**L1+R1** on PlayStation layouts), to open or close the Debug Menu. The game pauses while it is open. It has three pages, **Overview**, **Teleport** and **Cheats**, and uses the keyboard or a controller.

| Action | Keyboard | Controller |
| :--- | :--- | :--- |
| Select a row | ↑ / ↓ | D-pad up / down |
| Change a value | ← / → | D-pad left / right |
| Confirm | Enter | A |
| Return or close | Esc | B |
| Previous / next page | Q or Tab / E | LB / RB |
| Change Cheats category | Select the category row, then ← / → | LT / RT |
| Open or close the overlay | F1 | LB+RB |

### Overview: captures and game actions

**Overview** shows the current map and has the menu language, **Capture render state**, **Save Anywhere**, **Return to title** (outside battles) and an action that wins the current battle.

- **Capture render state:** confirm it, then close the menu. The archive is saved in `captures/` with screenshots, rendering data and logs; look through it before sharing.
- **Save Anywhere** turns on the game's own **System → Save**.
- **Return to title** asks for confirmation before returning to the title screen. Unsaved progress will be lost. It is unavailable from the battle transition through the victory/results screens until control returns to the field or world map. Close the save/load screen and wait for saving or loading to finish before using it.

### Teleport: positions within the current map

**Teleport** has a position bookmark, editable X/Y/Z coordinates and the map's points of interest. On the coordinate row, press **Enter** to pick X, Y or Z and **←/→** to move it. Confirm, then close the menu to move.

**Debug Event Room**, at the bottom of the page, takes you to the game's own event-debug map (z0g_9). Hold **LB** and press **Up** there to open Scenario Jump.

### Cheats: speed and game-data tools

| Category | What it does |
| :--- | :--- |
| **Quick tools** | Fast-forward, **Encounter rate**, independent **Battle EXP ×2** and **Battle SP ×2**, menu animation speed, **Allow memory edits**, gold and HP/MP. |
| **Characters** | HP/MP, EXP (0–99, not the level) and skills. |
| **Inventory** | Set items and materials to 1, 10, 50 or 99, or fill whole categories. Sort the in-game inventory to refresh it. |
| **Equipment** | Weapon, ring and accessory changes. |
| **Party** | Party members, rows and field character. |
| **Developer** | The original **EDIT MENU**: enable it, close F1, press **LT+RT**. |

**Menu animations** sets camp menu opening, closing and submenu transitions to **Normal (1×)**, **Fast (2×)** or **Faster (4×)**. The choice is saved; **Normal** is the default. Gameplay and audio keep their usual speed.

**Fast-forward** runs at 2×–8× while you hold the selected button (**Hold**) or after you press it (**Toggle**). Set it in **Cheats** → **Fast-forward button** (next to **Speed mode**); choose LT (default) or LB. With LT selected, holding RT at the same time blocks fast-forward; with LB selected, RB does. **Fast-forward audio** defaults to **Keep pitch**; choose **Speed up** to raise the pitch with the speed. The choices are saved.

**Encounter rate** offers **Normal**, **Half**, **None** and **Every step**. Half skips every other random encounter; Every step starts a battle on each step in areas with random encounters. Story battles and bosses are unaffected. Every step is a temporary testing aid: restarting restores your last Normal/Half/None choice. **Battle EXP ×2** is a separate, saved switch for experience earned from won battles.

The EXP bonus applies after the game's level-based calculation and keeps its normal per-battle cap. **Battle SP ×2** doubles the victory SP award before skill learning consumes it; existing skill progress is unchanged. Both switches are saved independently. Gold is unchanged.

**Memory edits** are off by default. Back up your save, turn on **Allow memory edits**, choose an action, confirm **Yes** and close F1 to apply it.

## Files and folders

The Windows ZIP is **portable**: everything stays in the folder you extracted it to. The AppImage, Flatpak and macOS app keep your files in your user folders:

| Package | Settings | Saves, cache and game data | Logs |
| :--- | :--- | :--- | :--- |
| Windows ZIP | folder with `LostOdysseyRecomp.exe` | same | same |
| Linux AppImage | `~/.config/lost-odyssey-recomp/` | `~/.local/share/lost-odyssey-recomp/` | `~/.local/state/lost-odyssey-recomp/` |
| Linux Flatpak | `~/.var/app/io.github.freefrank.LostOdysseyRecomp/config/lost-odyssey-recomp/` | `~/.var/app/io.github.freefrank.LostOdysseyRecomp/data/` | `~/.var/app/io.github.freefrank.LostOdysseyRecomp/.local/state/lost-odyssey-recomp/` |
| macOS app | `~/Library/Application Support/LostOdysseyRecomp/` | same | `~/Library/Logs/LostOdysseyRecomp/` |
| Steprant Android | inside the app | saves inside the app by default, optionally `<game folder>/save/`; game data in `Android/data/io.github.freefrank.lostodyssey.steprant/files/game/` or the chosen folder | `Android/data/io.github.freefrank.lostodyssey.steprant/files/logs/` |

| What | Where | Notes |
| :--- | :--- | :--- |
| Saves | `save/` | Keep when updating. Xenia and Xbox 360 saves can be [imported](docs/INSTALLING.md#importing-saves). |
| Profiles | `profile/` | Keep when updating. |
| Settings | `settings.ini` | Delete it to go back to the default settings. |
| Imported game | `game/` with `disc1/`–`disc4/` and `dlc/` | The importer's default location; `game-path.txt` remembers another one. |
| Shader cache | `cache/shaders/` | Rebuilt if deleted. |
| Downloaded shaders | `shaders/` | |
| Logs | `logs/runtime-*.log` | The last three runs are kept. |
| Render captures | `captures/` | |
| Mods | `mods/` | |

More detail is in [file locations](docs/INSTALLING.md#file-locations).

## Command-line options

| Option | Effect |
| :--- | :--- |
| `--game <path>` | Use this game folder (with `default.xex` or `disc1/`) or `default.xex` file, skipping the importer. |
| `--install` | Open the importer even when a game is already set up. |
| `--add-to-steam` | Add the game to Steam with library artwork, then exit with a status code. Close Steam first; add it again if you move the game. |
| `--setup` | Windows: open the setup page, then start the game. |
| `--prepare-shaders-only` | Prepare all shaders, then exit without starting the game. |

```bash
LostOdysseyRecomp.exe --game "D:\Games\Lost Odyssey"
./LostOdysseyRecomp-linux-x64-<version>.AppImage --game ~/Games/LostOdyssey
flatpak run io.github.freefrank.LostOdysseyRecomp --game ~/Games/LostOdyssey
```

| Environment variable | Effect |
| :--- | :--- |
| `LO_GRAPHICS_API` | `d3d12` or `vulkan` on Windows. |
| `LO_FPS` | Frame-rate cap; `0` means uncapped. |
| `LO_FAST_LOADING=0/1` | Override Loading speed: `0` selects Original pacing; `1` selects Fast. Without the override, `loading_transitions=0/2` in `settings.ini` stores Original/Fast. Fast loading on Android and devices with a detected battery is capped at 4× the selected limit, up to 240 FPS (240 FPS if uncapped). |
| `LO_NO_UPDATE=1` | Skip the update check. |
| `LO_MODS=0` | Disable mods. |
| `LO_AUDIO_MUTE=1` | Mute audio. |
| `LO_CONTROLLER_RUMBLE=0` | Turn rumble off. |
| `LO_OPTISCALER_PATH` | Load your own `OptiScaler.dll` on Windows ([setup](docs/notes/vulkan-fg-fsr4-metalfx.md#optional-optiscaler-loading-on-windows)). |

## Reporting a problem

For a fork-specific problem, include the Steprant version and follow the [Android reporting guidance](docs/STEPRANT_ANDROID.md#testing-and-contributions). Confirm a problem also occurs in an official build before [opening an upstream issue](https://github.com/freefrank/LostOdysseyRecomp/issues) with the version, operating system, graphics backend, GPU and driver, game edition and disc, and the steps or scene that show the problem. Attach the newest `logs/runtime-<timestamp>.log`. **Settings → System → Debug log** is on by default. If you turned it off, turn it back on, save and restart before showing the problem again.

For a visual problem, make a [render capture](#overview-captures-and-game-actions) while it is on screen. Do not attach game files, saves or personal data.

Optional log collection is off by default; see [Privacy](PRIVACY.md).

## Screenshots

<img src="docs/images/title-screen.png" alt="Lost Odyssey title screen — Press START" width="960">

| Ring combat | City exploration |
| :---: | :---: |
| ![Kaim attacking with the Ring timing interface](docs/images/ring-battle.png) | ![Exploring the industrial city](docs/images/city-exploration.png) |

## Development

See [Building](docs/BUILDING.md), [Developer tools](tools/README.md), the [roadmap](docs/ROADMAP.md) and the [documentation index](docs/README.md).

## Sponsors

Thank you to **CarlosVonSexron**, **Atlas**, **stratosG**, **iambRad**, **Frenzy Fresh**, **José Antonio Martínez Godoy**, **C_BAR**, **Arakon**, **Efren V**, **Torresmo**, **doc_haz**, **Whitesun** and **Cristian** on [Ko-fi](https://ko-fi.com/dotslash), and to **Norghat**, **Morgan** and one anonymous supporter on [Buy Me a Coffee](https://buymeacoffee.com/dotslash), for supporting the project.

## Credits and game data

Thanks to everyone who sent pull requests and patches: [MikeRavenelle](https://github.com/MikeRavenelle), [dj5927](https://github.com/dj5927), [Xarishark](https://github.com/Xarishark), [navjack](https://github.com/navjack), [frankzzz](https://github.com/frankzzz), [cngjd](https://github.com/cngjd), [faisalkindi](https://github.com/faisalkindi) and [Ad0ky](https://github.com/Ad0ky). The macOS port started from MikeRavenelle's Apple Silicon work, and Ad0ky ported the game to the Nintendo Switch on the `LostOdysseyNX` branch.

With research and tools from [UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp), [re:Blue](https://github.com/zolaware/reblue), [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), [XenosRecomp](https://github.com/hedge-dev/XenosRecomp), [plume](https://github.com/renderbag/plume) and [Xenia](https://github.com/xenia-project/xenia). Audio uses the [Xenia FFmpeg fork](https://github.com/xenia-project/FFmpeg) ([license](thirdparty/ffmpeg-LICENSE.txt)). The Android Turnip GPU drivers come from the driver list of the [Eden](https://git.eden-emu.dev/eden-emu/eden) emulator and load through [libadrenotools](https://github.com/bylaws/libadrenotools); the Mali PanVK driver comes from [panvk-kbase-android](https://github.com/zenithblue-oss/panvk-kbase-android).

Lost Odyssey and its assets belong to their respective owners. This is an unofficial project. Supply data extracted from your own discs; do not commit game executables, resource archives, textures, audio, video, generated game code or captures to this repository. Dependencies retain their respective licenses.
