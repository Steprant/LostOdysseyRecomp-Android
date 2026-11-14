#pragma once
// Steprant 2.1.1-C: a real wait for guest threads that poll a critical
// section.
//
// The 14/11 sessions showed a game job worker ("Guest 82beca90") calling
// RtlTryEnterCriticalSection and RtlLeaveCriticalSection 150-180 thousand
// times a second: take the lock, look at the queue, let it go, again. It
// used 28% of a core and woke 5,600 times a second while there was no work.
//
// What such a loop reads under the lock can only change when another thread
// takes and releases the same lock. So each lock has a release generation
// (hashed into kBuckets counters, like libc++'s contention table): a full
// Leave bumps it and wakes any thread waiting on it. When a thread asks for
// a lock it released itself, the generation has not moved since (nobody else
// released it) and that has happened kPollsBeforeWait times in a row, the
// thread sleeps until another release or kMaxWaitNs, then tries as before.
// Nothing else changes: TryEnter still never blocks for longer than
// kMaxWaitNs, a lock held by another thread is still reported as busy, and
// a change made outside the lock is seen at most kMaxWaitNs late. A hash
// collision only wakes a thread early.
//
// Linux and Android (futex); elsewhere NoteTryEnter never waits.
//
// OFF by default (LO_CS_POLL_WAIT=1 turns it on, for experiments only).
// The 2.1.1-C build had it on and the game did not start: a thread that
// does real work under a lock only it uses (take, pop an item, release,
// again) looks exactly like a poller to this rule, because no other thread
// ever releases that lock. It then slept 0.5 ms per item: 2,000 items took
// 1.1 s instead of microseconds. The lock alone cannot tell "nothing new"
// from "the thread changed the data itself", so this needs a different
// design (the job worker's own idle loop) before it can be used.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#if defined(__linux__)
#include <climits>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace cs_poll_wait
{
inline constexpr uint32_t kBuckets = 4096;
inline constexpr uint32_t kPollsBeforeWait = 32;
inline constexpr int64_t kMaxWaitNs = 500'000; // 0.5 ms

struct Table
{
    std::atomic<uint32_t> generation[kBuckets]{};
    std::atomic<uint32_t> waiters[kBuckets]{};
};
inline Table g_table;

struct Stats
{
    std::atomic<uint64_t> waits{0}, woken{0}, timeouts{0}, waitNs{0};
};
inline Stats g_stats;

inline bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("LO_CS_POLL_WAIT");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

inline uint32_t Bucket(const void* lock)
{
    uint64_t x = uint64_t(reinterpret_cast<uintptr_t>(lock)) >> 3;
    x ^= x >> 17; x *= 0xed5ad4bbu; x ^= x >> 11;
    return uint32_t(x) & (kBuckets - 1);
}

// What the calling thread last did with a lock.
struct ThreadState
{
    const void* lock = nullptr; // the lock it last released completely
    uint32_t generation = 0;    // that lock's generation right after the release
    uint32_t unchanged = 0;     // consecutive requests that found it unchanged
};
inline ThreadState& Current()
{
    thread_local ThreadState state;
    return state;
}

inline void FutexWake(std::atomic<uint32_t>& word)
{
#if defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#else
    (void)word;
#endif
}

// Returns true when woken by a change, false on the time limit.
inline bool FutexWait(std::atomic<uint32_t>& word, uint32_t expected, int64_t ns)
{
#if defined(__linux__)
    timespec timeout{time_t(ns / 1'000'000'000), long(ns % 1'000'000'000)};
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT_PRIVATE, expected, &timeout, nullptr, 0);
    return word.load(std::memory_order_acquire) != expected;
#else
    (void)word; (void)expected; (void)ns;
    return false;
#endif
}

// After a complete release of `lock` by the calling thread.
inline void NoteRelease(const void* lock)
{
    if (!Enabled()) return;
    const uint32_t b = Bucket(lock);
    const uint32_t after = g_table.generation[b].fetch_add(1, std::memory_order_seq_cst) + 1;
    if (g_table.waiters[b].load(std::memory_order_seq_cst)) FutexWake(g_table.generation[b]);
    auto& self = Current();
    if (self.lock != lock) self.unchanged = 0;
    self.lock = lock;
    self.generation = after;
}

// Before a try-enter of `lock` by the calling thread; may sleep up to
// kMaxWaitNs when the thread is polling a lock nobody else has released.
inline void NoteTryEnter(const void* lock)
{
    if (!Enabled()) return;
    auto& self = Current();
    if (self.lock != lock) { self.unchanged = 0; return; }
    const uint32_t b = Bucket(lock);
    auto& generation = g_table.generation[b];
    if (generation.load(std::memory_order_acquire) != self.generation) { self.unchanged = 0; return; }
    if (++self.unchanged < kPollsBeforeWait) return;
#if defined(__linux__)
    g_table.waiters[b].fetch_add(1, std::memory_order_seq_cst);
    const auto start = std::chrono::steady_clock::now();
    bool woken = generation.load(std::memory_order_seq_cst) != self.generation;
    if (!woken) woken = FutexWait(generation, self.generation, kMaxWaitNs);
    g_table.waiters[b].fetch_sub(1, std::memory_order_relaxed);
    g_stats.waits.fetch_add(1, std::memory_order_relaxed);
    (woken ? g_stats.woken : g_stats.timeouts).fetch_add(1, std::memory_order_relaxed);
    g_stats.waitNs.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count()), std::memory_order_relaxed);
    // Woken by another release: the next requests start counting again.
    // After the time limit the thread waits again at its next request.
    if (woken) self.unchanged = 0;
#endif
}

// The thread blocked or switched locks: start counting again.
inline void NoteOther() { Current().unchanged = 0; }
}
