#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include "cinematic_power.h"

namespace gpu::android_fg {

using Clock = std::chrono::steady_clock;
inline constexpr uint32_t kSourceFps = 30;
inline constexpr uint32_t kOutputFps = 60;
inline constexpr auto kDisplayPeriod = std::chrono::nanoseconds(1000000000 / kOutputFps);

enum class Pressure { None, NativeCadence, Present, Inputs, Deadline };
enum class ExtraFrame { Presented, Busy, MissingInputs, Late };

struct DisplaySlots {
    Clock::time_point native{}, prediction{};
};

// Keep the real frame and its prediction two consecutive refreshes apart
// from the next real frame. Once available, use SurfaceFlinger's actual
// presentation timestamps to anchor the lattice to the physical display.
class DisplayCadence {
public:
    void Reset() { period = kDisplayPeriod; lead = std::chrono::milliseconds(5); phase = lastNative = {}; lateRun = 0; }
    void Interrupt() { lastNative = {}; }
    void SetPeriod(Clock::duration value)
    {
        using namespace std::chrono_literals;
        if (value >= 15ms && value <= 18ms && value != period) {
            period = value;
            phase = lastNative = {};
        }
    }
    void ObserveScanout(Clock::time_point actual, Clock::time_point desired = {})
    {
        using namespace std::chrono_literals;
        if (actual <= phase) return;
        phase = actual;
        if (lastNative != Clock::time_point{} && lastNative >= phase) {
            const auto delta = lastNative - phase;
            lastNative = phase + period * ((delta + period / 2) / period);
        }
        // If the compositor repeatedly misses the requested refresh, move
        // the next pair a little earlier relative to its deadline. Keep the
        // lead bounded to avoid building up presentation latency.
        if (desired != Clock::time_point{} && actual > desired + period / 2) {
            if (++lateRun >= 3) {
                lateRun = 0;
                const auto adjusted = std::min(lead + Clock::duration(1ms), Clock::duration(9ms));
                if (adjusted != lead) {
                    lead = adjusted;
                    lastNative = {};
                }
            }
        } else {
            lateRun = 0;
        }
    }
    DisplaySlots Schedule(Clock::time_point now)
    {
        using namespace std::chrono_literals;
        const auto earliest = now + lead;
        const auto next = lastNative + period * 2;
        if (lastNative != Clock::time_point{} && next >= now + 2ms &&
            next <= earliest + period) {
            lastNative = next;
        } else if (phase != Clock::time_point{}) {
            // The observation may be newer than the last application frame.
            // Round up to the first usable slot; never schedule a catch-up burst.
            const auto delta = std::max(Clock::duration::zero(), earliest - phase);
            lastNative = phase + period * ((delta + period - Clock::duration(1)) / period);
        } else {
            lastNative = earliest;
        }
        return {lastNative, lastNative + period};
    }
    Clock::duration Period() const { return period; }

private:
    Clock::duration period = kDisplayPeriod;
    Clock::duration lead = std::chrono::milliseconds(5);
    Clock::time_point phase{}, lastNative{};
    uint32_t lateRun = 0;
};

// Only successful native presents establish the cadence. Optional work starts
// after a stable run and yields its capture budget when it misses repeatedly.
class Budget {
public:
    bool Capturing() const { return capturing; }

    bool ThermallyAllowed(int batteryTenths)
    {
        if (batteryTenths >= cinematic_power::kWarmBatteryTenths) thermalWarm = true;
        else if (batteryTenths > 0 && batteryTenths <= cinematic_power::kCoolBatteryTenths) thermalWarm = false;
        return !thermalWarm;
    }

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
        priming = 2;
    }

    Pressure Pause(Clock::time_point now, Pressure reason)
    {
        Interrupt();
        retryAfter = now + std::chrono::seconds(2u << backoff);
        backoff = std::min(backoff + 1, 2u);
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
        const bool inBudget = continuous && interval >= 23ms && interval <= 40ms && work <= 16ms;
        if (!capturing) {
            healthy = end >= retryAfter && inBudget ? healthy + 1 : 0;
            capturing = healthy >= 24;
            return Pressure::None;
        }
        if (continuous && interval >= 46ms)
            return Pause(end, Pressure::NativeCadence);
        if (work >= 30ms)
            return Pause(end, Pressure::Present);
        late = inBudget ? 0 : late + 1;
        if (late >= 3)
            return Pause(end, interval > 40ms ? Pressure::NativeCadence : Pressure::Present);
        roomThisFrame = inBudget;
        return Pressure::None;
    }

    bool CanGenerate(Clock::time_point now) const
    {
        using namespace std::chrono_literals;
        return roomThisFrame && now - previous <= 18ms;
    }

    Pressure ExtraFinished(Clock::time_point now, ExtraFrame result,
        Clock::duration nativeBlocked = Clock::duration::zero())
    {
        using namespace std::chrono_literals;
        if (!capturing) return Pressure::None;
        // A slow call on the output worker is harmless while the next game
        // frame is being recorded. Back off only when it impedes that frame
        // or repeatedly fails to fill its display slot.
        if (nativeBlocked >= 8ms) return Pause(now, Pressure::Present);
        if (priming) {
            --priming;
            if (result == ExtraFrame::MissingInputs) return Pressure::None;
        }
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
            if (outputMisses >= 4) return Pause(now, result == ExtraFrame::Late ? Pressure::Deadline : Pressure::Present);
            outputWindow = outputMisses = 0;
        }
        if (misses < 4) return Pressure::None;
        return Pause(now, result == ExtraFrame::Busy ? Pressure::Present :
            result == ExtraFrame::Late ? Pressure::Deadline : Pressure::Inputs);
    }

private:
    Clock::time_point previous{}, retryAfter{};
    uint32_t healthy = 0, late = 0, misses = 0, goodOutput = 0, backoff = 0;
    uint32_t outputWindow = 0, outputMisses = 0;
    uint32_t priming = 2;
    bool capturing = false;
    bool roomThisFrame = false;
    bool thermalWarm = false;
};

} // namespace gpu::android_fg
