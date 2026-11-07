// M1: interface promotion of scene-less SGSR2 frames (gpu/idle_ui_promotion.h).
#include <gpu/idle_ui_promotion.h>
#include <cstdio>
#include <stdexcept>
using namespace gpu::idle_ui;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        DrawFacts menu;
        menu.enabled = menu.finalTargetKnown = menu.finalTarget = menu.sgsr2Upscaling = true;
        menu.idleFrames = 1;
        Check(Wanted(menu), "an interface draw on the final target of a scene-less frame is promoted");
        auto f = menu; f.backgroundOnly = true;
        Check(!Wanted(f), "the background (resolved scene, blur, untextured fill) stays at the render size");
        f = menu; f.idleFrames = 0;
        Check(!Wanted(f), "the first frame after a 3D scene is not promoted (a transition may still be 3D)");
        f = menu; f.sceneThisFrame = true;
        Check(!Wanted(f), "a frame that already drew 3D is not promoted");
        f = menu; f.depth = true;
        Check(!Wanted(f), "a depth draw is never promoted");
        f = menu; f.finalTarget = false;
        Check(!Wanted(f), "only the frontbuffer's target is promoted");
        f = menu; f.finalTargetKnown = false;
        Check(!Wanted(f), "nothing before the first present names the frontbuffer target");
        f = menu; f.sgsr2Upscaling = false;
        Check(!Wanted(f), "only an SGSR2 plan that renders below the output size");
        f = menu; f.promotionBusy = true;
        Check(!Wanted(f), "one promotion per frame");
        f = menu; f.atOutputSize = true;
        Check(!Wanted(f), "a target already at the output size needs nothing");
        f = menu; f.blocked = true;
        Check(!Wanted(f), "the back-off after an undone promotion holds");
        f = menu; f.enabled = false;
        Check(!Wanted(f), "the fallback switch disables it");
        Check(NextIdleFrames(0, false) == 1 && NextIdleFrames(5, false) == 6 && NextIdleFrames(5, true) == 0 &&
            NextIdleFrames(1u << 30, false) == 1u << 30, "idle counter");
        std::printf("PASS: %u idle UI promotion checks\n", checks);
        return 0;
    } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
}
