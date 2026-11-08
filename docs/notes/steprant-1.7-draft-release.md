# Lost Odyssey Steprant Android 1.7 — optimization test build (draft, not published)

Built by the Android APK workflow from `integration/v1.7-optimization-20261108`. Package `io.github.freefrank.lostodyssey.steprant`, versionName 1.7, signed with the same certificate as 1.5 and 1.6 (SHA-256 `ecaadcdc715ff7e4aeaaf265ec11f1854d4039175dbc7e53fd4f86fd03af6f3f`), so it installs over them.

## Changes since the 1.6 candidate

Optimization only: no new features and no intended change to the image.

- Android runtime built for ARMv8.2-A (inline LSE atomics).
- Render passes no longer load attachments whose contents are undefined (Tester A/B row "Skip loading undefined attachments").
- Diagnostics: the Tester's GPU profile now says what became of each pass's stored attachments (read, loaded again, or cleared unread), why passes end, and how many state commands each frame records. `tools/android/compare-tester.py` compares two Tester sessions.

Details: `docs/notes/android-optimization-20261108.md`.

Game data is supplied by the player; this APK contains no game executable or assets. GPL-3.0; dependency notices are included in the app.
