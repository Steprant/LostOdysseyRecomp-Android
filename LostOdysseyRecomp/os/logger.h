#pragma once

#include <fmt/core.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef __ANDROID__
#include <android/log.h>
#endif

enum class LogType
{
    Info,
    Warning,
    Error,
    Utility,
    Kernel,
    Verbose,   // per-frame chatter (interrupts, scratch writebacks...): LO_VERBOSE=1 to see it
    Notice,    // always written: build, device, map and rendering mismatches; read by log collection
};

namespace os::logger
{
    inline std::mutex g_mutex;
    // Info and Kernel lines need the Debug log setting; main applies it once
    // the settings file has been read, the settings menu when it is saved.
    inline std::atomic<bool> g_infoTrace{true};
    inline std::atomic<bool> g_kernelTrace{true};
    inline bool g_quietKernel = false; // --quiet-kernel

    // LO_DEBUG_LOG=1 forces the debug log on (test harnesses), =0 off.
    inline void SetDebugLog(bool enabled)
    {
        if (const char* forced = getenv("LO_DEBUG_LOG"); forced && *forced)
            enabled = forced[0] != '0';
        g_infoTrace.store(enabled, std::memory_order_relaxed);
        g_kernelTrace.store(enabled && !g_quietKernel, std::memory_order_relaxed);
    }
    // Opened once by main; intentionally kept open until process termination.
    inline FILE* g_file = nullptr;
    inline const bool g_verbose = getenv("LO_VERBOSE") != nullptr;

    // Every line carries the seconds since the process started and a short
    // thread tag, so a log can be read as a timeline ("what was it doing at
    // 72 s, and on which thread") rather than an unordered pile of messages.
    inline const std::chrono::steady_clock::time_point g_start = std::chrono::steady_clock::now();

    inline double ElapsedSeconds()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    }

    inline uint32_t ThreadTag()
    {
        static thread_local const uint32_t tag = uint32_t(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF);
        return tag;
    }

    inline const char* Prefix(LogType type)
    {
        switch (type)
        {
        case LogType::Info: return "[info] ";
        case LogType::Warning: return "[warn] ";
        case LogType::Error: return "[error]";
        case LogType::Utility: return "[util] ";
        case LogType::Kernel: return "[krnl] ";
        case LogType::Verbose: return "[verb] ";
        case LogType::Notice: return "[note] ";
        }
        return "";
    }

    // Steprant 2.0.2: asynchronous sink.
    //
    // A log line used to be written by the thread that logged it: under one
    // mutex, a write and a flush of the file plus a logcat record. A game,
    // render or audio thread that logs then waits for the storage and for
    // every other thread that is logging. With the sink started (Android,
    // from main; LO_LOG_ASYNC=0 keeps the old path) Info, Kernel, Utility and
    // Verbose lines are formatted by their thread and handed to a writer
    // thread through a bounded queue: the caller takes a short queue lock and
    // never waits for I/O. A full queue drops the line and counts it; the
    // writer reports the count. Warning, Error and Notice lines stay
    // synchronous and first write what is queued, so the order of the file is
    // kept and the lines before a failure are on disk with it.
    // Elsewhere, and in every test that does not start it, logging is
    // synchronous as before.
    struct AsyncLine
    {
        LogType type;
        std::string text;
    };
    struct AsyncSink
    {
        static constexpr size_t kMaxBytes = 1u << 20;
        static constexpr size_t kMaxLines = 8192;
        std::mutex queueMutex;
        std::condition_variable wake;
        std::vector<AsyncLine> lines;
        size_t bytes = 0;
        uint64_t dropped = 0;       // since the writer last reported
        uint64_t droppedTotal = 0, queuedTotal = 0;
        size_t peakLines = 0;
        bool stop = false, running = false;
        std::thread writer;
    };
    inline std::atomic<bool> g_async{false};
    inline AsyncSink& Sink()
    {
        static AsyncSink* sink = new AsyncSink(); // lives until the process ends
        return *sink;
    }

    // Requires g_mutex. One line to the file (or stderr) and to logcat.
    inline void WriteLineLocked(LogType type, const std::string& line, bool flush)
    {
        if (g_file)
        {
            fwrite(line.data(), 1, line.size(), g_file);
            if (flush) fflush(g_file);
        }
#ifdef __ANDROID__
        // Android stderr is a file (native-stderr.log); logcat replaces the
        // terminal copy, and stderr keeps only lines without a log file.
        const int priority = type == LogType::Error ? ANDROID_LOG_ERROR
            : type == LogType::Warning ? ANDROID_LOG_WARN
            : type == LogType::Info || type == LogType::Notice ? ANDROID_LOG_INFO
            : type == LogType::Verbose ? ANDROID_LOG_VERBOSE : ANDROID_LOG_DEBUG;
        __android_log_write(priority, "LostOdyssey", line.c_str());
        if (g_file)
            return;
#else
        (void)type;
#endif
        fwrite(line.data(), 1, line.size(), stderr);
        if (flush) fflush(stderr);
    }

    // Requires g_mutex. Writes everything queued, in order, with one flush.
    inline void WritePendingLocked()
    {
        auto& sink = Sink();
        std::vector<AsyncLine> batch;
        uint64_t dropped = 0;
        {
            std::lock_guard queue(sink.queueMutex);
            batch.swap(sink.lines);
            sink.bytes = 0;
            dropped = sink.dropped;
            sink.dropped = 0;
        }
        if (batch.empty() && !dropped) return;
        for (const auto& line : batch) WriteLineLocked(line.type, line.text, false);
        if (dropped)
            WriteLineLocked(LogType::Warning, fmt::format("[{:9.3f} t{:04x}] {} log: {} lines dropped, the log queue was full\n",
                ElapsedSeconds(), ThreadTag(), Prefix(LogType::Warning), dropped), false);
        if (g_file) fflush(g_file);
        else fflush(stderr);
    }

    // Writes what is queued now (a snapshot of the log, the end of the process).
    inline void WritePending()
    {
        if (!g_async.load(std::memory_order_acquire)) return;
        std::lock_guard lock(g_mutex);
        WritePendingLocked();
    }

    inline void StartAsync()
    {
        auto& sink = Sink();
        {
            std::lock_guard queue(sink.queueMutex);
            if (sink.running) return;
            sink.running = true;
            sink.stop = false;
        }
        sink.writer = std::thread([&sink] {
            for (;;) {
                {
                    std::unique_lock queue(sink.queueMutex);
                    sink.wake.wait(queue, [&] { return sink.stop || !sink.lines.empty() || sink.dropped; });
                    if (sink.stop && sink.lines.empty() && !sink.dropped) return;
                }
                std::lock_guard lock(g_mutex);
                WritePendingLocked();
            }
        });
        g_async.store(true, std::memory_order_release);
    }

    // Back to synchronous logging; everything queued is written first.
    inline void StopAsync()
    {
        auto& sink = Sink();
        {
            std::lock_guard queue(sink.queueMutex);
            if (!sink.running) return;
        }
        g_async.store(false, std::memory_order_release);
        {
            std::lock_guard queue(sink.queueMutex);
            sink.stop = true;
        }
        sink.wake.notify_all();
        if (sink.writer.joinable()) sink.writer.join();
        {
            std::lock_guard queue(sink.queueMutex);
            sink.running = false;
        }
        std::lock_guard lock(g_mutex);
        WritePendingLocked();
    }

    struct AsyncStats { uint64_t queued = 0, dropped = 0; size_t peakLines = 0; };
    inline AsyncStats GetAsyncStats()
    {
        auto& sink = Sink();
        std::lock_guard queue(sink.queueMutex);
        return {sink.queuedTotal, sink.droppedTotal, sink.peakLines};
    }

    // Never waits for I/O: a short queue lock, or a counted drop.
    inline void Enqueue(LogType type, std::string&& line)
    {
        auto& sink = Sink();
        bool notify = false;
        {
            std::lock_guard queue(sink.queueMutex);
            if (sink.lines.size() >= AsyncSink::kMaxLines || sink.bytes + line.size() > AsyncSink::kMaxBytes) {
                notify = sink.dropped == 0 && sink.lines.empty();
                ++sink.dropped;
                ++sink.droppedTotal;
            } else {
                notify = sink.lines.empty();
                sink.bytes += line.size();
                sink.lines.push_back({type, std::move(line)});
                ++sink.queuedTotal;
                if (sink.lines.size() > sink.peakLines) sink.peakLines = sink.lines.size();
            }
        }
        if (notify) sink.wake.notify_one();
    }

    template<typename... Args>
    inline void Log(LogType type, const char* func, fmt::format_string<Args...> format, Args&&... args)
    {
        if (type == LogType::Kernel && !g_kernelTrace.load(std::memory_order_relaxed))
            return;
        if (type == LogType::Info && !g_infoTrace.load(std::memory_order_relaxed))
            return;
        if (type == LogType::Verbose && !g_verbose)
            return;

        if (g_async.load(std::memory_order_acquire))
        {
            std::string msg = fmt::format(format, std::forward<Args>(args)...);
            auto line = func
                ? fmt::format("[{:9.3f} t{:04x}] {} {}: {}\n", ElapsedSeconds(), ThreadTag(), Prefix(type), func, msg)
                : fmt::format("[{:9.3f} t{:04x}] {} {}\n", ElapsedSeconds(), ThreadTag(), Prefix(type), msg);
            if (type != LogType::Warning && type != LogType::Error && type != LogType::Notice)
            {
                Enqueue(type, std::move(line));
                return;
            }
            std::lock_guard lock(g_mutex);
            WritePendingLocked();
            WriteLineLocked(type, line, true);
            return;
        }

        std::lock_guard lock(g_mutex);
        std::string msg = fmt::format(format, std::forward<Args>(args)...);
        const auto line = func
            ? fmt::format("[{:9.3f} t{:04x}] {} {}: {}\n", ElapsedSeconds(), ThreadTag(), Prefix(type), func, msg)
            : fmt::format("[{:9.3f} t{:04x}] {} {}\n", ElapsedSeconds(), ThreadTag(), Prefix(type), msg);
        WriteLineLocked(type, line, true);
    }
}

#define LOG_IMPL(type, ...) os::logger::Log(LogType::type, nullptr, __VA_ARGS__)
#define LOGFN_IMPL(type, ...) os::logger::Log(LogType::type, __FUNCTION__, __VA_ARGS__)

#define LOG_INFO(...)      LOG_IMPL(Info, __VA_ARGS__)
#define LOG_NOTICE(...)    LOG_IMPL(Notice, __VA_ARGS__)
#define LOG_WARNING(...)   LOG_IMPL(Warning, __VA_ARGS__)
#define LOG_ERROR(...)     LOG_IMPL(Error, __VA_ARGS__)
#define LOG_VERBOSE(...)   LOG_IMPL(Verbose, __VA_ARGS__)
#define LOGFN_INFO(...)    LOGFN_IMPL(Info, __VA_ARGS__)
#define LOGFN_WARNING(...) LOGFN_IMPL(Warning, __VA_ARGS__)
#define LOGFN_ERROR(...)   LOGFN_IMPL(Error, __VA_ARGS__)
#define LOG_UTILITY(...)   LOGFN_IMPL(Utility, __VA_ARGS__)
#define LOGF_UTILITY(...)  LOGFN_IMPL(Utility, __VA_ARGS__)
#define LOG_KERNEL(...)    LOGFN_IMPL(Kernel, __VA_ARGS__)
#define LOG_STUB()         LOGFN_IMPL(Kernel, "!!! STUB !!!")
