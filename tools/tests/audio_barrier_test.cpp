// Steprant 2.1.6: the wait of the audio workers' rendezvous
// (patches/audio_barrier.h). The full guest function is compared with the
// generated one in CI (tools/audio_barrier_equivalence.py).
#include <patches/audio_barrier.h>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
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
void SetByte(uint64_t& word, int i) { __atomic_store_n(reinterpret_cast<uint8_t*>(&word) + i, uint8_t(1), __ATOMIC_RELEASE); }
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
            alignas(8) uint64_t word = Raw({0, 1, 2});
            Check(WaitForWord(&word, Raw({0, 1, 2}), s, st) == Raw({0, 1, 2}), "complete word not returned");
            word = 0;
            Check(WaitForWord(&word, Raw({0, 1, 2}), s, st) == 0, "closed word not returned");
            Check(st.waits.load() == 0, "waited without a reason");
        }
        // 2. The last participant arrives later: woken with the full word.
        {
            Stats st;
            alignas(8) uint64_t word = Raw({0, 1});
            std::thread late([&] { std::this_thread::sleep_for(std::chrono::microseconds(300)); SetByte(word, 2); });
            const auto start = Clock::now();
            const uint64_t seen = WaitForWord(&word, Raw({0, 1, 2}), s, st);
            late.join();
            Check(seen == Raw({0, 1, 2}), "arrival not seen");
            Check(Ms(start) < 200, "arrival seen late");
            Check(st.waits.load() == 1 && st.waitNs.load() > 0, "wait not counted");
        }
        // 3. Another participant closes the round (stores 0): the wait ends with 0.
        {
            Stats st;
            alignas(8) uint64_t word = Raw({3});
            std::thread closer([&] { std::this_thread::sleep_for(std::chrono::microseconds(300)); __atomic_store_n(&word, uint64_t(0), __ATOMIC_RELEASE); });
            const uint64_t seen = WaitForWord(&word, Raw({0, 3}), s, st);
            closer.join();
            Check(seen == 0, "closing zero not seen");
        }
        // 4. A long wait sleeps between checks and still ends on the arrival.
        {
            Stats st;
            Settings quick = s;
            quick.spinNs = 0;
            quick.wfeUs = 0;
            quick.sleepUs = 50;
            alignas(8) uint64_t word = Raw({1});
            std::thread late([&] { std::this_thread::sleep_for(std::chrono::milliseconds(3)); SetByte(word, 4); });
            const auto start = Clock::now();
            const uint64_t seen = WaitForWord(&word, Raw({1, 4}), quick, st);
            late.join();
            Check(seen == Raw({1, 4}), "arrival after sleeps not seen");
            Check(st.sleeps.load() > 0 && Ms(start) < 500, "sleep phase not used or too slow");
            Check(st.over1ms.load() == 1, "long wait not counted");
        }
        // 5. Rounds of the guest protocol between three threads: each sets its
        // byte, waits, and the one that sees the full word stores 0. A
        // std::barrier separates rounds, as the engine's own pass does.
        {
            Stats st;
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
                        SetByte(word, t);
                        if (WaitForWord(&word, expected, s, st) != 0) {
                            __atomic_store_n(&word, uint64_t(0), __ATOMIC_RELEASE);
                            closes.fetch_add(1);
                        }
                        gate.arrive_and_wait();
                    }
                });
            for (auto& thread : pool) thread.join();
            Check(word == 0, "a round left the word set");
            Check(closes.load() >= rounds, "a round was never closed");
            std::printf("audio barrier: %d rounds of 3 threads in %lld ms, %llu waits, %llu sleeps\n",
                rounds, (long long)Ms(start), (unsigned long long)st.waits.load(), (unsigned long long)st.sleeps.load());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "audio barrier test failed: %s (after %u checks)\n", e.what(), checks);
        return 1;
    }
    std::printf("audio barrier test: %u checks passed\n", checks);
    return 0;
}
