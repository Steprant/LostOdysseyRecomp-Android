// Steprant 2.0.2 D13: no step in the waveform around an XMA frame the decoder
// rejected (apu/xma_frame_join.h).
#include <apu/xma_frame_join.h>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace apu::xma;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }

// One channel through the join the way ProcessPacket uses it.
static std::vector<float> Frame(FrameJoin& join, const std::vector<float>& decoded, uint32_t skip = 0)
{
    std::vector<float> out(decoded.size());
    const bool joining = join.pending;
    const uint32_t from = skip * 128;
    for (uint32_t i = 0; i < decoded.size(); ++i)
        out[i] = joining && i >= from ? join.Decoded(0, i - from, decoded[i]) : decoded[i];
    if (skip < 4) join.Wrote(out.back(), 0);
    return out;
}
static float MaxStep(float before, const std::vector<float>& frame, uint32_t from = 0)
{
    float step = std::fabs(frame[from] - before);
    for (size_t i = from + 1; i < frame.size(); ++i) step = std::max(step, std::fabs(frame[i] - frame[i - 1]));
    return step;
}

int main()
{
    try {
        Check(FrameJoinEnabled(nullptr) && FrameJoinEnabled("1") && !FrameJoinEnabled("0"), "LO_XMA_FRAME_JOIN=0 only turns it off");
        // A context that never loses a frame is untouched.
        {
            FrameJoin join;
            std::vector<float> a(512), b(512);
            for (int i = 0; i < 512; ++i) { a[i] = std::sin(i * 0.3f) * 0.7f; b[i] = std::cos(i * 0.11f) * 0.9f; }
            Check(Frame(join, a) == a && Frame(join, b) == b && Frame(join, a) == a, "no loss: bit-identical frames");
            Check(join.last[0] == a.back() && !join.pending, "the last sample is remembered");
        }
        // A frame lost between +0.8 and a frame that starts at -0.6: the
        // 2.0 output has a step of 1.4 there.
        {
            FrameJoin join;
            std::vector<float> a(512, 0.8f), b(512, -0.6f);
            Frame(join, a);
            join.Lost();
            const auto out = Frame(join, b);
            Check(std::fabs(out[0] - 0.8f) < 0.002f, "the joined frame starts at the last sample written");
            Check(MaxStep(0.8f, out) < 1.4f * 0.03f, "no step larger than the cosine's slope");
            for (uint32_t i = FrameJoin::Samples; i < 512; ++i) Check(out[i] == b[i], "after the join the decoded samples are untouched");
            for (uint32_t i = 1; i < FrameJoin::Samples; ++i) Check(out[i] <= out[i - 1], "the join is monotonic between two constants");
            Check(!join.pending && join.last[0] == -0.6f, "one frame is joined, then nothing is pending");
            Check(Frame(join, a) == a, "the frame after the join is untouched");
        }
        // The guest skips the first subframes of the frame: the join starts
        // at the first sample it will hear.
        {
            FrameJoin join;
            std::vector<float> a(512, 0.5f), b(512, -0.5f);
            Frame(join, a);
            join.Lost();
            const auto out = Frame(join, b, 2);
            for (uint32_t i = 0; i < 256; ++i) Check(out[i] == b[i], "skipped samples are untouched");
            Check(std::fabs(out[256] - 0.5f) < 0.002f && MaxStep(0.5f, out, 256) < 0.03f, "the join starts at the first heard sample");
        }
        // A frame the guest skips whole neither joins nor forgets the loss.
        {
            FrameJoin join;
            std::vector<float> a(512, 0.5f), b(512, -0.5f);
            Frame(join, a);
            join.Lost();
            Check(Frame(join, b, 4) == b && join.pending && join.last[0] == 0.5f, "a whole skipped frame keeps the join pending");
        }
        // Silent frames after repeated failures: they start from the last
        // sample and the next decoded frame rises from zero.
        {
            FrameJoin join;
            std::vector<float> a(512, -0.9f);
            Frame(join, a);
            join.Lost();
            std::vector<float> silent(512);
            for (uint32_t i = 0; i < 512; ++i) silent[i] = join.Silent(0, i);
            join.WroteSilence();
            Check(std::fabs(silent[0] + 0.9f) < 0.002f && silent[FrameJoin::Samples] == 0.0f && silent[511] == 0.0f, "silence starts at the last sample and reaches zero");
            Check(MaxStep(-0.9f, silent) < 0.9f * 0.03f, "no step into the silence");
            Check(join.pending && join.last[0] == 0.0f, "the frame after the silence is joined from zero");
            std::vector<float> second(512);
            for (uint32_t i = 0; i < 512; ++i) second[i] = join.Silent(0, i);
            for (float v : second) Check(v == 0.0f, "a second silent frame is plain silence");
            const auto out = Frame(join, std::vector<float>(512, 0.7f));
            Check(out[0] < 0.002f && out[511] == 0.7f && MaxStep(0.0f, out) < 0.7f * 0.03f, "the voice rises from zero");
        }
        // Both channels keep their own value.
        {
            FrameJoin join;
            join.Wrote(0.3f, -0.4f);
            join.Lost();
            Check(std::fabs(join.Decoded(0, 0, 1.0f) - 0.3f) < 0.002f && std::fabs(join.Decoded(1, 0, 1.0f) + 0.4f) < 0.003f, "per-channel join");
            Check(join.Decoded(0, FrameJoin::Samples, 1.0f) == 1.0f, "outside the window: the decoded value");
            join.Clear();
            Check(!join.pending && join.last[0] == 0 && join.last[1] == 0 && join.Decoded(0, 0, 1.0f) == 1.0f, "cleared with the context");
        }
        std::printf("xma frame join: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "xma frame join test failed: %s\n", e.what());
        return 1;
    }
}
