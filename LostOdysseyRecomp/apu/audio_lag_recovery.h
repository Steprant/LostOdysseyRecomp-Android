#pragma once
// Steprant 2.0.6: audio/video re-alignment after an audio stall (Super
// Roadmap 2, WP3.6).
//
// The audio driver runs the game's audio callback once per 5.33 ms period
// and keeps a reserve of frames queued ahead of the device. When a callback
// (or the driver) stalls longer than the reserve, the device runs dry and
// plays silence. The driver then resets its clock instead of replaying the
// overdue periods, so the game's audio timeline stays behind the wall clock
// by the length of that silence, for good: in the 18:39 session of 11/11
// (guard with the flowers, then Jansen) nine stalls of 50-78 ms left the
// voices about a quarter of a second behind the picture.
//
// Recovery: after a stall the lag is counted in guest frames. While it is
// not zero, and the previous callback was quick, one period in four runs a
// second guest frame whose output is dropped; the next frame starts from the
// last sample played (a 2.7 ms join), so there is no click. The game's audio
// thus runs 25% ahead for a moment and catches up: 250 ms of lag in about
// one second, 5.3 ms skipped at a time.
//
// Not recovered, on purpose: a gap with the app paused or in the background
// (the game stops with the audio), a gap over two seconds, a gap while the
// device queue still had audio (no silence was played), and anything under
// fast-forward, which moves the game ahead by itself.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace apu::detail
{
class AudioLagRecovery
{
public:
    // One guest frame skipped at most every kSkipEvery driver periods.
    static constexpr uint32_t kSkipEvery = 4;
    // Two seconds of periods: a longer gap is a pause, not a stall.
    static constexpr uint32_t kMaxLagFrames = 375;

    void SetEnabled(bool enabled)
    {
        enabled_ = enabled;
        if (!enabled) Clear();
    }
    bool Enabled() const { return enabled_; }

    // The driver reset its clock after `overdue` periods without running the
    // game's callback. `queued` is the device queue at that moment, `reserve`
    // the frames normally kept in it, `pauseSeen` whether the app paused or
    // went to the background since the previous period. Returns the frames
    // of lag added.
    uint32_t OnClockReset(uint32_t overdue, uint32_t queued, uint32_t reserve, bool pauseSeen)
    {
        if (!enabled_ || pauseSeen || queued > 0 || overdue <= reserve || overdue > kMaxLagFrames) return 0;
        const uint32_t added = overdue - reserve;
        lag_ = std::min(kMaxLagFrames, lag_ + added);
        maxLag_ = std::max(maxLag_, lag_);
        ++stalls_;
        return added;
    }

    // Once per driver period, after its regular callback. True: run one more
    // guest frame now and drop its output. `healthy`: the regular callback
    // took under half a period, so an extra one will not starve the device.
    bool SkipThisPeriod(bool healthy, bool fastForward)
    {
        if (fastForward) { Clear(); return false; }
        if (!lag_ || !healthy) { phase_ = 0; return false; }
        if (++phase_ < kSkipEvery) return false;
        phase_ = 0;
        return true;
    }

    // The extra frame ran and its output was dropped.
    void Repaid()
    {
        if (!lag_) return;
        --lag_;
        ++repaid_;
    }

    void Clear()
    {
        if (lag_) ++abandoned_;
        lag_ = 0;
        phase_ = 0;
    }

    uint32_t PendingFrames() const { return lag_; }
    uint32_t RepaidFrames() const { return repaid_; }
    uint32_t MaxLagFrames() const { return maxLag_; }
    uint32_t Stalls() const { return stalls_; }
    uint32_t Abandoned() const { return abandoned_; }

private:
    bool enabled_ = true;
    uint32_t lag_ = 0, phase_ = 0, repaid_ = 0, maxLag_ = 0, stalls_ = 0, abandoned_ = 0;
};

// The frame after a dropped one: its first `fade` samples of every plane
// ramp from the last sample played (`last`, one value per plane), as the
// fast-forward exit does. Guest planes are big-endian floats, `samples` per
// plane, `planes` planes; `in` and `out` may not overlap.
inline void JoinAfterSkip(const float* last, const uint32_t* in, uint32_t* out,
    uint32_t planes, uint32_t samples, uint32_t fade)
{
    const auto swap = [](uint32_t word) {
        return (word >> 24) | ((word >> 8) & 0xFF00u) | ((word << 8) & 0xFF0000u) | (word << 24);
    };
    for (uint32_t c = 0; c < planes; ++c)
        for (uint32_t i = 0; i < samples; ++i)
        {
            const uint32_t word = in[c * samples + i];
            if (i >= fade) { out[c * samples + i] = word; continue; }
            float value = std::bit_cast<float>(swap(word));
            if (!std::isfinite(value)) value = 0;
            const float start = std::isfinite(last[c]) ? last[c] : 0.0f;
            const float weight = float(i) / float(fade);
            out[c * samples + i] = swap(std::bit_cast<uint32_t>(start + (value - start) * weight));
        }
}
}
