#pragma once

#include "deadline_wait.h"
#include <diagnostics/android_trace.h>
#include <chrono>

namespace gpu
{
// One pacing deadline for the guest swap. It may gate the old path before any
// presentation work, or gate queuePresent after the composition submit.
class PresentDeadline
{
public:
    using Clock = std::chrono::steady_clock;
    enum class Point { BeforeSubmit, AfterSubmit };
    struct Sample {
        Clock::duration requested{}, actual{}, overshoot{};
    };

    PresentDeadline(DeadlineWait& timer, Clock::time_point deadline, bool measure)
        : timer_(timer), deadline_(deadline), measure_(measure) {}
    PresentDeadline(const PresentDeadline&) = delete;
    PresentDeadline& operator=(const PresentDeadline&) = delete;

    void Release(Point point = Point::BeforeSubmit)
    {
        if (released_) return;
        [[maybe_unused]] const ::diagnostics::AndroidTraceScope trace(point == Point::AfterSubmit
            ? "LO/Pace after submit" : "LO/Pace before submit");
        ReleaseWith(point, Clock::now, [this](auto deadline) { timer_.Until(deadline); });
    }

    // The clock and absolute wait are supplied by the deterministic test.
    template <typename Now, typename Until>
    void ReleaseWith(Point point, Now now, Until until)
    {
        if (released_) return;
        released_ = true;
        submittedEarly_ = point == Point::AfterSubmit;
        if (!measure_ && !submittedEarly_) {
            until(deadline_);
            return;
        }

        const auto ready = now();
        auto releasedAt = ready;
        if (ready < deadline_) {
            until(deadline_);
            releasedAt = now();
            if (measure_) {
                sample_.requested = deadline_ - ready;
                sample_.actual = releasedAt - ready;
                if (releasedAt > deadline_) sample_.overshoot = releasedAt - deadline_;
            }
        }
        // Absorb normal timer jitter. After a missed deadline, start a new
        // period so the next successful frame doesn't try to catch up.
        if (submittedEarly_ && releasedAt - deadline_ >= std::chrono::milliseconds(1))
            lateRelease_ = releasedAt;
    }

    bool Released() const { return released_; }
    bool SubmittedBeforePacing() const { return submittedEarly_; }
    Clock::time_point LateRelease() const { return lateRelease_; }
    const Sample& Timing() const { return sample_; }

private:
    DeadlineWait& timer_;
    Clock::time_point deadline_, lateRelease_{};
    Sample sample_;
    bool measure_, released_ = false, submittedEarly_ = false;
};
}
