#pragma once
// Steprant 2.0.2: priority of the game's own audio worker thread (guest entry
// 0x82CC4D98), from what the thread really does.
//
// The guest audio callback hands this thread work and waits for it without a
// timeout, so a worker that runs late stalls the audio queue. 2.0.1 put it at
// Android's audio priority (nice -16) from its first instruction. That is
// right only for a thread that sleeps almost all the time; one that computes
// for long stretches at -16 takes the big cores from the game and render
// threads. The thread now starts at an intermediate priority (nice -8) and is
// moved by its measured CPU share:
//   - two windows in a row at or under kIdlePermille: audio priority;
//   - one window over kBusyPermille: back to the intermediate priority.
// In between it keeps what it has. LO_AUDIO_WORKER_PRIORITY=0 leaves the
// thread alone (2.0), =audio keeps it at the audio priority always (2.0.1).
//
// Steprant 2.1.1: the CPU share alone misreads a hot phone. With the cores
// capped by the thermal limit the same work takes a larger share of a core,
// so the worker crossed kBusyPermille and lost the audio priority exactly
// while the audio callback waited for it (13/11 session: 29.7% ->
// intermediate, then thousands of empty device queues). The driver now
// reports a callback that waited kCallbackWaitMs or more (NoteCallbackWait):
// the window that saw one ends at the audio priority, and the next
// kHoldWindows windows never lower it. With no waits the policy is the 2.0.2
// one.
#include <cstdint>

namespace apu::worker_priority {

enum class Level : uint8_t { Intermediate, Audio };
enum class Mode : uint8_t { Off, Adaptive, AlwaysAudio };

inline constexpr int kNiceIntermediate = -8;
inline constexpr int kNiceAudio = -16; // ANDROID_PRIORITY_AUDIO
inline constexpr uint64_t kWindowNs = 2'000'000'000ull;
inline constexpr uint32_t kIdlePermille = 100;  // 10% of a core
inline constexpr uint32_t kBusyPermille = 250;  // 25% of a core
inline constexpr uint32_t kIdleWindowsToRaise = 2;
inline constexpr uint32_t kCallbackWaitMs = 30;  // as the stall sampler's audio threshold
inline constexpr uint32_t kHoldWindows = 5;      // 10 s at the audio priority after a wait

constexpr int Nice(Level level) { return level == Level::Audio ? kNiceAudio : kNiceIntermediate; }
constexpr const char* Name(Level level) { return level == Level::Audio ? "audio" : "intermediate"; }

inline Mode ParseMode(const char* value)
{
    if (!value || !*value) return Mode::Adaptive;
    if (value[0] == '0' && !value[1]) return Mode::Off;
    if (value[0] == 'a' || (value[0] == '2' && !value[1])) return Mode::AlwaysAudio;
    return Mode::Adaptive;
}

struct Decision
{
    bool windowClosed = false;
    bool changed = false;
    Level level = Level::Intermediate;
    uint32_t busyPermille = 0;
    // The audio callback waited for the worker in this window (2.1.1).
    bool callbackWaited = false;
    // A busy window that did not lower the priority because of a recent wait.
    bool held = false;
};

class Policy
{
    Level level_ = Level::Intermediate;
    bool started_ = false;
    uint64_t wallStart_ = 0, cpuStart_ = 0;
    uint32_t idleWindows_ = 0;
    uint32_t holdWindows_ = 0;
    bool waitedInWindow_ = false;
public:
    Level Current() const { return level_; }
    void Restart() { *this = Policy(); }
    // The guest audio callback waited kCallbackWaitMs or more (driver thread).
    void NoteCallbackWait() { waitedInWindow_ = true; }
    // Thread CPU time and wall time, both monotonic nanoseconds.
    Decision Sample(uint64_t wallNs, uint64_t cpuNs)
    {
        Decision d;
        d.level = level_;
        if (!started_ || wallNs < wallStart_ || cpuNs < cpuStart_) {
            started_ = true; wallStart_ = wallNs; cpuStart_ = cpuNs;
            return d;
        }
        const uint64_t wall = wallNs - wallStart_;
        if (wall < kWindowNs) return d;
        const uint64_t cpu = cpuNs - cpuStart_;
        d.windowClosed = true;
        d.busyPermille = uint32_t(cpu >= wall ? 1000 : cpu * 1000 / wall);
        wallStart_ = wallNs; cpuStart_ = cpuNs;
        if (waitedInWindow_) {
            // The callback waited for this thread: audio first, for a while.
            waitedInWindow_ = false;
            d.callbackWaited = true;
            holdWindows_ = kHoldWindows;
            if (d.busyPermille <= kIdlePermille) { if (idleWindows_ < kIdleWindowsToRaise) ++idleWindows_; }
            else idleWindows_ = 0;
            if (level_ != Level::Audio) { level_ = Level::Audio; d.changed = true; }
            d.level = level_;
            return d;
        }
        // Each window without a wait uses up one window of the hold.
        const bool holding = holdWindows_ != 0;
        if (holding) --holdWindows_;
        if (d.busyPermille > kBusyPermille) {
            idleWindows_ = 0;
            if (holding) d.held = level_ == Level::Audio;
            else if (level_ != Level::Intermediate) { level_ = Level::Intermediate; d.changed = true; }
        } else if (d.busyPermille <= kIdlePermille) {
            if (idleWindows_ < kIdleWindowsToRaise) ++idleWindows_;
            if (idleWindows_ >= kIdleWindowsToRaise && level_ != Level::Audio) { level_ = Level::Audio; d.changed = true; }
        } else {
            idleWindows_ = 0;
        }
        d.level = level_;
        return d;
    }
};

}
