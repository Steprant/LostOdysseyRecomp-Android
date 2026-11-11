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
        Check(!Paused(Pause{}, 3), "no pause by default");
        Check(Paused(Pause{true, 1}, 1), "an undo pauses promotions within its plan epoch");
        Check(!Paused(Pause{true, 1}, 2), "a later plan epoch (the boot title's plan switches) resumes them");
        // 2.0.1: loading screens and movies are promoted from their first draw.
        f = menu; f.transition = true; f.backgroundOnly = true;
        Check(Wanted(f), "a loading screen's or movie's picture is promoted although it is the background");
        f = menu; f.transition = true; f.blocked = true;
        Check(Wanted(f), "an earlier undo does not hold a transition back");
        f = menu; f.transition = true; f.depth = true;
        Check(!Wanted(f), "a depth draw is never promoted, transition or not");
        f = menu; f.transition = true; f.sceneThisFrame = true;
        Check(!Wanted(f), "a transition frame that drew a 3D scene is not promoted");
        // 2.0.2: no promote/undo loop. A screen that undid its promotion stays
        // paused for as many frames as it lasts (2.0.1 retried every 60).
        {
            Pause pause;
            unsigned promotions = 0;
            for (uint64_t frame = 0; frame < 10000; ++frame) {
                auto draw = menu; draw.blocked = Paused(pause, 7);
                if (Wanted(draw)) { ++promotions; pause = {true, 7}; } // undone in its frame by a depth draw
            }
            Check(promotions == 1, "a menu that draws depth on the final target is promoted and undone once, not every two seconds");
            pause.active = false; // an SGSR2 frame
            auto draw = menu; draw.blocked = Paused(pause, 7);
            Check(Wanted(draw), "after the next SGSR2 frame promotions resume");
        }
        {
            // A loading screen that draws 3D on the final target: its
            // transition promotion is undone once and not retried.
            Pause pause, transitionPause;
            unsigned promotions = 0;
            for (uint64_t frame = 0; frame < 10000; ++frame) {
                auto draw = menu; draw.backgroundOnly = true;
                draw.blocked = Paused(pause, 7);
                draw.transition = !Paused(transitionPause, 7);
                if (Wanted(draw)) { ++promotions; pause = transitionPause = {true, 7}; }
            }
            Check(promotions == 1, "a transition that undoes its promotion is not promoted again while it lasts");
            pause.active = transitionPause.active = false; // gameplay came back
            auto draw = menu; draw.backgroundOnly = true; draw.transition = !Paused(transitionPause, 7);
            Check(Wanted(draw), "the next loading screen after gameplay is promoted from its first draw again");
        }
        {
            Stats stats;
            Check(!stats.SummaryDue(5000), "no summary without a promotion");
            stats.Promoted(false); stats.Promoted(true); stats.Undone(true);
            Check(stats.promoted == 1 && stats.transitionPromoted == 1 && stats.undone == 0 && stats.transitionUndone == 1,
                "promotions and undos are counted by kind");
            Check(!stats.SummaryDue(kSummaryFrames - 1), "not before the first interval");
            Check(stats.SummaryDue(kSummaryFrames), "one summary when a counter moved");
            Check(!stats.SummaryDue(kSummaryFrames * 5), "none while nothing changes");
            stats.Promoted(false);
            Check(!stats.SummaryDue(kSummaryFrames + 10), "at most one per interval");
            Check(stats.SummaryDue(kSummaryFrames * 2), "the next one an interval later");
        }
        Check(NextIdleFrames(0, false) == 1 && NextIdleFrames(5, false) == 6 && NextIdleFrames(5, true) == 0 &&
            NextIdleFrames(1u << 30, false) == 1u << 30, "idle counter");
        std::printf("PASS: %u idle UI promotion checks\n", checks);
        return 0;
    } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
}
