#include "gpu/android_frame_generation_budget.h"
#include "gpu/android_frame_presenter.h"

#include <cstdio>
#include <cstdlib>
#include <future>

using namespace gpu::android_fg;
using namespace std::chrono_literals;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
    std::exit(1); \
} } while (false)

struct Run {
    Clock::time_point next{1s};
    Budget budget;

    Pressure Native(Clock::duration gap = 33ms, Clock::duration work = 3ms)
    {
        next += gap;
        return budget.NativePresented(next, next + work);
    }

    void Stable(unsigned frames)
    {
        for (unsigned i = 0; i < frames; ++i)
            CHECK(Native() == Pressure::None);
    }
};

int main()
{
    Run run;
    CHECK(run.budget.ThermallyAllowed(414));
    CHECK(!run.budget.ThermallyAllowed(415));
    CHECK(!run.budget.ThermallyAllowed(412));
    CHECK(!run.budget.ThermallyAllowed(0));
    CHECK(run.budget.ThermallyAllowed(405));
    for (int i = 0; i < 90; ++i)
        CHECK(run.Native(58ms, 40ms) == Pressure::None && !run.budget.Capturing());
    run.Stable(23);
    CHECK(!run.budget.Capturing());
    run.Stable(1);
    CHECK(run.budget.Capturing());
    run.Stable(1);
    CHECK(run.budget.CanGenerate(run.next + 10ms));
    CHECK(!run.budget.CanGenerate(run.next + 19ms));

    // The long native intervals observed with the generator enabled suspend
    // input capture before another prediction can be queued.
    CHECK(run.Native(65ms, 38ms) == Pressure::NativeCadence);
    CHECK(!run.budget.Capturing() && !run.budget.CanGenerate(run.next + 4ms));
    run.Stable(60);
    CHECK(!run.budget.Capturing());
    run.Stable(28);
    CHECK(run.budget.Capturing());

    // A 30ms driver call on the worker is safe if it finished while the next
    // real frame was being rendered. Only a join that delays the game's
    // presentation (or missed display slots) should cut off generation.
    CHECK(run.budget.ExtraFinished(run.next + 30ms, ExtraFrame::Presented) == Pressure::None);
    CHECK(run.budget.Capturing());
    run.Stable(1);
    CHECK(run.budget.ExtraFinished(run.next + 30ms, ExtraFrame::Presented, 9ms) == Pressure::Present);
    CHECK(!run.budget.Capturing());
    run.Stable(120);
    CHECK(!run.budget.Capturing());
    run.Stable(28);
    CHECK(run.budget.Capturing());

    // Two history frames can prime a new scene. Empty captures then stop.
    for (int i = 0; i < 5; ++i) {
        CHECK(run.budget.ExtraFinished(run.next + 3ms, ExtraFrame::MissingInputs) == Pressure::None);
        run.Stable(1);
    }
    CHECK(run.budget.ExtraFinished(run.next + 3ms, ExtraFrame::MissingInputs) == Pressure::Inputs);
    CHECK(!run.budget.Capturing());

    Run intermittent;
    intermittent.Stable(25);
    for (int i = 0; i < 11; ++i) {
        CHECK(intermittent.budget.ExtraFinished(intermittent.next + 8ms,
            i % 2 ? ExtraFrame::Busy : ExtraFrame::Presented) == Pressure::None);
        intermittent.Stable(1);
    }
    CHECK(intermittent.budget.ExtraFinished(intermittent.next + 8ms, ExtraFrame::Busy) == Pressure::Present);
    CHECK(!intermittent.budget.Capturing());
    intermittent.budget.Interrupt();
    intermittent.Stable(25);
    CHECK(!intermittent.budget.Capturing());

    Run deadlines;
    deadlines.Stable(25);
    for (int i = 0; i < 3; ++i) {
        CHECK(deadlines.budget.ExtraFinished(deadlines.next + 33ms, ExtraFrame::Late) == Pressure::None);
        deadlines.Stable(1);
    }
    CHECK(deadlines.budget.ExtraFinished(deadlines.next + 33ms, ExtraFrame::Late) == Pressure::Deadline);

    DisplayCadence cadence;
    cadence.SetPeriod(16666667ns);
    cadence.ObserveScanout(Clock::time_point(1s));
    const auto first = cadence.Schedule(Clock::time_point(1003ms));
    CHECK(first.native == Clock::time_point(1s + cadence.Period()));
    CHECK(first.prediction - first.native == cadence.Period());
    // The next real present stays two refreshes apart despite CPU jitter.
    const auto second = cadence.Schedule(Clock::time_point(1037ms));
    CHECK(second.native - first.native == 2 * cadence.Period());
    const auto afterStall = cadence.Schedule(Clock::time_point(1150ms));
    CHECK(afterStall.native >= Clock::time_point(1155ms));
    CHECK(afterStall.native <= Clock::time_point(1155ms) + cadence.Period());

    struct Work {
        std::promise<void>* started;
        std::shared_future<void> release;
        int value;
    };
    Presenter<Work, int> presenter([](const Work& work) {
        work.started->set_value();
        work.release.wait();
        return work.value;
    });
    for (int i = 0; i < 3; ++i) {
        std::promise<void> started, release;
        CHECK(presenter.Submit({&started, release.get_future().share(), i}));
        CHECK(started.get_future().wait_for(1s) == std::future_status::ready);
        // The producer can continue while output is blocked, and may never
        // overwrite the outstanding image or its result.
        CHECK(!presenter.Submit({nullptr, {}, -1}));
        release.set_value();
        CHECK(presenter.Take() == i);
    }
}
