# SGSR2 image: render size from the resolution row, SMAA status, Soft filter (Steprant 2.0, WP7)

Bugs reported by Steprant on 2026-11-08 (TV session 18:50): SGSR2 stayed at
720p → 1080p on the TV and 800 lines on the phone with the row at 900p;
heavy aliasing in Quality; wish for a softer image.

## 7.1 The render resolution row decides SGSR2's render size

- `gpu/frame_plan.h` `Sgsr2InputCap(upscaler, optimal, row, outW, outH, quality)`:
  with SGSR2 and a fixed row (576/720/900/1080/1440/2160) the render size is
  that many lines at the output's shape; a row at or above the output renders
  at the output size. Row value 0, shown as **Auto** with SGSR2 ("Follow
  output" for the other modes), keeps the quality ratio (1.5/1.7/2). Native
  AA always renders 1:1. With a fixed row the quality only decides SMAA
  (Quality) or not (Balanced, Performance).
- Examples: 900p → 1600×900 on a 1080p TV (1.2×), 2003×900 on the
  2670×1200 panel (1.33×); 720p → 1280×720 on the TV, 1602×720 on the panel.
- TV mode (`gpu/frame_plan.cpp` `BeginCpuFrame`): the TV fixes the output;
  with SGSR2 the row is no longer replaced by the TV height. Other modes keep
  the previous behaviour (render at the TV height). The TV output region is
  never supersampled, as before.
- The scene mip bias follows the real scale automatically (renderer
  `SrMipBiasCode`, log2(render/output) in quarter steps): 1600/1920 →
  −0.25 (log2 is −0.26), 2003/2670 → −0.5.
- Expected log in TV: `Sgsr2: frame=… input=1600x900 output=1920x1080`, the
  `renderer: scene texture mip bias -0.25 (render 1600x900, output 1920x1080)` line.
- Cost: more render pixels (+56% at 900p versus 720p on the TV, +27% on the
  phone versus 1780×800); to be measured on the phone. This is also the first
  point of the pixel-scale experiment of WP-A.1.

## 7.2.2 SMAA status and other input formats

- Before: SMAA ran only when the SGSR2 colour input was RGBA8, and the log
  said so once. A scene in a 10-bit or float target had no SMAA, silently.
- Now `sgsr2_upscaler.cpp` blends RGBA8 in an 8-bit stage (as before) and
  RGB10A2 or RGBA16F inputs in a float stage, so the input never loses
  precision. One line whenever state, format or size changes:
  `SGSR2: smaa=on format=rgba8 requested=yes encoding=sdr filter=standard input=… output=…`.
- The performance overlay names it: `SGSR2`, `SGSR2+SMAA`, `SGSR2 SOFT`,
  `SGSR2+SMAA SOFT` (`gpu/sgsr2_status.h`).

## 7.3 Soft filter

- The FSR sharpness row (hidden with SGSR2) becomes **SGSR2 filter:
  Standard / Soft** with SGSR2 (`settings.ini sgsr2_filter`, applied live).
- Soft (`sgsr2_source.h` `SoftKernel`): a Gaussian of the same argument as
  Qualcomm's Lanczos-shaped weight, same peak (0.25) so the summed weight
  that drives the history blend stays comparable, no negative lobe;
  window 0.7 (`kernelScale`); scene mip bias halved. Same samples: no extra
  GPU cost. SMAA unchanged.
- Lavapipe (`tools/tests/sgsr2`, reset frame on a hard edge): pixels past the
  plateaus Standard 0.051, Soft 0.0012 (no halo); flat colour preserved.
- Decision D7 (default Standard or Soft) after screenshots on the phone and TV.

## Tests

- `frame_plan_test` (FG CPU contracts): bases for every row on the phone and
  the TV, Auto, Native AA, other upscalers.
- `tools/tests/sgsr2` (lavapipe): float SDR input with SMAA matches the
  8-bit case (edge rmse 0.1086 → 0.0874), status reported, Soft filter halo
  and flat colour.
- Settings menu suites (labels, help text widths, translations).
- Not yet measured or seen on the phone.

## WP5 G6 / WP7.2.5 — trails without motion vectors (anti-trail)

- With a still camera SGSR2's clamp box is several times the neighbourhood's
  spread (`scaleFactor` up to 20), so a particle or an alpha-tested edge that
  moved without motion vectors stays in the history as a trail.
- `sgsr2_source.h`: when the history's luma lies outside the range of every
  current neighbour by more than 0.12, it is clamped to the neighbours' own
  range and the current frame is taken whole. A full reactive mask (one more
  render target written by the transparent draws) is not needed for this.
- Developer switch *SGSR2 reactive mask* (`LO_SGSR2_REACTIVE=0` restores the
  reference box).
- Lavapipe: a bright particle moving over still ground without motion
  vectors leaves 0.049 of trail with the reference box, 0.000 with the rule;
  on the same jittered frame sequence the detail scene's error is unchanged
  (0.1103 against 0.1102).
