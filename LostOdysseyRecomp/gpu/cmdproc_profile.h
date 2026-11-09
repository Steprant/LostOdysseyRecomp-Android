#pragma once
// Pass-profile switch and the GPU command thread's CPU split (Steprant 2.0
// WP-A.5), without the plume dependency of pass_profile.h, for the command
// processor.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#if defined(__ANDROID__)
#include <diagnostics/android_tester.h>
#endif

namespace gpu::pass_profile
{
// Per-render-pass profiling (pass_profile.h) runs while the Android Tester
// records with the Advanced or Intensive profile, or with LO_PASS_PROFILE=1.
inline bool Enabled()
{
    static const bool forced = [] {
        const char* value = std::getenv("LO_PASS_PROFILE");
        return value && std::strcmp(value, "1") == 0;
    }();
#if defined(__ANDROID__)
    if (::diagnostics::android_tester::g_recorder.CollectGpu()) return true;
#endif
    return forced;
}

// Steprant 2.0 WP-A.5: where the GPU command thread's CPU time goes while
// the pass profile runs: the whole thread (CLOCK_THREAD_CPUTIME_ID per swap)
// and the part inside the renderer's draw recording (renderer::Draw, which
// also performs EDRAM resolves, and renderer::Flush), as thread CPU time so
// waits do not count. parse = cpu - record: reading the Xenos command stream,
// register writes, events, waits for the guest. Decides WP-C (two threads)
// when the thread exceeds 14 ms per frame at 60 FPS.
inline std::atomic<uint64_t> g_cmdprocCpuNs{0}, g_cmdprocRecordNs{0}, g_cmdprocSwaps{0}, g_cmdprocMaxCpuNs{0};
inline uint64_t ThreadCpuNs()
{
#if defined(_WIN32)
    return 0;
#else
    timespec time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return uint64_t(time.tv_sec) * 1000000000ull + uint64_t(time.tv_nsec);
#endif
}
inline void NoteCmdprocSwap(uint64_t cpuNs)
{
    g_cmdprocCpuNs.fetch_add(cpuNs, std::memory_order_relaxed);
    g_cmdprocSwaps.fetch_add(1, std::memory_order_relaxed);
    uint64_t seen = g_cmdprocMaxCpuNs.load(std::memory_order_relaxed);
    while (cpuNs > seen && !g_cmdprocMaxCpuNs.compare_exchange_weak(seen, cpuNs, std::memory_order_relaxed)) {}
}
inline std::string TakeFormatCmdproc()
{
    const uint64_t swaps = g_cmdprocSwaps.exchange(0, std::memory_order_relaxed);
    const uint64_t cpu = g_cmdprocCpuNs.exchange(0, std::memory_order_relaxed);
    const uint64_t record = g_cmdprocRecordNs.exchange(0, std::memory_order_relaxed);
    const uint64_t maximum = g_cmdprocMaxCpuNs.exchange(0, std::memory_order_relaxed);
    if (!swaps) return {};
    const double f = double(swaps);
    char buffer[200];
    std::snprintf(buffer, sizeof(buffer), " cmdproc_per_frame: cpu_ms=%.2f record_ms=%.2f parse_ms=%.2f max_cpu_ms=%.2f swaps=%llu",
        double(cpu) / 1e6 / f, double(record) / 1e6 / f, double(cpu > record ? cpu - record : 0) / 1e6 / f,
        double(maximum) / 1e6, static_cast<unsigned long long>(swaps));
    return buffer;
}

}
