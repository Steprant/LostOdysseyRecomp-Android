// Steprant 2.1.1-C: guest threads polling a critical section wait for a
// release instead of spinning (kernel/cs_poll_wait.h).
#include <kernel/critical_section.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <cstdlib>
#include <unistd.h>

namespace
{
unsigned checks = 0;
void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }

struct alignas(8) Section { int32_t recursion = 0; uint32_t owner = 0; };

using Clock = std::chrono::steady_clock;
uint64_t Ms(Clock::duration d) { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(d).count()); }
}

int main()
{
#if !defined(__linux__)
    std::printf("cs poll wait: futex waits are Linux/Android only, skipped\n");
    return 0;
#endif
    try {
        namespace cpw = cs_poll_wait;
        // 0. Off by default: a thread working alone under a lock it takes
        //    with TryEnter (the 2.1.1-C start-up hang) runs at full speed.
        //    Then the process runs itself again with the poll wait on, for
        //    the mechanism tests (Enabled() is read once per process).
        if (std::getenv("LO_CS_POLL_WAIT_CHILD") == nullptr) {
            if (std::getenv("LO_CS_POLL_WAIT") == nullptr) {
                Check(!cpw::Enabled(), "the poll wait is off unless LO_CS_POLL_WAIT=1");
                Section cs;
                const auto start = Clock::now();
                for (int i = 0; i < 20000; ++i) {
                    Check(GuestCriticalSection::TryEnter(cs.recursion, cs.owner, 0x1234), "free lock taken");
                    GuestCriticalSection::Leave(cs.recursion, cs.owner);
                }
                const uint64_t ms = Ms(Clock::now() - start);
                std::printf("default off: one thread, 20000 TryEnter/Leave in %llu ms (%u checks)\n", (unsigned long long)ms, checks);
                Check(ms < 200, "a lock used by one thread is never slowed down when off");
                Check(cpw::g_stats.waits.load() == 0, "no waits when off");
            }
            std::fflush(stdout);
            Check(setenv("LO_CS_POLL_WAIT", "1", 1) == 0 && setenv("LO_CS_POLL_WAIT_CHILD", "1", 1) == 0, "setenv");
            execl("/proc/self/exe", "cs_poll_wait_test", static_cast<char*>(nullptr));
            throw std::runtime_error("re-exec with the poll wait on failed");
        }
        Check(cpw::Enabled(), "LO_CS_POLL_WAIT=1 turns the poll wait on");
        // 1. A queue polled with TryEnter/Leave; the producer pushes under the lock.
        {
            Section cs;
            int queue = 0; // guarded by cs
            uint64_t polls = 0;
            const uint64_t waitsBefore = cpw::g_stats.waits.load(), wokenBefore = cpw::g_stats.woken.load();
            Clock::time_point pushed{}, seen{};
            std::thread poller([&] {
                for (;;) {
                    ++polls;
                    if (GuestCriticalSection::TryEnter(cs.recursion, cs.owner, 0x1001)) {
                        const bool work = queue != 0;
                        GuestCriticalSection::Leave(cs.recursion, cs.owner);
                        if (work) { seen = Clock::now(); return; }
                    }
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            GuestCriticalSection::Enter(cs.recursion, cs.owner, 0x2002);
            queue = 1;
            pushed = Clock::now();
            GuestCriticalSection::Leave(cs.recursion, cs.owner);
            poller.join();
            const uint64_t waits = cpw::g_stats.waits.load() - waitsBefore;
            std::printf("queue: %llu polls in 100 ms, %llu waits, pickup %.3f ms\n", (unsigned long long)polls,
                (unsigned long long)waits, std::chrono::duration<double, std::milli>(seen - pushed).count());
            Check(waits > 0 && cpw::g_stats.woken.load() > wokenBefore, "the poller waited and a release woke it");
            // Spinning would be millions of polls; waiting with a 0.5 ms limit
            // is at most a few hundred plus kPollsBeforeWait per wakeup.
            Check(polls < 20000, "far fewer polls than a spin");
            Check(seen - pushed < std::chrono::milliseconds(20), "the producer's release wakes the poller promptly");
        }
        // 2. A change made outside the lock is seen within the time limit.
        {
            Section cs;
            std::atomic<bool> flag{false};
            const uint64_t timeoutsBefore = cpw::g_stats.timeouts.load();
            Clock::time_point set{}, seen{};
            std::thread poller([&] {
                for (;;) {
                    if (GuestCriticalSection::TryEnter(cs.recursion, cs.owner, 0x1003)) {
                        GuestCriticalSection::Leave(cs.recursion, cs.owner);
                        if (flag.load()) { seen = Clock::now(); return; }
                    }
                }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            set = Clock::now();
            flag = true;
            poller.join();
            Check(cpw::g_stats.timeouts.load() > timeoutsBefore, "with nobody releasing, the waits end at the limit");
            Check(seen - set < std::chrono::milliseconds(15), "an outside change is seen soon after the limit");
        }
        // 3. A thread that owns the lock never waits on it; another lock resets the count.
        {
            Section a, b;
            const uint64_t waitsBefore = cpw::g_stats.waits.load();
            GuestCriticalSection::Enter(a.recursion, a.owner, 0x1004);
            for (int i = 0; i < 1000; ++i) {
                Check(GuestCriticalSection::TryEnter(a.recursion, a.owner, 0x1004), "recursive try-enter");
                GuestCriticalSection::Leave(a.recursion, a.owner);
            }
            GuestCriticalSection::Leave(a.recursion, a.owner);
            Check(cpw::g_stats.waits.load() == waitsBefore, "no wait while owning the lock");
            const auto start = Clock::now();
            for (int i = 0; i < 1000; ++i) {
                Section& s = (i & 1) ? a : b;
                Check(GuestCriticalSection::TryEnter(s.recursion, s.owner, 0x1004), "alternating locks");
                GuestCriticalSection::Leave(s.recursion, s.owner);
            }
            Check(cpw::g_stats.waits.load() == waitsBefore && Ms(Clock::now() - start) < 50,
                "alternating between two locks is not a poll of one");
        }
        // 4. A busy lock: the poller waits for the holder's release.
        {
            Section cs;
            GuestCriticalSection::Enter(cs.recursion, cs.owner, 0x2005);
            std::atomic<bool> got{false};
            Clock::time_point released{}, acquired{};
            std::thread poller([&] {
                // First a release of its own, as a polling loop has.
                GuestCriticalSection::Enter(cs.recursion, cs.owner, 0x1005); // blocks until the holder leaves once
                GuestCriticalSection::Leave(cs.recursion, cs.owner);
                got = true;
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            released = Clock::now();
            GuestCriticalSection::Leave(cs.recursion, cs.owner);
            poller.join();
            acquired = Clock::now();
            Check(got.load(), "a blocking enter still works");
            (void)released; (void)acquired;
        }
        std::printf("cs poll wait: %u checks passed (waits=%llu woken=%llu timeouts=%llu)\n", checks,
            (unsigned long long)cs_poll_wait::g_stats.waits.load(), (unsigned long long)cs_poll_wait::g_stats.woken.load(),
            (unsigned long long)cs_poll_wait::g_stats.timeouts.load());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cs poll wait test failed: %s\n", e.what());
        return 1;
    }
}
