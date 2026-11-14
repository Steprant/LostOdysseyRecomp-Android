#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

// The presentation thread never waits for the Android collector. The latter
// drains this bounded queue on its own thread; a full or busy queue drops a
// sample and reports the loss, rather than delaying the game.
namespace diagnostics::android_tester {
using Clock = std::chrono::steady_clock;
inline uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now().time_since_epoch()).count());
}

enum class Profile : uint32_t { Standard, Advanced, Intensive };
struct Frame {
    uint64_t timestampNs = 0, intervalNs = 0, presentWallNs = 0, presentCpuNs = 0;
    uint64_t rendererFrame = 0, draws = 0, gpuBatchTotalNs = 0, gpuBatches = 0;
    uint64_t audioStarved = 0, audioClockResets = 0;
    uint64_t xmaFrames = 0, xmaFailures = 0, xmaDecodeWallNs = 0;
    uint64_t movieFrames = 0, moviePtsMs = 0, movieUpdateWallNs = 0;
    uint64_t pipelineCreates = 0, pipelineFailures = 0, pipelineWallNs = 0, pipelineVs = 0, pipelinePs = 0;
    bool movieActive = false, presented = false, spike = false;
    // The interval since the previous present spans a pause of the game
    // (overlay, background): a suspension, left out of the frame statistics.
    bool gamePaused = false;
    // Steprant 2.1.1: the heavy GPU/CPU profiling was on for this frame.
    bool profiling = false;
};

// Steprant 2.1.1: the heavy profiling (per-render-pass GPU timestamps, the
// GPU command thread's per-draw timers, the sampled draw-by-draw frames)
// costs CPU and GPU time, so a recording that kept it on all the time
// measured the Tester as much as the game. It now runs only in the last
// kBurstNs of every kCycleNs of a recording; frame_timing.csv marks those
// frames (profiling column) and the summary leaves them out of power, CPU
// and frame figures. Light measurements (frames, power, temperature, CPU,
// threads, memory, GPU time per queue batch) stay on for the whole
// recording. LO_TESTER_CONTINUOUS_PROFILE=1 keeps the old behaviour.
inline constexpr uint64_t kCycleNs = 60'000'000'000ull;
inline constexpr uint64_t kBurstNs = 12'000'000'000ull;
inline bool ProfilingBurst(uint64_t sinceStartNs)
{
    static const bool continuous = [] {
        const char* value = std::getenv("LO_TESTER_CONTINUOUS_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return continuous || sinceStartNs % kCycleNs >= kCycleNs - kBurstNs;
}

class Recorder {
public:
    static constexpr size_t Capacity = 16384;
    std::atomic<bool> enabled{false}, recording{false}, automatic{true}, thermal{true};
    std::atomic<bool> collectorIdle{true};
    // Steprant 2.0.3: one profile, the most detailed one. The Tester page no
    // longer offers Standard/Advanced, the sample period, slow-frame
    // detection or thermal monitoring: every recording is Intensive, sampled
    // every 250 ms, with both detections on.
    std::atomic<uint32_t> profile{uint32_t(Profile::Intensive)};
    // Java reads this through nativeTesterStatus; changes take effect next recording.
    std::atomic<uint32_t> samplePeriodMs{250};

    bool Start(uint64_t now, uint32_t targetFps) {
        if (!enabled.load() || recording.load() || !collectorIdle.exchange(false)) return false;
        std::lock_guard lock(mutex_);
        if (!frames_) frames_.reset(new (std::nothrow) Frame[Capacity]);
        if (!frames_) { collectorIdle.store(true); return false; }
        head_ = count_ = 0;
        dropped_ = 0;
        previousPresent_.store(0, std::memory_order_relaxed);
        xmaFrames_ = xmaFailures_ = xmaWall_ = 0;
        movieFrames_ = moviePts_ = movieWall_ = 0;
        pipelineCreates_ = pipelineFailures_ = pipelineWall_ = pipelineVs_ = pipelinePs_ = 0;
        started_ = now;
        ended_ = 0;
        targetFps_ = targetFps;
        // A 30 FPS target dropping to 22 FPS has ~45 ms intervals. A 30%
        // tolerance and 25 ms floor catches that without treating normal
        // 60/120 Hz intervals as stalls.
        threshold_ = std::max<uint64_t>(25000000ull, 13ull * 1000000000ull / (10ull * std::max(1u, targetFps)));
        startedNs_.store(now, std::memory_order_relaxed);
        burst_.store(ProfilingBurst(0), std::memory_order_relaxed);
        recording.store(true, std::memory_order_release);
        return true;
    }
    void Stop(uint64_t now) {
        if (!recording.exchange(false, std::memory_order_acq_rel)) return;
        std::lock_guard lock(mutex_);
        ended_ = now;
    }
    bool Detailed() const {
        return recording.load(std::memory_order_relaxed) && profile.load(std::memory_order_relaxed) != 0;
    }
    // Heavy profiling: only inside a burst (see ProfilingBurst).
    bool CollectGpu() const { return Detailed() && burst_.load(std::memory_order_relaxed); }
    // Light, continuous measurements (GPU time per queue batch).
    bool CollectLight() const { return recording.load(std::memory_order_relaxed); }
    void SetRenderer(uint64_t frame, uint64_t draws) {
        rendererFrame_.store(frame, std::memory_order_relaxed);
        draws_.store(draws, std::memory_order_relaxed);
    }
    void SetAudio(uint64_t starved, uint64_t clockResets) {
        starved_.store(starved, std::memory_order_relaxed);
        clockResets_.store(clockResets, std::memory_order_relaxed);
    }
    void AddXma(bool failed, uint64_t wallNs) {
        xmaFrames_.fetch_add(1, std::memory_order_relaxed);
        if (failed) xmaFailures_.fetch_add(1, std::memory_order_relaxed);
        xmaWall_.fetch_add(wallNs, std::memory_order_relaxed);
    }
    void SetMovieActive(bool active) { movieActive_.store(active, std::memory_order_relaxed); }
    void AddPipeline(bool failed, uint64_t wallNs, uint64_t vs, uint64_t ps) {
        pipelineCreates_.fetch_add(1, std::memory_order_relaxed);
        if (failed) pipelineFailures_.fetch_add(1, std::memory_order_relaxed);
        pipelineWall_.fetch_add(wallNs, std::memory_order_relaxed);
        pipelineVs_.store(vs, std::memory_order_relaxed);
        pipelinePs_.store(ps, std::memory_order_relaxed);
    }
    void SetMovieUpdate(bool shown, uint64_t ptsMs, uint64_t wallNs) {
        if (shown) {
            movieFrames_.fetch_add(1, std::memory_order_relaxed);
            moviePts_.store(ptsMs, std::memory_order_relaxed);
        }
        movieWall_.store(wallNs, std::memory_order_relaxed);
    }
    void Add(uint64_t now, uint64_t wall, uint64_t cpu, bool presented, uint64_t gpuTotal, uint64_t gpuBatches,
             uint32_t suspendEpoch = 0, bool pausedNow = false) {
        if (!recording.load(std::memory_order_acquire)) return;
        // The burst state for the next frames; this one is recorded with the
        // state it was produced under.
        const bool profiled = burst_.load(std::memory_order_relaxed);
        const uint64_t started = startedNs_.load(std::memory_order_relaxed);
        burst_.store(ProfilingBurst(now >= started ? now - started : 0), std::memory_order_relaxed);
        const uint32_t previousEpoch = suspendEpoch_.exchange(suspendEpoch, std::memory_order_relaxed);
        const bool gamePaused = pausedNow || previousEpoch != suspendEpoch;
        // Advance even when the queue is busy or full; otherwise a dropped
        // diagnostic sample would look like a slow game frame in the next one.
        uint64_t interval = 0;
        if (presented) {
            const uint64_t previous = previousPresent_.exchange(now, std::memory_order_relaxed);
            if (previous && now >= previous) interval = now - previous;
        }
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
        if (!recording.load(std::memory_order_relaxed)) return;
        Frame sample{now, interval, wall, cpu, rendererFrame_.load(std::memory_order_relaxed),
            draws_.load(std::memory_order_relaxed), gpuTotal, gpuBatches,
            starved_.load(std::memory_order_relaxed), clockResets_.load(std::memory_order_relaxed),
            xmaFrames_.load(std::memory_order_relaxed), xmaFailures_.load(std::memory_order_relaxed), xmaWall_.load(std::memory_order_relaxed),
            movieFrames_.load(std::memory_order_relaxed), moviePts_.load(std::memory_order_relaxed), movieWall_.load(std::memory_order_relaxed),
            pipelineCreates_.load(std::memory_order_relaxed), pipelineFailures_.load(std::memory_order_relaxed),
            pipelineWall_.load(std::memory_order_relaxed), pipelineVs_.load(std::memory_order_relaxed), pipelinePs_.load(std::memory_order_relaxed),
            movieActive_.load(std::memory_order_relaxed),
            presented, automatic.load(std::memory_order_relaxed) && interval >= threshold_ && !gamePaused, gamePaused,
            profiled && profile.load(std::memory_order_relaxed) != 0};
        if (count_ == Capacity) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
        frames_[(head_ + count_) % Capacity] = sample;
        ++count_;
    }
    std::vector<Frame> Drain() {
        std::lock_guard lock(mutex_);
        std::vector<Frame> result;
        result.reserve(count_);
        for (size_t i = 0; i < count_; ++i) result.push_back(frames_[(head_ + i) % Capacity]);
        head_ = (head_ + count_) % Capacity;
        count_ = 0;
        return result;
    }
    void ReleaseBuffer() {
        std::lock_guard lock(mutex_);
        if (!recording.load(std::memory_order_relaxed)) { frames_.reset(); head_ = count_ = 0; }
    }
    uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }
    uint64_t Started() { std::lock_guard lock(mutex_); return started_; }
    uint64_t TargetFps() { std::lock_guard lock(mutex_); return targetFps_; }
    uint64_t Threshold() { std::lock_guard lock(mutex_); return threshold_; }
    uint64_t Duration(uint64_t now) {
        std::lock_guard lock(mutex_);
        if (!started_) return 0;
        const uint64_t end = ended_ ? ended_ : now;
        return end >= started_ ? end - started_ : 0;
    }
private:
    mutable std::mutex mutex_;
    std::unique_ptr<Frame[]> frames_;
    size_t head_ = 0, count_ = 0;
    std::atomic<uint64_t> dropped_{0}, rendererFrame_{0}, draws_{0}, starved_{0}, clockResets_{0};
    std::atomic<uint64_t> previousPresent_{0};
    std::atomic<uint64_t> startedNs_{0};
    std::atomic<bool> burst_{false};
    std::atomic<uint32_t> suspendEpoch_{0};
    std::atomic<uint64_t> xmaFrames_{0}, xmaFailures_{0}, xmaWall_{0}, movieFrames_{0}, moviePts_{0}, movieWall_{0};
    std::atomic<uint64_t> pipelineCreates_{0}, pipelineFailures_{0}, pipelineWall_{0}, pipelineVs_{0}, pipelinePs_{0};
    std::atomic<bool> movieActive_{false};
    uint64_t started_ = 0, ended_ = 0, threshold_ = 0, targetFps_ = 0;
};
inline Recorder g_recorder;
}
