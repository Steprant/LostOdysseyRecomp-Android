#pragma once
// Steprant 2.0.2: which screen-space draws move with the SGSR2 jitter.
//
// 2.0.1 (5c41812e) jittered every draw in the full 1280x720 viewport that did
// not test depth, came before SGSR2 took the scene and did not sample a
// surface resolved in the same frame. That fixed the swimming backdrop of the
// palace-door scene but also moved draws that are not scene content: glows and
// additive effects, fades, interface drawn before the capture, passes into
// other targets with the same viewport (masks, distortion maps). Moving one of
// those by a fraction of a pixel while what it is combined with stays still is
// a glitch of its own.
//
// From 2.0.2 only a draw that is certainly a backdrop or scene quad moves:
//   - it does not test depth and comes before the SGSR2 capture (as before);
//   - it writes the scene's own colour target, the one the jittered 3D draws
//     of this or the previous frame wrote;
//   - it is one quad (a rectangle, or two triangles);
//   - it samples at least one ordinary texture and no resolved surface at all
//     (no post-processing, no feedback, no fade without a texture);
//   - it replaces what is under it: no blending, all colour channels written
//     (no interface, no cross-fade, no additive light).
// LO_SR_JITTER_SCREEN_DRAWS=0 turns the screen-draw jitter off (2.0);
// LO_SR_JITTER_SCREEN_DRAWS_WIDE=1 restores the 2.0.1 rule for an A/B test.
#include <cstddef>
#include <cstdint>

namespace gpu::screen_jitter {

enum class Mode : uint8_t { Off, Certain, Wide };

inline Mode ParseMode(const char* enabled, const char* wide)
{
    if (enabled && enabled[0] == '0' && !enabled[1]) return Mode::Off;
    if (wide && wide[0] == '1' && !wide[1]) return Mode::Wide;
    return Mode::Certain;
}

// The colour targets the jittered 3D scene draws write (the scene target, and
// at most a few more when a frame renders 3D into a second one).
struct SceneTargets
{
    struct Entry { uint64_t frame = ~0ull; uint32_t base = 0, format = 0; };
    Entry entries[4];
    void Note(uint64_t renderFrame, uint32_t edramBase, uint32_t colorFormat)
    {
        Entry* oldest = &entries[0];
        for (auto& entry : entries) {
            if (entry.frame != ~0ull && entry.base == edramBase && entry.format == colorFormat) { entry.frame = renderFrame; return; }
            if (entry.frame == ~0ull || (oldest->frame != ~0ull && entry.frame < oldest->frame)) oldest = &entry;
        }
        *oldest = {renderFrame, edramBase, colorFormat};
    }
    bool Matches(uint64_t renderFrame, uint32_t edramBase, uint32_t colorFormat) const
    {
        for (const auto& entry : entries)
            if (entry.frame != ~0ull && renderFrame >= entry.frame && renderFrame - entry.frame <= 1 &&
                entry.base == edramBase && entry.format == colorFormat)
                return true;
        return false;
    }
};

enum class Reason : uint8_t
{
    Jittered, Off, DepthTested, AfterCapture, SamplesFrameResolved, // the 2.0.1 conditions
    NotSceneTarget, NotOneQuad, NoOrdinaryTexture, SamplesResolved, Blended, PartialColorWrite,
    Count
};

constexpr const char* Name(Reason reason)
{
    switch (reason) {
    case Reason::Jittered: return "jittered";
    case Reason::Off: return "off";
    case Reason::DepthTested: return "depth_tested";
    case Reason::AfterCapture: return "after_capture";
    case Reason::SamplesFrameResolved: return "samples_frame_resolved";
    case Reason::NotSceneTarget: return "not_scene_target";
    case Reason::NotOneQuad: return "not_one_quad";
    case Reason::NoOrdinaryTexture: return "no_ordinary_texture";
    case Reason::SamplesResolved: return "samples_resolved";
    case Reason::Blended: return "blended";
    case Reason::PartialColorWrite: return "partial_color_write";
    case Reason::Count: break;
    }
    return "unknown";
}

struct Facts
{
    bool depthTested = false;          // depth bound and the depth test enabled
    bool afterCapture = false;         // SGSR2 already took (or is taking) this frame's scene
    bool samplesFrameResolved = false; // a texture resolved in this frame
    bool samplesResolved = false;      // a resolved surface of any frame
    uint32_t ordinaryTextures = 0;     // sampled textures that are not resolved surfaces
    bool sceneTarget = false;          // SceneTargets::Matches for the bound colour target
    uint32_t primitive = 0;            // Xenos primitive type
    uint32_t indexCount = 0;
    uint32_t blendControl = 0;         // RB_BLENDCONTROL0
    uint32_t colorMask = 0;
};

// One rectangle (rectangle list, 3 vertices), or two triangles as a list (6),
// strip or fan (4), or one quad of a quad list (4).
constexpr bool OneQuad(uint32_t primitive, uint32_t indexCount)
{
    return (primitive == 8 && indexCount == 3) || (primitive == 4 && indexCount == 6) ||
        ((primitive == 5 || primitive == 6 || primitive == 13) && indexCount == 4);
}

// Source ONE, operation ADD, destination ZERO for colour and for alpha.
constexpr bool Replaces(uint32_t blendControl)
{
    return (blendControl & 0x1FFFu) == 1u && ((blendControl >> 16) & 0x1FFFu) == 1u;
}

constexpr Reason Decide(Mode mode, const Facts& f)
{
    if (mode == Mode::Off) return Reason::Off;
    if (f.depthTested) return Reason::DepthTested;
    if (f.afterCapture) return Reason::AfterCapture;
    if (f.samplesFrameResolved) return Reason::SamplesFrameResolved;
    if (mode == Mode::Wide) return Reason::Jittered;
    if (!f.sceneTarget) return Reason::NotSceneTarget;
    if (!OneQuad(f.primitive, f.indexCount)) return Reason::NotOneQuad;
    if (f.samplesResolved) return Reason::SamplesResolved;
    if (!f.ordinaryTextures) return Reason::NoOrdinaryTexture;
    if (!Replaces(f.blendControl)) return Reason::Blended;
    if ((f.colorMask & 7u) != 7u) return Reason::PartialColorWrite;
    return Reason::Jittered;
}

// Per reason, for one log line every kSummaryFrames frames.
inline constexpr uint64_t kSummaryFrames = 1800;
struct Stats
{
    uint64_t counts[size_t(Reason::Count)] = {};
    uint64_t loggedFrame = 0;
    void Count(Reason reason) { ++counts[size_t(reason)]; }
    bool SummaryDue(uint64_t frame)
    {
        if (frame < loggedFrame + kSummaryFrames) return false;
        loggedFrame = frame;
        return true;
    }
    void Clear() { for (auto& count : counts) count = 0; }
};

}
