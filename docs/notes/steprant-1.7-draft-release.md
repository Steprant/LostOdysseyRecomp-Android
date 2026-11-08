# Lost Odyssey Steprant Android 1.7 — optimization test build (draft, not published)

Built by the Android APK workflow from `integration/v1.7-optimization-20261108`. Package `io.github.freefrank.lostodyssey.steprant`, versionName 1.7, signed with the same certificate as 1.5 and 1.6 (SHA-256 `ecaadcdc715ff7e4aeaaf265ec11f1854d4039175dbc7e53fd4f86fd03af6f3f`), so it installs over them.

## Changes since the 1.6 candidate

Optimization only: no new features and no intended change to the image.

- Android runtime built for ARMv8.2-A (inline LSE atomics).
- Render passes no longer load attachments whose contents are undefined (Tester A/B row "Skip loading undefined attachments").
- Diagnostics: the Tester's GPU profile now says what became of each pass's stored attachments (read, loaded again, or cleared unread), why passes end, and how many state commands each frame records. `tools/android/compare-tester.py` compares two Tester sessions.

- Render passes begin over the draws' scissor instead of the whole target, so partial passes (shadow cascades, sub-rectangles of EDRAM targets) load and store only that part (Tester A/B row "Render passes limited to the scissor").
- The boot title screen is promoted to the output size with SGSR2 again; SSAO no longer draws stripes on the sky and distant terrain.
- Diagnostics: slow frames (over 50 ms) are broken down in the log while the Tester records; the GPU profile names the transitions of barriers that end passes; the existing CPU performance hints (ADPF) can be turned on from the Tester.
- Android build fetches nlohmann/json by git tag (the release archive CDN was failing).
- Frame pacing follows the display when the present blocks (the city stutter).
- The render area grows at most once per framebuffer (the shadow map regressed with step-by-step growth).
- The Tester's A/B rows are removed; the confirmed behaviour is built in.

Details: `docs/notes/android-optimization-20261108.md`.

Game data is supplied by the player; this APK contains no game executable or assets. GPL-3.0; dependency notices are included in the app.
