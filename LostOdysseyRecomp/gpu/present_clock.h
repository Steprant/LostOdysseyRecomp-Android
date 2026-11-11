#pragma once
// Steprant 2.0.8: when the picture last reached the display, for the audio
// lag measure (apu/audio_lag_recovery.h). While no frame is presented the
// game's picture is frozen (a load, a shader compile, a host stall) and its
// audio stops with it: a lag that builds up then is not a desync.
#include <atomic>
#include <chrono>
#include <cstdint>

namespace gpu::present_clock {
inline std::atomic<int64_t> g_lastPresentNs{0};

inline int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Called after every frame the swap chain accepted.
inline void NotePresented() { g_lastPresentNs.store(NowNs(), std::memory_order_relaxed); }

// No frame presented for `limitMs` (and at least one presented ever).
inline bool PictureFrozen(int64_t nowNs, int64_t limitMs = 100)
{
    const int64_t last = g_lastPresentNs.load(std::memory_order_relaxed);
    return last != 0 && nowNs - last > limitMs * 1000000;
}
}
