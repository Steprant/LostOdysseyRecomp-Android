# Steprant Android 2.1

**[Download APK 2.1](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/download/v2.1-steprant.20261112-Release/LostOdyssey-Steprant-v2.1-Release-20261112.apk) · [Release](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/tag/v2.1-steprant.20261112-Release) · [Full release notes](notes/steprant-2.1-release.md)**

This Android-focused edition builds on [Lost Odyssey Recomp](https://github.com/freefrank/LostOdysseyRecomp) by **Frank / freefrank and the original community**, from its official **v1.1.0** base. **2.1** is Steprant's app version.

## What's included

The [2.1 release notes](notes/steprant-2.1-release.md) describe the changes and the benchmarks.

- **SGSR2 temporal upscaling** (Settings → Graphics → Anti-aliasing → SGSR2) with SMAA in Quality mode and a SMAA fallback when SGSR2 cannot run. A fresh install defaults to 900p, SGSR2 Quality, shadows ×2 and 30 FPS.
- **SGSR1** spatial upscaling in the Vulkan SDR presentation path (Settings → Graphics → SGSR1).
- **Rendering:** a frame graph drops memory stores that later passes never read and groups copies and transitions at the end of render passes.
- **Frame pacing:** Android uses four swapchain images to reduce stalls during presentation.
- **Audio:** AAudio and XMA fixes, a 64 ms buffer and audio resynchronization after stalls.
- **TV and HDMI mode** ([guide](ANDROID_TV_HDMI.md)), **Tester** diagnostics ([guide](ANDROID_TESTER.md)), **EXP up to ×10, SP ×2 and 0.75× encounter rate** in Quick Tools, MediaCodec replacement movies.

## Installation

Install **`LostOdyssey-Steprant-v2.1-Release-20261112.apk`** from the download above. It requires Android 8.0+, ARM64 and Vulkan. The launcher name is **Lost Odyssey Steprant** and the package is `io.github.freefrank.lostodyssey.steprant`. It installs over earlier Steprant builds with the same signing key (certificate SHA-256 `ecaadcdc715ff7e4aeaaf265ec11f1854d4039175dbc7e53fd4f86fd03af6f3f`).

The app can coexist with the official package. Settings and saves are separate: export saves from the original app and import them into the fork if needed. Supply your own supported game data and import it as described in the [installation guide](INSTALLING.md#android). Do not include game files or private saves in bug reports.

## Recommended GPU driver on Adreno

Open **CTRL → GPU driver**, select **Download recommended: Mr. Purple T30**, then restart. The first start after a driver change prepares shaders for about 50 seconds. The driver was tested on Adreno 750.

## Testing and contributions

The 2.1 benchmarks used a Xiaomi 14 (Adreno 750) with Mr. Purple T30. To compare builds, keep scenes, settings, driver and starting temperature consistent. Record with Tester and compare the exported ZIPs with `tools/android/compare-tester.py`.

For a problem, record the Steprant version, Android version, device/driver, settings, scene and reproduction steps. Review exported diagnostics for private information before sharing.

The runtime retains its [GPL-3.0 license](../LICENSE), dependency notices and original community credits.
