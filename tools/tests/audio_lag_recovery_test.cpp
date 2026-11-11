// Steprant 2.0.6: audio/video re-alignment after an audio stall
// (apu/audio_lag_recovery.h).
#include <apu/audio_lag_recovery.h>
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

int main()
{
    using apu::detail::AudioLagRecovery;
    constexpr uint32_t reserve = 8;
    {
        AudioLagRecovery lag;
        // The 18:39 stalls: 50-78 ms callbacks against a 43 ms reserve.
        Check(lag.OnClockReset(9, 0, reserve, false) == 1, "a 48 ms gap past an 8-frame reserve leaves one frame of lag");
        Check(lag.OnClockReset(14, 0, reserve, false) == 6, "a 75 ms gap leaves six frames");
        Check(lag.PendingFrames() == 7 && lag.MaxLagFrames() == 7 && lag.Stalls() == 2, "lag adds up");
        Check(lag.OnClockReset(14, 3, reserve, false) == 0, "no lag while the device queue still had audio");
        Check(lag.OnClockReset(8, 0, reserve, false) == 0, "a gap the reserve covered leaves no lag");
        Check(lag.OnClockReset(40, 0, reserve, true) == 0, "no lag across a pause or the background");
        Check(lag.OnClockReset(AudioLagRecovery::kMaxLagFrames + 1, 0, reserve, false) == 0, "a gap over two seconds is a pause");
        Check(lag.PendingFrames() == 7, "rejected gaps change nothing");
    }
    {
        AudioLagRecovery lag;
        lag.OnClockReset(8 + 5, 0, reserve, false);
        uint32_t periods = 0, skips = 0;
        while (lag.PendingFrames() && periods < 1000)
        {
            ++periods;
            if (lag.SkipThisPeriod(true, false)) { ++skips; lag.Repaid(); }
        }
        Check(skips == 5 && periods == 5 * AudioLagRecovery::kSkipEvery, "one skip every four periods until repaid");
        Check(lag.RepaidFrames() == 5 && lag.PendingFrames() == 0, "repaid count");
        Check(!lag.SkipThisPeriod(true, false), "nothing to skip once caught up");
    }
    {
        AudioLagRecovery lag;
        lag.OnClockReset(8 + 2, 0, reserve, false);
        for (uint32_t i = 0; i < 20; ++i) Check(!lag.SkipThisPeriod(false, false), "never while callbacks are slow");
        Check(lag.PendingFrames() == 2, "slow periods keep the lag");
        uint32_t i = 0;
        while (!lag.SkipThisPeriod(true, false)) ++i;
        Check(i + 1 == AudioLagRecovery::kSkipEvery, "a slow period restarts the spacing");
        lag.Repaid();
        Check(!lag.SkipThisPeriod(true, true) && lag.PendingFrames() == 0 && lag.Abandoned() == 1,
              "fast-forward drops the remaining lag");
    }
    {
        AudioLagRecovery lag;
        lag.SetEnabled(false);
        Check(lag.OnClockReset(20, 0, reserve, false) == 0 && !lag.SkipThisPeriod(true, false), "LO_AUDIO_RESYNC=0");
        lag.SetEnabled(true);
        for (int n = 0; n < 100; ++n) lag.OnClockReset(AudioLagRecovery::kMaxLagFrames, 0, reserve, false);
        Check(lag.PendingFrames() == AudioLagRecovery::kMaxLagFrames, "the lag never exceeds two seconds");
        lag.Clear();
        Check(lag.PendingFrames() == 0 && lag.Abandoned() == 1, "clear");
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
    std::printf("audio lag recovery: %d checks passed\n", checks);
    return 0;
}
