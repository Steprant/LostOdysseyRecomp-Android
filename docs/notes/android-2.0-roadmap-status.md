# Steprant 2.0 — status of the Super Roadmap 2 packages (test builds 1.9)

Branch `steprant/v2.0-dev`. The full plan and its status live in the project
document "Super Roadmap 2"; the technical notes of each package:

| Package | Note |
|---|---|
| Quick tools EXP ×3/×4, encounters 0.75× | `android-2.0-quick-tools.md` |
| WP0 tools, developer page, frame samples | `android-2.0-route.md` (route), this branch's CHANGELOG |
| WP7 SGSR2 base, SMAA status, Soft filter; WP5 G6 anti-trail | `android-2.0-sgsr2-image.md` |
| WP-A bottleneck map, WP1 stage 1a what-if | `android-2.0-bottlenecks.md` |
| WP3 frame and audio flow (first steps) | `android-2.0-flow.md` |

Decision D8 (9/11, logs 17:51, 18:29, 19:25, TV 1600x900 → 1920x1080):
GPU 15.7 ms per frame (13.4 in passes, 2.3 outside), 57–66% busy at 30 FPS;
what-if savable traffic 15.8% with 2.2 mergeable passes; the dead-store
discard experiment discarded 46 MB per frame and saved no GPU time (+1.6 ms
against the twin sessions, different scenes); GPU command thread 11.3–11.9 ms
(record 6–7, parse 4–6). By the WP-A table no P2 package passes its gate:
2.0 goes on P3 and P4 (fluidity, image). No untimed per-draw session and no
60 FPS session were recorded; WP2 and WP-C stay closed unless such a session
changes the numbers.

Second 1.9 build (9/11 evening): EXP ×10, fast-forward in battle, save point
healing, D6 settings cleanup, developer page removed, settings menu raster on
a worker (WP3.4), audio stall lines without app pauses.

Third 1.9 build (9/11 night):
- WP3.5: the "4–5 s block after settings saved" was the Quick tools / F1
  overlay, which pauses the game and saves settings.ini. Every pause, resume,
  background and foreground bumps `host_ui::SuspendEpoch()`; slow frame lines
  over such an interval end with `game_paused=1`, the Tester frame CSV has a
  `game_paused` column, and the session summary and `compare-tester.py` leave
  those intervals out (`paused_intervals`).
- WP5: the shadow projection (VS 99c2) the game feeds NaN in c2..c5 on the
  first frame after a load is skipped (`renderer: skipped shadow projection
  with non-finite c2..c5`); suspected cause of the black spot after loads.
  `LO_SKIP_NAN_SHADOW_PROJECTION=0` draws it.
- WP7.2.3 classified, nothing left to compensate: `unjittered_known=0` in all
  three logs; every unknown-camera draw that tests depth already gets the SR
  viewport jitter; the rest (depth test off: rect lists and quads with
  blending) are screen-space overlays and effects.
- WP3.1 deferred: slow frames whose slowest stage is `present-fence` were 18 in
  about 4,300 s of logs; the remaining stalls are the game's own loads
  (`cp_idle` ~250 ms at each battle start).

Open: 3.2/3.3 game stalls at loads, 3.7 (60 FPS per scene); D4; capture of the scene before SGSR2
(WP0); D7 default filter; WP6 release. Nothing in 1.9 is measured on the
phone yet beyond the logs above.
