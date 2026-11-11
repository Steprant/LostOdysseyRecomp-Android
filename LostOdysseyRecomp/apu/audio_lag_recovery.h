#pragma once
// Steprant 2.0.6: audio/video re-alignment after an audio stall (Super
// Roadmap 2, WP3.6).
//
// The audio driver is paced by the device: it runs the game's audio callback
// whenever the device queue falls under its reserve. The game's audio
// timeline therefore follows what the device actually played. Every time the
// device runs dry (a stalled callback, a late driver, a background trip
// that is not a pause) it plays silence, and from then on the game's audio
// is behind the picture by that silence, for good.
//
// 2.0.6 first counted that lag only at a driver clock reset. The 20:19
// session of 11/11 (2.0.5, phone) had one clock reset but 27 dry queues
// ("starved"): most silences never reset the clock, so the voices drifted
// behind again a little after the flowers/Jansen scene.
//
// Now the lag is measured, not guessed: once per period the driver compares
// the wall clock with the game's audio position as heard (guest frames run,
// dropped ones included, minus the frames still queued). Both start equal
// at an anchor; a timeline that gets ahead (start-up fill, a device clock a
// little fast) moves the anchor, so only falling behind counts.
//
// Recovery: from kStartFrames of lag (32 ms) until kStopFrames, and only
// after a quick callback, one period in four runs a second guest frame whose
// output is dropped; the next frame ramps from the last sample played (a
// 2.7 ms join). 250 ms of lag is repaid in about a second, 5.3 ms at a time.
//
// Not recovered, on purpose: the app paused or in the background (the game
// stops with the audio), fast-forward (it moves the game ahead by itself), a
// device reopened, a reserve change, and a lag over two seconds (abandoned).
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
    // Repay from 6 frames (32 ms) of lag down to 2 (10.7 ms). The device
    // takes 512-sample chunks, so the measure moves by about two frames
    // between periods even when nothing is lost.
    static constexpr uint32_t kStartFrames = 6;
    static constexpr uint32_t kStopFrames = 2;
    // Two seconds of periods: a longer lag is a pause, not a stall.
    static constexpr uint32_t kMaxLagFrames = 375;

    void SetEnabled(bool enabled)
    {
        enabled_ = enabled;
        if (!enabled) { lag_ = 0; repaying_ = false; phase_ = 0; anchored_ = false; }
    }
    bool Enabled() const { return enabled_; }

    // The wall clock and the game's audio agree here (start, pause, resume,
    // fast-forward, device reopen, reserve change). `wallFrames`: wall time
    // in periods since any fixed origin; `heardFrames`: guest frames run
    // (dropped ones included) minus frames still queued for the device.
    void Rebase(double wallFrames, int64_t heardFrames)
    {
        if (repaying_) ++abandoned_;
        baseWall_ = wallFrames;
        baseHeard_ = heardFrames;
        anchored_ = true;
        lag_ = 0;
        repaying_ = false;
        phase_ = 0;
    }

    // Once per driver period at normal speed. Returns true when a repayment
    // has just finished (for the log).
    bool Observe(double wallFrames, int64_t heardFrames)
    {
        if (!enabled_) return false;
        if (!anchored_) { Rebase(wallFrames, heardFrames); return false; }
        const double lag = (wallFrames - baseWall_) - double(heardFrames - baseHeard_);
        if (lag < 0)
        {
            // Ahead of the wall clock: move the anchor, nothing to repay.
            baseWall_ = wallFrames;
            baseHeard_ = heardFrames;
            lag_ = 0;
        }
        else if (lag > kMaxLagFrames)
        {
            repaying_ = false;
            Rebase(wallFrames, heardFrames);
            ++abandoned_;
            return false;
        }
        else lag_ = uint32_t(lag);
        maxLag_ = std::max(maxLag_, lag_);
        if (!repaying_ && lag_ >= kStartFrames) { repaying_ = true; phase_ = 0; ++stalls_; return false; }
        if (repaying_ && lag_ <= kStopFrames) { repaying_ = false; phase_ = 0; return true; }
        return false;
    }

    // Once per driver period, after Observe. True: run one more guest frame
    // now and drop its output. `healthy`: the regular callback took under
    // half a period, so an extra one will not starve the device.
    bool SkipThisPeriod(bool healthy)
    {
        if (!enabled_ || !repaying_ || !healthy) { phase_ = 0; return false; }
        if (++phase_ < kSkipEvery) return false;
        phase_ = 0;
        return true;
    }

    // The extra frame ran and its output was dropped. The lag itself is
    // measured again at the next Observe.
    void Repaid() { ++repaid_; }

    bool Repaying() const { return repaying_; }
    uint32_t PendingFrames() const { return lag_; }
    uint32_t RepaidFrames() const { return repaid_; }
    uint32_t MaxLagFrames() const { return maxLag_; }
    uint32_t Stalls() const { return stalls_; }
    uint32_t Abandoned() const { return abandoned_; }

private:
    bool enabled_ = true, anchored_ = false, repaying_ = false;
    double baseWall_ = 0;
    int64_t baseHeard_ = 0;
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
