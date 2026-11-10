#pragma once
// M1: when the renderer promotes the final target of a frame without a 3D
// scene (menu, loading screen, fade) to the output size, instead of switching
// the render size. Pure decision, shared with the host test.
#include <algorithm>
#include <cstdint>

namespace gpu::idle_ui
{
struct DrawFacts
{
    bool enabled = false;          // LO_SGSR2_UI_PROMOTION / native-switch fallback
    bool finalTargetKnown = false; // a present has told which target becomes the frontbuffer
    bool finalTarget = false;      // this draw writes that target
    bool depth = false;            // depth bound: a 3D draw, never promoted
    bool sgsr2Upscaling = false;   // SGSR2 plan whose input is smaller than the output
    bool sceneThisFrame = false;   // SGSR2 already upscaled a scene in this frame
    uint32_t idleFrames = 0;       // presented frames in a row that SGSR2 did not upscale
    bool blocked = false;          // a promotion was undone in its frame (Paused): until the next SGSR2 frame or plan epoch
    bool promotionBusy = false;    // a promotion is active or was already made this frame
    bool atOutputSize = false;     // the target already has the output size
    bool backgroundOnly = false;   // the draw samples only resolved surfaces, or nothing
    // Steprant 2.0.1: a loading screen or a movie. Its picture is the
    // background, so it is promoted from the first draw (drawn once at the
    // output size from its own texture instead of at the render size and
    // enlarged twice), and an earlier undo does not hold it back.
    bool transition = false;
};

inline bool Wanted(const DrawFacts& f)
{
    return f.enabled && f.finalTargetKnown && f.finalTarget && !f.depth && f.sgsr2Upscaling &&
        !f.sceneThisFrame && f.idleFrames > 0 && (f.transition || !f.blocked) && !f.promotionBusy && !f.atOutputSize &&
        (f.transition || !f.backgroundOnly);
}

// A promotion undone in its own frame pauses promotions until SGSR2 runs
// again, but only within the plan geometry epoch it happened in: an undo
// caused by the plan changing under it (the boot title screen switches plans
// in its first frames) must not keep a screen without a 3D scene, like the
// title, at the render size for good.
// Steprant 2.0.1: the back-off lasts kPauseFrames (2 s at 30 FPS). Until
// 2.0 it lasted until the next SGSR2 frame: a movie or a loading screen that
// followed an undo stayed at the render size to its end.
inline constexpr uint64_t kPauseFrames = 60;

struct Pause
{
    bool active = false;
    uint64_t epoch = 0;
    uint64_t frame = 0; // render frame of the undo
};

inline bool Paused(const Pause& pause, uint64_t currentEpoch, uint64_t currentFrame)
{
    return pause.active && pause.epoch == currentEpoch && currentFrame < pause.frame + kPauseFrames;
}

inline uint32_t NextIdleFrames(uint32_t previous, bool sceneFrame)
{
    return sceneFrame ? 0 : std::min<uint32_t>(previous + 1, 1u << 30);
}
}
