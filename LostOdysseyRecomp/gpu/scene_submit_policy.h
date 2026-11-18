#pragma once
// Steprant 2.4.6: heavy scenes submitted to the GPU in pieces.
//
// The renderer records a whole frame into one batch and submits it at the
// swap. At the ticket gate of the station (3,000-4,500 draws a frame, 18/11
// 10:48 session) the command thread spent about 22-27 ms recording, the GPU
// about 31 ms executing, but the GPU only started when the recording ended:
// frames of 45-50 ms, with the command thread waiting 15-30 ms at present for
// the GPU. Up to about 2,500 draws the game held 30 FPS.
//
// When the previous frame had at least minFrameDraws draws, the renderer
// submits what the open batch holds before a draw once the batch has
// pieceDraws draws and the draw changes render target (a pass boundary,
// nothing to store and reload), or once it has twice that many anyway. It is
// the renderer's existing split between draws (Flush, then Begin on the other
// slot), used today when descriptor pools, the upload ring or the vertex arena
// fill: same commands, same order, in more submissions. The GPU starts on the
// first pieces while the command thread records the rest.
//
// Only before the scene goes to the upscaler (the swap batch keeps SR and the
// post-processing), not with draw profiling or debug captures.
// LO_SCENE_SUBMIT_DRAWS=<n> sets the piece size (0 off), LO_SCENE_SUBMIT_MIN=<n>
// the previous-frame draws that turn it on.
#include <cstdint>
#include <cstdlib>

namespace gpu::scene_submit
{
    struct Settings
    {
        uint32_t pieceDraws = 768;
        uint32_t minFrameDraws = 1800;
    };

    inline uint32_t ParseCount(const char* value, uint32_t fallback)
    {
        if (!value || !*value) return fallback;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (!end || *end || parsed > 1000000ul) return fallback;
        return uint32_t(parsed);
    }

    inline Settings FromValues(const char* pieceDraws, const char* minFrameDraws)
    {
        Settings s;
        s.pieceDraws = ParseCount(pieceDraws, s.pieceDraws);
        s.minFrameDraws = ParseCount(minFrameDraws, s.minFrameDraws);
        return s;
    }

    inline Settings FromEnvironment()
    {
        return FromValues(std::getenv("LO_SCENE_SUBMIT_DRAWS"), std::getenv("LO_SCENE_SUBMIT_MIN"));
    }

    enum class Split : uint8_t { None, AtTargetChange, InsidePass };

    // Before a draw: submit the open batch now?
    inline Split Due(const Settings& s, uint32_t previousFrameDraws, uint32_t drawsThisBatch, bool targetChanged)
    {
        if (s.pieceDraws == 0 || previousFrameDraws < s.minFrameDraws || drawsThisBatch < s.pieceDraws) return Split::None;
        if (targetChanged) return Split::AtTargetChange;
        if (uint64_t(drawsThisBatch) >= uint64_t(s.pieceDraws) * 2) return Split::InsidePass;
        return Split::None;
    }

    // Counted between log lines.
    struct Stats
    {
        uint64_t frames = 0, splitFrames = 0, atTargetChange = 0, insidePass = 0, splitNs = 0;
        bool frameSplit = false;

        void Note(Split split, uint64_t ns)
        {
            if (split == Split::AtTargetChange) ++atTargetChange;
            else if (split == Split::InsidePass) ++insidePass;
            else return;
            splitNs += ns;
            frameSplit = true;
        }
        void EndFrame()
        {
            ++frames;
            if (frameSplit) ++splitFrames;
            frameSplit = false;
        }
        uint64_t Submits() const { return atTargetChange + insidePass; }
    };
}
