#pragma once

#include <os/platform.h>
#include <chrono>
#include <thread>
#if LO_PLATFORM_MACOS
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif

// Host scheduling hints for a real-time game loop. Windows raises the timer
// resolution with timeBeginPeriod(1) in main.cpp; these are the macOS
// counterparts. Other platforms need nothing and fall back to the standard
// library.
namespace os::scheduling
{
// Declares the process latency-critical for its whole lifetime. On macOS this
// opts out of App Nap. Call once, early in main().
void BeginLatencyCriticalActivity();

// Android: restricts the calling game thread (guest threads and the render
// thread) to the performance cores, so the scheduler stops parking it on the
// efficiency cores, where these threads run at less than half the speed.
// No-op elsewhere, on devices without efficiency cores, or with
// LO_PERFORMANCE_CORES=0. guestMain marks the thread LO_FRAME_STATS reports.
void PreferPerformanceCores(bool guestMain = false);
// WP3.2 A/B (LO_PIN_GAME_THREADS=1): pin the calling critical thread to the
// prime core (guest main) or the fastest big cores (GPU command thread).
enum class CriticalThread { GuestMain, GpuCommands };
void PinCriticalThread(CriticalThread which);
// Render thread, once per guest swap: LO_FRAME_STATS=<seconds> logs frame
// interval percentiles and per-frame thread CPU time (Android only).
void OnRenderFrame();
// Android Performance Hint (ADPF) session for the guest main and render
// threads: lets the system lower CPU clocks while frames keep their margin
// (performance_hint_policy.h). Off by default; applied from the next frame.
void SetPerformanceHints(bool enabled);
// Audio producer thread, once at its start: Android's audio thread priority,
// so a busy, thermally capped CPU still runs it on time. No-op elsewhere.
void RaiseAudioThreadPriority();
// Steprant 2.0.2 (Android): the calling thread's id for SetThreadNice, and the
// nice value of a thread of this process (0 normal, -8 intermediate, -16
// audio). Both return 0/false elsewhere.
int CurrentThreadId();
bool SetThreadNice(int threadId, int nice);

// Sleeps until deadline with sub-millisecond wakeup accuracy. macOS applies
// timer coalescing to ordinary threads, which wakes 18 ms sleeps about 5 ms
// late (worse than a whole audio period). A time-constraint (real-time) policy
// removes that slack; it is held only while the thread is blocked, so the
// kernel never demotes a thread for running long computations as real-time.
inline void PreciseSleepUntil(std::chrono::steady_clock::time_point deadline)
{
#if LO_PLATFORM_MACOS
    const auto remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
        return;
    static const mach_timebase_info_data_t timebase = [] {
        mach_timebase_info_data_t info{};
        mach_timebase_info(&info);
        return info;
    }();
    const auto toAbsolute = [](int64_t nanoseconds) {
        return uint32_t(std::min<uint64_t>(uint64_t(nanoseconds) * timebase.denom / timebase.numer, UINT32_MAX));
    };
    const thread_act_t self = mach_thread_self();
    thread_time_constraint_policy_data_t realtime{
        toAbsolute(remaining.count()), toAbsolute(500000), toAbsolute(remaining.count()), 0};
    const bool elevated = thread_policy_set(self, THREAD_TIME_CONSTRAINT_POLICY,
        reinterpret_cast<thread_policy_t>(&realtime), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
    std::this_thread::sleep_until(deadline);
    if (elevated) {
        thread_standard_policy_data_t standard{};
        thread_policy_set(self, THREAD_STANDARD_POLICY,
            reinterpret_cast<thread_policy_t>(&standard), THREAD_STANDARD_POLICY_COUNT);
    }
    mach_port_deallocate(mach_task_self(), self);
#else
    std::this_thread::sleep_until(deadline);
#endif
}

inline void PreciseSleepFor(std::chrono::steady_clock::duration duration)
{
    PreciseSleepUntil(std::chrono::steady_clock::now() + duration);
}
}
