#pragma once
#include <cstdint>
namespace frame_timing
{
bool Enabled();
// Enabled(), or an Android Tester recording: the cheap per-frame breakdown
// behind the "slow frame" log lines runs whenever a session is recorded.
bool Active();
// Wall time a thread spent waiting on a GPU fence (video::WaitForGpuFence),
// charged to the frame being presented.
void AddGpuFenceWait(double milliseconds);
// Wall time the command processor spent in PM4 WAIT_REG_MEM (the guest's
// command stream waiting on a register or memory value, e.g. its vblank or
// interrupt-handler handshakes).
void AddWaitRegMem(double milliseconds);
struct PacingSample
{
    double requestedMs = 0, actualMs = 0, overshootMs = 0;
    // Previous present end -> current final flush start. Includes post-present
    // diagnostics, PM4 production/execution and waits; this is NOT CPU time.
    double betweenMs = 0;
    // Diagnostics after the previous swap, within the following betweenMs
    // when swaps/presents are one-to-one. Never add these to frame_ms.
    double previousSwapLogMs = 0, previousSwapPostMs = 0;
    bool hasPrevious = false;
    // The swapchain accepted this present API call; this does not imply scanout.
    bool presentAccepted = false;
};
void Present(uint32_t swap, uint32_t fps, double flushMs, double waitMs, double presentMs,
    const PacingSample& pacing);
// CP worker only: a no-readable-primary-work episode, including yield/event
// pump/sleep. Subset of betweenMs, not an additional frame-time component.
void CpIdle(double milliseconds);
void GuestInterval(uint32_t requested, uint32_t encoded);
// Call on the actual engine entry before forwarding its original arguments.
void EngineTick(double delta);
// Monotonic actual engine-entry serial for opt-in deterministic test input.
uint64_t InputTick();
}
