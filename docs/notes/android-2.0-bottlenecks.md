# Bottleneck map (Steprant 2.0, WP-A) — instruments and protocol

WP-A decides which heavy packages (P2: WP1 frame graph, WP2 grouped draws,
WP-C two command threads, WP4) the 2.0 gets. Nothing heavy starts before
its table exists. This note lists what the 1.9 test builds measure and how a
session becomes the table; the table itself is filled from Steprant's
sessions (time limit: two test builds).

## Instruments (all in the runtime log of a Tester recording, profile Advanced)

| # | Question | Instrument | Log |
|---|---|---|---|
| 1 | Pixels and shaders | the render resolution row now sets SGSR2's base (WP7.1): same scene at 576, 720, 900, 1080 | `Sgsr2: … input=WxH`; `gpu passes` pass_ms/outside_ms |
| 2 | Hardware counters | `VK_KHR_performance_query` probe (read-only) at startup; the counter list if exposed | `vulkan profiling: VK_KHR_performance_query=yes|no counters=N VK_EXT_multi_draw=…`, `vulkan profiling counters …` |
| 3 | Memory traffic | developer switch *Discard dead stores (test only)* (`LO_VK_DIAG_DISCARD_DEAD_STORES=1`): stores whose previous store was cleared unread become DONT_CARE; never shipped (a wrong prediction corrupts that texture for a frame) | `frame graph what-if: … diag_discarded_stores= diag_discarded_mb=`; GPU ms against the twin session |
| 4 | Real cost per draw | developer switch *Per-draw timestamps* Off (`LO_DRAW_TIMESTAMPS=0`): sampled frames without per-draw timestamps, least-squares of frame GPU time against draw count | `draw classes (session, untimed …) fit: us_per_draw= fixed_ms= r2=` |
| 5 | CPU at 60 FPS | GPU command thread CPU per swap, split into recording (renderer::Draw incl. EDRAM resolves, Flush) and parsing (the rest), thread CPU time | `gpu passes … cmdproc_per_frame: cpu_ms= record_ms= parse_ms= max_cpu_ms=` |
| 6 | Waits | present sub-phases (WP3.1) and the existing slow-frame breakdown | `slow frame …` |
| — | Frame graph stage 1a | what a frame graph could save without changing commands: dead stores, passes resuming the same framebuffer after unrelated barriers (merge), copies already moved to pass end | `frame graph what-if: traffic_mb= dead_store_mb= merge_passes= merge_mb= savable_mb= savable_share=` |

## Sessions to record (same route stop, 3 minutes each, 30 FPS unless said)

City (stop 3 of the route), field (stop 1), battle (stop 2), TV (stop 8):

1. base 576p, 2. base 900p (reference), 3. base 900p with *Discard dead
stores* On, 4. base 900p with *Per-draw timestamps* Off, 5. base 576p at
60 FPS (CPU budget). Switches apply after restarting the game.

```
python3 tools/android/bottleneck-map.py city-576=a.zip city-900=b.zip \
    city-900-deadstores=c.zip city-900-untimed=d.zip city-60fps-576=e.zip \
    field-900=… tv-900=…
```

The script prints the table «dove vanno i ms» per scene and applies the D8
rules of Super Roadmap 2:

- WP1 if what-if share ≥ 25%, or ≥ 8 mergeable passes per frame, or the
  measured dead-store saving ≥ 3 ms — and traffic must cost real GPU time
  (the dead-store session measures it);
- WP2 if the real cost per draw ≥ 5 µs and ≥ 3 ms per frame;
- WP-C if the command thread exceeds 14 ms per frame at 60 FPS;
- WP4 if pixel-dependent time is at least half the GPU frame;
- otherwise only fluidity (P3) and image (P4).

## Tests (host and lavapipe)

- `frame_graph_whatif_test` (lavapipe): one resume after an unrelated
  barrier is mergeable, not after its own attachment's barrier; with the
  diagnostic switch the predicted dead stores are stored with DONT_CARE and a
  sampled texture never is.
- `pass_profile_test`: the two lines and the what-if arithmetic.
- `draw_class_profile_test`: untimed sampling and the least-squares fit.
- `compare_tester_test.py`, `test_bottleneck_map.py`: parsing and decisions.
- Nothing here is measured on the phone yet.
