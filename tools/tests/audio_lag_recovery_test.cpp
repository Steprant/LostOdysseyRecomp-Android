// Steprant 2.0.6: audio/video re-alignment after a dry device queue
// (apu/audio_lag_recovery.h).
#include <apu/audio_lag_recovery.h>
#include <gpu/present_clock.h>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int checks = 0;
static void Check(bool ok, const char* what)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

static uint32_t Swap(uint32_t w) { return (w >> 24) | ((w >> 8) & 0xFF00u) | ((w << 8) & 0xFF0000u) | (w << 24); }
static uint32_t Word(float v) { return Swap(std::bit_cast<uint32_t>(v)); }
static float Value(uint32_t w) { return std::bit_cast<float>(Swap(w)); }

// A device-paced driver: each period the wall clock moves one frame and the
// device plays one, unless it is dry (silence). `run` counts guest frames,
// dropped ones included; the queue holds what was produced but not played.
struct Driver
{
    apu::detail::AudioLagRecovery lag;
    double wall = 1000;
    int64_t run = 0, queued = 8;
    uint32_t skips = 0, finished = 0;
    int64_t Heard() const { return run - queued; }
    // One normal period; the device played `played` frames (0: dry).
    void Period(double played = 1, bool quick = true)
    {
        wall += 1;
        run += int64_t(played); // the driver refills what the device took
        if (lag.Observe(wall, Heard())) ++finished;
        if (lag.SkipThisPeriod(quick)) { ++run; ++skips; lag.Repaid(); }
    }
    // A stall of `frames` periods during which the device ran dry after the
    // queue: the guest timeline stops, the wall clock does not.
    void Silence(double frames) { wall += frames; }
};

int main()
{
    using apu::detail::AudioLagRecovery;
    {
        Driver d;
        for (int i = 0; i < 2000; ++i) d.Period();
        Check(d.lag.PendingFrames() == 0 && d.skips == 0 && d.lag.Stalls() == 0, "in step: nothing to repay");
    }
    {
        Driver d;
        for (int i = 0; i < 10; ++i) d.Period();
        d.Silence(9.4); // a 50 ms dry queue
        d.Period();
        Check(d.lag.Repaying() && d.lag.PendingFrames() == 9 && d.lag.Stalls() == 1, "a 50 ms silence is measured");
        int periods = 0;
        while (d.lag.Repaying() && periods < 1000) { d.Period(); ++periods; }
        Check(d.finished == 1 && d.lag.PendingFrames() <= AudioLagRecovery::kStopFrames, "repaid down to the stop level");
        Check(d.skips == d.lag.RepaidFrames() && d.skips >= 7 && d.skips <= 9, "one dropped frame per frame of lag");
        Check(periods >= int(d.skips) * int(AudioLagRecovery::kSkipEvery) - 4 && periods <= int(d.skips) * int(AudioLagRecovery::kSkipEvery) + 4,
            "one dropped frame every four periods");
        for (int i = 0; i < 2000; ++i) d.Period();
        Check(!d.lag.Repaying() && d.lag.Stalls() == 1, "stays in step afterwards");
    }
    {
        // The 20:19 case: short silences that never reset the driver clock add up.
        Driver d;
        d.Period();
        d.Silence(3.5); d.Period();
        Check(!d.lag.Repaying() && d.lag.PendingFrames() == 3, "a 19 ms silence alone is not repaid");
        for (int i = 0; i < 50; ++i) d.Period();
        d.Silence(3.5); d.Period();
        Check(d.lag.Repaying() && d.lag.PendingFrames() == 7, "two of them are");
    }
    {
        // The device takes 512-sample chunks: two frames, then none.
        Driver d;
        for (int i = 0; i < 4000; ++i) d.Period(i % 2 ? 2 : 0);
        Check(d.skips == 0 && d.lag.Stalls() == 0, "chunked playback is not a lag");
    }
    {
        // Ahead of the wall clock (start-up fill, a fast device clock): the
        // anchor moves, so a later silence counts in full.
        Driver d;
        d.Period();
        d.run += 40; d.Period();
        Check(d.lag.PendingFrames() == 0, "ahead is not a lag");
        d.Silence(8); d.Period();
        Check(d.lag.PendingFrames() == 8 && d.lag.Repaying(), "the silence after it counts in full");
    }
    {
        Driver d;
        d.Period();
        d.Silence(20); d.Period();
        for (int i = 0; i < 20; ++i) d.Period(1, false);
        Check(d.skips == 0 && d.lag.PendingFrames() == 20, "never while callbacks are slow");
        uint32_t i = 0;
        while (!d.lag.SkipThisPeriod(true) && i < 100) ++i;
        Check(i + 1 == AudioLagRecovery::kSkipEvery, "a slow period restarts the spacing");
        d.lag.Rebase(d.wall, d.Heard());
        Check(!d.lag.Repaying() && d.lag.PendingFrames() == 0 && d.lag.Abandoned() == 1,
            "a pause, fast-forward or reopened device drops the remaining lag");
        d.Silence(5); d.Period();
        Check(d.lag.PendingFrames() == 5, "measuring restarts from the rebase");
    }
    {
        Driver d;
        d.Period();
        d.Silence(AudioLagRecovery::kMaxLagFrames + 10); d.Period();
        Check(!d.lag.Repaying() && d.lag.PendingFrames() == 0 && d.lag.Abandoned() == 1, "over two seconds is abandoned");
        d.Silence(7); d.Period();
        Check(d.lag.PendingFrames() == 7, "and measuring goes on");
    }
    {
        Driver d;
        d.lag.SetEnabled(false);
        d.Period(); d.Silence(30);
        for (int i = 0; i < 100; ++i) d.Period();
        Check(d.skips == 0 && d.lag.PendingFrames() == 0, "LO_AUDIO_RESYNC=0");
    }
    {
        // Join after a dropped frame: two planes of 8 samples, fade 4.
        constexpr uint32_t planes = 2, samples = 8, fade = 4;
        std::vector<uint32_t> in(planes * samples), out(planes * samples);
        for (uint32_t c = 0; c < planes; ++c)
            for (uint32_t i = 0; i < samples; ++i) in[c * samples + i] = Word(c ? -0.5f : 0.5f);
        in[1] = Word(NAN);
        const float last[planes] = { -1.0f, 1.0f };
        apu::detail::JoinAfterSkip(last, in.data(), out.data(), planes, samples, fade);
        Check(Value(out[0]) == -1.0f && Value(out[samples]) == 1.0f, "the join starts at the last sample played");
        Check(std::fabs(Value(out[2]) - (-1.0f + 1.5f * 0.5f)) < 1e-6f, "linear ramp towards the new frame");
        Check(Value(out[1]) == -0.75f, "a non-finite sample joins as silence");
        bool tail = true;
        for (uint32_t c = 0; c < planes; ++c)
            for (uint32_t i = fade; i < samples; ++i) tail &= out[c * samples + i] == in[c * samples + i];
        Check(tail, "after the fade the frame is unchanged, bit for bit");
    }
    {
        // Steprant 2.0.8: a frozen picture rebases the audio lag measure.
        using namespace gpu::present_clock;
        Check(!PictureFrozen(5'000'000'000), "nothing presented yet: not frozen");
        g_lastPresentNs.store(5'000'000'000);
        Check(!PictureFrozen(5'050'000'000), "50 ms since the last frame: running");
        Check(PictureFrozen(5'150'000'000), "150 ms since the last frame: frozen");
        Check(PictureFrozen(5'080'000'000, 50), "the limit is a parameter");
        g_lastPresentNs.store(0);
    }
    std::printf("audio lag recovery: %d checks passed\n", checks);
    return 0;
}
