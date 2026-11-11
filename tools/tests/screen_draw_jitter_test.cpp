// Steprant 2.0.2: only certain backdrop and scene quads move with the SGSR2
// jitter (gpu/screen_draw_jitter.h).
#include <gpu/screen_draw_jitter.h>
#include <cstdio>
#include <stdexcept>
using namespace gpu::screen_jitter;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        Check(ParseMode(nullptr, nullptr) == Mode::Certain, "the certain rule by default");
        Check(ParseMode("0", nullptr) == Mode::Off && ParseMode("0", "1") == Mode::Off, "0 turns the screen-draw jitter off");
        Check(ParseMode(nullptr, "1") == Mode::Wide && ParseMode("1", "1") == Mode::Wide, "the 2.0.1 rule only when asked for");
        Check(ParseMode("1", "0") == Mode::Certain && ParseMode("", "") == Mode::Certain, "anything else is the certain rule");

        // The painted backdrop of the palace-door scene: one opaque textured quad on the scene target.
        Facts backdrop;
        backdrop.sceneTarget = true; backdrop.ordinaryTextures = 1;
        backdrop.primitive = 8; backdrop.indexCount = 3; backdrop.blendControl = 0x00010001; backdrop.colorMask = 0xF;
        Check(Decide(Mode::Certain, backdrop) == Reason::Jittered, "an opaque textured quad on the scene target moves with the scene");
        Check(Decide(Mode::Wide, backdrop) == Reason::Jittered, "and it did in 2.0.1");
        Check(Decide(Mode::Off, backdrop) == Reason::Off, "off is off");

        auto f = backdrop; f.depthTested = true;
        Check(Decide(Mode::Certain, f) == Reason::DepthTested && Decide(Mode::Wide, f) == Reason::DepthTested,
            "depth-tested draws are not screen draws (they have their own jitter)");
        f = backdrop; f.afterCapture = true;
        Check(Decide(Mode::Certain, f) == Reason::AfterCapture && Decide(Mode::Wide, f) == Reason::AfterCapture,
            "nothing after SGSR2 took the scene (interface, final passes)");
        f = backdrop; f.samplesFrameResolved = true; f.samplesResolved = true;
        Check(Decide(Mode::Certain, f) == Reason::SamplesFrameResolved && Decide(Mode::Wide, f) == Reason::SamplesFrameResolved,
            "a pass over this frame's resolved scene stays still");

        // What 2.0.1 moved and 2.0.2 leaves still.
        f = backdrop; f.sceneTarget = false;
        Check(Decide(Mode::Certain, f) == Reason::NotSceneTarget && Decide(Mode::Wide, f) == Reason::Jittered,
            "a quad into another target (a mask, a distortion map) is not scene content");
        f = backdrop; f.primitive = 4; f.indexCount = 96;
        Check(Decide(Mode::Certain, f) == Reason::NotOneQuad && Decide(Mode::Wide, f) == Reason::Jittered,
            "a batch of sprites or particles is not a backdrop quad");
        f = backdrop; f.ordinaryTextures = 0;
        Check(Decide(Mode::Certain, f) == Reason::NoOrdinaryTexture && Decide(Mode::Wide, f) == Reason::Jittered,
            "a fade or fill without a texture stays still");
        f = backdrop; f.samplesResolved = true;
        Check(Decide(Mode::Certain, f) == Reason::SamplesResolved && Decide(Mode::Wide, f) == Reason::Jittered,
            "a quad that samples an earlier frame's resolved surface (feedback, cross-fade) stays still");
        f = backdrop; f.blendControl = 0x00010706; // source alpha, inverse source alpha
        Check(Decide(Mode::Certain, f) == Reason::Blended && Decide(Mode::Wide, f) == Reason::Jittered,
            "an alpha-blended quad (interface, dissolve) stays still");
        f = backdrop; f.blendControl = 0x00010101; // one, one
        Check(Decide(Mode::Certain, f) == Reason::Blended, "an additive quad (glow, light shaft, water distortion) stays still");
        f = backdrop; f.blendControl = 0x07060001; // colour replaced, alpha blended
        Check(Decide(Mode::Certain, f) == Reason::Blended, "blended alpha counts as blending");
        f = backdrop; f.colorMask = 0x8;
        Check(Decide(Mode::Certain, f) == Reason::PartialColorWrite, "an alpha-only write is not a picture");
        f = backdrop; f.colorMask = 0x7;
        Check(Decide(Mode::Certain, f) == Reason::Jittered, "RGB without alpha is a picture");

        Check(OneQuad(8, 3) && OneQuad(4, 6) && OneQuad(5, 4) && OneQuad(6, 4) && OneQuad(13, 4), "the quad forms");
        Check(!OneQuad(8, 6) && !OneQuad(4, 3) && !OneQuad(4, 12) && !OneQuad(5, 6) && !OneQuad(1, 4) && !OneQuad(2, 4),
            "two rectangles, one triangle, longer strips, points and lines are not one quad");
        Check(Replaces(0x00010001) && !Replaces(0x00010000) && !Replaces(0x00000001) && !Replaces(0x00010021) && !Replaces(0x00010101),
            "replace means ONE/ADD/ZERO for colour and alpha");

        SceneTargets target;
        Check(!target.Matches(10, 0, 0), "no scene target before a jittered 3D draw");
        target.Note(10, 0x2d0, 0);
        Check(target.Matches(10, 0x2d0, 0), "the target the frame's 3D draws write");
        Check(target.Matches(11, 0x2d0, 0), "a backdrop drawn before the next frame's first 3D draw still matches");
        Check(!target.Matches(12, 0x2d0, 0), "but not two frames later (a menu or a movie in between)");
        Check(!target.Matches(10, 0x2d0, 7) && !target.Matches(10, 0x5a0, 0), "another format or EDRAM base is another target");
        Check(!target.Matches(9, 0x2d0, 0), "never backwards");
        target.Note(12, 0x5a0, 7);
        Check(target.Matches(12, 0x5a0, 7) && !target.Matches(12, 0x2d0, 0), "a second 3D target is remembered beside the first, each with its frame");
        target.Note(12, 0x2d0, 0);
        Check(target.Matches(12, 0x2d0, 0) && target.Matches(12, 0x5a0, 7), "both when both were written");
        for (uint32_t i = 0; i < 8; ++i) target.Note(20 + i, 0x100 + i, 1);
        Check(target.Matches(27, 0x107, 1) && target.Matches(27, 0x106, 1) && !target.Matches(21, 0x100, 1),
            "the oldest entries are replaced");
        Stats stats;
        stats.Count(Reason::Jittered); stats.Count(Reason::Blended); stats.Count(Reason::Blended);
        Check(stats.counts[size_t(Reason::Jittered)] == 1 && stats.counts[size_t(Reason::Blended)] == 2, "counts by reason");
        Check(!stats.SummaryDue(kSummaryFrames - 1) && stats.SummaryDue(kSummaryFrames) && !stats.SummaryDue(kSummaryFrames + 1) &&
            stats.SummaryDue(2 * kSummaryFrames), "one summary per interval");
        stats.Clear();
        Check(stats.counts[size_t(Reason::Blended)] == 0, "cleared after the summary");
        for (unsigned i = 0; i < unsigned(Reason::Count); ++i)
            Check(Name(Reason(i))[0] != 'u', "every reason has a name");
        std::printf("screen draw jitter: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "screen draw jitter test failed: %s\n", e.what());
        return 1;
    }
}
