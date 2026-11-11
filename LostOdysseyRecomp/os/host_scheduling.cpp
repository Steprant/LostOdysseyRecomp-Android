#include "host_scheduling.h"
#include <os/platform.h>

#if LO_PLATFORM_MACOS
#include <cstdint>
#include <objc/message.h>
#include <objc/runtime.h>
#endif
#if defined(__ANDROID__)
#include <os/logger.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <ctime>
#include <cmath>
#include <dlfcn.h>
#include <android/thermal.h>
#include "performance_hint_policy.h"
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>
#endif

namespace os::scheduling
{
#if LO_PLATFORM_MACOS
void BeginLatencyCriticalActivity()
{
    // [[NSProcessInfo processInfo] beginActivityWithOptions:reason:] through the
    // Objective-C runtime, so the runtime needs no Objective-C++ source. The
    // returned token is intentionally retained for the process lifetime.
    // NSActivityUserInitiated (0x00FFFFFF) | NSActivityLatencyCritical (0xFF00000000)
    constexpr uint64_t kOptions = 0x00FFFFFFull | 0xFF00000000ull;
    using ProcessInfoFn = id (*)(Class, SEL);
    using StringFn = id (*)(Class, SEL, const char*);
    using BeginFn = id (*)(id, SEL, uint64_t, id);
    using RetainFn = id (*)(id, SEL);

    Class processInfoClass = objc_getClass("NSProcessInfo");
    Class stringClass = objc_getClass("NSString");
    if (!processInfoClass || !stringClass)
        return;
    id processInfo = reinterpret_cast<ProcessInfoFn>(objc_msgSend)(processInfoClass, sel_registerName("processInfo"));
    id reason = reinterpret_cast<StringFn>(objc_msgSend)(stringClass, sel_registerName("stringWithUTF8String:"),
        "Real-time game rendering and audio");
    if (!processInfo || !reason)
        return;
    id activity = reinterpret_cast<BeginFn>(objc_msgSend)(processInfo,
        sel_registerName("beginActivityWithOptions:reason:"), kOptions, reason);
    if (activity)
        reinterpret_cast<RetainFn>(objc_msgSend)(activity, sel_registerName("retain"));
}
#else
void BeginLatencyCriticalActivity()
{
}
#endif

#if defined(__ANDROID__)
namespace
{
// CPUs whose capacity is at most half the largest one are efficiency cores.
// Empty when the device has none or does not report capacities.
cpu_set_t PerformanceCores(int* count)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    *count = 0;
    const char* env = getenv("LO_PERFORMANCE_CORES");
    if (env && env[0] == '0')
        return set;
    const long cpus = std::min<long>(sysconf(_SC_NPROCESSORS_CONF), CPU_SETSIZE);
    std::vector<int> capacity(size_t(std::max<long>(cpus, 0)), 0);
    int largest = 0;
    for (long cpu = 0; cpu < cpus; ++cpu) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpu_capacity", cpu);
        if (FILE* file = fopen(path, "re")) {
            if (fscanf(file, "%d", &capacity[cpu]) != 1) capacity[cpu] = 0;
            fclose(file);
        }
        largest = std::max(largest, capacity[cpu]);
    }
    bool hasEfficiency = false;
    for (long cpu = 0; cpu < cpus; ++cpu) {
        if (capacity[cpu] > 0 && capacity[cpu] * 2 <= largest) hasEfficiency = true;
        else if (capacity[cpu] > 0) { CPU_SET(cpu, &set); ++*count; }
    }
    if (!hasEfficiency) {
        CPU_ZERO(&set);
        *count = 0;
    }
    return set;
}

int64_t ThreadCpuNs(pid_t tid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", int(tid));
    unsigned long long ns = 0;
    if (FILE* file = fopen(path, "re")) {
        if (fscanf(file, "%llu", &ns) != 1) ns = 0;
        fclose(file);
    }
    return int64_t(ns);
}

std::atomic<pid_t> g_guestMainTid{0};

// Read-only diagnostics. Resolve API 30/31 entry points at runtime so the
// minimum Android API stays unchanged. No affinity, quality or clock policy.
struct ThermalStats
{
    void* library = nullptr;
    AThermalManager* manager = nullptr;
    using Release = void (*)(AThermalManager*);
    using Status = AThermalStatus (*)(AThermalManager*);
    using Headroom = float (*)(AThermalManager*, int);
    Release release = nullptr;
    Status status = nullptr;
    Headroom headroom = nullptr;
    std::chrono::steady_clock::time_point next{};

    ThermalStats()
    {
        const char* env = getenv("LO_THERMAL_STATS");
        if (!env || std::strcmp(env, "1") != 0) return;
        library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (library) {
            using Acquire = AThermalManager* (*)();
            auto acquire = reinterpret_cast<Acquire>(dlsym(library, "AThermal_acquireManager"));
            release = reinterpret_cast<Release>(dlsym(library, "AThermal_releaseManager"));
            status = reinterpret_cast<Status>(dlsym(library, "AThermal_getCurrentThermalStatus"));
            headroom = reinterpret_cast<Headroom>(dlsym(library, "AThermal_getThermalHeadroom"));
            if (acquire && release && status) manager = acquire();
        }
        if (!manager) LOG_WARNING("thermal stats: Android thermal API unavailable");
    }
    ~ThermalStats()
    {
        if (manager) release(manager);
        if (library) dlclose(library);
    }
    void Frame()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now < next) return;
        next = now + std::chrono::seconds(10);
        const auto current = status(manager);
        const float value = headroom ? headroom(manager, 0) : NAN;
        // Status is an OS assessment, not a measured CPU/GPU frequency cap.
        // NaN (common on unsupported OEMs) is reported, never used as policy.
        if (std::isfinite(value))
            LOG_INFO("thermal stats: status={} headroom={:.3f} forecast_seconds=0", int(current), value);
        else
            LOG_INFO("thermal stats: status={} headroom=unavailable", int(current));
    }
};

std::atomic<bool> g_performanceHints{false};

// APerformanceHint (API 33; prefer-power-efficiency API 35), resolved at
// runtime so the minimum Android API stays unchanged. Runs on the render
// thread from OnRenderFrame; the guest main thread is read through schedstat.
struct PerformanceHints
{
    struct APerformanceHintManager;
    struct APerformanceHintSession;
    using GetManager = APerformanceHintManager* (*)();
    using Create = APerformanceHintSession* (*)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
    using Report = int (*)(APerformanceHintSession*, int64_t);
    using Update = int (*)(APerformanceHintSession*, int64_t);
    using Close = void (*)(APerformanceHintSession*);
    using PreferEfficiency = int (*)(APerformanceHintSession*, bool);
    void* library = nullptr;
    GetManager getManager = nullptr;
    Create create = nullptr;
    Report report = nullptr;
    Update update = nullptr;
    Close close = nullptr;
    PreferEfficiency preferEfficiency = nullptr;
    APerformanceHintManager* manager = nullptr;
    APerformanceHintSession* session = nullptr;
    bool unavailable = false;
    os::performance_hint::Policy policy{[] {
        const char* env = getenv("LO_ADPF_TARGET_PERCENT");
        return env ? uint32_t(atoi(env)) : 50u;
    }()};
    int64_t target = 0, lastMain = 0, lastRender = 0;
    std::chrono::steady_clock::time_point lastFrame{}, nextLog{};
    double workMs = 0;
    uint32_t reports = 0;

    bool Load()
    {
        if (manager) return true;
        if (unavailable) return false;
        library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (library) {
            getManager = reinterpret_cast<GetManager>(dlsym(library, "APerformanceHint_getManager"));
            create = reinterpret_cast<Create>(dlsym(library, "APerformanceHint_createSession"));
            report = reinterpret_cast<Report>(dlsym(library, "APerformanceHint_reportActualWorkDuration"));
            update = reinterpret_cast<Update>(dlsym(library, "APerformanceHint_updateTargetWorkDuration"));
            close = reinterpret_cast<Close>(dlsym(library, "APerformanceHint_closeSession"));
            preferEfficiency = reinterpret_cast<PreferEfficiency>(dlsym(library, "APerformanceHint_setPreferPowerEfficiency"));
        }
        if (getManager && create && report && update && close) manager = getManager();
        if (!manager) {
            unavailable = true;
            LOG_WARNING("performance hints: APerformanceHint unavailable on this device");
        }
        return manager != nullptr;
    }
    void Stop()
    {
        if (!session) return;
        close(session);
        session = nullptr;
        LOG_INFO("performance hints: session closed");
    }
    void Frame()
    {
        if (!g_performanceHints.load(std::memory_order_relaxed)) { Stop(); return; }
        const pid_t mainTid = g_guestMainTid.load(std::memory_order_relaxed);
        if (!mainTid || !Load()) return;
        const auto now = std::chrono::steady_clock::now();
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        const int64_t renderCpu = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
        const int64_t mainCpu = ThreadCpuNs(mainTid);
        if (!session) {
            const int32_t tids[2] = {int32_t(mainTid), int32_t(gettid())};
            target = policy.TargetNs();
            session = create(manager, tids, 2, target);
            if (!session) { unavailable = true; LOG_WARNING("performance hints: session creation failed"); return; }
            const bool efficient = preferEfficiency && preferEfficiency(session, true) == 0;
            LOG_INFO("performance hints: session for guest main {} and render {} target_ms={:.2f} ({}% of the frame) prefer_efficiency={}",
                mainTid, gettid(), target / 1e6, policy.Percent(), efficient);
            lastFrame = now; lastMain = mainCpu; lastRender = renderCpu;
            nextLog = now + std::chrono::seconds(10);
            return;
        }
        policy.ObserveInterval(std::chrono::duration<double, std::milli>(now - lastFrame).count());
        if (const int64_t wanted = policy.TargetNs(); wanted != target && update(session, wanted) == 0) target = wanted;
        const int64_t work = policy.WorkNs(lastMain && mainCpu ? mainCpu - lastMain : 0, renderCpu - lastRender);
        if (work > 0 && report(session, work) == 0) { workMs += work / 1e6; ++reports; }
        lastFrame = now; lastMain = mainCpu; lastRender = renderCpu;
        if (now >= nextLog) {
            LOG_INFO("performance hints: target_ms={:.2f} reported={} mean_work_ms={:.2f}", target / 1e6, reports,
                reports ? workMs / reports : 0.0);
            workMs = 0; reports = 0;
            nextLog = now + std::chrono::seconds(10);
        }
    }
};

// LO_FRAME_STATS=<seconds> (1 means 10): frame interval percentiles and the
// render and guest main threads' CPU time per frame, without LO_RENDER_TIMING.
struct FrameStats
{
    int seconds = 0;
    std::chrono::steady_clock::time_point last{}, windowStart{};
    int64_t lastRenderCpu = 0, lastMainCpu = 0;
    std::vector<float> intervals;
    double renderCpuMs = 0, mainCpuMs = 0;

    FrameStats()
    {
        if (const char* env = getenv("LO_FRAME_STATS")) seconds = env[0] == '1' && !env[1] ? 10 : atoi(env);
    }

    void Frame()
    {
        const auto now = std::chrono::steady_clock::now();
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        const int64_t renderCpu = int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
        const pid_t mainTid = g_guestMainTid.load(std::memory_order_relaxed);
        const int64_t mainCpu = mainTid ? ThreadCpuNs(mainTid) : 0;
        if (last == std::chrono::steady_clock::time_point{}) {
            windowStart = now;
        } else {
            intervals.push_back(std::chrono::duration<float, std::milli>(now - last).count());
            renderCpuMs += (renderCpu - lastRenderCpu) / 1e6;
            mainCpuMs += lastMainCpu && mainCpu ? (mainCpu - lastMainCpu) / 1e6 : 0;
        }
        last = now;
        lastRenderCpu = renderCpu;
        lastMainCpu = mainCpu;
        if (now - windowStart < std::chrono::seconds(seconds) || intervals.empty())
            return;
        std::vector<float> sorted = intervals;
        std::sort(sorted.begin(), sorted.end());
        const auto at = [&](double p) { return sorted[std::min(sorted.size() - 1, size_t(p * (sorted.size() - 1) + 0.5))]; };
        const double frames = double(intervals.size());
        LOG_INFO("frame stats: frames={} fps={:.1f} interval_p50={:.2f} p95={:.2f} p99={:.2f} max={:.2f} render_cpu={:.2f} main_cpu={:.2f}",
            intervals.size(), frames / std::chrono::duration<double>(now - windowStart).count(), at(0.50), at(0.95), at(0.99),
            sorted.back(), renderCpuMs / frames, mainCpuMs / frames);
        intervals.clear();
        renderCpuMs = mainCpuMs = 0;
        windowStart = now;
    }
};
}

void PreferPerformanceCores(bool guestMain)
{
    static int count = 0;
    static const cpu_set_t cores = PerformanceCores(&count);
    if (guestMain)
        g_guestMainTid.store(gettid(), std::memory_order_relaxed);
    if (count > 0 && sched_setaffinity(0, sizeof(cores), &cores) != 0)
        LOG_WARNING("thread placement: sched_setaffinity failed, errno {}", errno);
    static std::once_flag logged;
    std::call_once(logged, [] { LOG_INFO("thread placement: game threads on {} performance cores", count); });
}

// Steprant 2.0 WP3.2 A/B (LO_PIN_GAME_THREADS=1, developer page): the guest
// main thread on the cores of the largest capacity (the prime core) and the
// GPU command thread on the next capacity level (the fastest big cores). In
// the city on 8/11 the prime core idled at 902 MHz while both threads shared
// the big cores. Off by default until measured.
void PinCriticalThread(CriticalThread which)
{
    static const bool enabled = [] {
        const char* value = getenv("LO_PIN_GAME_THREADS");
        return value && std::strcmp(value, "1") == 0;
    }();
    if (!enabled) return;
    const long cpus = std::min<long>(sysconf(_SC_NPROCESSORS_CONF), CPU_SETSIZE);
    std::vector<int> capacity(size_t(std::max<long>(cpus, 0)), 0);
    for (long cpu = 0; cpu < cpus; ++cpu) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpu_capacity", cpu);
        if (FILE* file = fopen(path, "re")) {
            if (fscanf(file, "%d", &capacity[cpu]) != 1) capacity[cpu] = 0;
            fclose(file);
        }
    }
    std::vector<int> levels(capacity.begin(), capacity.end());
    std::sort(levels.begin(), levels.end(), std::greater<int>());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    if (levels.empty() || levels.front() <= 0) return;
    const int wanted = which == CriticalThread::GuestMain || levels.size() < 2 || levels[1] <= 0 ? levels[0] : levels[1];
    cpu_set_t set;
    CPU_ZERO(&set);
    int count = 0;
    for (long cpu = 0; cpu < cpus; ++cpu)
        if (capacity[cpu] == wanted) { CPU_SET(cpu, &set); ++count; }
    if (count && sched_setaffinity(0, sizeof(set), &set) != 0)
        LOG_WARNING("thread placement: pinning failed, errno {}", errno);
    else
        LOG_INFO("thread placement: {} pinned to {} core(s) of capacity {}",
            which == CriticalThread::GuestMain ? "guest main" : "GPU command thread", count, wanted);
}

void OnRenderFrame()
{
    static FrameStats stats;
    if (stats.seconds > 0)
        stats.Frame();
    static ThermalStats thermal;
    if (thermal.manager)
        thermal.Frame();
    static PerformanceHints hints;
    hints.Frame();
}

void SetPerformanceHints(bool enabled)
{
    // LO_ADPF=0/1 overrides the setting for A/B runs.
    if (const char* env = getenv("LO_ADPF")) enabled = std::strcmp(env, "0") != 0;
    g_performanceHints.store(enabled, std::memory_order_relaxed);
}

void RaiseAudioThreadPriority()
{
    // ANDROID_PRIORITY_AUDIO; an app may raise its own threads this far.
    if (setpriority(PRIO_PROCESS, gettid(), -16) != 0)
        LOG_WARNING("audio thread priority: setpriority failed, errno {}", errno);
}

int CurrentThreadId()
{
    return int(gettid());
}

bool SetThreadNice(int threadId, int nice)
{
    return threadId > 0 && setpriority(PRIO_PROCESS, id_t(threadId), nice) == 0;
}
#else
int CurrentThreadId() { return 0; }
bool SetThreadNice(int, int) { return false; }
void PinCriticalThread(CriticalThread) {}
void PreferPerformanceCores(bool)
{
}

void OnRenderFrame()
{
}

void SetPerformanceHints(bool)
{
}

void RaiseAudioThreadPriority()
{
}
#endif
}
