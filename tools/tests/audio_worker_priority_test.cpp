// Steprant 2.0.2: the game's audio worker thread gets the audio priority only
// while it really sleeps almost all the time (apu/audio_worker_priority.h).
#include <apu/audio_worker_priority.h>
#include <cstdio>
#include <stdexcept>
using namespace apu::worker_priority;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        Check(ParseMode(nullptr) == Mode::Adaptive && ParseMode("") == Mode::Adaptive && ParseMode("1") == Mode::Adaptive,
            "adaptive by default");
        Check(ParseMode("0") == Mode::Off, "0 leaves the thread alone (2.0)");
        Check(ParseMode("audio") == Mode::AlwaysAudio && ParseMode("2") == Mode::AlwaysAudio, "audio keeps the 2.0.1 behaviour");
        Check(Nice(Level::Audio) == -16 && Nice(Level::Intermediate) == -8, "nice values");

        const uint64_t s = 1'000'000'000ull;
        Policy p;
        Check(p.Current() == Level::Intermediate, "the thread starts at the intermediate priority");
        auto d = p.Sample(10 * s, 0);
        Check(!d.windowClosed && !d.changed, "the first sample only starts a window");
        d = p.Sample(11 * s, s / 100);
        Check(!d.windowClosed, "no decision before a whole window");
        // A worker that waits: 1% of a core.
        d = p.Sample(12 * s, 2 * s / 100);
        Check(d.windowClosed && d.busyPermille == 10 && !d.changed && d.level == Level::Intermediate,
            "one idle window is not enough to raise it");
        d = p.Sample(14 * s, 4 * s / 100);
        Check(d.windowClosed && d.changed && d.level == Level::Audio, "two idle windows in a row raise it to the audio priority");
        d = p.Sample(16 * s, 6 * s / 100);
        Check(d.windowClosed && !d.changed && d.level == Level::Audio, "and it stays there while it keeps waiting");
        // Between the thresholds: keep.
        uint64_t cpu = 6 * s / 100 + 2 * s * 18 / 100;
        d = p.Sample(18 * s, cpu);
        Check(d.busyPermille == 180 && !d.changed && d.level == Level::Audio, "a moderate window changes nothing");
        // It starts computing: 60% of a core.
        cpu += 2 * s * 60 / 100;
        d = p.Sample(20 * s, cpu);
        Check(d.busyPermille == 600 && d.changed && d.level == Level::Intermediate, "one busy window lowers it at once");
        cpu += 2 * s * 5 / 100;
        d = p.Sample(22 * s, cpu);
        Check(!d.changed && d.level == Level::Intermediate, "after a busy window, one idle window does not raise it");
        cpu += 2 * s * 18 / 100;
        d = p.Sample(24 * s, cpu);
        Check(!d.changed, "a moderate window resets the idle count");
        cpu += 2 * s * 5 / 100;
        d = p.Sample(26 * s, cpu);
        Check(!d.changed && d.level == Level::Intermediate, "so the next idle window is the first again");
        cpu += 2 * s * 5 / 100;
        d = p.Sample(28 * s, cpu);
        Check(d.changed && d.level == Level::Audio, "and the second raises it");
        // More CPU than wall (two samples racing): clamp.
        d = p.Sample(30 * s, cpu + 5 * s);
        Check(d.busyPermille == 1000 && d.level == Level::Intermediate, "a CPU share over one core is clamped and counts as busy");
        // Clocks going back (a new thread took the slot): restart the window.
        d = p.Sample(31 * s, 0);
        Check(!d.windowClosed && !d.changed, "a CPU clock that went back starts a new window");
        // A worker that is always busy never gets the audio priority.
        Policy busy;
        uint64_t wall = 0, used = 0;
        bool everAudio = false;
        for (int i = 0; i < 200; ++i) {
            wall += 2 * s; used += 2 * s * 40 / 100;
            everAudio |= busy.Sample(wall, used).level == Level::Audio;
        }
        Check(!everAudio, "a thread that computes 40% of the time never runs at the audio priority");
        busy.Restart();
        Check(busy.Current() == Level::Intermediate, "restart for a new thread");
        std::printf("audio worker priority: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "audio worker priority test failed: %s\n", e.what());
        return 1;
    }
}
