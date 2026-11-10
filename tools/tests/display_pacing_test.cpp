// Steprant 2.0.1: display-timed pacing (gpu/display_pacing.h) against a model
// of a FIFO compositor: an image is shown at the first refresh at or after
// both its ready time (plus one refresh of compositor latency) and the time it
// asked for. Host frames arrive every 33.3 ms with +-1.2 ms of jitter and a
// phase that sits on a refresh boundary, the case that stutters without it.
#include <gpu/display_pacing.h>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <random>

static void Require(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

struct Result { uint32_t frames = 0, exact = 0, other = 0; };

// Simulates `frames` presents; returns how many intervals between shown frames
// equal `k` refreshes.
static Result Simulate(bool paced, uint64_t refresh, uint32_t fps, uint32_t frames, uint64_t phase, double jitterMs,
    uint32_t slowEvery = 0, gpu::display_pacing::Stats* statsOut = nullptr)
{
    using namespace gpu::display_pacing;
    Scheduler scheduler;
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> jitter(-jitterMs, jitterMs);
    const uint64_t period = 1000000000ull / fps;
    struct Pending { uint32_t id; uint64_t desired, shown; };
    std::deque<Pending> feedback;
    uint64_t lastShown = 0;
    std::vector<uint64_t> shownTimes;
    const uint64_t start = 1000000000ull + phase;
    for (uint32_t i = 1; i <= frames; ++i) {
        uint64_t ready = start + uint64_t(i) * period + int64_t(jitter(rng) * 1e6);
        if (slowEvery && i % slowEvery == 0) ready += 20000000ull; // one slow frame
        // Feedback for frames already shown by now (a refresh later than shown).
        while (!feedback.empty() && feedback.front().shown + refresh <= ready) {
            scheduler.Observe({feedback.front().id, feedback.front().desired, feedback.front().shown});
            feedback.pop_front();
        }
        const uint64_t desired = scheduler.Next(i, ready, refresh, period, paced);
        const uint64_t earliest = std::max(ready + refresh, desired);
        uint64_t shown = (earliest + refresh - 1) / refresh * refresh; // refresh grid at 0
        if (shown <= lastShown) shown = lastShown + refresh;            // FIFO order
        lastShown = shown;
        feedback.push_back({i, desired, shown});
        shownTimes.push_back(shown);
    }
    const uint32_t k = uint32_t((period + refresh / 2) / refresh);
    Result r;
    for (size_t i = 1; i < shownTimes.size(); ++i) {
        ++r.frames;
        const uint64_t delta = shownTimes[i] - shownTimes[i - 1];
        if (delta == uint64_t(k) * refresh) ++r.exact; else ++r.other;
    }
    if (statsOut) *statsOut = scheduler.TakeStats();
    return r;
}

int main()
{
    using namespace gpu::display_pacing;
    // Cadence detection.
    Require(RefreshesPerFrame(8333333, 33333333) == 4, "30 FPS at 120 Hz is 4 refreshes");
    Require(RefreshesPerFrame(16666667, 33333333) == 2, "30 FPS at 60 Hz is 2 refreshes");
    Require(RefreshesPerFrame(11111111, 33333333) == 3, "30 FPS at 90 Hz is 3 refreshes");
    Require(RefreshesPerFrame(8333333, 16666667) == 2, "60 FPS at 120 Hz is 2 refreshes");
    Require(RefreshesPerFrame(6944444, 33333333) == 0, "30 FPS at 144 Hz has no whole cadence");
    Require(RefreshesPerFrame(0, 33333333) == 0, "unknown refresh disables pacing");
    Require(RefreshesPerFrame(8333333, 0) == 0, "unknown frame rate disables pacing");

    const uint64_t refresh120 = 8333333;
    // Phase on a refresh boundary (ready + latency lands on a refresh): the
    // unpaced FIFO alternates 3 and 5 refreshes, the paced one holds 4.
    const uint64_t boundary = refresh120 - 333333;
    const Result unpaced = Simulate(false, refresh120, 30, 3000, boundary, 1.2);
    const Result paced = Simulate(true, refresh120, 30, 3000, boundary, 1.2);
    std::printf("120 Hz boundary phase: unpaced exact=%u/%u paced exact=%u/%u\n",
        unpaced.exact, unpaced.frames, paced.exact, paced.frames);
    Require(unpaced.other > unpaced.frames / 10, "model reproduces the uneven cadence without pacing");
    Require(paced.other * 100 <= paced.frames, "paced frames hold 4 refreshes (>= 99%)");

    // 60 Hz television.
    const Result tv = Simulate(true, 16666667, 30, 3000, 16000000, 1.2);
    Require(tv.other * 100 <= tv.frames, "60 Hz: 2 refreshes per frame (>= 99%)");

    // A slow frame every 100: it is shown late once, the cadence restarts from
    // it without a burst, and the rest stay even.
    Stats stats;
    const Result slow = Simulate(true, refresh120, 30, 3000, boundary, 1.2, 100, &stats);
    std::printf("slow frame every 100: exact=%u/%u restarts=%u late=%u\n", slow.exact, slow.frames, stats.restarts, stats.late);
    Require(slow.other <= slow.frames * 6 / 100, "only the slow frames and their neighbours differ");

    // Disabled pacing always presents immediately.
    Scheduler off;
    for (uint32_t i = 1; i < 10; ++i) Require(off.Next(i, 1000000000ull * i, refresh120, 33333333, false) == 0, "off is immediate");
    Require(off.TakeStats().immediate == 9, "immediate presents counted");

    // A frame far behind its schedule restarts the cadence instead of asking
    // for a refresh in the past.
    Scheduler late;
    late.Observe({1, 0, 1000000000ull});
    late.Next(1, 999000000ull, refresh120, 33333333, true); // establishes cadence
    late.Observe({1, 0, 1000000000ull});
    const uint64_t target = late.Next(2, 1000000000ull + 10000000ull, refresh120, 33333333, true);
    Require(target == 1000000000ull + 4 * refresh120 - refresh120 / 4, "on-time frame asks for 4 refreshes later");
    Require(late.Next(3, 1000000000ull + 200000000ull, refresh120, 33333333, true) == 0, "late frame presents immediately");
    std::printf("display pacing: ok\n");
    return 0;
}
