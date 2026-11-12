// Steprant 2.0.9: per-frame scheduling split (diagnostics/sched_window.h).
#include <diagnostics/sched_window.h>
#include <cstdio>
#include <cstdlib>

static int checks = 0;
static void Check(bool ok, const char* what)
{
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}

int main()
{
    using namespace diagnostics::sched_window;
    {
        ThreadClock c;
        double run = -1, wait = -1;
        Check(!c.Next({ 1'000'000, 0, true }, run, wait) && run == 0 && wait == 0, "first sample has no delta");
        Check(c.Next({ 21'000'000, 5'000'000, true }, run, wait) && run == 20.0 && wait == 5.0, "deltas in ms");
        Check(!c.Next({ 0, 0, false }, run, wait), "invalid sample");
        Check(!c.Next({ 30'000'000, 6'000'000, true }, run, wait), "after an invalid sample: restart");
        Check(c.Next({ 31'000'000, 6'500'000, true }, run, wait) && run == 1.0 && wait == 0.5, "then deltas again");
        Check(!c.Next({ 1'000, 0, true }, run, wait), "a counter that goes back is not a delta");
    }
    {
        Window w;
        w.Add(33.3, 18, 2, 10, 1);
        w.Add(50.0, 16, 12, 9, 3);
        Check(w.frames == 2 && w.slowFrames == 1, "slow frames over 40 ms");
        Check(Window::Per(w.cpRunMs, w.frames) == 17.0 && Window::Per(w.cpWaitMs, w.frames) == 7.0, "averages");
        Check(w.slowCpWaitMs == 12 && w.maxCpWaitMs == 12 && Window::Per(w.slowWallMs, w.slowFrames) == 50.0, "slow split");
        Check(Window::Per(1, 0) == 0.0, "no frames");
        w.Reset();
        Check(w.frames == 0 && w.cpRunMs == 0, "reset");
    }
    std::printf("sched window: %d checks passed\n", checks);
    return 0;
}
