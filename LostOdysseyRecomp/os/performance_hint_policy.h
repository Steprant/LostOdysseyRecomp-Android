#pragma once
#include <algorithm>
#include <cstdint>

// Android Dynamic Performance Framework (APerformanceHint) policy, kept free of
// Android headers so it can be tested on the host. Each presented frame reports
// the CPU time of the busiest game thread (guest main or render), the critical
// path of a frame those two threads pipeline. The target is a fraction of the
// frame period, so the system lowers CPU clocks only while that thread keeps a
// safety margin for waits, synchronisation and frame-to-frame variation.
namespace os::performance_hint
{
inline constexpr double kPeriod30Ms = 1000.0 / 30.0;
inline constexpr double kPeriod60Ms = 1000.0 / 60.0;

class Policy
{
    double emaIntervalMs = 0;
    uint32_t targetPercent = 50;

public:
    explicit Policy(uint32_t percent = 50) : targetPercent(std::clamp(percent, 20u, 90u)) {}

    // Presented frame interval in milliseconds; returns the frame period class
    // (30 or 60 FPS) the target follows. Hitches longer than 100 ms are ignored.
    double ObserveInterval(double intervalMs)
    {
        if (!(intervalMs > 0) || intervalMs > 100.0) return Period();
        emaIntervalMs = emaIntervalMs == 0 ? intervalMs : emaIntervalMs * 0.9 + intervalMs * 0.1;
        return Period();
    }
    double Period() const { return emaIntervalMs != 0 && emaIntervalMs < 25.0 ? kPeriod60Ms : kPeriod30Ms; }
    int64_t TargetNs(bool warmScene = false) const { return int64_t(Period() * 1e6 * Percent(warmScene) / 100.0); }
    uint32_t Percent(bool warmScene = false) const { return warmScene ? std::max(targetPercent, 65u) : targetPercent; }

    // CPU nanoseconds the two threads used in this frame. A negative or missing
    // reading counts as zero; a value above four periods (a stall) is capped so
    // one hitch cannot pin the clocks high for long.
    int64_t WorkNs(int64_t mainDeltaNs, int64_t renderDeltaNs) const
    {
        const int64_t work = std::max<int64_t>({0, mainDeltaNs, renderDeltaNs});
        return std::min<int64_t>(work, int64_t(Period() * 4e6));
    }
};
}
