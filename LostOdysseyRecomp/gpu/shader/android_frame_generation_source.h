#pragma once

namespace gpu::android_fg
{
// Motion is in render pixels and points from the current scene to the previous
// one. Reconstruct the halfway scene from both clean SGSR2 images. The compact
// scatter field resolves overlapping geometry before the full-size color pass.
inline constexpr const char* kShader = R"HLSL(
Texture2D<float4> finalColor : register(t0);
Texture2D<float4> currentScene : register(t1);
Texture2D<float4> previousScene : register(t2);
Texture2D<float2> motion : register(t3);
Texture2D<float> currentDepth : register(t4);
Texture2D<float> previousDepth : register(t5);
Texture2D<float> invalidity : register(t6);
SamplerState linearClamp : register(s7);
[[vk::image_format("r32ui")]] RWTexture2D<uint> visibility : register(u8);
[[vk::image_format("rgba8")]] RWTexture2D<float4> generated : register(u9);

struct Arguments {
    uint2 outputSize;
    uint2 fieldSize;
    uint2 renderSize;
    uint reversedDepth;
    uint reserved;
    float2 currentJitter;
    float2 previousJitter;
};
[[vk::push_constant]] ConstantBuffer<Arguments> args;

// At most 1920 x 720 locations. Zero is reserved for an uncovered pixel.
static const uint indexBits = 21;
static const uint indexMask = (1u << indexBits) - 1u;

int3 GeometryPixel(float2 uv, float2 jitter) {
    return int3(clamp(uv * float2(args.renderSize) + jitter, 0.0, float2(args.renderSize) - 1.0), 0);
}

bool InBounds(float2 uv) {
    return all(uv >= 0.0) && all(uv < 1.0);
}

float NearDepth(float depth) {
    return args.reversedDepth ? depth : 1.0 - depth;
}

bool ReliableMotion(float2 uv, out float2 backward) {
    int3 pixel = GeometryPixel(uv, args.currentJitter);
    backward = motion.Load(pixel) / float2(args.renderSize);
    return invalidity.Load(pixel) < 0.5 && all(isfinite(backward)) &&
        all(abs(backward) <= 0.25);
}

[numthreads(8, 8, 1)]
void clearVisibility(uint3 id : SV_DispatchThreadID) {
    if (all(id.xy < args.fieldSize)) visibility[id.xy] = 0;
}

[numthreads(8, 8, 1)]
void scatterVisibility(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= args.fieldSize)) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(args.fieldSize);
    float2 backward;
    if (!ReliableMotion(uv, backward) || all(abs(backward) * float2(args.fieldSize) < 0.5)) return;
    float2 halfway = uv + 0.5 * backward;
    float depth = NearDepth(currentDepth.Load(GeometryPixel(uv, args.currentJitter)));
    if (!InBounds(halfway) || !(depth > 0.0 && depth <= 1.0)) return;
    // Positive IEEE float bits preserve depth order including tiny reversed-Z
    // values. The upper 11 bits hold that priority; nearer surfaces win.
    uint key = ((asuint(depth) >> 19) << indexBits) | (id.y * args.fieldSize.x + id.x + 1u);
    InterlockedMax(visibility[uint2(halfway * float2(args.fieldSize))], key);
}

float MaxComponent(float3 value) {
    return max(value.r, max(value.g, value.b));
}

bool DifferentDepth(float a, float b) {
    return abs(a - b) > 0.0002 + max(abs(a), abs(b)) * 0.08;
}

float4 ReconstructPixel(uint2 pixel) {
    float2 uv = (float2(pixel) + 0.5) / float2(args.outputSize);
    float4 real = finalColor.Load(int3(pixel, 0));
    uint hit = visibility[uint2(uv * float2(args.fieldSize))] & indexMask;
    float2 anchor = uv;
    if (hit != 0) {
        uint index = hit - 1u;
        anchor = (float2(index % args.fieldSize.x, index / args.fieldSize.x) + 0.5) / float2(args.fieldSize);
    }
    float2 backward;
    if (!ReliableMotion(anchor, backward) || all(abs(backward) * float2(args.outputSize) < 0.25)) return real;
    float2 currentUv = uv - 0.5 * backward;
    float2 previousUv = uv + 0.5 * backward;
    if (!InBounds(currentUv) || !InBounds(previousUv)) return real;

    // Preserve present-frame UI and fades at their original coordinates.
    // Warped samples come only from the scene before guest UI was drawn.
    if (MaxComponent(abs(real.rgb - currentScene.Load(int3(pixel, 0)).rgb)) > 3.0 / 255.0) return real;
    float depth = NearDepth(currentDepth.Load(GeometryPixel(currentUv, args.currentJitter)));
    float anchorDepth = NearDepth(currentDepth.Load(GeometryPixel(anchor, args.currentJitter)));
    if (!(depth > 0.0)) return real;
    if (DifferentDepth(depth, anchorDepth)) {
        // A foreground surface that has moved away from this midpoint can
        // expose background that was still visible in the earlier image.
        float exposed = NearDepth(previousDepth.Load(GeometryPixel(uv, args.previousJitter)));
        if (hit == 0 && exposed > 0.0 && exposed < anchorDepth && !DifferentDepth(exposed, depth))
            return float4(previousScene.Load(int3(pixel, 0)).rgb, real.a);
        return real;
    }
    // A stationary foreground layer can occlude a moving surface in the field.
    float targetDepth = NearDepth(currentDepth.Load(GeometryPixel(uv, args.currentJitter)));
    if (targetDepth > depth && DifferentDepth(targetDepth, depth)) {
        float2 targetMotion;
        if (!ReliableMotion(uv, targetMotion) || all(abs(targetMotion) * float2(args.fieldSize) < 0.5))
            return real;
    }

    float3 newColor = currentScene.SampleLevel(linearClamp, currentUv, 0).rgb;
    float oldDepth = NearDepth(previousDepth.Load(GeometryPixel(previousUv, args.previousJitter)));
    float3 oldColor = previousScene.SampleLevel(linearClamp, previousUv, 0).rgb;
    // If a ray was hidden in one input, or the material changed too much to
    // trust its history, keep the visible current sample instead of blending.
    if (oldDepth > 0.0 && !DifferentDepth(depth, oldDepth) &&
        MaxComponent(abs(oldColor - newColor)) < 0.20)
        newColor = 0.5 * (newColor + oldColor);
    return float4(newColor, real.a);
}
[numthreads(8, 8, 1)]
void reconstruct(uint3 id : SV_DispatchThreadID) {
    if (all(id.xy < args.outputSize)) generated[id.xy] = ReconstructPixel(id.xy);
}
)HLSL";
}
