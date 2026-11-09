#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

namespace apu::detail
{
// Largest duration recorded since the last Take(), in microseconds. Lock-free
// and safe from any thread; the audio log takes it once per report period.
class PeakMicros
{
    std::atomic<uint64_t> m_peak{ 0 };

public:
    void Record(std::chrono::steady_clock::duration duration)
    {
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
        const uint64_t value = micros > 0 ? uint64_t(micros) : 0;
        uint64_t current = m_peak.load(std::memory_order_relaxed);
        while (value > current && !m_peak.compare_exchange_weak(current, value, std::memory_order_relaxed)) {}
    }
    uint64_t Take() { return m_peak.exchange(0, std::memory_order_relaxed); }
    uint64_t Peek() const { return m_peak.load(std::memory_order_relaxed); }
};

// How long a full device queue has been waiting since the app could play.
// The driver's wait may have begun before the app went to the background;
// time spent there is not evidence that the device failed to restart.
inline std::chrono::steady_clock::duration StalledSinceResume(std::chrono::steady_clock::time_point now,
    std::chrono::steady_clock::time_point waitStarted, std::chrono::steady_clock::time_point resumed)
{
    return now - std::max(waitStarted, resumed);
}

// Device-queue reserve in 256-sample frames. 0 selects the platform default;
// otherwise only the supported sizes are accepted (12 ≈ 64 ms, 16 ≈ 85 ms).
inline uint32_t ReserveFrames(uint32_t requested, uint32_t platformDefault)
{
    return requested == 12 || requested == 16 ? std::max(requested, platformDefault) : platformDefault;
}

// SDL device buffer size in sample frames: a power of two from 256 to 2048.
inline uint32_t DeviceSampleFrames(uint32_t requested, uint32_t fallback)
{
    return requested >= 256 && requested <= 2048 && (requested & (requested - 1)) == 0 ? requested : fallback;
}

// XMA worker: whether a chunk that ran out of its packet budget made
// progress and should continue on the next worker pass. A chunk without
// progress stops as before, so a stuck context cannot spin forever.
inline bool ContinueXmaChunk(bool budgetExhausted, int32_t freeBlocksBefore, int32_t freeBlocksAfter,
    uint32_t inputOffsetBefore, uint32_t inputOffsetAfter, bool bufferBefore, bool bufferAfter)
{
    return budgetExhausted && (freeBlocksAfter < freeBlocksBefore || inputOffsetAfter != inputOffsetBefore
        || bufferAfter != bufferBefore);
}
}
