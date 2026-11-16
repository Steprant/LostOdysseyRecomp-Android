// Steprant 2.1.5: back-off for guest polls (kernel/poll_backoff.h) and the
// bounded wait it uses (kernel::wait::Target::WaitFor).
#include <kernel/dispatcher_wait.h>
#include <kernel/poll_backoff.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace
{
unsigned checks = 0;
void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
using Table = kernel::poll_backoff::Table;
using Clock = std::chrono::steady_clock;
constexpr int64_t kUs = 1000;
}

int main()
{
    namespace pb = kernel::poll_backoff;
    try {
        // 1. Short spins stay plain polls: below kFreePolls failures, or a run
        // shorter than kMinSpinNs, never waits.
        {
            Table table;
            auto& e = table.Find(0x100);
            int64_t now = 0;
            for (uint32_t i = 0; i < pb::kFreePolls; ++i, now += 10 * kUs) {
                Check(!Table::ShouldWait(e, now), "waited before kFreePolls failures");
                Table::Record(e, false, now);
            }
            Check(Table::ShouldWait(e, now), "no wait after a long run of failures");

            Table quick;
            auto& q = quick.Find(0x100);
            for (uint32_t i = 0; i < 4 * pb::kFreePolls; ++i) Table::Record(q, false, i);
            Check(!Table::ShouldWait(q, 4 * pb::kFreePolls), "waited after a run shorter than kMinSpinNs");
        }
        // 2. A success, or a gap longer than kGapNs, ends the run.
        {
            Table table;
            auto& e = table.Find(0x200);
            int64_t now = 0;
            for (uint32_t i = 0; i < 2 * pb::kFreePolls; ++i, now += 10 * kUs) Table::Record(e, false, now);
            Check(Table::ShouldWait(e, now), "run not detected");
            Table::Record(e, true, now);
            Check(!Table::ShouldWait(e, now + kUs), "success did not end the run");
            for (uint32_t i = 0; i < 2 * pb::kFreePolls; ++i, now += 10 * kUs) Table::Record(e, false, now);
            Check(Table::ShouldWait(e, now), "second run not detected");
            Check(!Table::ShouldWait(e, now + pb::kGapNs + kUs), "gap did not end the run");
        }
        // 3. Objects are tracked apart: a success on one does not end the
        // run on another, and a fifth object evicts the oldest entry.
        {
            Table table;
            int64_t now = 0;
            for (uint32_t i = 0; i < 2 * pb::kFreePolls; ++i, now += 10 * kUs) {
                Table::Record(table.Find(0x300), false, now);
                Table::Record(table.Find(0x304), true, now);
            }
            Check(Table::ShouldWait(table.Find(0x300), now), "success on another object ended the run");
            for (uint32_t h = 0x400; h < 0x404; ++h) table.Find(h);
            Check(table.Find(0x300).failures == 0, "evicted entry kept its run");
        }
        // 4. WaitFor times out after about its bound on an unsignaled event.
        {
            kernel::wait::Event event(false, false);
            const auto start = Clock::now();
            Check(event.WaitFor(pb::kWaitUs) == kernel::wait::Timeout, "unsignaled event did not time out");
            const auto took = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
            Check(took >= pb::kWaitUs - 50, "returned well before the bound");
            Check(took < 50'000, "far past the bound");
        }
        // 5. A signal wakes it at once; an auto-reset event is consumed; a
        // manual-reset event stays signaled.
        {
            kernel::wait::Event event(false, false);
            std::thread setter([&] { std::this_thread::sleep_for(std::chrono::microseconds(100)); event.Set(); });
            const auto start = Clock::now();
            uint32_t result = kernel::wait::Timeout;
            while (result == kernel::wait::Timeout) result = event.WaitFor(200'000);
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
            setter.join();
            Check(result == kernel::wait::Success, "signal not seen");
            Check(took < 100, "signal did not wake the wait");
            Check(!event.IsSignaled(), "auto-reset event not consumed");

            kernel::wait::Event manual(true, true);
            Check(manual.WaitFor(1) == kernel::wait::Success && manual.IsSignaled(), "manual-reset event consumed");
        }
        // 6. Semaphores and mutants keep their rules.
        {
            kernel::wait::Semaphore sem(1, 2);
            Check(sem.WaitFor(1) == kernel::wait::Success, "semaphore count not taken");
            Check(sem.WaitFor(1) == kernel::wait::Timeout, "empty semaphore taken");
            kernel::wait::Mutant mutant(7);
            Check(mutant.WaitFor(1, 7) == kernel::wait::Success, "owner could not re-acquire");
            Check(mutant.WaitFor(1, 8) == kernel::wait::Timeout, "other thread acquired an owned mutant");
        }
        // 7. Locks held are counted per thread.
        {
            pb::t_locksHeld = 2;
            int other = -1;
            std::thread([&] { other = pb::t_locksHeld; }).join();
            Check(other == 0, "lock count shared between threads");
            pb::t_locksHeld = 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "poll back-off test failed: %s (after %u checks)\n", e.what(), checks);
        return 1;
    }
    std::printf("poll back-off test: %u checks passed\n", checks);
    return 0;
}
