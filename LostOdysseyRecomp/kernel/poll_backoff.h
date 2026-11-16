#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>

// Steprant 2.1.5: back-off for guest polls of a kernel object.
//
// In the town scenes of the 16/11 sessions the guest main thread called
// NtWaitForSingleObjectEx with a zero timeout up to 370,000 times a second
// (sub_822D2248 / sub_822D2460 -> sub_822D2518 -> WaitForSingleObject(h, 0)),
// while the render thread waited for the GPU. Minutes with such polling
// averaged 6.2 W and 190% CPU, the others 4.5 W and 137%; the spinning thread
// also reports its CPU time to ADPF as frame work, which keeps the clocks up.
//
// After a thread has polled the same object unsuccessfully kFreePolls times
// over at least kMinSpinNs, without a gap longer than kGapNs between polls, its
// next poll of that object waits on the object itself for at most kWaitUs. A
// signal wakes it at once, so the guest sees what a later poll would have seen:
// success as soon as the object is signaled, STATUS_TIMEOUT otherwise. The wait
// is bounded, so it cannot block anything the guest would not have waited for.
// A thread holding a guest critical section or spinlock never waits here, so a
// back-off cannot delay a thread that needs that lock to signal the object.
// LO_POLL_BACKOFF=0 restores plain polls.
namespace kernel::poll_backoff {
inline constexpr uint32_t kFreePolls = 64;
inline constexpr int64_t kMinSpinNs = 100'000;
inline constexpr int64_t kGapNs = 2'000'000;
inline constexpr uint32_t kWaitUs = 500;

struct Stats {
    std::atomic<uint64_t> polls{0}, waits{0}, woken{0}, lockHeld{0};
};
inline Stats g_stats;

inline bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("LO_POLL_BACKOFF");
        return !(value && value[0] == '0' && value[1] == 0);
    }();
    return enabled;
}

// Guest critical sections and spinlocks the current thread holds (acquire
// imports count up, release imports count down).
inline thread_local int t_locksHeld = 0;

// Per thread: the last few objects it polled, each with its run of failures.
class Table {
public:
    struct Entry {
        uint32_t handle = 0, failures = 0;
        int64_t firstNs = 0, lastNs = 0;
    };

    Entry& Find(uint32_t handle)
    {
        for (auto& entry : entries)
            if (entry.handle == handle) return entry;
        Entry& entry = entries[next++ % entries.size()];
        entry = Entry{handle};
        return entry;
    }

    // Before a poll of handle at nowNs: true when it should wait briefly.
    static bool ShouldWait(Entry& entry, int64_t nowNs)
    {
        if (entry.failures && nowNs - entry.lastNs > kGapNs) entry.failures = 0;
        return entry.failures >= kFreePolls && nowNs - entry.firstNs >= kMinSpinNs;
    }

    // After the poll (or the brief wait) ended at nowNs.
    static void Record(Entry& entry, bool signaled, int64_t nowNs)
    {
        if (signaled) {
            entry.failures = 0;
            return;
        }
        if (!entry.failures) entry.firstNs = nowNs;
        ++entry.failures;
        entry.lastNs = nowNs;
    }

private:
    std::array<Entry, 4> entries{};
    uint32_t next = 0;
};
}
