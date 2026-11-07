# XMA decode-failure bursts (A1) — 2026-11-07

## Evidence

Every Tester recording of 7/11 (18:21, 18:41, 18:45) has bursts of about
12,000–14,000 XMA decode failures within one second (`frame_timing.csv`,
`xma_failures`), always in frames of 1,177 draws: probably one place or one
sound. The 18:12 recording also has 91 empty audio-queue events (A2).

## Why a failure could repeat so fast

`ProcessPacket` assembles one XMA frame from the 2 KB packets and hands it
to FFmpeg. On a failed decode it returned without output, already past the
frame, and `Work` called it again at once: the next "frame" was parsed from
the following bits. When the frame boundaries are lost (or the decoder
keeps failing), every subsequent parse fails as well, and since a failure
produces no output the loop never fills the output ring that bounds it: the
worker runs through the whole input at full speed, thousands of failures a
second, while the voice is silent or garbled.

## Change

- **Diagnostics (one line per burst).** Per context, more than 100
  failures within one second is a burst. Its first failure past the
  threshold logs `xma decode burst: context= stream=<input buffer address>
  buffer= packet= bit_offset= frame_bytes= stage= send_result= result=
  consecutive= same_packet= frames_sent= rate= stereo= loop= reason=
  action=` (`result` is FFmpeg's code, `reason` the decoder's message), and
  the end of the window logs `xma decode burst ended: context= failures=
  resyncs= silenced=`. The existing first-16 detailed reports and the
  optional packet capture (`LO_XMA_ERROR_CAPTURE_DIR`) are unchanged.
- **Recovery** (`xma_decode_reporting.h`, `FailureRun`):
  - a single failure changes nothing (the next frame is assembled exactly
    as before);
  - a second failure within the same packet resynchronises at the next
    packet, whose header gives its first frame;
  - from the eighth failure in a row the decoder is flushed
    (`avcodec_flush_buffers`) and each failed frame becomes 512 samples of
    silence: the output ring advances at the audio rate, the voice keeps its
    timing, and the kick ends when the ring is full instead of burning
    through the input.
  - one decoded frame resets the run.
- **Verification (host, XMA streaming fixture with the production
  `xma.cpp`).** A single failed frame leaves every later frame bit-exact
  and triggers no recovery; with a decoder that always fails, a kick on a
  20-frame stream tries 11 frames instead of 20, flushes the decoder, and
  the silence moves the output cursor to block 30 of 31. All 1,529
  streaming checks and the 63,493 output-ring cases pass. Not heard on the
  phone.

## Device check

Same path as the 7/11 recordings (the frames with 1,177 draws): the log
should show no burst, or one `xma decode burst` line naming the stream and
packet with `resyncs`/`silenced` counts. If it still bursts, that line and
a capture with `LO_XMA_ERROR_CAPTURE_DIR` identify the sound for a real fix.
