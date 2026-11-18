#include "gpu/android_frame_generation_budget.h"

#include <cstdio>
#include <cstdlib>

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
    for (int i = 0; i < 90; ++i)
        CHECK(run.Native(58ms, 40ms) == Pressure::None && !run.budget.Capturing());
    run.Stable(23);
    CHECK(!run.budget.Capturing());
    run.Stable(1);
    CHECK(run.budget.Capturing());
    run.Stable(1);
    CHECK(run.budget.CanGenerate(run.next + 10ms));
    CHECK(!run.budget.CanGenerate(run.next + 13ms));

    // The long native intervals observed with the generator enabled suspend
    // input capture before another prediction can be queued.
    CHECK(run.Native(65ms, 38ms) == Pressure::NativeCadence);
    CHECK(!run.budget.Capturing() && !run.budget.CanGenerate(run.next + 4ms));
    run.Stable(60);
    CHECK(!run.budget.Capturing());
    run.Stable(28);
    CHECK(run.budget.Capturing());

    // A blocked extra present suspends the feature even when it is accepted.
    CHECK(run.budget.ExtraFinished(run.next + 30ms, ExtraFrame::Presented) == Pressure::Present);
    CHECK(!run.budget.Capturing());
    run.Stable(120);
    CHECK(!run.budget.Capturing());
    run.Stable(28);
    CHECK(run.budget.Capturing());

    // Two history frames can prime a new scene. Empty captures then stop.
    for (int i = 0; i < 3; ++i) {
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
}
