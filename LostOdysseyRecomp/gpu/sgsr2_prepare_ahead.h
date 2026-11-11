#pragma once
// Steprant 2.0.2 B5: when the renderer prepares SGSR2's session ahead.
// Until 2.0 the session (two compute pipelines, five images, SMAA's four
// pipelines and its stage images) was made inside the scene-copy draw of the
// first frame that used SGSR2 with a plan: the first 3D frame after the
// title, and the first one after every change of size (phone and TV,
// resolution). The renderer now makes it at the start of a frame, before any
// draw, once per plan, and only while no session exists: the frames before
// the first 3D scene (logos, title, a loading screen) pay for it instead of
// the first frame of the scene. What is made is the same; nothing a frame
// draws changes. LO_SGSR2_PREPARE_AHEAD=0 keeps the 2.0 moment.
#include <cstdint>

namespace gpu::sgsr2_ahead {

struct Plan
{
    bool sgsr2 = false; // the plan upscales with SGSR2
    uint32_t width = 0, height = 0, outputWidth = 0, outputHeight = 0;
    uint64_t deviceEpoch = 0;
    bool operator==(const Plan&) const = default;
};

struct Tracker
{
    Plan attempted{};
    bool any = false;

    // At the start of a frame: whether to prepare the session of `plan` now.
    // True at most once per plan until Released(); never while a session
    // exists (its images may be in use and its size is the frames' own).
    bool Due(bool enabled, const Plan& plan, uint32_t drawsThisFrame, bool sessionExists)
    {
        if (!enabled || !plan.sgsr2 || drawsThisFrame != 0) return false;
        if (!plan.width || !plan.height || !plan.outputWidth || !plan.outputHeight ||
            plan.width > plan.outputWidth || plan.height > plan.outputHeight)
            return false;
        if (any && attempted == plan) return false;
        attempted = plan;
        any = true;
        return !sessionExists;
    }
    // The renderer released the session's images (a change of size after a
    // GPU drain): the next frame start prepares the new ones.
    void Released() { any = false; }
};

inline bool Enabled(const char* value) { return !(value && value[0] == '0' && value[1] == '\0'); }

}
