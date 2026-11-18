#include "os/platform.h"

#if LO_PLATFORM_ANDROID && defined(LO_GPU_PLUME)
#include "android_frame_generation.h"
#include "shader/android_frame_generation_source.h"
#include "shader/dxc_compiler.h"
#include <os/logger.h>
#include <plume_render_interface_builders.h>
#include <utility>

namespace gpu::android_fg
{
using namespace plume;

namespace
{
struct Arguments {
    uint32_t output[2], field[2], render[2], reversedDepth, reserved;
    float currentJitter[2], previousJitter[2];
};
static_assert(sizeof(Arguments) == 48);

constexpr uint32_t kGroup = 8;
constexpr uint32_t Groups(uint32_t size) { return (size + kGroup - 1) / kGroup; }
}

bool Generator::CreatePipelines(RenderDevice& device)
{
    RenderDescriptorSetBuilder bindings;
    bindings.begin();
    for (uint32_t index = 0; index < 7; ++index) bindings.addTexture(index);
    bindings.addSampler(7);
    bindings.addReadWriteTexture(8);
    bindings.addReadWriteTexture(9);
    bindings.end();
    RenderPipelineLayoutBuilder builder;
    builder.begin(false, false);
    builder.addPushConstant(0, 0, sizeof(Arguments), RenderShaderStageFlag::COMPUTE);
    builder.addDescriptorSet(bindings);
    builder.end();
    layout = builder.create(&device);
    descriptors = bindings.create(&device);
    RenderSamplerDesc samplerDesc;
    samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
    samplerDesc.mipmapMode = RenderMipmapMode::NEAREST;
    sampler = device.createSampler(samplerDesc);
    if (!layout || !descriptors || !sampler) return false;
    descriptors->setSampler(7, sampler.get());

    constexpr std::array entries{"clearVisibility", "scatterVisibility", "reconstruct"};
    for (size_t index = 0; index < entries.size(); ++index) {
        const char* entry = entries[index];
        const auto code = xenos::CompileCachedHlsl(kShader, entry, "cs_6_0", xenos::ShaderBinaryFormat::Spirv);
        if (!code.ok) {
            LOG_WARNING("Android FG: {} shader compilation failed: {}", entry, code.errors);
            return false;
        }
        auto shader = device.createShader(code.bytecode.data(), code.bytecode.size(), entry, RenderShaderFormat::SPIRV);
        if (!shader) return false;
        pipelines[index] = device.createComputePipeline(
            RenderComputePipelineDesc(layout.get(), shader.get(), kGroup, kGroup, 1));
        if (!pipelines[index]) return false;
    }
    return true;
}

bool Generator::EnsureImages(RenderDevice& device, resolution::Size extent, resolution::Size render)
{
    const auto storage = RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS;
    // Cap the scatter bandwidth. The resolve still samples both color images
    // at full output resolution and keeps the original UI in those pixels.
    const uint32_t height = std::min(render.height, 720u);
    const uint32_t width = std::clamp(uint32_t(uint64_t(render.width) * height / render.height), 1u, 1920u);
    const resolution::Size bounded{width, height};
    if (!field || fieldSize != bounded) {
        field = device.createTexture(RenderTextureDesc::Texture2D(width, height, 1, RenderFormat::R32_UINT, storage));
        fieldSize = bounded;
    }
    if (!output || outputSize != extent) {
        output = device.createTexture(RenderTextureDesc::Texture2D(extent.width, extent.height, 1,
            RenderFormat::R8G8B8A8_UNORM, storage));
        outputSize = extent;
    }
    return field && output;
}

bool Generator::Prepare(RenderDevice& device, const frame_generation::CompositeHandoff& capture,
    resolution::Size extent, Clock::time_point now)
{
    auto previous = std::exchange(last, Sample{});
    if (failed || !capture.ReadyForOrderedSubmission() ||
        capture.producer->purpose != frame_generation::SnapshotPurpose::AndroidInterpolation ||
        capture.plan.consumer != upscaling::TemporalConsumer::Sgsr2Sr ||
        capture.producer->inputs.colorEncoding != temporal::ColorEncoding::Sdr ||
        capture.producer->sceneColorCandidate.region.allocation != extent)
        return false;
    last = {capture.producer, now};
    const auto& current = last.images->inputs;
    if (current.resetHistory || !previous.images || now - previous.time < std::chrono::milliseconds(20) ||
        now - previous.time > std::chrono::milliseconds(46))
        return false;
    const auto& prior = previous.images->inputs;
    if (prior.renderFrameId + 1 != current.renderFrameId || prior.temporalEpoch != current.temporalEpoch ||
        prior.depthConvention != current.depthConvention || previous.images->lineageCanceled ||
        !dlss_fg::SameHistoryConfiguration(prior.plan, current.plan))
        return false;
    if (!pipelines.back() && !CreatePipelines(device)) {
        failed = true;
        LOG_WARNING("Android FG: Vulkan pipeline unavailable");
        return false;
    }
    if (!EnsureImages(device, extent, last.images->qualifiedInputExtent)) {
        failed = true;
        LOG_WARNING("Android FG: interpolation images unavailable");
        return false;
    }
    currentInput = last.images;
    previousInput = std::move(previous.images);
    return true;
}

RenderTexture* Generator::Record(RenderCommandList& commands, RenderTexture* original)
{
    const auto& current = *currentInput;
    const auto& previous = *previousInput;
    const std::array sources{original, current.sceneColorCandidate.region.texture,
        previous.sceneColorCandidate.region.texture, current.motion.region.texture,
        current.depth.region.texture, previous.depth.region.texture, current.motionInvalidity.region.texture};
    for (uint32_t index = 0; index < sources.size(); ++index)
        descriptors->setTexture(index, sources[index], RenderTextureLayout::SHADER_READ);
    descriptors->setTexture(8, field.get(), RenderTextureLayout::GENERAL);
    descriptors->setTexture(9, output.get(), RenderTextureLayout::GENERAL);
    commands.barriers(RenderBarrierStage::COMPUTE, {
        RenderTextureBarrier(original, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(field.get(), RenderTextureLayout::GENERAL),
        RenderTextureBarrier(output.get(), RenderTextureLayout::GENERAL)});
    const Arguments args{{outputSize.width, outputSize.height}, {fieldSize.width, fieldSize.height},
        {current.qualifiedInputExtent.width, current.qualifiedInputExtent.height},
        current.inputs.depthConvention == temporal::DepthConvention::Reversed ? 1u : 0u, 0u,
        {float(current.inputs.jitter.pixelX), float(current.inputs.jitter.pixelY)},
        {float(previous.inputs.jitter.pixelX), float(previous.inputs.jitter.pixelY)}};

    commands.setComputePipelineLayout(layout.get());
    commands.setComputePushConstants(0, &args);
    commands.setComputeDescriptorSet(descriptors.get(), 0);
    for (size_t index = 0; index < pipelines.size(); ++index) {
        commands.setPipeline(pipelines[index].get());
        const auto extent = index < 2 ? fieldSize : outputSize;
        commands.dispatch(Groups(extent.width), Groups(extent.height), 1);
        if (index < 2)
            commands.barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(field.get(), RenderTextureLayout::GENERAL));
    }
    commands.barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(output.get(), RenderTextureLayout::SHADER_READ));
    commands.barriers(RenderBarrierStage::COPY, RenderTextureBarrier(original, RenderTextureLayout::COPY_SOURCE));
    return output.get();
}
}
#endif
