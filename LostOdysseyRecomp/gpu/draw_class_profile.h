#pragma once
// P4: where the ~10 µs per guest draw go. On a sampled frame (one about every
// ten seconds while the Android Tester records with Advanced/Intensive, or
// with LO_PASS_PROFILE=1) the renderer writes a GPU timestamp after every
// guest draw; each draw is charged the time since the previous timestamp
// of its command list (bottom of pipe: when its work retired). The time
// is summed per draw class and per VS/PS pair over the session, and one
// cumulative line goes to the runtime log after each sampled frame.
//
// Caveats, kept in the log line: timestamps between draws can change how a
// tiled GPU schedules that frame (Turnip may render it in sysmem), and
// overlapping draws are charged where they retire. The numbers rank classes
// and shaders; they are not exact per-draw costs.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace gpu::draw_profile
{
enum class DrawClass : uint8_t
{
    DepthOnly,   // no colour written (depth pre-pass, shadow maps, stencil, occlusion boxes)
    Shadow,      // VS 99c2 shadow projection
    ColorOpaque, // colour with depth bound, no blending
    ColorBlend,  // colour with depth bound and blending
    Fullscreen,  // no depth, a full-target quad (post-processing)
    Interface,   // no depth, anything smaller (2D, text)
    Upscaler,    // not a draw: SGSR2/FSR's isolated list between the prefix and the continuation
    Count
};
inline constexpr size_t ClassCount = size_t(DrawClass::Count);

inline const char* ClassName(DrawClass c)
{
    static constexpr const char* names[ClassCount] = {"depth_only", "shadow", "opaque", "blend", "fullscreen", "interface", "upscaler"};
    return size_t(c) < ClassCount ? names[size_t(c)] : "unknown";
}

inline DrawClass Classify(bool depthOnly, bool shadowProjection, bool depthBound, bool blend, bool fullTargetQuad)
{
    if (shadowProjection) return DrawClass::Shadow;
    if (depthOnly) return DrawClass::DepthOnly;
    if (!depthBound) return fullTargetQuad ? DrawClass::Fullscreen : DrawClass::Interface;
    return blend ? DrawClass::ColorBlend : DrawClass::ColorOpaque;
}

struct DrawRecord
{
    uint64_t vs = 0, ps = 0;
    DrawClass drawClass = DrawClass::Interface;
};

// Timestamps per command list: index 0 at the list's start, index i + 1
// after draw i.
inline constexpr uint32_t MaxDrawsPerList = 4096;

class Accumulator
{
public:
    struct Stats { uint64_t ns = 0, draws = 0; };

    // timestamps: draws.size() + 1 values in nanoseconds; an unavailable or
    // backwards value charges nothing to that draw.
    void AddList(const std::vector<DrawRecord>& draws, const uint64_t* timestamps)
    {
        if (!timestamps) return;
        for (size_t i = 0; i < draws.size(); ++i) {
            const uint64_t begin = timestamps[i], end = timestamps[i + 1];
            const bool valid = begin != ~0ull && end != ~0ull && end >= begin && end - begin < 1000000000ull;
            const uint64_t ns = valid ? end - begin : 0;
            auto& cls = classes_[size_t(draws[i].drawClass)];
            cls.ns += ns; ++cls.draws;
            auto& pair = pairs_[Key{draws[i].vs, draws[i].ps}];
            pair.ns += ns; ++pair.draws;
            pair.drawClass = draws[i].drawClass;
            invalid_ += !valid;
        }
    }
    void EndFrame() { ++frames_; }
    uint64_t Frames() const { return frames_; }
    const Stats& Class(DrawClass c) const { return classes_[size_t(c)]; }

    std::string Format(size_t top = 10) const
    {
        const double f = double(std::max<uint64_t>(frames_, 1));
        char buffer[256];
        uint64_t totalNs = 0, totalDraws = 0;
        for (size_t i = 0; i < ClassCount; ++i) {
            totalNs += classes_[i].ns;
            if (DrawClass(i) != DrawClass::Upscaler) totalDraws += classes_[i].draws;
        }
        const uint64_t drawNs = totalNs - classes_[size_t(DrawClass::Upscaler)].ns;
        std::snprintf(buffer, sizeof(buffer), "draw classes (session, timestamped after each draw; indicative): frames=%llu per_frame: draws=%.0f ms=%.2f us_per_draw=%.2f untimed=%llu;",
            static_cast<unsigned long long>(frames_), double(totalDraws) / f, double(totalNs) / 1e6 / f,
            totalDraws ? double(drawNs) / 1e3 / double(totalDraws) : 0.0, static_cast<unsigned long long>(invalid_));
        std::string line = buffer;
        for (size_t i = 0; i < ClassCount; ++i) {
            const auto& c = classes_[i];
            std::snprintf(buffer, sizeof(buffer), " %s=%.2fms/%.0f(%.1fus)", ClassName(DrawClass(i)),
                double(c.ns) / 1e6 / f, double(c.draws) / f, c.draws ? double(c.ns) / 1e3 / double(c.draws) : 0.0);
            line += buffer;
        }
        std::vector<std::pair<Key, PairStats>> sorted(pairs_.begin(), pairs_.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            return a.second.ns != b.second.ns ? a.second.ns > b.second.ns : a.first.vs < b.first.vs;
        });
        line += "; top:";
        for (size_t i = 0; i < sorted.size() && i < top; ++i) {
            const auto& [key, stats] = sorted[i];
            std::snprintf(buffer, sizeof(buffer), " %016llx/%016llx %s %.2fms/%.0f;",
                static_cast<unsigned long long>(key.vs), static_cast<unsigned long long>(key.ps),
                ClassName(stats.drawClass), double(stats.ns) / 1e6 / f, double(stats.draws) / f);
            line += buffer;
        }
        return line;
    }

private:
    struct Key
    {
        uint64_t vs, ps;
        bool operator==(const Key& o) const { return vs == o.vs && ps == o.ps; }
    };
    struct KeyHash { size_t operator()(const Key& k) const { return size_t(k.vs * 0x9e3779b97f4a7c15ull ^ k.ps); } };
    struct PairStats { uint64_t ns = 0, draws = 0; DrawClass drawClass = DrawClass::Interface; };
    std::array<Stats, ClassCount> classes_{};
    std::unordered_map<Key, PairStats, KeyHash> pairs_;
    uint64_t frames_ = 0, invalid_ = 0;
};
}
