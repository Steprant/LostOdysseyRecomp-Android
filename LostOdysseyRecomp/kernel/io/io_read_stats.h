#pragma once
// Steprant 2.1.1: what the game's file reads cost (NtReadFile and
// NtReadFileScatter), for the Android Tester. The game data usually sits in
// Android's shared storage, which goes through a FUSE layer; this measures
// whether reads matter at all before anything is changed there.
//
// Counted only while Active() (the Tester records); otherwise each read pays
// one relaxed load. Taken and logged every ten seconds by the stall sampler
// thread as one "io reads:" line.
#include <atomic>
#include <cstdint>
#include <string>

namespace io_read_stats
{
inline std::atomic<bool> g_active{false};
inline std::atomic<uint64_t> g_reads{0}, g_bytes{0}, g_transferNs{0}, g_lockWaitNs{0};
inline std::atomic<uint64_t> g_maxTransferNs{0}, g_maxLockWaitNs{0}, g_slowReads{0};
inline constexpr uint64_t kSlowReadNs = 20'000'000ull; // 20 ms

inline bool Active() { return g_active.load(std::memory_order_relaxed); }
inline void SetActive(bool active) { g_active.store(active, std::memory_order_relaxed); }

inline void Max(std::atomic<uint64_t>& peak, uint64_t value)
{
    uint64_t seen = peak.load(std::memory_order_relaxed);
    while (value > seen && !peak.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {}
}

// One read: bytes transferred, the time waiting for the handle's I/O lock
// and the time of the transfer itself (seek and read under that lock).
inline void Add(uint64_t bytes, uint64_t lockWaitNs, uint64_t transferNs)
{
    g_reads.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_transferNs.fetch_add(transferNs, std::memory_order_relaxed);
    g_lockWaitNs.fetch_add(lockWaitNs, std::memory_order_relaxed);
    Max(g_maxTransferNs, transferNs);
    Max(g_maxLockWaitNs, lockWaitNs);
    if (transferNs + lockWaitNs >= kSlowReadNs) g_slowReads.fetch_add(1, std::memory_order_relaxed);
}

struct Window
{
    uint64_t reads = 0, bytes = 0, transferNs = 0, lockWaitNs = 0, maxTransferNs = 0, maxLockWaitNs = 0, slowReads = 0;
};

inline Window Take()
{
    Window w;
    w.reads = g_reads.exchange(0, std::memory_order_relaxed);
    w.bytes = g_bytes.exchange(0, std::memory_order_relaxed);
    w.transferNs = g_transferNs.exchange(0, std::memory_order_relaxed);
    w.lockWaitNs = g_lockWaitNs.exchange(0, std::memory_order_relaxed);
    w.maxTransferNs = g_maxTransferNs.exchange(0, std::memory_order_relaxed);
    w.maxLockWaitNs = g_maxLockWaitNs.exchange(0, std::memory_order_relaxed);
    w.slowReads = g_slowReads.exchange(0, std::memory_order_relaxed);
    return w;
}
}
