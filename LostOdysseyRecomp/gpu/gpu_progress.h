#pragma once
// Steprant 2.1.1-D: a guest thread waiting for the GPU sleeps until the
// command processor makes progress, instead of polling every 50 us.
//
// The 15/11 session: one game thread spent 84% of its samples in
// sub_823B62A0, D3D's wait for a GPU timestamp (it compares the requested
// timestamp with the completed one the GPU writes, see
// docs/archive/notes/third-map-hang.md). poll_wait's GpuPoll back-off let it
// poll 32 times without pausing at the start of every wait, then every
// 50 us: 34% of a core and about 5,000 wakeups a second.
//
// A pending timestamp can only complete when the command processor makes
// something visible to the guest: the sync points that advance its sync
// epoch (memory and event writes, scratch write-back, the end of a batch,
// waits on guest memory, resolves) and the guest interrupt handlers it runs.
// Each of them publishes here, after its write; the waiting thread reads the
// generation before its poll and, while the poll still says pending, sleeps
// until the generation moves or kWaitUs. The guest code keeps every read,
// comparison and timeout of its own; only the host pause between two polls
// changes.
//
// Self-check: a completion seen after a wait that ended on the time limit,
// with no publish since, means a path this does not cover. After
// kUnannouncedLimit of them the back-off returns to the timed 50 us polling
// for the rest of the process (logged once). LO_GPU_PROGRESS_WAIT=0 keeps
// the timed polling from the start.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#if defined(__linux__)
#include <climits>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace gpu_progress
{
inline constexpr uint32_t kFreePolls = 2;       // polls before the first sleep of a wait
inline constexpr uint32_t kWaitUs = 2000;       // the longest sleep between two polls
inline constexpr uint32_t kUnannouncedLimit = 8;

inline std::atomic<uint32_t> g_generation{0};
inline std::atomic<uint32_t> g_waiters{0};
inline std::atomic<bool> g_fallback{false};

struct Stats
{
    std::atomic<uint64_t> sleeps{0}, woken{0}, atLimit{0}, unannounced{0}, publishes{0}, wakeCalls{0};
};
inline Stats g_stats;

inline bool Configured()
{
    static const bool on = [] {
        const char* value = std::getenv("LO_GPU_PROGRESS_WAIT");
        return !(value && std::strcmp(value, "0") == 0);
    }();
    return on;
}

#if defined(__linux__)
inline constexpr bool kSupported = true;
#else
inline constexpr bool kSupported = false;
#endif

inline bool Enabled() { return kSupported && Configured() && !g_fallback.load(std::memory_order_relaxed); }

inline uint32_t Generation() { return g_generation.load(std::memory_order_seq_cst); }

// Command processor side, after the guest-visible write of a sync point.
inline void Publish()
{
    g_generation.fetch_add(1, std::memory_order_seq_cst);
    g_stats.publishes.fetch_add(1, std::memory_order_relaxed);
#if defined(__linux__)
    if (g_waiters.load(std::memory_order_seq_cst))
    {
        g_stats.wakeCalls.fetch_add(1, std::memory_order_relaxed);
        syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_generation), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
    }
#endif
}

// Waiting side: true when the generation moved past `seen`, false at `us`.
inline bool WaitPast(uint32_t seen, uint32_t us)
{
    g_stats.sleeps.fetch_add(1, std::memory_order_relaxed);
#if defined(__linux__)
    g_waiters.fetch_add(1, std::memory_order_seq_cst);
    bool moved = Generation() != seen;
    if (!moved)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
        for (;;)
        {
            const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) break;
            timespec timeout{time_t(left / 1'000'000'000), long(left % 1'000'000'000)};
            syscall(SYS_futex, reinterpret_cast<uint32_t*>(&g_generation), FUTEX_WAIT_PRIVATE, seen, &timeout, nullptr, 0);
            if ((moved = Generation() != seen)) break;
        }
    }
    g_waiters.fetch_sub(1, std::memory_order_relaxed);
#else
    std::this_thread::sleep_for(std::chrono::microseconds(us));
    const bool moved = Generation() != seen;
#endif
    (moved ? g_stats.woken : g_stats.atLimit).fetch_add(1, std::memory_order_relaxed);
    return moved;
}

// A completion seen after a wait that ended on the time limit, with the
// generation still where it was: returns true when this turned the
// back-off back to timed polling.
inline bool NoteUnannounced()
{
    const uint64_t n = g_stats.unannounced.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n < kUnannouncedLimit) return false;
    return !g_fallback.exchange(true, std::memory_order_relaxed);
}
}
