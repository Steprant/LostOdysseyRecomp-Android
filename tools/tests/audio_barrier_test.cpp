// Steprant 2.1.6: the wait of the audio workers' rendezvous
// (patches/audio_barrier.h). The full guest function is compared with the
// generated one in CI (tools/audio_barrier_equivalence.py).
//
// Steprant 2.4.5: rounds back to back, with nothing between them, where the
// 2.1.6 rules (and the guest's own spin) lose a closing 0 or erase a flag
// with a second close and stop both threads within a few thousand rounds.
#include <patches/audio_barrier.h>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
unsigned checks = 0;
void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
using Clock = std::chrono::steady_clock;
int64_t Ms(Clock::time_point since) { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count(); }

// Guest bytes 0..7 of the flag word as the raw little-endian host value.
uint64_t Raw(std::initializer_list<int> bytes)
{
    uint8_t b[8] = {};
    for (int i : bytes) b[i] = 1;
    uint64_t v;
    std::memcpy(&v, b, 8);
    return v;
}
}

int main()
{
    using namespace audio_barrier;
    try {
        Settings s;
        s.enabled = true;
        // 1. Nothing to wait for: complete, or already closed.
        {
            Stats st;
            Slot slot;
            alignas(8) uint64_t word = Raw({0, 1, 2});
            Check(WaitForWord(&word, Raw({0, 1, 2}), slot, s, st) == Raw({0, 1, 2}), "complete word not returned");
            word = 0;
            Check(WaitForWord(&word, Raw({0, 1, 2}), slot, s, st) == 0, "closed word not returned");
            Check(st.waits.load() == 0, "waited without a reason");
        }
        // 2. The last participant arrives through the protocol while the
        // waiter sleeps on the futex: woken by the arrival, not the timeout.
        {
            Stats st;
            Slot slot;
            Settings sleepy = s;
            sleepy.spinNs = 0;
            sleepy.wfeUs = 0;
            sleepy.sleepUs = 1'000'000;
            alignas(8) uint64_t word = Raw({0, 1});
            std::thread late([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                StoreFlag(reinterpret_cast<uint8_t*>(&word) + 2, slot, st);
            });
            const auto start = Clock::now();
            const uint64_t seen = WaitForWord(&word, Raw({0, 1, 2}), slot, sleepy, st);
            late.join();
            Check(seen == Raw({0, 1, 2}), "arrival not seen");
            Check(Ms(start) < 300, "the arrival did not wake the sleeper");
            Check(st.sleeps.load() >= 1 && st.timeouts.load() == 0 && st.wakes.load() >= 1, "no futex sleep, or woken by the timeout");
        }
        // 3. A write made outside the protocol (no wake) is seen at the
        // next timeout.
        {
            Stats st;
            Slot slot;
            Settings sleepy = s;
            sleepy.spinNs = 0;
            sleepy.wfeUs = 0;
            sleepy.sleepUs = 1000;
            alignas(8) uint64_t word = Raw({3});
            std::thread closer([&] { std::this_thread::sleep_for(std::chrono::milliseconds(3)); __atomic_store_n(&word, uint64_t(0), __ATOMIC_SEQ_CST); });
            const auto start = Clock::now();
            const uint64_t seen = WaitForWord(&word, Raw({0, 3}), slot, sleepy, st);
            closer.join();
            Check(seen == 0, "closing zero not seen");
            Check(st.timeouts.load() >= 1 && Ms(start) < 300, "timeout did not catch an unannounced write");
        }
        // 4. Short arrival with the default phases: no sleep needed.
        {
            Stats st;
            Slot slot;
            alignas(8) uint64_t word = Raw({1});
            std::thread late([&] { StoreFlag(reinterpret_cast<uint8_t*>(&word) + 4, slot, st); });
            const uint64_t seen = WaitForWord(&word, Raw({1, 4}), slot, s, st);
            late.join();
            Check(seen == Raw({1, 4}), "quick arrival not seen");
        }
        // 5. Rounds of the guest protocol between three threads: each sets its
        // byte, waits, and the one that sees the full word stores 0. A
        // std::barrier separates rounds, as the engine's own pass does. Run
        // with the default phases and with every wait on the futex.
        for (int variant = 0; variant < 2; ++variant) {
            Stats st;
            Slot slot;
            Settings run = s;
            if (variant == 1) { run.spinNs = 0; run.wfeUs = 0; run.sleepUs = 200'000; }
            alignas(8) uint64_t word = 0;
            constexpr int threads = 3, rounds = 20000;
            const uint64_t expected = Raw({0, 1, 2});
            std::barrier gate(threads);
            std::atomic<int> closes{0};
            std::vector<std::thread> pool;
            const auto start = Clock::now();
            for (int t = 0; t < threads; ++t)
                pool.emplace_back([&, t] {
                    for (int r = 0; r < rounds; ++r) {
                        StoreFlag(reinterpret_cast<uint8_t*>(&word) + t, slot, st);
                        if (WaitForWord(&word, expected, slot, run, st) != 0 && CloseRound(&word, expected, slot, st))
                            closes.fetch_add(1);
                        gate.arrive_and_wait();
                    }
                });
            for (auto& thread : pool) thread.join();
            Check(word == 0, "a round left the word set");
            Check(closes.load() == rounds, "a round was not closed exactly once");
            // With a 200 ms timeout, any timeout would be a lost wake: every
            // arrival goes through StoreFlag. (With the default 1 ms one, an
            // oversubscribed host can make a participant legitimately late.)
            Check(variant == 0 || st.timeouts.load() == 0, "a protocol arrival needed the timeout");
            std::printf("audio barrier (%s): %d rounds of 3 threads in %lld ms, %llu waits, %llu sleeps, %llu timeouts\n",
                variant ? "futex only" : "default phases", rounds, (long long)Ms(start), (unsigned long long)st.waits.load(),
                (unsigned long long)st.sleeps.load(), (unsigned long long)st.timeouts.load());
        }
        // 6. Back-to-back rounds as the guest function runs them (Run): set
        // the own byte, wait with the own byte watched, close by compare and
        // exchange. No thread may leave a round before every participant
        // arrived at it, and the rounds must all end (a watchdog reports a
        // stop instead of hanging the test).
        struct Shape { int threads, rounds; bool futexOnly; };
        for (const Shape shape : {Shape{2, 20000, false}, Shape{2, 20000, true}, Shape{3, 10000, false}, Shape{3, 10000, true}}) {
            Stats st;
            Slot slot;
            Settings run = s;
            if (shape.futexOnly) { run.spinNs = 0; run.wfeUs = 0; run.sleepUs = 200'000; }
            alignas(8) uint64_t word = 0;
            uint64_t expected = 0;
            for (int t = 0; t < shape.threads; ++t) expected |= Raw({t});
            std::atomic<int> arrivals[3] = {};
            std::atomic<uint64_t> progress{0}, ahead{0};
            std::atomic<int> finished{0};
            std::vector<std::thread> pool;
            const auto start = Clock::now();
            for (int t = 0; t < shape.threads; ++t)
                pool.emplace_back([&, t] {
                    for (int r = 0; r < shape.rounds; ++r) {
                        arrivals[t].store(r + 1, std::memory_order_seq_cst);
                        StoreFlag(reinterpret_cast<uint8_t*>(&word) + t, slot, st);
                        const uint64_t seen = WaitForWord(&word, expected, slot, run, st, OwnMask(uint32_t(t)));
                        for (int u = 0; u < shape.threads; ++u)
                            if (arrivals[u].load(std::memory_order_seq_cst) < r + 1) ahead.fetch_add(1);
                        Finish(&word, seen, expected, slot, st);
                        progress.fetch_add(1, std::memory_order_relaxed);
                    }
                    finished.fetch_add(1);
                });
            uint64_t last = ~0ull;
            auto lastChange = Clock::now();
            while (finished.load() < shape.threads) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                const uint64_t now = progress.load();
                if (now != last) { last = now; lastChange = Clock::now(); }
                else if (Ms(lastChange) > 10'000) {
                    std::fprintf(stderr, "audio barrier test failed: %d threads stopped after %llu of %d rounds\n", shape.threads,
                        (unsigned long long)(now / shape.threads), shape.rounds);
                    std::fflush(stderr);
                    std::_Exit(1);
                }
            }
            for (auto& thread : pool) thread.join();
            Check(ahead.load() == 0, "a thread left a round before every participant arrived");
            Check(word == 0, "the last round left the word set");
            Check(!shape.futexOnly || st.timeouts.load() == 0, "a protocol write needed the timeout");
            std::printf("audio barrier back to back (%s): %d rounds of %d threads in %lld ms, %llu waits, %llu sleeps, %llu missed closes, %llu late closes\n",
                shape.futexOnly ? "futex only" : "default phases", shape.rounds, shape.threads, (long long)Ms(start),
                (unsigned long long)st.waits.load(), (unsigned long long)st.sleeps.load(),
                (unsigned long long)st.missedCloses.load(), (unsigned long long)st.lateCloses.load());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "audio barrier test failed: %s (after %u checks)\n", e.what(), checks);
        return 1;
    }
    std::printf("audio barrier test: %u checks passed\n", checks);
    return 0;
}
