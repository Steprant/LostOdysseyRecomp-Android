#pragma once
// Steprant 2.0.2: when SGSR2's spatial continuation (2.0.1) may take a frame.
//
// A scene frame whose motion is not ready used to leave SGSR2 for the SGSR1
// presentation path (2.0). The continuation runs SGSR2 over zero motion with
// its history reset instead. 2.0.1 allowed it for any frame with colour and
// depth and created its zero-motion images where it first needed them, in the
// middle of a draw. From 2.0.2:
//   - only a complete 3D scene is continued: a full-size colour and depth
//     capture, one valid scene camera and at least kSceneDraws draws before
//     the capture. Anything else takes the 2.0 path;
//   - the zero-motion images are made and cleared by the first command list
//     of a frame, before any draw (Images::NeededAtFrameStart). A frame that
//     finds them missing or too small takes the 2.0 path.
// LO_SGSR2_CONTINUATION=0 turns the continuation off.
#include <cstdint>

namespace gpu::sgsr2_continuation {

// The "3D scene" of the "SGSR2: fallback with a 3D scene" log line.
inline constexpr uint32_t kSceneDraws = 100;
inline constexpr uint32_t kMaxWidth = 7680, kMaxHeight = 4320;

struct Facts
{
    bool enabled = false;          // LO_SGSR2_CONTINUATION
    bool sgsr2Plan = false;        // the active plan upscales with SGSR2
    bool motionMissing = false;    // the selection is Pending or Incomplete
    bool frameOnly = false;        // the failure concerns this frame, not the request
    bool colorComplete = false, depthComplete = false;
    bool depthConventionKnown = false;
    bool sdrColor = false;
    bool sceneCamera = false;      // one valid scene camera observed in this frame
    uint32_t draws = 0;            // draws of this frame before the capture
    uint32_t colorWidth = 0, colorHeight = 0;
    uint32_t planWidth = 0, planHeight = 0;
    uint32_t imageWidth = 0, imageHeight = 0; // zero-motion images, 0 when missing
};

inline bool SceneComplete(const Facts& f)
{
    return f.colorComplete && f.depthComplete && f.depthConventionKnown && f.sdrColor && f.sceneCamera &&
        f.draws >= kSceneDraws && f.planWidth && f.planHeight &&
        f.colorWidth == f.planWidth && f.colorHeight == f.planHeight;
}

inline bool ImagesReady(const Facts& f)
{
    return f.imageWidth && f.imageHeight && f.colorWidth <= f.imageWidth && f.colorHeight <= f.imageHeight;
}

inline bool Wanted(const Facts& f)
{
    return f.enabled && f.sgsr2Plan && f.motionMissing && f.frameOnly && SceneComplete(f) && ImagesReady(f);
}

// The zero-motion images. Sizes only grow; a failed creation is not retried
// for the same size (the 2.0 path stays in use).
struct Images
{
    uint32_t width = 0, height = 0;
    uint32_t failedWidth = 0, failedHeight = 0;

    // Whether the frame that starts now must (re)create them for this plan.
    bool NeededAtFrameStart(bool enabled, bool sgsr2Plan, uint32_t planWidth, uint32_t planHeight,
        uint32_t drawsThisFrame) const
    {
        if (!enabled || !sgsr2Plan || drawsThisFrame != 0) return false;
        if (!planWidth || !planHeight || planWidth > kMaxWidth || planHeight > kMaxHeight) return false;
        if (planWidth <= width && planHeight <= height) return false;
        // A size up to the one that failed would fail again.
        if (failedWidth && planWidth <= failedWidth && planHeight <= failedHeight) return false;
        return true;
    }
    void Created(uint32_t newWidth, uint32_t newHeight) { width = newWidth; height = newHeight; failedWidth = failedHeight = 0; }
    void Failed(uint32_t wantedWidth, uint32_t wantedHeight) { failedWidth = wantedWidth; failedHeight = wantedHeight; }
};

}
