// Steprant 2.0.2: the asynchronous log sink (os/logger.h) and the repeat gate
// (os/log_repeat.h). The game's threads never wait for the log's I/O.
#include <os/logger.h>
#include <os/log_repeat.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <unistd.h>
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
static std::vector<std::string> ReadLines(const std::string& path)
{
    std::ifstream in(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}
int main()
{
    // Off Android every line is also copied to stderr: keep the test quiet.
    if (!freopen("/dev/null", "w", stderr)) return 2;
    try {
        char path[] = "/tmp/lo-log-async-XXXXXX";
        const int fd = mkstemp(path);
        Check(fd >= 0, "temporary log file");
        os::logger::g_file = fdopen(fd, "ab");
        Check(os::logger::g_file != nullptr, "log stream");

        // 1. Not started: synchronous as before.
        LOG_INFO("sync line {}", 1);
        Check(ReadLines(path).size() == 1 && ReadLines(path)[0].find("sync line 1") != std::string::npos,
            "without the sink a line is in the file when the call returns");
        os::logger::WritePending(); // no-op
        Check(ReadLines(path).size() == 1, "WritePending does nothing without the sink");

        // 2. Started: every line arrives, each thread's lines in its order.
        os::logger::StartAsync();
        os::logger::StartAsync(); // idempotent
        constexpr int kThreads = 4, kLines = 2000;
        {
            std::vector<std::thread> threads;
            for (int t = 0; t < kThreads; ++t)
                threads.emplace_back([t] { for (int i = 0; i < kLines; ++i) LOG_INFO("async thread={} line={}", t, i); });
            for (auto& thread : threads) thread.join();
        }
        // 3. A warning is synchronous and writes what is queued first.
        LOG_INFO("before the warning");
        LOG_WARNING("the warning");
        {
            const auto lines = ReadLines(path);
            Check(lines.size() == 1 + kThreads * kLines + 2, "everything queued is in the file when a warning returns");
            Check(lines[lines.size() - 2].find("before the warning") != std::string::npos &&
                lines.back().find("the warning") != std::string::npos && lines.back().find("[warn]") != std::string::npos,
                "the warning comes after the lines logged before it");
            std::map<int, int> next;
            bool ordered = true;
            for (const auto& line : lines) {
                const auto at = line.find("async thread=");
                if (at == std::string::npos) continue;
                int t = 0, i = 0;
                if (std::sscanf(line.c_str() + at, "async thread=%d line=%d", &t, &i) != 2 || next[t]++ != i) ordered = false;
            }
            Check(ordered && int(next.size()) == kThreads, "each thread's lines keep their order");
        }
        Check(os::logger::GetAsyncStats().dropped == 0, "nothing dropped while the writer keeps up");
        LOG_INFO("queued at stop");
        os::logger::StopAsync();
        Check(ReadLines(path).back().find("queued at stop") != std::string::npos, "stopping writes what is queued");
        LOG_INFO("sync again");
        Check(ReadLines(path).back().find("sync again") != std::string::npos, "after the stop logging is synchronous again");
        fclose(os::logger::g_file);
        unlink(path);

        // 4. A sink that does not take data (a full pipe): the callers do not
        // wait, the queue fills, lines are dropped and counted.
        int pipeFds[2];
        Check(pipe(pipeFds) == 0, "pipe");
        os::logger::g_file = fdopen(pipeFds[1], "ab");
        const auto before = os::logger::GetAsyncStats();
        os::logger::StartAsync();
        const std::string payload(200, 'x');
        constexpr int kFlood = 40000; // 8 MB, against a 64 KiB pipe and a 1 MiB queue
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kFlood; ++i) LOG_INFO("flood {} {}", i, payload);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const auto during = os::logger::GetAsyncStats();
        Check(seconds < 5.0, "40000 lines against a blocked sink return without waiting for it");
        Check(during.dropped > before.dropped, "a full queue drops lines and counts them");
        Check(during.peakLines <= os::logger::AsyncSink::kMaxLines, "the queue stays within its limit");
        // Let the writer finish: read the pipe until the sink has stopped.
        std::atomic<bool> done{false};
        size_t floodLines = 0, notices = 0;
        std::thread reader([&] {
            FILE* in = fdopen(pipeFds[0], "rb");
            char buffer[1024];
            while (fgets(buffer, sizeof(buffer), in)) {
                if (std::strstr(buffer, "flood ")) ++floodLines;
                if (std::strstr(buffer, "lines dropped")) ++notices;
            }
            fclose(in);
            done = true;
        });
        os::logger::StopAsync();
        fclose(os::logger::g_file); // closes the write end: the reader sees the end
        os::logger::g_file = nullptr;
        reader.join();
        const auto after = os::logger::GetAsyncStats();
        Check(floodLines + (after.dropped - before.dropped) == size_t(kFlood), "every line was either written or counted as dropped");
        Check(notices >= 1, "the writer reports the dropped lines in the log");
        std::printf("  blocked sink: %d lines in %.3f s, %llu dropped, %zu written\n", kFlood, seconds,
            static_cast<unsigned long long>(after.dropped - before.dropped), floodLines);

        // 5. The repeat gate.
        os::log_repeat::Gate gate;
        const auto t0 = std::chrono::steady_clock::now();
        using os::log_repeat::Action;
        for (int i = 0; i < 4; ++i) Check(gate.Note(t0) == Action::Verbatim, "the first occurrences are logged as they are");
        Check(gate.Note(t0 + std::chrono::seconds(1)) == Action::Silent, "then they are only counted");
        for (int i = 0; i < 98; ++i) gate.Note(t0 + std::chrono::seconds(2));
        Check(gate.Note(t0 + std::chrono::seconds(31)) == Action::Summary, "one summary per interval");
        Check(gate.Take() == 100 && gate.total == 104, "the summary reports what happened since the last line");
        Check(gate.Note(t0 + std::chrono::seconds(32)) == Action::Silent && gate.Take() == 1, "and the count starts again");
        std::printf("log async: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("log async test failed: %s\n", e.what());
        return 1;
    }
}
