# Steprant Android 1.8

**[Download APK 1.8](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/download/v1.8-steprant.20261108/LostOdyssey-Steprant-v1.8-20261108.apk) · [Release](https://github.com/Steprant/LostOdysseyRecomp-Android/releases/tag/v1.8-steprant.20261108) · [Full release notes](notes/steprant-1.8-release.md)**

This Android-focused edition builds on [Lost Odyssey Recomp](https://github.com/freefrank/LostOdysseyRecomp) by **Frank / freefrank and the original community**, from its official **v1.1.0** base; **1.8** is Steprant's app version. Its goals are smoother sustained play, sharper upscaling and less redundant CPU/GPU work on phones.

## What's included

The [release notes](notes/steprant-1.8-release.md) list every change since the first Steprant build. In short:

- **SGSR2 temporal upscaling** (Settings → Graphics → Anti-aliasing → SGSR2, quality row), with SMAA before the upscale in Quality; the default on a fresh install is 900p, SGSR2 Quality, shadows ×2, 30 FPS.
- **SGSR1** spatial upscaling in the Vulkan SDR presentation path (Settings → Graphics → SGSR1).
- **Rendering work** for tile-based GPUs: compact render targets, fewer render passes, render passes limited to the drawn area, EDRAM resolves that no longer split passes, early depth rejection.
- **Frame pacing:** far fewer frames over 50 ms than the 1.7 test builds in the measured scenes.
- **Audio:** AAudio and XMA fixes, 64 ms buffer.
- **TV and HDMI mode** ([guide](ANDROID_TV_HDMI.md)), **Tester** diagnostics ([guide](ANDROID_TESTER.md)), **EXP ×2 / SP ×2** quick tools, MediaCodec replacement movies.

## Installation

Install **`LostOdyssey-Steprant-v1.8-20261108.apk`** from the download above. It requires Android 8.0+, ARM64 and Vulkan. The launcher name is **Lost Odyssey Steprant**, package `io.github.freefrank.lostodyssey.steprant`; it installs over earlier Steprant builds (same signing key, certificate SHA-256 `ecaadcdc715ff7e4aeaaf265ec11f1854d4039175dbc7e53fd4f86fd03af6f3f`).

The app can coexist with the official package. Settings and saves are separate: export saves from the original app and import them into the fork if needed. Supply your own supported game data and import it as described in the [installation guide](INSTALLING.md#android). Do not include game files or private saves in bug reports.

## Testing and contributions

Performance numbers in the release notes come from one device (Xiaomi 14, Adreno 750, Turnip Mesa). For comparisons, use identical scenes, settings, driver and starting temperature, record with the Tester (Advanced profile) and compare the exported ZIPs with `tools/android/compare-tester.py`.

For a problem, record the Steprant version, Android version, device/driver, settings, scene and reproduction steps. Review exported diagnostics for private information before sharing.

The runtime retains its [GPL-3.0 license](../LICENSE), dependency notices and original community credits.
