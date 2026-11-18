#pragma once

namespace gpu::android_fg
{
// SGSR2's camera/depth motion seeds a local search in the clean scene images.
// Photo and depth agreement qualify the prediction, including independently
// moving objects when the camera vector alone would put them in the wrong place.
inline constexpr const char* kShader = R"HLSL(
Texture2D<float4> finalColor : register(t0);
Texture2D<float4> currentScene : register(t1);
Texture2D<float4> previousScene : register(t2);
Texture2D<float2> cameraMotion : register(t3);
Texture2D<float> currentDepth : register(t4);
Texture2D<float> previousDepth : register(t5);
SamplerState linearClamp : register(s6);
[[vk::image_format("rgba16f")]] RWTexture2D<float4> field : register(u7);
[[vk::image_format("rgba8")]] RWTexture2D<float4> generated : register(u8);

struct Arguments {
    uint2 outputSize;
    uint2 fieldSize;
    uint2 renderSize;
    uint reversedDepth;
    float prediction;
    float2 currentJitter;
    float2 previousJitter;
};
[[vk::push_constant]] ConstantBuffer<Arguments> args;

int3 GeometryPixel(float2 uv, float2 jitter) {
    return int3(clamp(uv * float2(args.renderSize) + jitter, 0.0, float2(args.renderSize) - 1.0), 0);
}
bool InBounds(float2 uv) {
    return all(uv >= 0.0) && all(uv < 1.0);
}
float NearDepth(float depth) {
    return args.reversedDepth ? depth : 1.0 - depth;
}
bool ValidDepth(float depth) {
    return depth > 0.0 && depth <= 1.0;
}
bool DifferentDepth(float a, float b) {
    return abs(a - b) > 0.0002 + max(a, b) * 0.08;
}
float MaxComponent(float3 value) {
    return max(value.r, max(value.g, value.b));
}

struct Patch {
    float3 center, left, right, top, bottom;
};
Patch CurrentPatch(float2 uv, float2 dx, float2 dy) {
    Patch patch;
    patch.center = currentScene.SampleLevel(linearClamp, uv, 0).rgb;
    patch.left = currentScene.SampleLevel(linearClamp, uv - dx, 0).rgb;
    patch.right = currentScene.SampleLevel(linearClamp, uv + dx, 0).rgb;
    patch.top = currentScene.SampleLevel(linearClamp, uv - dy, 0).rgb;
    patch.bottom = currentScene.SampleLevel(linearClamp, uv + dy, 0).rgb;
    return patch;
}
float PatchError(Patch patch, float2 uv, float2 dx, float2 dy, float depth) {
    if (!InBounds(uv)) return 1.0;
    float oldDepth = NearDepth(previousDepth.Load(GeometryPixel(uv, args.previousJitter)));
    if (!ValidDepth(oldDepth) || DifferentDepth(depth, oldDepth)) return 1.0;
    float center = MaxComponent(abs(patch.center - previousScene.SampleLevel(linearClamp, uv, 0).rgb));
    float edges = MaxComponent(abs(patch.left - previousScene.SampleLevel(linearClamp, uv - dx, 0).rgb)) +
        MaxComponent(abs(patch.right - previousScene.SampleLevel(linearClamp, uv + dx, 0).rgb)) +
        MaxComponent(abs(patch.top - previousScene.SampleLevel(linearClamp, uv - dy, 0).rgb)) +
        MaxComponent(abs(patch.bottom - previousScene.SampleLevel(linearClamp, uv + dy, 0).rgb));
    return (2.0 * center + edges) / 6.0;
}

[numthreads(8, 8, 1)]
void estimateMotion(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= args.fieldSize)) return;
    field[id.xy] = 0.0;
    float2 uv = (float2(id.xy) + 0.5) / float2(args.fieldSize);
    int3 pixel = GeometryPixel(uv, args.currentJitter);
    float depth = NearDepth(currentDepth.Load(pixel));
    if (!ValidDepth(depth)) return;

    float2 invSize = 1.0 / float2(args.outputSize);
    float2 dx = float2(3.0 * invSize.x, 0.0);
    float2 dy = float2(0.0, 3.0 * invSize.y);
    Patch patch = CurrentPatch(uv, dx, dy);
    float zeroError = PatchError(patch, uv, dx, dy, depth);
    // No measurable change. Avoid finding motion in texture noise.
    if (zeroError < 0.012) return;
    float2 camera = cameraMotion.Load(pixel) * float2(args.outputSize) / float2(args.renderSize);
    if (!all(isfinite(camera)) || any(abs(camera) > 96.0)) camera = 0.0;
    float2 best = 0.0;
    float bestError = zeroError;
    if (any(abs(camera) > 0.25)) {
        float error = PatchError(patch, uv + camera * invSize, dx, dy, depth);
        if (error < bestError) {
            best = camera;
            bestError = error;
        }
    }

    static const float2 directions[] = {
        float2(-1, 0), float2(1, 0), float2(0, -1), float2(0, 1),
        float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1)
    };
    static const float steps[] = {16.0, 6.0, 2.0, 0.75};
    [unroll]
    for (uint level = 0; level < 4; ++level) {
        if (bestError < 0.012) break;
        float2 center = best;
        [unroll]
        for (uint direction = 0; direction < (level < 2 ? 8 : 4); ++direction) {
            float2 candidate = center + directions[direction] * steps[level];
            if (any(abs(candidate) > 96.0)) continue;
            float error = PatchError(patch, uv + candidate * invSize, dx, dy, depth);
            if (error < bestError) {
                best = candidate;
                bestError = error;
            }
        }
    }
    if (bestError < 0.07 && bestError + 0.012 < zeroError * 0.75 && any(abs(best) > 0.25))
        field[id.xy] = float4(best, depth, 1.0);
}

[numthreads(8, 8, 1)]
void predict(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= args.outputSize)) return;
    int3 pixel = int3(id.xy, 0);
    float4 real = finalColor.Load(pixel);
    float2 uv = (float2(id.xy) + 0.5) / float2(args.outputSize);
    float4 flow = field[uint2(uv * float2(args.fieldSize))];
    if (flow.w > 0.0) {
        // Keep the current HUD in place. Only sample colour from the pre-UI scene.
        float3 scene = currentScene.Load(pixel).rgb;
        if (MaxComponent(abs(real.rgb - scene)) <= 3.0 / 255.0) {
            float2 source = uv + args.prediction * flow.xy / float2(args.outputSize);
            if (InBounds(source)) {
                float depth = NearDepth(currentDepth.Load(GeometryPixel(source, args.currentJitter)));
                float here = NearDepth(currentDepth.Load(GeometryPixel(uv, args.currentJitter)));
                // Never smear a nearby silhouette with a prediction from behind it.
                if (ValidDepth(depth) && !DifferentDepth(flow.z, depth) &&
                    (here <= depth || !DifferentDepth(here, depth)))
                    real.rgb = currentScene.SampleLevel(linearClamp, source, 0).rgb;
            }
        }
    }
    generated[id.xy] = real;
}
)HLSL";
}
