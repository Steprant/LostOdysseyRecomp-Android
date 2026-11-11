// Steprant 2.0.2: when the SGSR2 spatial continuation takes a frame, and when
// its zero-motion images are made (gpu/sgsr2_continuation.h).
#include <gpu/sgsr2_continuation.h>
#include <cstdio>
#include <stdexcept>
using namespace gpu::sgsr2_continuation;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        Facts scene;
        scene.enabled = scene.sgsr2Plan = scene.motionMissing = scene.frameOnly = true;
        scene.colorComplete = scene.depthComplete = scene.depthConventionKnown = scene.sdrColor = scene.sceneCamera = true;
        scene.draws = 820;
        scene.colorWidth = scene.planWidth = 2003; scene.colorHeight = scene.planHeight = 900;
        scene.imageWidth = 2003; scene.imageHeight = 900;
        Check(Wanted(scene), "a complete 3D scene without motion is continued");

        auto f = scene; f.enabled = false;
        Check(!Wanted(f), "LO_SGSR2_CONTINUATION=0 restores the 2.0 fallback");
        f = scene; f.sgsr2Plan = false;
        Check(!Wanted(f), "only an SGSR2 plan");
        f = scene; f.motionMissing = false;
        Check(!Wanted(f), "a frame with its motion needs nothing");
        f = scene; f.frameOnly = false;
        Check(!Wanted(f), "a failed request is not hidden by the continuation");
        f = scene; f.colorComplete = false;
        Check(!Wanted(f), "no colour capture, no continuation");
        f = scene; f.depthComplete = false;
        Check(!Wanted(f), "no depth capture, no continuation");
        f = scene; f.depthConventionKnown = false;
        Check(!Wanted(f), "an unknown depth convention is left to the 2.0 path");
        f = scene; f.sdrColor = false;
        Check(!Wanted(f), "colour that is not display-range is left to the 2.0 path");
        f = scene; f.sceneCamera = false;
        Check(!Wanted(f), "a frame without one valid scene camera is not a complete scene");
        f = scene; f.draws = kSceneDraws - 1;
        Check(!Wanted(f), "a handful of draws is not a 3D scene (a loading screen with a spinning model)");
        f = scene; f.draws = kSceneDraws;
        Check(Wanted(f), "the scene threshold is inclusive");
        f = scene; f.colorWidth = 1024; f.colorHeight = 512;
        Check(!Wanted(f), "a partial colour capture is not the full scene");
        f = scene; f.planWidth = f.planHeight = 0;
        Check(!Wanted(f), "no plan size, no continuation");
        f = scene; f.imageWidth = f.imageHeight = 0;
        Check(!Wanted(f), "without the images made at a frame start the frame takes the 2.0 path");
        Check(SceneComplete(f), "and it is counted as a complete scene waiting for its images");
        f = scene; f.imageWidth = 1600; f.imageHeight = 900;
        Check(!Wanted(f), "images smaller than the scene (TV size, then the phone) are not used");
        f = scene; f.colorWidth = f.planWidth = 1600;
        Check(Wanted(f), "larger images serve a smaller scene");

        Images images;
        Check(!images.NeededAtFrameStart(false, true, 2003, 900, 0), "nothing while the continuation is off");
        Check(!images.NeededAtFrameStart(true, false, 2003, 900, 0), "nothing without an SGSR2 plan");
        Check(!images.NeededAtFrameStart(true, true, 2003, 900, 5), "never after the frame's first draw");
        Check(!images.NeededAtFrameStart(true, true, 0, 900, 0), "nothing without a plan size");
        Check(!images.NeededAtFrameStart(true, true, kMaxWidth + 1, 900, 0), "absurd sizes are refused");
        Check(images.NeededAtFrameStart(true, true, 1600, 900, 0), "the first SGSR2 frame start makes them");
        images.Created(1600, 900);
        Check(!images.NeededAtFrameStart(true, true, 1600, 900, 0), "made once per size");
        Check(!images.NeededAtFrameStart(true, true, 1280, 720, 0), "a smaller plan reuses them");
        Check(images.NeededAtFrameStart(true, true, 2003, 900, 0), "a larger plan grows them at the next frame start");
        images.Failed(2003, 900);
        Check(!images.NeededAtFrameStart(true, true, 2003, 900, 0), "a failed size is not retried every frame");
        Check(!images.NeededAtFrameStart(true, true, 1800, 900, 0), "nor a smaller one that still needs new images");
        Check(!images.NeededAtFrameStart(true, true, 1600, 900, 0), "the images that exist keep serving their size");
        Check(images.NeededAtFrameStart(true, true, 2670, 1200, 0), "a larger plan than the failed one tries again");
        images.Created(2670, 1200);
        Check(!images.NeededAtFrameStart(true, true, 2003, 900, 0), "a success clears the failure");
        std::printf("sgsr2 continuation: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sgsr2 continuation test failed: %s\n", e.what());
        return 1;
    }
}
