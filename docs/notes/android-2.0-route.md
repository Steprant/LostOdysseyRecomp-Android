# Fixed test route (Steprant 2.0, WP0.2)

Every 1.9.x test build is measured on the same route, with the same saves,
the same settings and the Tester in **Advanced**, so that sessions compare
with `tools/android/compare-tester.py` and frame samples with
`tools/android/compare-frames.py`.

## Settings (unless a test says otherwise)

Render resolution 900p, Anti-aliasing SGSR2, SGSR2 quality Quality, SGSR2
filter Standard, shadow resolution ×2, 30 FPS, performance overlay Off (it is
drawn into the presented image), Tester profile Advanced, sample period
1000 ms. From 1.9 the 900p row really renders 900 lines with SGSR2
(2003×900 on the phone, 1600×900 on the TV): compare with 1.8 at the
same *render size*, not only the same row, when the GPU time matters.

## Route (about 15 minutes; one save per stop)

| # | Stop | What to do | Captures |
|---|---|---|---|
| 1 | **Field**, Ipsilon Mountains (maps 15/16/20): valley road | walk the same 60 s loop | 4 frames standing still, camera untouched |
| 2 | **Battle** in the same area | one complete random battle | — |
| 3 | **City**, Uhra residential area (maps 54/55): stairs and walls on the right | walk to the inn and back | 4 frames standing at the stairs |
| 4 | **Camp menu** over the city | open, wait 5 s, close | 4 frames with the menu open (static) |
| 5 | **Settings menu** | open and close twice | — |
| 6 | **Scene change**: Uhra → Great Gate | one transition | — |
| 7 | **Great Gate cutscene** (Jansen with the girls, the pirate girl's lines) | let it play | — |
| 8 | **TV mode** (HDMI 1080p) | repeat stops 1 and 3 for 2 minutes | 4 frames at stop 3 |

## Frame samples (developer page, test builds only)

Android diagnostics → *Capture 4 frames (developer)* stores the presented
image and the resolved frontbuffer of the next four presented frames,
losslessly, with a hash; the Tester export puts them under `frame-samples/`.
A static moment (stop 4, or standing still with the camera untouched) gives
deterministic frames; moving scenes depend on timing and are not expected to
be identical between two runs (WP0 acceptance: document which stops are).

## Reference

A 1.8 session on this route (with the same settings) is the baseline for
every 2.0 package. Steprant records it once with 1.8; 1.8 has no frame-sample
capture, so its frame references come from the first 1.9 build with every
developer switch at its default (the 1.8 behaviour).
