# Frame and audio flow (Steprant 2.0, WP3) — first build

What the first 1.9 test build changes and measures for the stutters of the
8/11 sessions. Everything here is host-compiled and unit-tested where a unit
exists; nothing is measured on the phone yet.

## 3.1 What the generic "present" stage waits on

`gpu/presentation_progress.h` gains sub-stages, so a slow frame's
`present_slowest=<stage>:<ms>` names the part instead of `present`:

| stage | what |
|---|---|
| `menu-draw` | the settings menu's CPU raster (`settings::DrawMenu`) |
| `settings-overlay` | copying the menu over the game frame |
| `achievement-toast` | the notification overlay |
| `cpu-frame-cache` | the host menu's CPU frame cache |
| `upload` | reserved for the pixel upload path |

The fence ring (several presentation command lists in flight) is **not** in
this build: the overlays reuse one upload buffer per frame and would need one
per frame in flight. It is planned once the sub-stages show that
`present-fence` waits are serialisation, not GPU time (scene changes).

## 3.4 Interface stutters

- Achievement notification (88 ms `record` on 8/11): the icon decode (first
  call parses the achievement list), the text raster and the chime are
  prepared on a worker; the toast shows a few frames later.
  Developer switch *Prepare menu resources ahead* (`LO_UI_PREWARM=0` reverts).
- Settings menu (113–137 ms per open): the backdrop is already cached per
  size; the remaining cost is measured by the new `menu-draw` stage before
  anything is moved off the presentation thread.

## 3.5 The 4.15 s block at the Great Gate tower

The log shows `settings saved` just before it: a settings change (render
resolution, shadows) rebuilds targets and pipelines at once. To confirm on
the route (stop 6) with the sub-stages; no change in this build.

## 3.8 Audio in cutscenes (Great Gate, Jansen and the girls)

- `audio callback stall: <ms> xma_wait_max_us= xma_wait_audio_max_us=
  xma_hold_max_us= xma_contended_audio= last_file='…' reserve_frames=` for
  every guest audio callback over 50 ms (32 per session): XMA lock waits and
  holds during it, and the file the game last opened.
- Streamed audio prefetch: when the game opens a `.xwv`, `.xma`, `.wma`,
  `.xwb` or `.wav` file, a background thread reads it once (1 MiB pieces,
  two files at a time, up to 96 MiB) so the game's synchronous reads hit the
  page cache; `audio prefetch: <file> <MiB> in <ms>`.
  Developer switch *Read cutscene voices ahead* (`LO_VOICE_PREFETCH=0` off).
- Not in this build: the callback itself is game code, so "no locks, no I/O
  in the callback" is approached from the host side (prefetch, diagnostics);
  re-aligning audio after a stall waits for the stall line's evidence.

## 3.2 Thread placement A/B

Developer switch *Game threads on big cores* (`LO_PIN_GAME_THREADS=1`):
guest main on the prime core, the GPU command thread on the fastest big
cores (`thread placement: … pinned to N core(s) of capacity C`). Off by
default; compare city sessions with `compare-tester.py`.

## Tests

Compile checks of the changed translation units (desktop and Android
branches); the presentation stage names are covered by the existing
`presentation_progress_test` (stage count follows the enum).
