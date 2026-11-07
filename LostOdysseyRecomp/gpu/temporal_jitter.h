#pragma once
#include "temporal_scene.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace gpu::temporal
{
struct JitterSample
{
    uint32_t phase = 0;
    double pixelX = 0, pixelY = 0;
    float ndcX = 0, ndcY = 0;
};

inline JitterSample FrameJitter(uint64_t frame, double width, double height, double scale = 1)
{
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1 || height < 1 ||
        !std::isfinite(scale) || scale < 0 || scale > 1) return {};
    const auto halton = [](uint32_t n, uint32_t base) {
        double value = 0, fraction = 1;
        while (n) { fraction /= base; value += fraction * (n % base); n /= base; }
        return value - .5;
    };
    const uint32_t phase = uint32_t(frame % 32 + 1);
    const double x = halton(phase, 2) * scale, y = halton(phase, 3) * scale;
    // Guest NDC Y is up; the supported host viewport is Y down.
    return {phase, x, y, float(2 * float(x) / width), float(-2 * float(y) / height)};
}

// Classify the guest camera before the independent, host-only lighting depth
// bias is added to ndcOffset.z. A biased layer still uses the same XY camera.
inline bool IsJitterViewport(const Viewport& viewport, uint32_t vte,
    const float* ndcScale, const float* guestNdcOffset)
{
    return viewport.x == 0 && viewport.y == 0 && viewport.width == 1280 && viewport.height == 720 &&
        vte == 0x43f && ndcScale[0] == 1 && ndcScale[1] == 1 && ndcScale[2] == -1 &&
        guestNdcOffset[0] == 0 && guestNdcOffset[1] == 0 && guestNdcOffset[2] == 1;
}

enum class JitterRejection
{
    None, Disabled, UnknownShader, IncompatibleViewport, MissingCamera,
    CameraMismatch, DepthMismatch, InvalidExtent, InvalidConstants, ShadowDepthMismatch,
    // VS 99c2 with a pixel shader whose ray constants are not proven to have
    // the c2/c3/c4 form: left unjittered rather than given a ray the shader
    // would rebuild half a pixel off (shadow acne in blocks of shadow texels).
    ShadowUnverified
};

inline const char* JitterRejectionName(JitterRejection rejection)
{
    static constexpr const char* names[] = {
        "none", "disabled", "unknown_shader", "incompatible_viewport", "missing_camera",
        "camera_mismatch", "depth_mismatch", "invalid_extent", "invalid_constants", "shadow_depth_mismatch",
        "shadow_unverified"};
    const auto index = static_cast<size_t>(rejection);
    return index < std::size(names) ? names[index] : "unknown";
}

// Remembers which known-scene VS/PS pairs were left unjittered, so the renderer
// logs each pair and reason once. Fixed storage: no allocation on a draw, and
// after MaxReports distinct entries every call returns false at once.
class JitterMissLog
{
public:
    static constexpr size_t MaxReports = 64;
    bool Done() const { return count_ >= MaxReports; }
    // True the first time this VS/PS/reason is seen.
    bool First(uint64_t vs, uint64_t ps, JitterRejection rejection)
    {
        if (Done()) return false;
        uint64_t h = (vs ^ (ps * 0x9e3779b97f4a7c15ull) ^ uint64_t(rejection)) * 0xc2b2ae3d27d4eb4full;
        for (size_t i = size_t(h ^ (h >> 29)) & (TableSize - 1);; i = (i + 1) & (TableSize - 1)) {
            auto& entry = table_[i];
            if (!entry.used) {
                entry = {vs, ps, rejection, true};
                ++count_;
                return true;
            }
            if (entry.vs == vs && entry.ps == ps && entry.rejection == rejection) return false;
        }
    }
private:
    // At most half full, so a probe ends quickly at a free slot.
    static constexpr size_t TableSize = 2 * MaxReports;
    struct Entry
    {
        uint64_t vs = 0, ps = 0;
        JitterRejection rejection = JitterRejection::None;
        bool used = false;
    };
    std::array<Entry, TableSize> table_{};
    size_t count_ = 0;
};

inline bool IsSceneDepthSample(uint64_t frame, const SceneResolve* sceneDepth, const SceneResolve* sampledDepth)
{
    return sceneDepth && sampledDepth && sceneDepth->frame == frame && sampledDepth->frame == frame &&
        sceneDepth->ordinal && sceneDepth->ordinal == sampledDepth->ordinal &&
        sceneDepth->address == sampledDepth->address && sceneDepth->format == sampledDepth->format &&
        sceneDepth->fullExtent && sampledDepth->fullExtent &&
        sceneDepth->width == sampledDepth->width && sceneDepth->height == sampledDepth->height;
}

inline bool IsFullSceneDepthFetch(uint32_t fetchSize, uint32_t fetchControl,
    uint32_t resolvedGuestWidth, uint32_t resolvedGuestHeight)
{
    // Match GetTexture's 2D logical size decode. A smaller fetch creates a
    // cropped view with a different normalized UV domain despite sharing the
    // parent resolve's address/frame/ordinal. Other dimensions have no verified
    // screen-depth reconstruction contract for this shadow shader pair.
    return ((fetchControl >> 9) & 3) == 1 &&
        (fetchSize & 0x1fff) + 1 == resolvedGuestWidth &&
        ((fetchSize >> 13) & 0x1fff) + 1 == resolvedGuestHeight;
}

// UE3 shadow projection pixel shaders drawn with VS 99c2: the light attenuation
// pass (d55a), the modulated per-object shadow that darkens characters in
// cutscenes (67b1), the adaptive PCF filter (a195) and their other 4/12/16-tap
// variants. All twelve sample scene depth at i0.xy/i0.w and reconstruct the
// receiver as linearDepth * (ndcX*c2 + ndcY*c3 + c4) + c5 with the same
// component order for c2-c5; this is every known pixel shader with that code.
inline bool IsShadowProjectionPair(uint64_t vs, uint64_t ps)
{
    if (vs != 0x99c2b4b0960a9ccdull) return false;
    switch (ps)
    {
    case 0xd55a20d004031279ull: case 0x67b10ad6a0e3d811ull: case 0xa195c4db25859691ull:
    case 0x04649260f1b96778ull: case 0x3ddddf554d19e6d5ull: case 0x7985cac15bd23af0ull:
    case 0x230fb18cbd76739full: case 0x55d83a2f4612a41bull: case 0x9a2473d303785a5aull:
    case 0xd2b7b151fbad68abull: case 0xaa57d5d3f60cde53ull: case 0xfa9c9d4fe29fc6ceull:
        return true;
    default:
        return false;
    }
}

constexpr uint64_t ShadowProjectionVS = 0x99c2b4b0960a9ccdull;

// What a pixel shader drawn with VS 99c2 reads of its ray constants c2..c5
// (bit k = component k), from xenos::TranslatedShader::lowConstantLanesRead.
// known=false: no analysis (the shader is not translated yet or failed).
struct ShadowRayLanes
{
    uint8_t c2 = 0, c3 = 0, c4 = 0, c5 = 0;
    bool known = false, relative = false;
};

inline ShadowRayLanes ShadowRayLanesFrom(const uint8_t (&lanes)[16], bool relative)
{
    return {uint8_t(lanes[2] & 15), uint8_t(lanes[3] & 15), uint8_t(lanes[4] & 15), uint8_t(lanes[5] & 15),
        true, relative};
}

// How a VS 99c2 draw's PS ray is treated under jitter.
enum class ShadowContract
{
    NotShadow,   // another VS
    Listed,      // one of the twelve reviewed pixel shaders
    Verified,    // unlisted, but reads c2, c3, c4 lanes together and c5, no relative constants
    Unverified,  // unlisted and not proven: the draw stays unjittered
};

inline const char* ShadowContractName(ShadowContract contract)
{
    switch (contract)
    {
    case ShadowContract::NotShadow: return "not_shadow";
    case ShadowContract::Listed: return "listed";
    case ShadowContract::Verified: return "verified";
    case ShadowContract::Unverified: return "unverified";
    }
    return "unknown";
}

// The components of c4 that get the ray correction: those where the shader
// reads c2, c3 and c4 alike (ndcX*c2 + ndcY*c3 + c4 per component). A listed
// shader without analysis (or with none of them seen) keeps all four.
inline uint8_t ShadowCompensationLanes(ShadowContract contract, const ShadowRayLanes* lanes)
{
    if (contract != ShadowContract::Listed && contract != ShadowContract::Verified) return 0;
    const uint8_t read = lanes && lanes->known ? uint8_t(lanes->c2 & lanes->c3 & lanes->c4 & 15) : 0;
    // A reviewed shader is known to use all four; analysis can only narrow it.
    return contract == ShadowContract::Listed && !read ? uint8_t(15) : read;
}

inline ShadowContract ClassifyShadowProjection(uint64_t vs, uint64_t ps, const ShadowRayLanes* lanes)
{
    // Without a pixel shader there is no ray to rebuild.
    if (vs != ShadowProjectionVS || !ps) return ShadowContract::NotShadow;
    if (IsShadowProjectionPair(vs, ps)) return ShadowContract::Listed;
    // Every reviewed shader reads all four c2..c5 for linearDepth *
    // (ndcX*c2 + ndcY*c3 + c4) + c5. An unlisted one qualifies only when the
    // translator saw it read c2, c3 and c4 in the same lanes and c5 too,
    // with no relative constant that could reach them some other way.
    if (lanes && lanes->known && !lanes->relative && lanes->c5 &&
        (lanes->c2 & lanes->c3 & lanes->c4 & 15))
        return ShadowContract::Verified;
    return ShadowContract::Unverified;
}

struct DrawJitter
{
    int slot = -1;
    JitterSample sample{};
    JitterRejection rejection = JitterRejection::Disabled;
    bool applied = false, shadowCompensated = false;
    ShadowContract shadow = ShadowContract::NotShadow;
    uint8_t shadowLanes = 0; // components of c4 corrected
};

// Reviewed e810/fe31 uses a 1x1 RGBA guest upload at texture0. A resolve can
// return a scaled/cropped host image, including an old-frame address alias;
// exclude it before treating projected UV movement as a constant sample.
inline bool IsSingleTexelScreenFetch(uint32_t typePitch, uint32_t addressFormat,
    uint32_t size, uint32_t control, bool hasResolvedSurface) {
    // Repeat U/V makes the single texel constant even at the screen edge;
    // border-address modes can vary with projected coordinates and stay held.
    return (typePitch & 3) == 2 && (typePitch & ((7u << 10) | (7u << 13))) == 0 &&
        (addressFormat & 0x3f) == 6 &&
        (addressFormat >> 12) != 0 && (size & 0x03ffffffu) == 0 &&
        ((control >> 9) & 3) == 1 && !hasResolvedSurface;
}

// Modify temporary upload copies only. The unmodified guest VP remains the
// camera identity, and the PS adjustment is authorized by an exact shader pair.
inline DrawJitter ApplyDrawJitter(uint64_t vs, uint64_t ps, uint64_t frame,
    bool enabled, bool compatibleViewport, const SceneAnchor* anchor,
    uint64_t depthAllocation, const Viewport& rasterViewport,
    uint32_t* vsConstants, uint32_t* psConstants,
    const SceneResolve* sceneDepth = nullptr, const SceneResolve* sampledDepth = nullptr,
    double jitterScale = 1, const JitterSample* frameSample = nullptr,
    bool constantScreenSample = false, const ShadowRayLanes* shadowRayLanes = nullptr)
{
    DrawJitter result;
    result.slot = DrawPositionVPSlot(vs, ps, constantScreenSample);
    result.shadow = ClassifyShadowProjection(vs, ps, shadowRayLanes);
    if (!enabled) return result;
    const auto reject = [&](JitterRejection why) { result.rejection = why; return result; };
    if (result.slot < 0) return reject(JitterRejection::UnknownShader);
    if (!compatibleViewport) return reject(JitterRejection::IncompatibleViewport);
    if (!anchor) return reject(JitterRejection::MissingCamera);
    auto* vp = vsConstants + result.slot * 4;
    if (!std::equal(anchor->vpBits.begin(), anchor->vpBits.end(), vp))
        return reject(JitterRejection::CameraMismatch);
    if (depthAllocation && anchor->depthAllocation != depthAllocation)
        return reject(JitterRejection::DepthMismatch);
    if (rasterViewport.x != anchor->viewport.x || rasterViewport.y != anchor->viewport.y ||
        rasterViewport.width != anchor->viewport.width || rasterViewport.height != anchor->viewport.height)
        return reject(JitterRejection::IncompatibleViewport);
    result.sample = frameSample ? *frameSample : FrameJitter(frame, rasterViewport.width, rasterViewport.height, jitterScale);
    if (!result.sample.phase) return reject(JitterRejection::InvalidExtent);
    if (result.shadow == ShadowContract::Unverified)
        return reject(JitterRejection::ShadowUnverified);
    const uint8_t shadowLanes = ShadowCompensationLanes(result.shadow, shadowRayLanes);
    const bool shadow = shadowLanes != 0;
    if (shadow && !IsSceneDepthSample(frame, sceneDepth, sampledDepth))
        return reject(JitterRejection::ShadowDepthMismatch);
    for (unsigned i = 0; i < 16; ++i)
        if (!std::isfinite(std::bit_cast<float>(vp[i]))) return reject(JitterRejection::InvalidConstants);
    // Only the components that enter the correction must be finite: a NaN
    // in a lane the shader never reads cannot reach its output.
    if (shadow)
        for (unsigned component = 0; component < 4; ++component)
            if ((shadowLanes >> component) & 1)
                for (unsigned row = 2; row <= 4; ++row)
                    if (!std::isfinite(std::bit_cast<float>(psConstants[row * 4 + component])))
                        return reject(JitterRejection::InvalidConstants);

    for (unsigned row = 0; row < 4; ++row)
    {
        auto* values = vp + row * 4;
        const float w = std::bit_cast<float>(values[3]);
        values[0] = std::bit_cast<uint32_t>(std::bit_cast<float>(values[0]) + result.sample.ndcX * w);
        values[1] = std::bit_cast<uint32_t>(std::bit_cast<float>(values[1]) + result.sample.ndcY * w);
    }
    if (shadow)
    {
        // VS 99c2 passes its jittered clip position to both oPos and o0. The PS
        // samples scene depth at i0.xy/i0.w, then reconstructs (before swizzling):
        // linearDepth * (ndcX*c2 + ndcY*c3 + c4) + c5.
        // Keep the jittered depth lookup, but remove the offset from that ray.
        // c0/c1 (depth UV/linearization), c2/c3 and c5 stay byte-for-byte intact.
        for (unsigned component = 0; component < 4; ++component)
        {
            if (!((shadowLanes >> component) & 1)) continue;
            const float x = std::bit_cast<float>(psConstants[2 * 4 + component]);
            const float y = std::bit_cast<float>(psConstants[3 * 4 + component]);
            const float origin = std::bit_cast<float>(psConstants[4 * 4 + component]);
            psConstants[4 * 4 + component] = std::bit_cast<uint32_t>(
                origin - result.sample.ndcX * x - result.sample.ndcY * y);
        }
        result.shadowCompensated = true;
        result.shadowLanes = shadowLanes;
    }
    result.applied = true;
    result.rejection = JitterRejection::None;
    return result;
}
}
