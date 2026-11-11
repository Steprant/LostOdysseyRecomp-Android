#pragma once
// Steprant 2.0.8: where the GPU command thread waits for the game.
//
// In the heavy scenes of 11/11 22:17 (two cameras, 1800-3000 draws a frame,
// 20 FPS) about 13 ms of every 50 ms frame were WAIT_REG_MEM: the command
// thread waiting for a value the game writes, usually the word its D3D
// interrupt handler clears after a PM4_INTERRUPT. Neither thread was busy
// (command thread ~40% of a core, guest main ~30%): the two wait for each
// other. This counts, per 10 s, the waits by what they poll and how long an
// interrupt takes from dispatch to the end of its handler, so the next
// session says which round trip costs the time. One log line per 10 s with
// waits over 1 ms, 120 lines a session at most.
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <cstdio>

namespace gpu::sync_stats {

// WAIT_REG_MEM durations by polled address (memory) or register.
class WaitTable
{
public:
    static constexpr size_t kSlots = 8;
    struct Entry { uint32_t key = 0; bool memory = false; uint32_t count = 0; double totalMs = 0, maxMs = 0; };

    void Add(bool memory, uint32_t key, double ms)
    {
        ++waits_;
        totalMs_ += ms;
        if (ms < 1.0) return; // short waits only in the totals
        ++longWaits_;
        Entry* slot = nullptr;
        for (auto& e : entries_)
            if (e.count && e.memory == memory && e.key == key) { slot = &e; break; }
        if (!slot)
            for (auto& e : entries_)
                if (!e.count) { slot = &e; slot->memory = memory; slot->key = key; break; }
        if (!slot) { ++dropped_; return; }
        ++slot->count;
        slot->totalMs += ms;
        slot->maxMs = std::max(slot->maxMs, ms);
    }

    uint32_t Waits() const { return waits_; }
    uint32_t LongWaits() const { return longWaits_; }
    double TotalMs() const { return totalMs_; }

    // The slots by total time, longest first: "mem 0xb000 n=12 sum=40.1 max=9.8".
    std::string Top(size_t n = 3) const
    {
        std::array<Entry, kSlots> sorted = entries_;
        std::sort(sorted.begin(), sorted.end(), [](const Entry& a, const Entry& b) { return a.totalMs > b.totalMs; });
        std::string out;
        for (size_t i = 0; i < std::min(n, sorted.size()); ++i)
        {
            if (!sorted[i].count) break;
            if (!out.empty()) out += "; ";
            char line[96];
            std::snprintf(line, sizeof(line), "%s %#x n=%u sum=%.1f max=%.1f", sorted[i].memory ? "mem" : "reg",
                sorted[i].key, sorted[i].count, sorted[i].totalMs, sorted[i].maxMs);
            out += line;
        }
        if (dropped_) { char line[32]; std::snprintf(line, sizeof(line), "; other n=%u", dropped_); out += line; }
        return out;
    }

    void Reset() { *this = {}; }

private:
    std::array<Entry, kSlots> entries_{};
    uint32_t waits_ = 0, longWaits_ = 0, dropped_ = 0;
    double totalMs_ = 0;
};

// Interrupt round trips: queue latency (dispatch to handler start) and the
// handler itself.
struct InterruptStats
{
    uint32_t count = 0;
    double queueMaxMs = 0, queueTotalMs = 0, handlerMaxMs = 0, handlerTotalMs = 0;
    uint32_t queueOver2Ms = 0;

    void Add(double queueMs, double handlerMs)
    {
        ++count;
        queueTotalMs += queueMs;
        handlerTotalMs += handlerMs;
        queueMaxMs = std::max(queueMaxMs, queueMs);
        handlerMaxMs = std::max(handlerMaxMs, handlerMs);
        if (queueMs > 2.0) ++queueOver2Ms;
    }
    std::string Text() const
    {
        if (!count) return "isr n=0";
        char line[160];
        std::snprintf(line, sizeof(line), "isr n=%u queue_avg=%.2f queue_max=%.1f queue_over_2ms=%u handler_avg=%.2f handler_max=%.1f",
            count, queueTotalMs / count, queueMaxMs, queueOver2Ms, handlerTotalMs / count, handlerMaxMs);
        return line;
    }
    void Reset() { *this = {}; }
};

}
