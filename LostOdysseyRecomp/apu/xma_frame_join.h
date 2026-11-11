#pragma once
// Steprant 2.0.2 D13: an XMA frame the decoder rejects produces no samples
// (about 48 of 1.28 million in the 10/11 session). The voice then jumps from
// the last sample of the frame before to the first sample of the next frame
// that decodes: a step in the waveform, heard as a click. After eight
// failures in a row the context emits silent frames, which start with the
// same step.
//
// FrameJoin removes the step and nothing else. It remembers the last sample
// written per channel; after a lost frame the first Samples samples of the
// next decoded frame start from that value and reach the decoded signal with
// a raised cosine, and a silent frame starts from it and reaches zero the
// same way. No frame is added or removed, so the timing of the voice is the
// one 2.0 has, and a context that never loses a frame is untouched.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace apu::xma
{
struct FrameJoin
{
    static constexpr uint32_t Samples = 64;
    float last[2]{};      // last sample written per channel
    bool pending = false; // a frame was lost since `last` was written

    static float Weight(uint32_t i)
    {
        return i >= Samples ? 1.0f : 0.5f * (1.0f - std::cos(3.14159265358979f * float(i + 1) / float(Samples + 1)));
    }
    void Clear() { last[0] = last[1] = 0; pending = false; }
    void Lost() { pending = true; }
    // Sample `i` (counted from the first sample the guest will hear) of a
    // decoded frame on channel `c`.
    float Decoded(uint32_t c, uint32_t i, float value) const
    {
        if (!pending || i >= Samples) return value;
        return last[c] + (value - last[c]) * Weight(i);
    }
    // Sample `i` of a silent frame on channel `c`.
    float Silent(uint32_t c, uint32_t i) const
    {
        return i >= Samples ? 0.0f : last[c] * (1.0f - Weight(i));
    }
    // After a decoded frame: its last samples, and nothing is pending.
    void Wrote(float left, float right) { last[0] = left; last[1] = right; pending = false; }
    // After a silent frame: the next decoded frame rises from zero.
    void WroteSilence() { last[0] = last[1] = 0; pending = true; }
};

// LO_XMA_FRAME_JOIN=0 keeps the 2.0 output.
inline bool FrameJoinEnabled(const char* value) { return !(value && value[0] == '0' && value[1] == '\0'); }

// Totals for one log line with the audio report; taken, not reset.
struct DecodeSummary
{
    uint64_t frames = 0, failed = 0, joined = 0, silenced = 0, resyncs = 0;
};
}
