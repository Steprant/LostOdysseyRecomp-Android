#pragma once
// Steprant 2.0, Super Roadmap 4.0 C-4: the profile of an instrumented build.
//
// A build configured with LO_PGO_MODE=generate counts how often every branch
// of the runtime and of the recompiled game code runs. The counters reach a
// file only when the profile runtime writes them, which by default happens at
// a clean process exit: an Android app is usually killed instead. Start()
// names the file in the app's own storage (pgo/lostodyssey-<pid>.profraw) and
// writes it every five minutes; the Tester export and every trip to the
// background write it too. Each write replaces the run's file with the
// counters so far. The Tester ZIP carries the files (pgo/), and
// llvm-profdata merges them into the profile of an LO_PGO_MODE=use build.
// In every other build these functions do nothing (and need no logger).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(LO_PGO_GENERATE)
#include <os/logger.h>
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
#endif

namespace os::pgo
{
inline constexpr bool Instrumented()
{
#if defined(LO_PGO_GENERATE)
    return true;
#else
    return false;
#endif
}

#if defined(LO_PGO_GENERATE)
namespace detail
{
inline std::mutex g_mutex;
inline std::string g_file; // The profile runtime keeps the pointer.
inline std::atomic<bool> g_started{false};
}
#endif

// Writes the counters so far; false when the build is not instrumented or the
// write failed.
inline bool Write([[maybe_unused]] const char* reason)
{
#if defined(LO_PGO_GENERATE)
    if (!detail::g_started.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(detail::g_mutex);
    const auto started = std::chrono::steady_clock::now();
    const bool ok = __llvm_profile_write_file() == 0;
    LOG_INFO("pgo: profile written ({}) ok={} {:.0f} ms", reason, ok ? 1 : 0,
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
    return ok;
#else
    return false;
#endif
}

// Once, early in main: the profile directory, the oldest files removed (the
// three newest runs stay), and the periodic writer.
inline void Start([[maybe_unused]] const std::filesystem::path& directory)
{
#if defined(LO_PGO_GENERATE)
    if (detail::g_started.load(std::memory_order_acquire)) return;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    std::vector<std::filesystem::directory_entry> old;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec))
        if (entry.path().extension() == ".profraw") old.push_back(entry);
    std::sort(old.begin(), old.end(), [](const auto& a, const auto& b) {
        std::error_code e;
        return a.last_write_time(e) > b.last_write_time(e);
    });
    for (size_t i = 3; i < old.size(); ++i) std::filesystem::remove(old[i].path(), ec);
    {
        std::lock_guard lock(detail::g_mutex);
        detail::g_file = (directory / "lostodyssey-%p.profraw").string();
        __llvm_profile_set_filename(detail::g_file.c_str());
    }
    detail::g_started.store(true, std::memory_order_release);
    LOG_INFO("pgo: instrumented build, profile in {}", directory.string());
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::minutes(5));
            Write("periodic");
        }
    }).detach();
#endif
}
}
