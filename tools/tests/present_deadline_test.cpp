#include <gpu/frame_pacer.h>
#include <gpu/present_deadline.h>
#include <cassert>
#include <chrono>

using namespace std::chrono_literals;
using gpu::PresentDeadline;
using Point = PresentDeadline::Point;

struct Timeline
{
    PresentDeadline::Clock::time_point now{1s};
    PresentDeadline::Clock::duration wakeDelay{};
    int waits = 0;

    void Release(PresentDeadline& gate, Point point)
    {
        gate.ReleaseWith(point, [&] { return now; }, [&](auto deadline) {
            assert(deadline >= now);
            ++waits;
            now = deadline + wakeDelay;
        });
    }
};

int main()
{
    gpu::DeadlineWait timer;
    const auto period = std::chrono::nanoseconds(1000000000 / 30);
    {
        Timeline timeline;
        const auto deadline = timeline.now + period;
        PresentDeadline gate(timer, deadline, true);

        // Acquire and submit use the original pacing window. QueuePresent can
        // run when the same deadline expires, after only the residual sleep.
        timeline.now += 21ms;
        timeline.Release(gate, Point::AfterSubmit);
        assert(gate.Released() && gate.SubmittedBeforePacing());
        assert(timeline.now == deadline && timeline.waits == 1);
        assert(gate.Timing().requested == period - 21ms);
        assert(gate.Timing().actual == gate.Timing().requested);
        assert(gate.LateRelease() == PresentDeadline::Clock::time_point{});

        // The command processor's fallback must be idempotent after present.
        timeline.Release(gate, Point::BeforeSubmit);
        assert(timeline.now == deadline && timeline.waits == 1);
    }
    {
        Timeline timeline;
        gpu::FramePacer pacer;
        const auto deadline = pacer.Schedule(timeline.now, 30);
        PresentDeadline gate(timer, deadline, true);
        timeline.now = deadline + 12ms;
        timeline.Release(gate, Point::AfterSubmit);
        assert(!timeline.waits && gate.Timing().actual == 0ns);
        assert(gate.LateRelease() == timeline.now);
        pacer.Reanchor(gate.LateRelease());
        assert(pacer.Schedule(timeline.now + 5ms, 30) == timeline.now + period);
    }
    {
        Timeline timeline;
        const auto deadline = timeline.now + 10ms;
        PresentDeadline gate(timer, deadline, true);
        timeline.wakeDelay = 250us;
        timeline.Release(gate, Point::AfterSubmit);
        assert(gate.Timing().overshoot == 250us);
        assert(gate.LateRelease() == PresentDeadline::Clock::time_point{});
    }
    {
        Timeline timeline;
        const auto deadline = timeline.now + 10ms;
        PresentDeadline gate(timer, deadline, false);
        timeline.wakeDelay = 2ms;
        timeline.Release(gate, Point::AfterSubmit);
        assert(gate.LateRelease() == deadline + 2ms);
        assert(gate.Timing().actual == 0ns);
    }
    {
        Timeline timeline;
        const auto deadline = timeline.now + 10ms;
        PresentDeadline gate(timer, deadline, true);
        timeline.Release(gate, Point::BeforeSubmit);
        timeline.Release(gate, Point::AfterSubmit);
        assert(!gate.SubmittedBeforePacing() && timeline.waits == 1);
        assert(timeline.now == deadline && gate.Timing().actual == 10ms);
    }
}
