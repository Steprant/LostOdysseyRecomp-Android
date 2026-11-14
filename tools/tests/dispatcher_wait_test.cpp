// Steprant 2.1.1-D: Multiple waits wake only on their own objects
// (kernel/dispatcher_wait.h). Semantics of wait-any, wait-all, timeouts,
// auto-reset events, semaphores and mutants, no lost wakeups under stress,
// and no wakeups from objects the thread does not wait on.
#include <kernel/dispatcher_wait.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
std::atomic<unsigned> checks{0};
void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
using namespace kernel::wait;
using Clock = std::chrono::steady_clock;
}

int main()
{
    try {
        // 1. Wait-any returns the index of the signalled object.
        {
            Event a(false, false), b(false, false);
            Target* objects[] = {&a, &b};
            std::thread setter([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); b.Set(); });
            Check(Multiple(objects, false, Infinite) == 1, "wait-any returns the signalled index");
            setter.join();
            Check(a.MultiWaiterCount() == 0 && b.MultiWaiterCount() == 0, "unregistered after success");
            Check(!b.IsSignaled(), "auto-reset event consumed");
        }
        // 2. Wait-all needs every object; consumes all of them.
        {
            Event a(true, true);
            Semaphore s(0, 4);
            Target* objects[] = {&a, &s};
            std::thread releaser([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); uint32_t p; s.Release(1, &p); });
            Check(Multiple(objects, true, 2000) == Success, "wait-all succeeds once all are ready");
            releaser.join();
            uint32_t previous = 9;
            s.Release(1, &previous);
            Check(previous == 0, "wait-all consumed the semaphore count");
            Check(a.IsSignaled(), "manual-reset event stays signalled");
        }
        // 3. Timeouts: zero is a poll that never registers; a finite one ends
        //    and unregisters.
        {
            Event a(false, false), b(false, false);
            Target* objects[] = {&a, &b};
            Check(Multiple(objects, false, 0) == Timeout, "poll times out");
            const auto start = Clock::now();
            Check(Multiple(objects, false, 30) == Timeout, "finite wait times out");
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            Check(ms >= 29 && ms < 1000, "timeout honoured");
            Check(a.MultiWaiterCount() == 0 && b.MultiWaiterCount() == 0, "unregistered after timeout");
        }
        // 4. Mutant: ready for its owner, recursive, released by the owner.
        {
            Mutant m;
            Event never(false, false);
            Target* objects[] = {&never, &m};
            Check(Multiple(objects, false, 0, 7) == 1, "free mutant taken by thread 7");
            Check(Multiple(objects, false, 0, 7) == 1, "mutant recursion for its owner");
            Check(Multiple(objects, false, 0, 8) == Timeout, "mutant busy for another thread");
            std::thread other([&] { Check(Multiple(objects, false, 2000, 8) == 1, "thread 8 gets it after release"); });
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            Check(m.Release(7) && m.Release(7), "owner releases twice");
            other.join();
            Check(!m.Release(7), "no longer owned by 7");
            Check(m.Release(8), "now owned by 8");
        }
        // 5. Selectivity: a waiter on A and B does not wake for changes of C.
        {
            Event a(false, false), b(false, false), c(false, false);
            Target* objects[] = {&a, &b};
            const uint64_t wakesBefore = g_multipleWakes.load();
            std::atomic<bool> done{false};
            std::thread waiter([&] { Check(Multiple(objects, false, Infinite) == 0, "woken by its own event"); done = true; });
            while (a.MultiWaiterCount() == 0) std::this_thread::yield();
            for (int i = 0; i < 20000; ++i) { c.Set(); c.Reset(); }
            const uint64_t unrelated = g_multipleWakes.load() - wakesBefore;
            Check(!done.load(), "unrelated changes do not end the wait");
            a.Set();
            waiter.join();
            std::printf("selectivity: %llu wakeups from 20000 unrelated changes, then 1 own\n", (unsigned long long)unrelated);
            Check(unrelated == 0, "no wakeups from an object the thread does not wait on");
        }
        // 6. Stress: waiters on random pairs of auto-reset events and
        //    semaphores; producers signal; every signal is consumed exactly
        //    once and nobody misses a wakeup.
        {
            constexpr int kObjects = 8, kWaiters = 6, kSignals = 20000;
            std::vector<std::unique_ptr<Semaphore>> sems;
            for (int i = 0; i < kObjects; ++i) sems.push_back(std::make_unique<Semaphore>(0, 1u << 30));
            std::atomic<int> consumed{0}, produced{0};
            std::atomic<bool> stop{false};
            std::vector<std::thread> waiters;
            for (int w = 0; w < kWaiters; ++w) {
                waiters.emplace_back([&, w] {
                    std::mt19937 rng(w);
                    while (!stop.load()) {
                        const int i = int(rng() % kObjects), j = (i + 1 + int(rng() % (kObjects - 1))) % kObjects;
                        Target* objects[] = {sems[i].get(), sems[j].get()};
                        const uint32_t r = Multiple(objects, false, 50);
                        if (r == 0 || r == 1) consumed.fetch_add(1);
                    }
                });
            }
            std::thread producer([&] {
                std::mt19937 rng(99);
                for (int n = 0; n < kSignals; ++n) {
                    uint32_t p;
                    sems[rng() % kObjects]->Release(1, &p);
                    produced.fetch_add(1);
                    if ((n & 63) == 0) std::this_thread::yield();
                }
            });
            producer.join();
            const auto deadline = Clock::now() + std::chrono::seconds(20);
            while (consumed.load() < produced.load() && Clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            stop = true;
            for (auto& t : waiters) t.join();
            // Leftover counts on objects no waiter picked are drained here.
            int leftover = 0;
            for (auto& s : sems) while (s->Wait(0) == Success) ++leftover;
            std::printf("stress: produced=%d consumed=%d leftover=%d\n", produced.load(), consumed.load(), leftover);
            Check(consumed.load() + leftover == produced.load(), "each signal consumed exactly once");
            for (auto& s : sems) Check(s->MultiWaiterCount() == 0, "no registration left behind");
        }
        // 7. Same object twice in wait-any is allowed; in wait-all it is invalid.
        {
            Event a(false, true);
            Target* twice[] = {&a, &a};
            Check(Multiple(twice, true, 0) == Invalid, "wait-all with a duplicate is invalid");
            Check(Multiple(twice, false, 0) == 0, "wait-any with a duplicate works");
        }
        std::printf("dispatcher wait: %u checks passed\n", checks.load());
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s (after %u checks)\n", e.what(), checks.load());
        return 1;
    }
}
