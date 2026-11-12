#include <stdafx.h>
#include "frame_timing.h"
#include <gpu/command_processor.h>
#include <gpu/frame_pacer.h>
#include <gpu/loading_pacing.h>
#include <settings/config.h>
#include <gpu/render_timing.h>
#include <os/logger.h>
#include <host_ui/host_ui.h>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <atomic>

#if defined(__ANDROID__)
#include <diagnostics/android_tester.h>
#include <diagnostics/sched_window.h>
#include <os/host_scheduling.h>
#endif

namespace frame_timing
{
bool Enabled() { static const bool enabled = getenv("LO_FRAME_TIMING") != nullptr; return enabled || gpu::render_timing::Enabled(); }
bool Active()
{
#if defined(__ANDROID__)
    if (::diagnostics::android_tester::g_recorder.recording.load(std::memory_order_relaxed)) return true;
#endif
    return Enabled();
}
namespace
{
std::atomic<uint64_t> gpuFenceWaitUs{0}, waitRegMemUs{0}, acquireUs{0};
std::mutex mutex;
std::atomic<uint64_t> inputTick{0};
uint64_t ticks = 0, intervals = 0;
double deltaSum = 0;
uint32_t requested = 0, encoded = 0;
double cpIdleMs = 0;
double cpPresentIdleMs = 0;
}
void CpIdle(double milliseconds) { if (Active()) { cpIdleMs += milliseconds; cpPresentIdleMs += milliseconds; } }
void AddGpuFenceWait(double milliseconds)
{
    if (milliseconds > 0 && Active()) gpuFenceWaitUs.fetch_add(uint64_t(milliseconds * 1000.0), std::memory_order_relaxed);
}
void AddPresentAcquire(double milliseconds)
{
    if (milliseconds > 0 && Active()) acquireUs.fetch_add(uint64_t(milliseconds * 1000.0), std::memory_order_relaxed);
}
void AddWaitRegMem(double milliseconds)
{
    if (milliseconds > 0 && Active()) waitRegMemUs.fetch_add(uint64_t(milliseconds * 1000.0), std::memory_order_relaxed);
}

// Roadmap 0.4 / P6: one line per frame over 50 ms while a session is
// recorded, splitting the time since the previous present: the command
// processor idle (waiting for the guest to submit), busy (executing PM4 and
// recording, which includes GPU fence waits and WAIT_REG_MEM), then flush,
// pacing sleep and present. Rate-limited: the first 300, then one in ten.
// Steprant 2.0.9 (sched_window.h): this frame of the command thread and of
// the guest main thread, on a CPU and runnable but waiting for one.
struct SchedFrame { bool valid = false; double cpRun = 0, cpWait = 0, mainRun = 0, mainWait = 0; };

static void ReportSlowFrame(uint32_t swap, double flushMs, double waitMs, double presentMs,
    const PacingSample& pacing, double idleMs, double fenceMs, double regMemMs, double acquireMs,
    const SchedFrame& sched)
{
    // An interval that spans a pause of the game (F1 / Quick tools overlay,
    // Android background) is tagged: it is a suspension, not a slow frame.
    static uint32_t lastEpoch = host_ui::SuspendEpoch();
    const uint32_t epoch = host_ui::SuspendEpoch();
    const bool suspended = epoch != lastEpoch || host_ui::IsGamePaused();
    lastEpoch = epoch;
    if (!pacing.hasPrevious || !pacing.presentAccepted || !std::isfinite(pacing.betweenMs)) return;
    const double total = pacing.betweenMs + flushMs + waitMs + presentMs;
    if (!(total > 50.0)) return;
    static uint32_t reported = 0;
    const uint32_t n = reported++;
    if (n >= 300 && n % 10 != 0) return;
    LOG_INFO("slow frame swap={} total={:.1f}ms between={:.1f}ms cp_idle={:.1f}ms cp_busy={:.1f}ms gpu_fence_wait={:.1f}ms "
        "wait_reg_mem={:.1f}ms flush={:.1f}ms pace_sleep={:.1f}ms present={:.1f}ms present_acquire={:.1f}ms present_slowest={}:{:.1f}ms previous_post={:.1f}ms n={}{}{}",
        swap, total, pacing.betweenMs, idleMs, std::max(0.0, pacing.betweenMs - idleMs), fenceMs, regMemMs,
        flushMs, waitMs, presentMs, acquireMs, pacing.presentSlowestStage, pacing.presentSlowestStageMs, pacing.previousSwapPostMs, n + 1,
        suspended ? " game_paused=1" : "",
        sched.valid ? fmt::format(" cp_oncpu={:.1f}ms cp_runq={:.1f}ms main_oncpu={:.1f}ms main_runq={:.1f}ms",
            sched.cpRun, sched.cpWait, sched.mainRun, sched.mainWait) : std::string());
}
void EngineTick(double delta)
{
    static const bool inputTicks = [] { const char* value = getenv("LO_TEST_INPUT_TICKS"); return value && strcmp(value, "1") == 0; }();
    if (inputTicks) inputTick.fetch_add(1, std::memory_order_relaxed);
    if (!Enabled()) return;
    std::lock_guard lock(mutex);
    ++ticks;
    if (std::isfinite(delta)) deltaSum += delta;
}
uint64_t InputTick() { return inputTick.load(std::memory_order_relaxed); }
void GuestInterval(uint32_t request, uint32_t encode)
{
    if (!Enabled()) return;
    std::lock_guard lock(mutex);
    ++intervals; requested = request; encoded = encode;
}
void Present(uint32_t swap, uint32_t fps, double flushMs, double waitMs, double presentMs,
    const PacingSample& pacing)
{
    if (!Active()) return;
    const double presentIdleMs = cpPresentIdleMs;
    cpPresentIdleMs = 0;
    const double fenceMs = double(gpuFenceWaitUs.exchange(0, std::memory_order_relaxed)) / 1000.0;
    const double regMemMs = double(waitRegMemUs.exchange(0, std::memory_order_relaxed)) / 1000.0;
    const double acquireMs = double(acquireUs.exchange(0, std::memory_order_relaxed)) / 1000.0;
    SchedFrame sched;
    {
        // Steprant 2.0.9 (sched_window.h): this runs on the command thread.
        namespace sw = diagnostics::sched_window;
        static sw::ThreadClock cpClock, mainClock;
        static sw::Window window;
        static auto windowStart = std::chrono::steady_clock::now();
        static uint32_t reports = 0;
        const auto toSample = [](const os::scheduling::SchedStat& s) { return sw::Sample{ s.runNs, s.waitNs, s.valid }; };
        const bool cpOk = cpClock.Next(toSample(os::scheduling::ReadSchedStat(os::scheduling::CurrentThreadId())), sched.cpRun, sched.cpWait);
        const bool mainOk = mainClock.Next(toSample(os::scheduling::ReadSchedStat(os::scheduling::GuestMainThreadId())), sched.mainRun, sched.mainWait);
        sched.valid = cpOk;
        if (!mainOk) sched.mainRun = sched.mainWait = 0;
        if (cpOk && pacing.hasPrevious && pacing.presentAccepted && std::isfinite(pacing.betweenMs))
            window.Add(pacing.betweenMs + flushMs + waitMs + presentMs, sched.cpRun, sched.cpWait, sched.mainRun, sched.mainWait);
        const auto now = std::chrono::steady_clock::now();
        if (now - windowStart >= std::chrono::seconds(10))
        {
            if (window.frames && reports < 120)
            {
                ++reports;
                LOG_INFO("cp sched: frames={} per_frame cp_oncpu={:.1f} cp_runq={:.1f} main_oncpu={:.1f} main_runq={:.1f} max_cp_runq={:.1f} "
                    "| slow(>40ms) frames={} wall={:.1f} cp_oncpu={:.1f} cp_runq={:.1f} main_oncpu={:.1f} main_runq={:.1f}",
                    window.frames, sw::Window::Per(window.cpRunMs, window.frames), sw::Window::Per(window.cpWaitMs, window.frames),
                    sw::Window::Per(window.mainRunMs, window.frames), sw::Window::Per(window.mainWaitMs, window.frames), window.maxCpWaitMs,
                    window.slowFrames, sw::Window::Per(window.slowWallMs, window.slowFrames),
                    sw::Window::Per(window.slowCpRunMs, window.slowFrames), sw::Window::Per(window.slowCpWaitMs, window.slowFrames),
                    sw::Window::Per(window.slowMainRunMs, window.slowFrames), sw::Window::Per(window.slowMainWaitMs, window.slowFrames));
            }
            window.Reset();
            windowStart = now;
        }
    }
    ReportSlowFrame(swap, flushMs, waitMs, presentMs, pacing, presentIdleMs, fenceMs, regMemMs, acquireMs, sched);
    if (!Enabled()) return;
    if (gpu::render_timing::Enabled())
    {
        // These adjacent intervals cover previous completed-present end through
        // this completed-present end. The first call has no preceding boundary.
        // PresentFrontbuffer plus the event pump comprise presentMs; betweenMs
        // includes command processing, intermediate flushes, waits and previous
        // post-present diagnostics. These are wall intervals, not CPU usage.
        const bool valid = pacing.presentAccepted && pacing.hasPrevious && std::isfinite(pacing.betweenMs) && pacing.betweenMs >= 0 &&
            std::isfinite(flushMs) && flushMs >= 0 && std::isfinite(waitMs) && waitMs >= 0 &&
            std::isfinite(presentMs) && presentMs >= 0;
        LOG_INFO("present timing completed={} target={} present_accepted={} has_previous={} sample_valid={} frame_ms={} "
            "between_ms={} flush_ms={:.6f} pace_ms={:.6f} present_and_events_ms={:.6f} "
            "sleep_requested_ms={:.6f} sleep_actual_ms={:.6f} wake_overshoot_ms={:.6f} previous_swap_log_ms={:.6f} previous_swap_post_ms={:.6f} cp_idle_ms={:.6f} scope=accepted_present_api_wall_intervals_not_display_latency",
            swap, fps, pacing.presentAccepted, pacing.hasPrevious, valid,
            valid ? fmt::format("{:.6f}", pacing.betweenMs + flushMs + waitMs + presentMs) : std::string("unknown"),
            pacing.hasPrevious ? fmt::format("{:.6f}", pacing.betweenMs) : std::string("unknown"),
            flushMs, waitMs, presentMs, pacing.requestedMs, pacing.actualMs, pacing.overshootMs,
            pacing.previousSwapLogMs, pacing.previousSwapPostMs, presentIdleMs);
    }
    // Keep the existing one-second report opt-in separately. LO_RENDER_TIMING
    // records every completed sample instead of estimating FPS from heartbeats.
    static const bool aggregate = getenv("LO_FRAME_TIMING") != nullptr;
    if (!aggregate) return;
    using Clock = std::chrono::steady_clock;
    static auto start = Clock::now();
    static uint32_t count = 0;
    static double flush = 0, wait = 0, present = 0;
    static double sleepRequest = 0, sleepActual = 0, wakeOver = 0, wakeMax = 0, between = 0;
    static uint32_t slept = 0, over1ms = 0, paired = 0;
    ++count; flush += flushMs; wait += waitMs; present += presentMs;
    sleepRequest += pacing.requestedMs; sleepActual += pacing.actualMs;
    wakeOver += pacing.overshootMs;
    wakeMax = std::max(wakeMax, pacing.overshootMs);
    slept += pacing.requestedMs > 0;
    over1ms += pacing.overshootMs > 1.0;
    if (pacing.hasPrevious) { between += pacing.betweenMs; ++paired; }
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - start).count();
    if (seconds < 1.0) return;
    uint64_t snapTicks = 0, snapIntervals = 0;
    double snapDelta = 0, snapIdle = 0;
    uint32_t snapRequested = 0, snapEncoded = 0;
    {
        std::lock_guard lock(mutex);
        snapTicks = ticks;
        snapDelta = deltaSum;
        snapIntervals = intervals;
        snapRequested = requested;
        snapEncoded = encoded;
        snapIdle = cpIdleMs;
        ticks = intervals = 0;
        deltaSum = cpIdleMs = 0;
    }
    const uint32_t snapCount = count;
    const double snapFlush = flush, snapWait = wait, snapPresent = present;
    const double snapSleepRequest = sleepRequest, snapSleepActual = sleepActual;
    const double snapWakeOver = wakeOver, snapWakeMax = wakeMax, snapBetween = between;
    const uint32_t snapSlept = slept, snapOver1ms = over1ms, snapPaired = paired;
    start = now;
    count = 0;
    flush = wait = present = 0;
    sleepRequest = sleepActual = wakeOver = wakeMax = between = 0;
    slept = over1ms = paired = 0;
    // Every-frame present timing already records this window. The 1s summary
    // is extra file I/O on the swap thread and has stalled city presents 200ms+.
    // LO_FRAME_TIMING_SUMMARY=1 keeps it anyway (scenario runs that check game time).
    static const bool forceSummary = [] {
        const char* value = std::getenv("LO_FRAME_TIMING_SUMMARY"); return value && value[0] == '1'; }();
    if (gpu::render_timing::Enabled() && !forceSummary) return;
    LOG_INFO("frame timing completed={} target={} window={:.6f}s presents={} rate={:.3f} flush={:.3f}ms pace={:.3f}ms present={:.3f}ms engine_ticks={} delta_sum={:.6f} intervals={} last_interval={:#x}->{:#x} source=guest_swap engine_rate={:.3f} game_time_ratio={:.6f}",
        swap, fps, seconds, snapCount, snapCount / seconds, snapFlush / snapCount, snapWait / snapCount, snapPresent / snapCount,
        snapTicks, snapDelta, snapIntervals, snapRequested, snapEncoded, snapTicks / seconds, snapDelta / seconds);
    LOG_INFO("frame pacing completed={} samples={} sleep_requested={:.3f}ms sleep_actual={:.3f}ms wake_over={:.3f}ms wake_max={:.3f}ms slept={} over_1ms={} between={:.3f}ms paired={} cp_idle={:.3f}ms",
        swap, snapCount, snapSleepRequest / snapCount, snapSleepActual / snapCount, snapWakeOver / snapCount, snapWakeMax,
        snapSlept, snapOver1ms, snapPaired ? snapBetween / snapPaired : 0.0, snapPaired, snapIdle / snapCount);
}
}

extern "C" PPC_FUNC(__imp__sub_827B6AD8);
PPC_FUNC(sub_827B6AD8)
{
    const auto requested = ctx.r7.u32;
    // Controlled A/B: disable only the guest interval conversion while keeping
    // the exact same host cap, video configuration and instrumentation.
    static const bool mapInterval = [] {
        const char* value = getenv("LO_GUEST_INTERVAL");
        return !value || strcmp(value, "0") != 0;
    }();
    // Native 90/120 use the same real-time engine path as 30/60. Only the
    // identified game's present interval changes; no clock or delta scaling.
    const auto config = settings::GetConfig();
    const auto mapped = mapInterval ? gpu::MapPresentInterval(requested, uint32_t(ctx.lr), gpu::GetFrameRateTarget(),
        config.variableRefreshRate || !config.vsync, gpu::loading::Active()) : requested;
    if (mapped != requested) ctx.r7.u64 = (ctx.r7.u64 & 0xFFFFFFFF00000000ull) | mapped;
    if (uint32_t(ctx.lr) == 0x827B4A4C) frame_timing::GuestInterval(requested, ctx.r7.u32);
    __imp__sub_827B6AD8(ctx, base);
}
