#pragma once

#include <chrono>
#include <cstdint>

namespace apu::xma
{
struct DecodeFailureReport
{
    uint64_t ordinal;
    uint64_t suppressed;
    bool emit;
    bool capture;
};

// ProcessPacket is serialized by the XMA worker's g_mutex. Keep one reporter
// for the worker, not one per reused context. Initial detailed reports and
// private packet captures retain the original process-wide limit of 16.
// Later warnings remain observable without allowing an unbounded log burst.
class DecodeFailureReports
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint64_t InitialReports = 16;
    static constexpr auto RepeatInterval = std::chrono::seconds(10);

    DecodeFailureReport Observe(Clock::time_point now) noexcept
    {
        const uint64_t ordinal = ++total_;
        const bool capture = ordinal <= InitialReports;
        const bool emit = capture || now - lastReport_ >= RepeatInterval;
        const uint64_t suppressed = emit ? suppressed_ : 0;
        if (emit)
        {
            lastReport_ = now;
            suppressed_ = 0;
        }
        else
        {
            ++suppressed_;
        }
        return {ordinal, suppressed, emit, capture};
    }

private:
    uint64_t total_ = 0;
    uint64_t suppressed_ = 0;
    Clock::time_point lastReport_{};
};

// A1: a run of failed frames on one context. More than BurstThreshold in one
// second is a burst; the first failure past the threshold reports it once,
// and the window that ends one returns its total for a closing line.
class FailureBurst
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr uint32_t BurstThreshold = 100;
    struct Result { bool burstStart = false; uint32_t endedBurstCount = 0; };

    Result Observe(Clock::time_point now) noexcept
    {
        Result result;
        if (count_ == 0 || now - windowStart_ >= std::chrono::seconds(1))
        {
            if (inBurst_) result.endedBurstCount = count_;
            windowStart_ = now;
            count_ = 0;
            inBurst_ = false;
        }
        ++count_;
        if (!inBurst_ && count_ > BurstThreshold)
        {
            inBurst_ = true;
            result.burstStart = true;
        }
        return result;
    }
    uint32_t Count() const noexcept { return count_; }
    bool InBurst() const noexcept { return inBurst_; }

private:
    Clock::time_point windowStart_{};
    uint32_t count_ = 0;
    bool inBurst_ = false;
};

// A1: what a context does after a failed frame. A second failure in the same
// packet means the assembler lost the frame boundaries there: skip to the
// next packet, whose header gives its first frame. From SilenceAfter
// failures in a row the decoder is flushed and the failed frame becomes 512
// samples of silence, so the voice keeps its timing and the worker stops
// chewing through the input as fast as it can.
enum class FailureAction : uint8_t { Continue, SkipPacket, Silence };

struct FailureRun
{
    static constexpr uint32_t SilenceAfter = 8;
    uint32_t consecutive = 0, samePacket = 0;
    uint64_t packetKey = ~0ull;

    FailureAction OnFailure(uint64_t key) noexcept
    {
        ++consecutive;
        samePacket = key == packetKey ? samePacket + 1 : 1;
        packetKey = key;
        if (consecutive >= SilenceAfter) return FailureAction::Silence;
        if (samePacket >= 2) return FailureAction::SkipPacket;
        return FailureAction::Continue;
    }
    void OnSuccess() noexcept { consecutive = samePacket = 0; packetKey = ~0ull; }
};

// receiveResult is the existing combined result and may equal sendResult
// when send failed. Do not describe that value as a receive result in logs.
constexpr const char* DecodeFailureStage(int sendResult, int receiveResult) noexcept
{
    return sendResult < 0 ? "send" : receiveResult < 0 ? "receive" : "frame-layout";
}
}
