#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace gpu::android_fg {

using Clock = std::chrono::steady_clock;
inline constexpr uint32_t kSourceFps = 30;
inline constexpr uint32_t kOutputFps = 60;
inline constexpr auto kDisplayPeriod = std::chrono::nanoseconds(1000000000 / kOutputFps);

enum class Pressure { None, NativeCadence, Present, Inputs };
enum class ExtraFrame { Presented, Busy, MissingInputs };

// Only successful native presents establish the cadence. Optional work starts
// after a stable run and yields its capture budget when it misses repeatedly.
class Budget {
public:
    bool Capturing() const { return capturing; }

    void Interrupt()
    {
        capturing = false;
        roomThisFrame = false;
        previous = {};
        healthy = 0;
        late = 0;
        misses = 0;
        outputWindow = outputMisses = 0;
        goodOutput = 0;
    }

    Pressure Pause(Clock::time_point now, Pressure reason)
    {
        Interrupt();
        retryAfter = now + std::chrono::seconds(2u << backoff);
        backoff = std::min(backoff + 1, 4u);
        return reason;
    }

    Pressure NativePresented(Clock::time_point start, Clock::time_point end)
    {
        using namespace std::chrono_literals;
        roomThisFrame = false;
        const bool continuous = previous != Clock::time_point{};
        const auto interval = start - previous;
        const auto work = end - start;
        previous = start;
        const bool inBudget = continuous && interval >= 23ms && interval <= 38ms && work <= 12ms;
        if (!capturing) {
            healthy = end >= retryAfter && inBudget ? healthy + 1 : 0;
            capturing = healthy >= 24;
            return Pressure::None;
        }
        if (continuous && interval >= 46ms)
            return Pause(end, Pressure::NativeCadence);
        if (work >= 25ms)
            return Pause(end, Pressure::Present);
        late = inBudget ? 0 : late + 1;
        if (late >= 3)
            return Pause(end, interval > 38ms ? Pressure::NativeCadence : Pressure::Present);
        roomThisFrame = inBudget;
        return Pressure::None;
    }

    bool CanGenerate(Clock::time_point now) const
    {
        using namespace std::chrono_literals;
        return roomThisFrame && now - previous <= 12ms;
    }

    Pressure ExtraFinished(Clock::time_point now, ExtraFrame result)
    {
        using namespace std::chrono_literals;
        if (!capturing) return Pressure::None;
        if (now - previous >= 25ms) return Pause(now, Pressure::Present);
        const bool succeeded = result == ExtraFrame::Presented;
        if (succeeded) {
            misses = 0;
            if (++goodOutput >= 120) {
                goodOutput = 0;
                backoff = 0;
            }
        } else {
            ++misses;
            ++outputMisses;
            goodOutput = 0;
        }
        // Four consecutive misses already cost multiple native frames.
        // Also suppress a steady alternation of successes and misses, which
        // produces uneven displayed motion even at a stable native rate.
        if (++outputWindow >= 12) {
            if (outputMisses >= 4) return Pause(now, Pressure::Present);
            outputWindow = outputMisses = 0;
        }
        if (misses < 4) return Pressure::None;
        return Pause(now, result == ExtraFrame::Busy ? Pressure::Present : Pressure::Inputs);
    }

private:
    Clock::time_point previous{}, retryAfter{};
    uint32_t healthy = 0, late = 0, misses = 0, goodOutput = 0, backoff = 0;
    uint32_t outputWindow = 0, outputMisses = 0;
    bool capturing = false;
    bool roomThisFrame = false;
};

} // namespace gpu::android_fg
