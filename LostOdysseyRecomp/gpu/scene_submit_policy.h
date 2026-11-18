#pragma once
// Steprant 2.4.6-2.4.7: heavy scenes submitted to the GPU in pieces.
//
// The renderer records a whole frame into one batch and submits it at the
// swap. At the ticket gate of the station (3,000-4,500 draws a frame, 18/11
// 10:48 session) the command thread recorded for 22-27 ms, the GPU worked
// about 31 ms, but only after the recording: frames of 45-50 ms.
//
// 2.4.6 split the batch with the renderer's existing split between draws
// (Flush, then Begin on the other slot). In the 12:32 session the frames of
// 3,000-3,500 draws went from 45 to 39.5 ms, but the command thread still
// waited about 19 ms a frame, now in Begin, while the GPU idled 40% of the
// time: with two slots, each piece waits for the one before the previous to
// finish, so whichever side is slower on a piece stalls the other.
//
// 2.4.7 (SameSlot): the open list goes to the GPU without a fence and the
// recording goes on in a new list of the same slot, with the same upload
// ring, descriptor pools and arena half. Nothing waits in the middle of the
// frame; the slot's fence, signalled by the swap submission after the pieces,
// covers them all (queue submission order). Same commands, same order.
//
// When the previous frame had at least minFrameDraws draws, the batch is split
// before a draw once it holds pieceDraws draws and the draw changes render
// target (a pass boundary: nothing to store and load again), or once it holds
// twice that anyway. Only before the scene goes to the upscaler, on Vulkan,
// without draw profiling or debug captures.
//
// Test build 2.4.7: by default the mode rotates every 10 s (SameSlot,
// SlotSwitch, Off) and each window logs its heavy frames, so one session at a
// crowded place compares the three. LO_SCENE_SUBMIT_MODE=same|switch|off|ab
// fixes it; LO_SCENE_SUBMIT_DRAWS=<n> sets the piece (0 off),
// LO_SCENE_SUBMIT_MIN=<n> the previous-frame draws.
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace gpu::scene_submit
{
    enum class Mode : uint8_t { Off, SlotSwitch, SameSlot };

    inline const char* Name(Mode mode)
    {
        switch (mode) {
        case Mode::Off: return "off";
        case Mode::SlotSwitch: return "switch";
        case Mode::SameSlot: return "same";
        }
        return "off";
    }

    struct Settings
    {
        uint32_t pieceDraws = 768;
        uint32_t minFrameDraws = 1800;
        bool rotate = true;          // A/B: SameSlot, SlotSwitch, Off in turn
        Mode fixed = Mode::SameSlot; // when not rotating
    };

    inline uint32_t ParseCount(const char* value, uint32_t fallback)
    {
        if (!value || !*value) return fallback;
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (!end || *end || parsed > 1000000ul) return fallback;
        return uint32_t(parsed);
    }

    inline Settings FromValues(const char* pieceDraws, const char* minFrameDraws, const char* mode = nullptr)
    {
        Settings s;
        s.pieceDraws = ParseCount(pieceDraws, s.pieceDraws);
        s.minFrameDraws = ParseCount(minFrameDraws, s.minFrameDraws);
        if (mode && *mode) {
            if (!std::strcmp(mode, "same")) { s.rotate = false; s.fixed = Mode::SameSlot; }
            else if (!std::strcmp(mode, "switch")) { s.rotate = false; s.fixed = Mode::SlotSwitch; }
            else if (!std::strcmp(mode, "off")) { s.rotate = false; s.fixed = Mode::Off; }
            // "ab" or anything else: rotate
        }
        if (s.pieceDraws == 0) { s.rotate = false; s.fixed = Mode::Off; }
        return s;
    }

    inline Settings FromEnvironment()
    {
        return FromValues(std::getenv("LO_SCENE_SUBMIT_DRAWS"), std::getenv("LO_SCENE_SUBMIT_MIN"),
            std::getenv("LO_SCENE_SUBMIT_MODE"));
    }

    // The mode of the window-th 10 s window.
    inline Mode ModeForWindow(const Settings& s, uint64_t window)
    {
        if (!s.rotate) return s.fixed;
        static constexpr Mode kOrder[3] = {Mode::SameSlot, Mode::SlotSwitch, Mode::Off};
        return kOrder[window % 3];
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

    // Counted per 10 s window.
    struct Stats
    {
        uint64_t frames = 0, splitFrames = 0, atTargetChange = 0, insidePass = 0, splitNs = 0;
        // Frames whose previous frame was heavy (minFrameDraws), their wall
        // time and the GPU fence waits of the command thread during the window.
        uint64_t heavyFrames = 0, heavyFrameNs = 0, fenceWaitUs = 0;
        bool frameSplit = false;

        void Note(Split split, uint64_t ns)
        {
            if (split == Split::AtTargetChange) ++atTargetChange;
            else if (split == Split::InsidePass) ++insidePass;
            else return;
            splitNs += ns;
            frameSplit = true;
        }
        void EndFrame(bool heavy, uint64_t frameNs)
        {
            ++frames;
            if (frameSplit) ++splitFrames;
            frameSplit = false;
            if (heavy) { ++heavyFrames; heavyFrameNs += frameNs; }
        }
        uint64_t Submits() const { return atTargetChange + insidePass; }
    };
}
