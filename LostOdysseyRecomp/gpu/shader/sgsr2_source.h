#pragma once
// Qualcomm Snapdragon Game Super Resolution 2 (SGSR2), 2-pass compute
// variant, adapted from SnapdragonStudios/snapdragon-gsr sgsr/v2
// (see thirdparty/licenses/SnapdragonGSR.txt). HLSL for DXC (cs_6_0, SPIR-V).
//
// Adaptations to the renderer's temporal inputs (temporal_frame_inputs.h):
// - Depth is R32_FLOAT, reversed Z when depthReversed != 0 (0 = far plane):
//   the nearest sample is the maximum, the far-plane test is depth > 1e-5 and
//   the depth-separation tolerance scales with the value instead of 1 - value.
// - Motion arrives as backward, unjittered render-pixel vectors (+X right,
//   +Y down: previous = current + motion) from motion replay and the camera
//   fill, so the convert pass stores SGSR2's NDC difference
//   (current - previous) * 2 instead of decoding a velocity texture or
//   reprojecting with a clip-to-previous-clip matrix.
// - A motion-invalidity value above 0.5 marks the pixel as disoccluded
//   (depth clip 1), so its history is replaced rather than blended.
// - Texture gathers use a point sampler; every read stays inside the image.
// - A reset frame never reads the previous history (it may be uninitialised).
// - A disocclusion test against the previous frame's depth also rejects
//   history where a moving object uncovered the background (the reference
//   has no previous depth); edgeClip can weight the depth-edge clip.
// - SDR input (display-referred, [0, 1]) skips the HDR tone map around the
//   YCoCg packing and the output is clipped to [0, 1]; HDR input keeps the
//   reference tone map with an exact inverse. Non-finite colour reads as 0.
namespace gpu::shader {
inline constexpr const char* kSgsr2Source = R"SGSR2(
//============================================================================================================
//
//
//                  Copyright (c) 2024, Qualcomm Innovation Center, Inc. All rights reserved.
//                              SPDX-License-Identifier: BSD-3-Clause
//
//============================================================================================================
struct Sgsr2Parameters {
    uint2  renderSize;
    uint2  displaySize;
    float2 renderSizeRcp;
    float2 displaySizeRcp;
    float2 jitterOffset;      // render pixels, raster displacement of this frame
    float  preExposure;       // 0: SDR input, no tone map; > 0: HDR (reference tone map)
    float  cameraFovAngleHor; // tan(horizontal fov / 2)
    float  minLerpContribution;
    uint   sameCamera;
    uint   reset;
    uint   depthReversed;
    float  sharpness;         // 0..1 strength of the sharpening pass
    uint   sharpen;           // 1: the upscale leaves the output to sgsr2Sharpen
    float2 previousJitter;    // last frame's jitterOffset
    float  edgeClip;          // weight of the reference's depth-edge clip (1 = reference)
    float  kernelScale;       // 1 = reference; below 1 a wider, softer upsampling kernel
    uint   kernelMode;        // 0: reference (fast Lanczos); 1: Soft, a Gaussian with no negative lobes
    uint   antiTrail;         // 1: history brighter or darker than every current neighbour is not kept (G6)
};
[[vk::push_constant]] ConstantBuffer<Sgsr2Parameters> params;

// YCoCg -> RGB. SDR: Lanczos overshoot past white is clipped here, as it
// would be later. HDR: the inverse tone map, exact (the reference subtracts
// from 1 + 600/65504, about 2% darker); the floor keeps the same fp16 ceiling.
float3 Sgsr2OutputColor(float3 ycocg, float colorMax)
{
    const float xz = ycocg.x - ycocg.z;
    float3 rgb = saturate(float3(xz + ycocg.y, ycocg.x + ycocg.z, xz - ycocg.y));
    if (params.preExposure > 0.0) {
        const float componentMax = max(max(rgb.x, rgb.y), rgb.z);
        float scale = params.preExposure / max(1.0 - componentMax, 600.0 / 65504.0);
        if (colorMax > 4000.0) scale = colorMax;
        rgb *= scale;
    }
    return rgb;
}

#ifdef SGSR2_CONVERT
[[vk::binding(0,0)]] Texture2D<float>  inputDepth : register(t0);
[[vk::binding(1,0)]] Texture2D<float2> inputMotion : register(t1);
[[vk::binding(2,0)]] Texture2D<float4> inputColor : register(t2);
[[vk::binding(3,0)]] Texture2D<float>  inputInvalidity : register(t3);
[[vk::binding(4,0)]] SamplerState pointClamp : register(s0);
[[vk::binding(5,0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> motionDepthClipAlpha : register(u0);
[[vk::binding(6,0)]] [[vk::image_format("r32ui")]]   RWTexture2D<uint>   ycocgColor : register(u1);
[[vk::binding(7,0)]] Texture2D<float>  previousDepth : register(t4);
[[vk::binding(8,0)]] [[vk::image_format("r32f")]]    RWTexture2D<float>  depthHistory : register(u2);
float Farthest(float a, float b) { return params.depthReversed != 0 ? min(a, b) : max(a, b); }

float Nearest(float a, float b) { return params.depthReversed != 0 ? max(a, b) : min(a, b); }
float Nearest4(float4 v) { return Nearest(Nearest(v.x, v.y), Nearest(v.z, v.w)); }

[numthreads(8, 8, 1)]
void sgsr2Convert(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.renderSize.x || id.y >= params.renderSize.y) return;
    const float2 texel = params.renderSizeRcp;
    // Gathers around the corners of this texel: x = (i0,j1), y = (i1,j1),
    // z = (i1,j0), w = (i0,j0) of the 2x2 footprint.
    const float2 gatherCoord = float2(id.xy) * texel;
    const float4 topLeft = inputDepth.GatherRed(pointClamp, gatherCoord);
    const float4 topRight = inputDepth.GatherRed(pointClamp, gatherCoord + float2(texel.x * 2.0, 0.0));
    const float4 bottomLeft = inputDepth.GatherRed(pointClamp, gatherCoord + float2(0.0, texel.y * 2.0));
    const float4 bottomRight = inputDepth.GatherRed(pointClamp, gatherCoord + texel * 2.0);
    const float nearest = Nearest(Nearest(topLeft.y, topRight.x), Nearest(bottomLeft.z, bottomRight.w));
    const float topLeft4 = Nearest4(topLeft);

    float depthClip = 0.0;
    const bool geometry = params.depthReversed != 0 ? nearest > 1.0e-05 : nearest < 1.0 - 1.0e-05;
    if (geometry) {
        const float topRight4 = Nearest4(topRight);
        const float bottomLeft4 = Nearest4(bottomLeft);
        const float bottomRight4 = Nearest4(bottomRight);
        const float Ksep = 1.37e-05;
        const float separation = Ksep * params.cameraFovAngleHor * length(float2(params.renderSize)) *
            (params.depthReversed != 0 ? nearest : 1.0 - nearest);
        const float EPSILON = 1.19e-07;
        float weight = 0.0;
        weight += saturate(separation / (abs(nearest - topLeft4) + EPSILON));
        weight += saturate(separation / (abs(nearest - topRight4) + EPSILON));
        weight += saturate(separation / (abs(nearest - bottomLeft4) + EPSILON));
        weight += saturate(separation / (abs(nearest - bottomRight4) + EPSILON));
        depthClip = saturate(1.0 - weight * 0.25);
    }
    // The reference clips history at every depth edge of the current frame
    // (weighted by edgeClip, 1 by default). On top of it, a disocclusion test
    // the reference has no data for: the surface now at this pixel lies
    // behind everything that was around its reprojected position last frame
    // (where a character stood, whose own motion the camera-only vectors do
    // not carry). Depth is jittered, so the previous position moves by the
    // jitter difference too.
    depthClip *= params.edgeClip;
    const float currentDepth = inputDepth.Load(int3(id.xy, 0));
    depthHistory[id.xy] = currentDepth;
    const float2 renderMotion = inputMotion.Load(int3(id.xy, 0));
    if (params.reset == 0 && geometry && all(isfinite(renderMotion))) {
        const float2 previousPosition = float2(id.xy) + 0.5 + renderMotion + params.previousJitter - params.jitterOffset;
        const float2 previousUv = previousPosition * texel;
        float disocclusion = 1.0; // off-screen last frame
        if (all(previousUv >= 0.0) && all(previousUv <= 1.0)) {
            const float4 around = previousDepth.GatherRed(pointClamp, previousUv);
            const float behind = Farthest(Farthest(around.x, around.y), Farthest(around.z, around.w));
            // Relative depth gap (reverse Z ~ near / z), positive when the
            // current surface is farther: none below 4%, full from 8%.
            const float scale = params.depthReversed != 0 ? max(behind, 1.0e-6) : max(1.0 - behind, 1.0e-6);
            const float gap = (params.depthReversed != 0 ? behind - currentDepth : currentDepth - behind) / scale;
            disocclusion = saturate((gap - 0.04) / 0.04);
        }
        depthClip = max(depthClip, disocclusion);
    }
    if (inputInvalidity.Load(int3(id.xy, 0)) > 0.5) depthClip = 1.0;

    // Backward render-pixel motion -> SGSR2's NDC (current - previous), +Y down.
    // A non-finite vector (never expected) is treated as a disocclusion.
    float2 motion = -2.0 * renderMotion * texel;
    if (any(!isfinite(motion))) { motion = float2(0.0, 0.0); depthClip = 1.0; }

    // YCoCg 11:11:10 packing, as the reference. Display-referred SDR input
    // ([0, 1]) already fits YCoCg's [0, 1] range, so the reference's
    // rgb / (max + preExposure) tone map runs only for HDR input: blending in
    // that per-pixel space biased SDR colours next to fine detail (the
    // lavapipe fixture measured about half the error without it).
    float3 rgb = inputColor.Load(int3(id.xy, 0)).rgb;
    if (any(!isfinite(rgb))) rgb = float3(0.0, 0.0, 0.0);
    float colorMax = 1.0;
    if (params.preExposure > 0.0) {
        rgb = max(rgb, 0.0);
        colorMax = max(max(rgb.x, rgb.y), rgb.z) + params.preExposure;
        rgb /= colorMax;
    } else {
        rgb = saturate(rgb);
    }
    float3 ycocg;
    ycocg.x = 0.25 * (rgb.x + 2.0 * rgb.y + rgb.z);
    ycocg.y = saturate(0.5 * rgb.x + 0.5 - 0.5 * rgb.z);
    ycocg.z = saturate(ycocg.x + ycocg.y - rgb.x);
    const uint x11 = uint(ycocg.x * 2047.5);
    const uint y11 = uint(ycocg.y * 2047.5);
    const uint z10 = uint(ycocg.z * 1023.5);
    ycocgColor[id.xy] = (x11 << 21u) | (y11 << 10u) | z10;
    motionDepthClipAlpha[id.xy] = float4(motion, depthClip, colorMax);
}
#endif

#ifdef SGSR2_UPSCALE
[[vk::binding(0,0)]] Texture2D<float4> previousHistory : register(t0);
[[vk::binding(1,0)]] Texture2D<float4> motionDepthClipAlpha : register(t1);
[[vk::binding(2,0)]] Texture2D<uint>   ycocgColor : register(t2);
[[vk::binding(3,0)]] SamplerState linearClamp : register(s0);
[[vk::binding(4,0)]] SamplerState pointClamp : register(s1);
[[vk::binding(5,0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> sceneColorOutput : register(u0);
[[vk::binding(6,0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> historyOutput : register(u1);

float FastLanczos(float base)
{
    const float y = base - 1.0;
    const float y2 = y * y;
    return (0.75 * y + y2) * y2;
}
float3 DecodeColor(uint sample32)
{
    const uint x11 = sample32 >> 21u;
    const uint y11 = sample32 & (2047u << 10u);
    const uint z10 = sample32 & 1023u;
    return float3(float(x11) * (1.0 / 2047.5), float(y11) * 4.76953602e-7 - 0.5, float(z10) * (1.0 / 1023.5) - 0.5);
}

struct Box {
    float4 upsampled;
    float3 center, variance, minimum, maximum;
    float weight;
};
// Not part of Qualcomm's SGSR2: the Soft filter (Steprant 2.0, WP7.3) swaps
// the Lanczos-shaped weight, whose negative lobe rings around edges, for a
// Gaussian of the same argument with the same peak (0.25), so the summed
// weight that drives the history blend stays comparable. No negative lobe,
// so no halo; with the wider window (kernelScale 0.7) a softer image.
float SoftKernel(float base)
{
    return 0.25 * exp(-4.0 * base);
}
void Accumulate(inout Box box, float3 color, float2 offset, float kernelBias2, float curveBias, bool first)
{
    const float offsetDot = dot(offset, offset);
    const float base = saturate(offsetDot * kernelBias2);
    const float lanczos = params.kernelMode == 1u ? SoftKernel(base) : FastLanczos(base);
    box.upsampled += float4(color * lanczos, lanczos);
    const float boxWeight = exp(offsetDot * curveBias);
    box.minimum = first ? color : min(box.minimum, color);
    box.maximum = first ? color : max(box.maximum, color);
    const float3 weighted = color * boxWeight;
    box.center += weighted;
    box.variance += color * weighted;
    box.weight += boxWeight;
}

[numthreads(8, 8, 1)]
void sgsr2Upscale(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.displaySize.x || id.y >= params.displaySize.y) return;
    const float2 renderSize = float2(params.renderSize);
    const float2 displaySize = float2(params.displaySize);
    const float biasMaxViewportScale = min(displaySize.x / renderSize.x, 1.99);
    const float scaleFactor = min(20.0, pow((displaySize.x / renderSize.x) * (displaySize.y / renderSize.y), 3.0));
    const float2 jitter = params.jitterOffset;

    const float2 outputUv = (float2(id.xy) + 0.5) * params.displaySizeRcp;
    const float2 jitterUv = saturate(outputUv + jitter * params.renderSizeRcp);
    const int2 inputPos = int2(min(jitterUv * renderSize, renderSize - 1.0));

    const float4 mda = motionDepthClipAlpha.SampleLevel(linearClamp, jitterUv, 0);
    const float2 motion = mda.xy;
    const float2 previousUv = saturate(outputUv - 0.5 * motion);
    const float depthFactor = mda.z;
    const float colorMax = mda.w;

    // After a reset the previous history is never read: it may be a fresh,
    // uninitialised image (or a discarded frame's), and NaN * 0 is NaN.
    float4 history = previousHistory.SampleLevel(linearClamp, previousUv, 0);
    if (params.reset != 0 || any(!isfinite(history))) history = float4(0.0, 0.0, 0.0, 0.0);
    float3 historyColor = history.xyz;
    const float wFactor = saturate(abs(history.w));

    const float kernelFactor = saturate(wFactor + float(params.reset));
    const float biasMax = biasMaxViewportScale - biasMaxViewportScale * kernelFactor;
    const float biasMin = max(1.0, 0.3 + 0.3 * biasMax);
    const float biasFactor = max(0.25 * depthFactor, kernelFactor);
    const float kernelBias = lerp(biasMax, biasMin, biasFactor) * 0.5 * params.kernelScale;
    const float kernelBias2 = kernelBias * kernelBias;
    const float motionViewportLength = length(motion * displaySize);
    const float curveBias = lerp(-2.0, -3.0, saturate(motionViewportLength * 0.02));

    const float2 sourceOffset = float2(inputPos) + 0.5 - jitter - outputUv * renderSize;
    const int2 last = int2(params.renderSize) - 1;
    const uint centre = ycocgColor.Load(int3(inputPos, 0));
    const uint left = ycocgColor.Load(int3(max(inputPos - int2(1, 0), 0), 0));
    const uint right = ycocgColor.Load(int3(min(inputPos + int2(1, 0), last), 0));
    const uint above = ycocgColor.Load(int3(max(inputPos - int2(0, 1), 0), 0));
    const uint below = ycocgColor.Load(int3(min(inputPos + int2(0, 1), last), 0));

    Box box = (Box)0;
    Accumulate(box, DecodeColor(below), sourceOffset + float2(0.0, 1.0), kernelBias2, curveBias, true);
    Accumulate(box, DecodeColor(right), sourceOffset + float2(1.0, 0.0), kernelBias2, curveBias, false);
    Accumulate(box, DecodeColor(left), sourceOffset + float2(-1.0, 0.0), kernelBias2, curveBias, false);
    Accumulate(box, DecodeColor(centre), sourceOffset, kernelBias2, curveBias, false);
    Accumulate(box, DecodeColor(above), sourceOffset + float2(0.0, -1.0), kernelBias2, curveBias, false);
    if (params.sameCamera != 0) {
        Accumulate(box, DecodeColor(ycocgColor.Load(int3(min(inputPos + 1, last), 0))),
            sourceOffset + float2(1.0, 1.0), kernelBias2, curveBias, false);
        Accumulate(box, DecodeColor(ycocgColor.Load(int3(clamp(inputPos + int2(-1, 1), 0, last), 0))),
            sourceOffset + float2(-1.0, 1.0), kernelBias2, curveBias, false);
        Accumulate(box, DecodeColor(ycocgColor.Load(int3(clamp(inputPos + int2(1, -1), 0, last), 0))),
            sourceOffset + float2(1.0, -1.0), kernelBias2, curveBias, false);
        Accumulate(box, DecodeColor(ycocgColor.Load(int3(max(inputPos - 1, 0), 0))),
            sourceOffset + float2(-1.0, -1.0), kernelBias2, curveBias, false);
    }

    const float boxWeightRcp = 1.0 / box.weight;
    box.center *= boxWeightRcp;
    box.variance *= boxWeightRcp;
    box.variance = sqrt(abs(box.variance - box.center * box.center));

    float4 upsampled = box.upsampled;
    upsampled.xyz = clamp(upsampled.xyz / upsampled.w, box.minimum - 0.05, box.maximum + 0.05);
    upsampled.w *= 1.0 / 3.0;

    const float oneMinusW = 1.0 - wFactor;
    float baseUpdate = oneMinusW - oneMinusW * depthFactor;
    baseUpdate = min(baseUpdate, lerp(baseUpdate, upsampled.w * 10.0, saturate(10.0 * motionViewportLength)));
    baseUpdate = min(baseUpdate, lerp(baseUpdate, upsampled.w, saturate(motionViewportLength * 0.05)));
    float baseAlpha = baseUpdate;

    const float EPSILON = 1.192e-07;
    const float boxScale = max(depthFactor, saturate(motionViewportLength * 0.05));
    const float boxSize = lerp(scaleFactor, 1.0, boxScale);
    const float3 scaledVariance = box.variance * boxSize;
    const float3 boxMaximum = min(box.maximum, box.center + scaledVariance);
    const float3 boxMinimum = max(box.minimum, box.center - scaledVariance);

    // Not part of Qualcomm's SGSR2 (Steprant 2.0, G6/G4): with a still camera
    // the box above can be several times the neighbourhood's spread, so a
    // particle or alpha-tested edge that moved without motion vectors stays
    // in the history as a trail (white dashed lines on the battle ground).
    // When the history's luma lies outside the range of every current
    // neighbour by more than 0.12, it shows something the frame no longer
    // has: clamp it to the neighbours' own range and let the current frame in.
    float3 trailMinimum = boxMinimum, trailMaximum = boxMaximum;
    bool transient = false;
    if (params.antiTrail != 0u && params.reset == 0u) {
        transient = historyColor.x > box.maximum.x + 0.12 || historyColor.x < box.minimum.x - 0.12;
        if (transient) {
            trailMinimum = box.minimum;
            trailMaximum = box.maximum;
        }
    }
    const float3 clampedColor = clamp(historyColor, trailMinimum, trailMaximum);
    const float startLerp = abs(motion.x) + abs(motion.y) > 0.000001 ? 0.0 : params.minLerpContribution;
    const bool outside = transient || any(boxMinimum > historyColor) || any(historyColor > boxMaximum);
    const float lerpContribution = saturate(outside ? (transient ? 0.0 : startLerp) : 1.0);
    historyColor = lerp(clampedColor, historyColor, lerpContribution);
    baseAlpha = lerp(min(baseAlpha, 0.1), baseAlpha, lerpContribution);

    const float alphaSum = max(EPSILON, baseAlpha + upsampled.w);
    const float alpha = saturate(upsampled.w / alphaSum + float(params.reset));
    upsampled.xyz = lerp(historyColor, upsampled.xyz, alpha);
    historyOutput[id.xy] = float4(upsampled.xyz, wFactor);
    if (params.sharpen == 0) sceneColorOutput[id.xy] = float4(Sgsr2OutputColor(upsampled.xyz, colorMax), 1.0);
}
#endif

#ifdef SGSR2_SHARPEN
// Not part of Qualcomm's SGSR2: a contrast-adaptive sharpen after the
// accumulation, in the spirit of AMD's RCAS. Each pixel gets the strongest
// negative cross lobe that keeps every channel inside the range of its four
// neighbours, so edges gain contrast without halos; one lobe for all three
// channels (no colour fringes), weakened where the cross looks like noise.
[[vk::binding(0,0)]] Texture2D<float4> currentHistory : register(t0);
[[vk::binding(1,0)]] Texture2D<float4> motionDepthClipAlpha : register(t1);
[[vk::binding(2,0)]] SamplerState linearClamp : register(s0);
[[vk::binding(3,0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> sceneColorOutput : register(u0);

float3 Sgsr2HistoryRgb(int2 position)
{
    const float3 ycocg = currentHistory.Load(int3(clamp(position, 0, int2(params.displaySize) - 1), 0)).xyz;
    const float xz = ycocg.x - ycocg.z;
    return saturate(float3(xz + ycocg.y, ycocg.x + ycocg.z, xz - ycocg.y));
}

[numthreads(8, 8, 1)]
void sgsr2Sharpen(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.displaySize.x || id.y >= params.displaySize.y) return;
    const int2 position = int2(id.xy);
    const float3 centre = Sgsr2HistoryRgb(position);
    const float3 north = Sgsr2HistoryRgb(position + int2(0, -1));
    const float3 south = Sgsr2HistoryRgb(position + int2(0, 1));
    const float3 west = Sgsr2HistoryRgb(position + int2(-1, 0));
    const float3 east = Sgsr2HistoryRgb(position + int2(1, 0));
    const float3 lowest = min(min(north, south), min(west, east));
    const float3 highest = max(max(north, south), max(west, east));
    // The lobe w gives c' = (c + w * sum) / (1 + 4w); these are the w at which
    // a channel would leave [0, 1] past its darkest or brightest neighbour.
    const float3 towardsBlack = min(lowest, centre) / (4.0 * highest + 1.0e-5);
    const float3 towardsWhite = (1.0 - max(highest, centre)) / (4.0 * lowest - 4.0 - 1.0e-5);
    const float3 limit = max(-towardsBlack, towardsWhite);
    float lobe = clamp(max(max(limit.x, limit.y), limit.z), -0.1875, 0.0);
    // A centre far from its cross average relative to the local range reads
    // as noise (or fine aliasing): sharpen it half as much.
    const float luma = dot(centre, float3(0.25, 0.5, 0.25));
    const float lumaLow = dot(lowest, float3(0.25, 0.5, 0.25)), lumaHigh = dot(highest, float3(0.25, 0.5, 0.25));
    const float crossMean = dot(north + south + west + east, float3(0.25, 0.5, 0.25)) * 0.25;
    const float range = max(max(lumaHigh, luma) - min(lumaLow, luma), 1.0e-4);
    lobe *= params.sharpness * (1.0 - 0.5 * saturate(abs(crossMean - luma) / range));
    float3 rgb = saturate((centre + lobe * (north + south + west + east)) / (1.0 + 4.0 * lobe));
    if (params.preExposure > 0.0) {
        const float2 uv = (float2(id.xy) + 0.5) * params.displaySizeRcp;
        const float colorMax = motionDepthClipAlpha.SampleLevel(linearClamp, uv, 0).w;
        const float componentMax = max(max(rgb.x, rgb.y), rgb.z);
        float scale = params.preExposure / max(1.0 - componentMax, 600.0 / 65504.0);
        if (colorMax > 4000.0) scale = colorMax;
        rgb *= scale;
    }
    sceneColorOutput[id.xy] = float4(rgb, 1.0);
}
#endif
)SGSR2";
}
