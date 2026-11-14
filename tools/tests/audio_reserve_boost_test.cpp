// Steprant 2.1.1: the elastic audio reserve (apu/audio_reserve_boost.h).
#include <apu/audio_reserve_boost.h>
#include <cstdio>
#include <stdexcept>
#include <string_view>
using apu::detail::ReserveBoost;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        const uint64_t ms = 1'000'000ull, s = 1000 * ms;
        ReserveBoost b;
        auto r = b.Update(1 * s, 12, 9, 0, false);
        Check(r.frames == 12 && r.changed && r.reason == ReserveBoost::Reason::None,
            "the first call keeps the configured reserve; empty queues before it are history");
        r = b.Update(1 * s + 5 * ms, 12, 9, 10, false);
        Check(r.frames == 12 && !r.changed, "a quiet period keeps it");

        r = b.Update(2 * s, 12, 10, 0, false);
        Check(r.frames == ReserveBoost::kBoostFrames && r.changed && r.reason == ReserveBoost::Reason::EmptyQueue,
            "an empty device queue doubles the reserve");
        r = b.Update(2 * s + 5 * ms, 12, 10, 0, false);
        Check(r.frames == 24 && !r.changed && r.reason == ReserveBoost::Reason::None && b.Boosted(), "and it stays");
        r = b.Update(2 * s + ReserveBoost::kHoldNs - ms, 12, 10, 0, false);
        Check(r.frames == 24, "until the hold after the last risk");
        r = b.Update(2 * s + ReserveBoost::kHoldNs, 12, 10, 0, false);
        Check(r.frames == 23 && r.changed && !b.Boosted(), "then it steps down one frame");
        r = b.Update(2 * s + ReserveBoost::kHoldNs + 50 * ms, 12, 10, 0, false);
        Check(r.frames == 23 && !r.changed, "one frame per step period, not one per call");
        uint64_t t = 2 * s + ReserveBoost::kHoldNs;
        for (uint32_t expected = 22; expected >= 12; --expected) {
            t += ReserveBoost::kStepNs;
            r = b.Update(t, 12, 10, 0, false);
            Check(r.frames == expected && r.changed, "each step period removes one frame");
        }
        r = b.Update(t + ReserveBoost::kStepNs, 12, 10, 0, false);
        Check(r.frames == 12 && !r.changed, "and it stops at the configured reserve");

        r = b.Update(30 * s, 12, 10, ReserveBoost::kStallMs, false);
        Check(r.frames == 24 && r.reason == ReserveBoost::Reason::LongCallback, "a long guest callback boosts it");
        r = b.Update(40 * s, 12, 10, 0, false);
        Check(r.frames == 24, "still inside the hold");
        r = b.Update(44 * s, 12, 10, ReserveBoost::kStallMs - 1, false);
        Check(r.frames == 24 && r.reason == ReserveBoost::Reason::None, "a callback just under the threshold does not renew it");
        r = b.Update(45 * s, 12, 10, 0, false);
        Check(r.frames == 23 && r.changed, "so it starts to end 15 s after the long callback");
        r = b.Update(45 * s + 50 * ms, 12, 11, 0, false);
        Check(r.frames == 24 && r.changed && r.reason == ReserveBoost::Reason::EmptyQueue, "a new risk while stepping down boosts it again");
        r = b.Update(46 * s + ReserveBoost::kHoldNs, 12, 11, 0, false);
        Check(r.frames == 23, "and the hold starts over");
        for (int i = 0; i < 20; ++i) r = b.Update(47 * s + ReserveBoost::kHoldNs + i * ReserveBoost::kStepNs, 12, 11, 0, false);
        Check(r.frames == 12, "back to 12 after the steps");

        r = b.Update(70 * s, 12, 11, 0, true);
        Check(r.frames == 24 && r.reason == ReserveBoost::Reason::FrozenPicture, "a frozen picture (load) boosts it");
        r = b.Update(80 * s, 12, 11, 0, true);
        Check(r.frames == 24, "a long load keeps renewing it");
        r = b.Update(94 * s, 12, 11, 0, false);
        Check(r.frames == 24, "until 15 s after the picture moved again");
        r = b.Update(95 * s, 12, 11, 0, false);
        Check(r.frames == 23, "then it steps down");
        for (int i = 1; i <= 20; ++i) r = b.Update(95 * s + i * ReserveBoost::kStepNs, 12, 11, 0, false);
        Check(r.frames == 12, "to the configured reserve");

        r = b.Update(100 * s, 16, 11, 0, false);
        Check(r.frames == 16 && r.changed, "a reserve set by the player is followed");
        r = b.Update(101 * s, 16, 12, 0, false);
        Check(r.frames == 24, "and boosted to 24");
        r = b.Update(102 * s, 32, 12, 0, false);
        Check(r.frames == 32, "a configured reserve above the boost is never lowered");
        r = b.Update(200 * s, 32, 12, 0, false);
        Check(r.frames == 32 && !r.changed, "nor stepped down below it");

        ReserveBoost restart;
        restart.Update(1 * s, 12, 50, 0, false);
        r = restart.Update(2 * s, 12, 0, 0, false);
        Check(r.frames == 12 && r.reason == ReserveBoost::Reason::None, "a counter that restarted is not a new empty queue");
        r = restart.Update(3 * s, 12, 1, 0, false);
        Check(r.frames == 24 && r.reason == ReserveBoost::Reason::EmptyQueue, "but the next empty queue after it is");
        Check(std::string_view(ReserveBoost::Name(ReserveBoost::Reason::EmptyQueue)) == "empty device queue", "names for the log");
        std::printf("audio reserve boost: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "audio reserve boost test failed: %s\n", e.what());
        return 1;
    }
}
