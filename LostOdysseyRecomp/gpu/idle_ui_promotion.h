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
    bool sceneThisFrame = false;   // a depth-writing draw already happened in this frame
    uint32_t idleFrames = 0;       // presented frames in a row without a 3D scene
    bool blocked = false;          // back-off after a promotion undone in its frame
    bool promotionBusy = false;    // a promotion is active or was already made this frame
    bool atOutputSize = false;     // the target already has the output size
    bool backgroundOnly = false;   // the draw samples only resolved surfaces, or nothing
};

inline bool Wanted(const DrawFacts& f)
{
    return f.enabled && f.finalTargetKnown && f.finalTarget && !f.depth && f.sgsr2Upscaling &&
        !f.sceneThisFrame && f.idleFrames > 0 && !f.blocked && !f.promotionBusy && !f.atOutputSize &&
        !f.backgroundOnly;
}

inline uint32_t NextIdleFrames(uint32_t previous, bool sceneFrame)
{
    return sceneFrame ? 0 : std::min<uint32_t>(previous + 1, 1u << 30);
}
}
