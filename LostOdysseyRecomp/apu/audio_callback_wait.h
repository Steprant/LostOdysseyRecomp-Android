#pragma once
// Steprant 2.0.2 D12: the game's audio callback (guest 0x82CC4F10) hands work
// to the game's own audio thread and waits for it with
// KeWaitForMultipleObjects and no timeout. While it waits the host audio
// driver thread, which runs that callback, queues nothing: after the reserve
// (43 ms on Android) the device runs dry in the middle of a waveform and the
// sound comes back just as abruptly. The 10/11 logs have 14 such waits of
// 50-322 ms.
//
// The wait itself is not shortened: the game asked for an infinite one and
// goes on to use what its thread produced, so returning early would hand it
// unfinished data. Run() makes the same wait in pieces and returns only what
// the infinite wait would have returned:
// - the first piece is kGraceMs long. A wait that ends within it, which is
//   every ordinary one, is one timed wait instead of one infinite wait;
// - after that the device queue is watched every kSliceMs. When at most
//   kReleaseAtFrames frames are left and the wait is still pending, `release`
//   runs once: the driver ends the queued sound with a short fade to silence
//   (FillTail) and fades the next frame in (FadeIn). The device plays silence
//   for the rest of the wait and the game resumes when its thread answers;
// - a wait that ends before the queue is nearly empty changes nothing at all;
// - then, or after kWatchMs, or when the device takes no audio, one infinite
//   wait.
#include <cmath>
#include <cstdint>

namespace apu::callback_wait
{
    inline constexpr uint32_t kTimeoutStatus = 0x102; // STATUS_TIMEOUT
    inline constexpr uint32_t kInfinite = 0xFFFFFFFF;
    inline constexpr uint32_t kGraceMs = 20;
    inline constexpr uint32_t kSliceMs = 2;
    inline constexpr uint32_t kWatchMs = 500;
    inline constexpr int kReleaseAtFrames = 2;

    struct Outcome
    {
        uint32_t status = 0;    // what the guest gets: never a timeout
        bool overGrace = false; // the wait lasted longer than kGraceMs
        bool released = false;  // the device was given the fade to silence
        uint32_t slices = 0;    // watch pieces waited
    };

    // wait(ms) -> NTSTATUS of one wait on the guest's objects (kInfinite: no
    // timeout). queuedFrames() -> whole frames queued to the device, or a
    // negative number while it takes no audio (paused, closed).
    template <typename Wait, typename QueuedFrames, typename Release>
    Outcome Run(Wait&& wait, QueuedFrames&& queuedFrames, Release&& release)
    {
        Outcome outcome;
        outcome.status = wait(kGraceMs);
        if (outcome.status != kTimeoutStatus) return outcome;
        outcome.overGrace = true;
        for (uint32_t watched = 0; watched < kWatchMs; watched += kSliceMs)
        {
            const int queued = queuedFrames();
            if (queued < 0) break;
            if (queued <= kReleaseAtFrames)
            {
                release();
                outcome.released = true;
                break;
            }
            outcome.status = wait(kSliceMs);
            ++outcome.slices;
            if (outcome.status != kTimeoutStatus) return outcome;
        }
        outcome.status = wait(kInfinite);
        return outcome;
    }

    // One frame that continues from the last queued sample of each channel
    // and reaches zero with a raised cosine: the queue ends without a step.
    inline void FillTail(float* interleaved, uint32_t channels, uint32_t samples, const float* last)
    {
        for (uint32_t i = 0; i < samples; ++i)
        {
            const float w = i + 1 == samples ? 0.0f
                : 0.5f * (1.0f + std::cos(3.14159265358979f * float(i + 1) / float(samples)));
            for (uint32_t c = 0; c < channels; ++c)
                interleaved[i * channels + c] = last[c] * w;
        }
    }

    // The first frame after the silence rises from zero the same way; its
    // last sample is untouched.
    inline void FadeIn(float* interleaved, uint32_t channels, uint32_t samples)
    {
        for (uint32_t i = 0; i + 1 < samples; ++i)
        {
            const float w = 0.5f * (1.0f - std::cos(3.14159265358979f * float(i + 1) / float(samples)));
            for (uint32_t c = 0; c < channels; ++c)
                interleaved[i * channels + c] *= w;
        }
    }

    // LO_AUDIO_CALLBACK_GUARD=0 keeps the plain infinite wait of 2.0.
    inline bool Enabled(const char* value) { return !(value && value[0] == '0' && value[1] == '\0'); }
}
