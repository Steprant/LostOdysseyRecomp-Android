#pragma once
// Steprant 2.0.2: a log line that repeats becomes a periodic summary.
// The first `verbatim` occurrences are logged as they are; after that the
// caller logs one summary per interval with the number of occurrences since
// the last one. Not thread-safe: each gate belongs to one thread or lock.
#include <chrono>
#include <cstdint>

namespace os::log_repeat {

enum class Action : uint8_t { Verbatim, Summary, Silent };

struct Gate
{
    uint32_t verbatim = 4;
    std::chrono::steady_clock::duration interval = std::chrono::seconds(30);
    uint64_t total = 0;
    uint64_t sinceSummary = 0;      // occurrences the next summary reports
    std::chrono::steady_clock::time_point lastSummary{};

    Action Note(std::chrono::steady_clock::time_point now)
    {
        ++total;
        if (total <= verbatim) { lastSummary = now; return Action::Verbatim; }
        ++sinceSummary;
        if (now - lastSummary < interval) return Action::Silent;
        lastSummary = now;
        return Action::Summary;
    }
    // The count for the summary line; clears it.
    uint64_t Take()
    {
        const uint64_t count = sinceSummary;
        sinceSummary = 0;
        return count;
    }
};

}
