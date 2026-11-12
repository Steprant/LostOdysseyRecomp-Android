#pragma once
// Steprant 2.0.9: where the GPU command thread's frame goes when it is not
// waiting for the game.
//
// In the two-camera scenes of 12/11 17:03 a frame cost the command thread
// ~26 ms of "busy" work plus a 6-12 ms flush, enough to miss the second
// vblank, so the game's own pacing waited for the third (50 ms, 20 FPS).
// Yet the thread used only ~32% of a core over that stretch (threads.csv):
// about half of its busy time it was not running. This splits each frame of
// the command thread (and of the guest main thread) into time on a CPU and
// time runnable but waiting for one (schedstat), so the next session says
// whether it is preemption (run-queue wait), blocking, or plain work.
//
// Weight: two small /proc reads per swap, only while the Tester records.
#include <algorithm>
#include <cstdint>

namespace diagnostics::sched_window {

struct Sample { int64_t runNs = 0, waitNs = 0; bool valid = false; };

// One thread: deltas between consecutive samples.
class ThreadClock
{
public:
    // Returns false for the first sample or an invalid one.
    bool Next(const Sample& s, double& runMs, double& waitMs)
    {
        runMs = waitMs = 0;
        if (!s.valid) { has_ = false; return false; }
        const bool ok = has_ && s.runNs >= last_.runNs && s.waitNs >= last_.waitNs;
        if (ok)
        {
            runMs = double(s.runNs - last_.runNs) / 1e6;
            waitMs = double(s.waitNs - last_.waitNs) / 1e6;
        }
        last_ = s;
        has_ = true;
        return ok;
    }

private:
    Sample last_{};
    bool has_ = false;
};

// Per-frame totals over a window, plus the frames slower than `slowMs`.
struct Window
{
    uint32_t frames = 0, slowFrames = 0;
    double cpRunMs = 0, cpWaitMs = 0, mainRunMs = 0, mainWaitMs = 0;
    double slowCpRunMs = 0, slowCpWaitMs = 0, slowMainRunMs = 0, slowMainWaitMs = 0, slowWallMs = 0;
    double maxCpWaitMs = 0;

    void Add(double wallMs, double cpRun, double cpWait, double mainRun, double mainWait, double slowMs = 40.0)
    {
        ++frames;
        cpRunMs += cpRun; cpWaitMs += cpWait; mainRunMs += mainRun; mainWaitMs += mainWait;
        maxCpWaitMs = std::max(maxCpWaitMs, cpWait);
        if (wallMs > slowMs)
        {
            ++slowFrames;
            slowWallMs += wallMs;
            slowCpRunMs += cpRun; slowCpWaitMs += cpWait; slowMainRunMs += mainRun; slowMainWaitMs += mainWait;
        }
    }
    static double Per(double total, uint32_t n) { return n ? total / n : 0.0; }
    void Reset() { *this = {}; }
};

}
