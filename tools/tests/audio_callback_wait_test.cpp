// Steprant 2.0.2 D12: the game's audio callback waits for its own audio
// thread without a timeout. The wait is made in pieces so the device can be
// given a clean fade to silence; the guest gets exactly what the infinite
// wait returns (apu/audio_callback_wait.h).
#include <apu/audio_callback_wait.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace apu::callback_wait;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }

// A wait that is signalled `signalAfterMs` into the wait with `result`, a
// device queue that drains one frame every 16/3 ms from `startFrames`.
struct Scene
{
    uint32_t signalAfterMs = 0;
    uint32_t result = 0;
    int startFrames = 8;
    bool deviceOpen = true;
    uint32_t elapsed = 0;
    std::vector<uint32_t> waits;
    unsigned queueReads = 0, releases = 0;
    uint32_t releasedAtMs = 0;
    Outcome Run()
    {
        return apu::callback_wait::Run(
            [&](uint32_t ms) -> uint32_t {
                waits.push_back(ms);
                if (ms == kInfinite || elapsed + ms >= signalAfterMs) { elapsed = signalAfterMs > elapsed ? signalAfterMs : elapsed; return result; }
                elapsed += ms;
                return kTimeoutStatus;
            },
            [&] { ++queueReads; return deviceOpen ? std::max(0, startFrames - int(elapsed * 3 / 16)) : -1; },
            [&] { ++releases; releasedAtMs = elapsed; });
    }
};

int main()
{
    try {
        Check(Enabled(nullptr) && Enabled("") && Enabled("1") && !Enabled("0") && Enabled("00"), "LO_AUDIO_CALLBACK_GUARD=0 only turns it off");
        // The ordinary wait: answered within the grace period.
        for (uint32_t after : {0u, 1u, 5u, 19u, 20u}) {
            for (uint32_t result : {0u, 1u, 3u}) {
                Scene s; s.signalAfterMs = after; s.result = result;
                const auto o = s.Run();
                Check(o.status == result && !o.overGrace && !o.released && o.slices == 0, "a short wait returns its result");
                Check(s.waits.size() == 1 && s.waits[0] == kGraceMs, "a short wait is one timed wait");
                Check(s.queueReads == 0 && s.releases == 0, "a short wait never looks at the device");
            }
        }
        // Longer than the grace period, over before the queue is nearly
        // empty: nothing is queued, the sound is what 2.0 plays.
        {
            Scene s; s.signalAfterMs = 30; s.result = 1;
            const auto o = s.Run();
            Check(o.status == 1 && o.overGrace && !o.released && s.releases == 0, "30 ms: watched, not released");
            Check(o.slices == 5 && s.waits.size() == 6, "30 ms: five watch pieces after the grace period");
            for (size_t i = 1; i < s.waits.size(); ++i) Check(s.waits[i] == kSliceMs, "watch pieces are short");
        }
        // The 10/11 stalls: 50-322 ms with a 43 ms reserve.
        for (uint32_t after : {50u, 120u, 322u, 5000u}) {
            Scene s; s.signalAfterMs = after; s.result = 0;
            const auto o = s.Run();
            Check(o.status == 0 && o.overGrace && o.released && s.releases == 1, "a long wait releases the device once");
            Check(s.releasedAtMs >= kGraceMs && s.releasedAtMs < 43, "released before the reserve is gone");
            Check(std::max(0, s.startFrames - int(s.releasedAtMs * 3 / 16)) <= kReleaseAtFrames, "released only when nearly empty");
            Check(s.waits.back() == kInfinite, "the rest is one infinite wait");
            unsigned infinite = 0;
            for (auto w : s.waits) infinite += w == kInfinite;
            Check(infinite == 1, "exactly one infinite wait");
            Check(s.elapsed == after, "the guest resumes when its thread answers, not earlier");
        }
        // A larger reserve (16 frames, 85 ms) is watched for longer.
        {
            Scene s; s.signalAfterMs = 300; s.startFrames = 16;
            const auto o = s.Run();
            Check(o.released && s.releasedAtMs > 60 && s.releasedAtMs < 85, "16-frame reserve: released near its end");
        }
        // A queue that is already nearly empty when the grace period ends.
        {
            Scene s; s.signalAfterMs = 100; s.startFrames = 2;
            const auto o = s.Run();
            Check(o.released && s.releasedAtMs == kGraceMs && o.slices == 0 && s.waits.size() == 2, "empty queue: released at once");
        }
        // Paused or closed device: nothing to protect, wait as the game asked.
        {
            Scene s; s.signalAfterMs = 400; s.deviceOpen = false; s.result = 2;
            const auto o = s.Run();
            Check(o.status == 2 && o.overGrace && !o.released && s.releases == 0, "paused device: no release");
            Check(s.waits.size() == 2 && s.waits[1] == kInfinite && s.queueReads == 1, "paused device: infinite wait after one look");
        }
        // A queue that never drains (the device stopped taking audio): the
        // watch ends after kWatchMs.
        {
            struct Stuck { std::vector<uint32_t> waits; } stuck;
            uint32_t elapsed = 0;
            const auto o = Run([&](uint32_t ms) -> uint32_t { stuck.waits.push_back(ms); if (ms == kInfinite) return 0u; elapsed += ms; return kTimeoutStatus; },
                [] { return 8; }, [] { throw std::runtime_error("released with a full queue"); });
            Check(o.status == 0 && !o.released && o.slices == kWatchMs / kSliceMs, "full queue: the watch is bounded");
            Check(stuck.waits.back() == kInfinite && elapsed == kGraceMs + kWatchMs, "full queue: then the infinite wait");
        }
        // The guest never sees a timeout it did not ask for.
        for (uint32_t after = 0; after < 700; after += 7) {
            for (int frames : {0, 1, 2, 3, 8, 12, 16}) {
                Scene s; s.signalAfterMs = after; s.startFrames = frames; s.result = after & 1;
                const auto o = s.Run();
                Check(o.status == (after & 1), "the result of the infinite wait");
                Check(s.elapsed >= after, "never returns before the signal");
                Check(s.releases <= 1, "at most one release");
            }
        }
        // Error statuses pass through unchanged.
        {
            Scene s; s.signalAfterMs = 0; s.result = 0xC0000008;
            Check(s.Run().status == 0xC0000008, "invalid handle passes through");
        }

        // The fade to silence continues from the last sample and ends at zero.
        {
            const float last[2] = {0.8f, -0.5f};
            float tail[256 * 2];
            FillTail(tail, 2, 256, last);
            Check(std::fabs(tail[0] - 0.8f) < 0.001f && std::fabs(tail[1] + 0.5f) < 0.001f, "the tail starts at the last sample");
            Check(tail[255 * 2] == 0.0f && tail[255 * 2 + 1] == 0.0f, "the tail ends at zero");
            float step = 0;
            for (int i = 1; i < 256; ++i) {
                Check(tail[i * 2] <= tail[(i - 1) * 2] && tail[i * 2 + 1] >= tail[(i - 1) * 2 + 1], "the tail only decays");
                step = std::max(step, std::fabs(tail[i * 2] - tail[(i - 1) * 2]));
            }
            Check(step < 0.8f * 0.0065f, "no step larger than the cosine's slope (an abrupt cut is 0.8)");
            const float zero[6] = {};
            float silent[256 * 6];
            FillTail(silent, 6, 256, zero);
            for (float v : silent) Check(v == 0.0f, "a silent tail is silence");
        }
        // The fade in rises from zero and leaves the last sample untouched.
        {
            float frame[256 * 6];
            for (auto& v : frame) v = 0.5f;
            FadeIn(frame, 6, 256);
            Check(frame[0] < 0.0001f && frame[5] < 0.0001f, "the fade in starts near zero");
            Check(frame[255 * 6] == 0.5f && frame[255 * 6 + 5] == 0.5f, "the last sample is untouched");
            for (int i = 1; i < 256; ++i) Check(frame[i * 6] >= frame[(i - 1) * 6], "the fade in only rises");
            Check(std::fabs(frame[127 * 6] - 0.25f) < 0.01f, "half way at the middle");
        }
        std::printf("audio callback wait: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "audio callback wait test failed: %s\n", e.what());
        return 1;
    }
}
