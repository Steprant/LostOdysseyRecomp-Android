#pragma once
// Steprant 2.1.1: an elastic audio queue reserve.
//
// The reserve (frames kept queued ahead of the device) is 12 frames, 64 ms,
// on Android. On 14/11 the guest audio callback stalled for 50-110 ms at the
// start of battles and at map changes, longer than the reserve: the device
// queue ran dry (63 empty queues in 25 minutes). On a hot phone (13/11) the
// callbacks ran 200-425 ms late and the queue emptied about twenty times a
// second: the crackle the player heard.
//
// A longer reserve all the time would delay every sound by as much. Instead
// the driver doubles it to kBoostFrames (128 ms) when it sees a risk: the
// queue ran dry, a callback ran kStallMs or longer, or the picture is frozen
// (a load or a shader compile, when nothing on screen has to match the
// sound). kHoldNs after the last risk it steps back to the configured
// reserve one frame every kStepNs: each step leaves out a single 5.3 ms guest
// period instead of pausing the game's audio for the whole 64 ms at once.
// A larger reserve set by the player or LO_AUDIO_RESERVE_FRAMES is never
// lowered. The driver rebases the lag measure whenever the reserve changes
// (audio.cpp), so no change is read as a stall to repay.
#include <algorithm>
#include <cstdint>

namespace apu::detail
{
class ReserveBoost
{
public:
    static constexpr uint32_t kBoostFrames = 24;               // 128 ms at 48 kHz, 256-sample frames
    static constexpr uint32_t kStallMs = 40;
    static constexpr uint64_t kHoldNs = 15'000'000'000ull;
    static constexpr uint64_t kStepNs = 100'000'000ull;

    enum class Reason : uint8_t { None, EmptyQueue, LongCallback, FrozenPicture };

    struct Result
    {
        uint32_t frames = 0;        // the reserve to use now
        bool changed = false;       // differs from the previous Update
        Reason reason = Reason::None; // what started (or renewed) a boost now
    };

    // Driver thread, once per period. starvedTotal is the running count of
    // frames that found the device queue empty; callbackMs the longest guest
    // callback since the previous call.
    Result Update(uint64_t nowNs, uint32_t base, uint64_t starvedTotal, uint32_t callbackMs, bool pictureFrozen)
    {
        Result r;
        if (!started_) { started_ = true; starvedSeen_ = starvedTotal; }
        if (starvedTotal < starvedSeen_) starvedSeen_ = starvedTotal; // counter restarted
        if (starvedTotal > starvedSeen_) r.reason = Reason::EmptyQueue;
        else if (callbackMs >= kStallMs) r.reason = Reason::LongCallback;
        else if (pictureFrozen) r.reason = Reason::FrozenPicture;
        starvedSeen_ = starvedTotal;
        if (r.reason != Reason::None) until_ = nowNs + kHoldNs;
        const bool boosted = until_ && nowNs < until_;
        if (!boosted) until_ = 0;
        if (boosted || last_ <= base) {
            r.frames = boosted ? std::max(base, kBoostFrames) : base;
            nextStep_ = 0;
        } else {
            // Stepping down: one frame per kStepNs.
            r.frames = last_;
            if (!nextStep_) nextStep_ = nowNs;
            if (nowNs >= nextStep_) { --r.frames; nextStep_ = nowNs + kStepNs; }
        }
        r.changed = r.frames != last_;
        last_ = r.frames;
        return r;
    }

    bool Boosted() const { return until_ != 0; }

    static const char* Name(Reason reason)
    {
        switch (reason) {
        case Reason::EmptyQueue: return "empty device queue";
        case Reason::LongCallback: return "long game audio callback";
        case Reason::FrozenPicture: return "frozen picture";
        default: return "none";
        }
    }

private:
    bool started_ = false;
    uint64_t starvedSeen_ = 0;
    uint64_t until_ = 0, nextStep_ = 0;
    uint32_t last_ = 0;
};
}
