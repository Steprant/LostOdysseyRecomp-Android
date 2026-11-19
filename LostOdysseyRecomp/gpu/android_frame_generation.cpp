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
    uint32_t output[2], field[2], render[2], reversedDepth;
    float prediction, currentJitter[2], previousJitter[2];
};
static_assert(sizeof(Arguments) == 48);

// One motion estimate per small patch of the output. The reconstruction
// still writes native display pixels, including the unwarped guest HUD.
constexpr uint32_t kCell = 12;
constexpr uint32_t kGroup = 8;
constexpr uint32_t Groups(uint32_t size) { return (size + kGroup - 1) / kGroup; }
}

Generator::Programs Generator::CreatePipelines(RenderDevice& device)
{
    Programs result;
    RenderDescriptorSetBuilder bindings;
    bindings.begin();
    for (uint32_t index = 0; index < 6; ++index) bindings.addTexture(index);
    bindings.addSampler(6);
    bindings.addReadWriteTexture(7);
    bindings.addReadWriteTexture(8);
    bindings.end();
    RenderPipelineLayoutBuilder builder;
    builder.begin(false, false);
    builder.addPushConstant(0, 0, sizeof(Arguments), RenderShaderStageFlag::COMPUTE);
    builder.addDescriptorSet(bindings);
    builder.end();
    result.layout = builder.create(&device);
    result.descriptors = bindings.create(&device);
    RenderSamplerDesc samplerDesc;
    samplerDesc.addressU = samplerDesc.addressV = samplerDesc.addressW = RenderTextureAddressMode::CLAMP;
    samplerDesc.mipmapMode = RenderMipmapMode::NEAREST;
    result.sampler = device.createSampler(samplerDesc);
    if (!result.layout || !result.descriptors || !result.sampler) return {};
    result.descriptors->setSampler(6, result.sampler.get());

    constexpr std::array entries{"estimateMotion", "predict"};
    for (size_t index = 0; index < entries.size(); ++index) {
        const char* entry = entries[index];
        const auto code = xenos::CompileCachedHlsl(kShader, entry, "cs_6_0", xenos::ShaderBinaryFormat::Spirv);
        if (!code.ok) {
            LOG_WARNING("Android FG: {} shader compilation failed: {}", entry, code.errors);
            return {};
        }
        auto shader = device.createShader(code.bytecode.data(), code.bytecode.size(), entry, RenderShaderFormat::SPIRV);
        if (!shader) return {};
        result.pipelines[index] = device.createComputePipeline(
            RenderComputePipelineDesc(result.layout.get(), shader.get(), kGroup, kGroup, 1));
        if (!result.pipelines[index]) return {};
    }
    return result;
}

bool Generator::Warming() const
{
    return warming.valid() && warming.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

bool Generator::Warmup(RenderDevice& device)
{
    if (failed) return false;
    if (programs.pipelines.back()) return true;
    if (Warming()) return false;
    try {
        if (!warming.valid()) {
            // The device outlives Generator and any unfinished warmup.
            warming = std::async(std::launch::async, [&device] { return CreatePipelines(device); });
            return false;
        }
        programs = warming.get();
        if (programs.pipelines.back()) return true;
    } catch (const std::exception& error) {
        LOG_WARNING("Android FG: pipeline preparation failed: {}", error.what());
    }
    failed = true;
    LOG_WARNING("Android FG: Vulkan pipeline unavailable");
    return false;
}

bool Generator::EnsureImages(RenderDevice& device, resolution::Size extent)
{
    if (output && field && outputSize == extent) return true;
    const auto storage = RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS;
    fieldSize = {(extent.width + kCell - 1) / kCell, (extent.height + kCell - 1) / kCell};
    field = device.createTexture(RenderTextureDesc::Texture2D(fieldSize.width, fieldSize.height, 1,
        RenderFormat::R16G16B16A16_FLOAT, storage));
    output = device.createTexture(RenderTextureDesc::Texture2D(extent.width, extent.height, 1,
        RenderFormat::R8G8B8A8_UNORM, storage));
    outputSize = extent;
    return field && output;
}

bool Generator::Prepare(RenderDevice& device, const frame_generation::CompositeHandoff& capture,
    resolution::Size extent, Clock::time_point now)
{
    auto previous = std::exchange(last, Sample{});
    if (failed || !programs.pipelines.back() || !capture.ReadyForOrderedSubmission() ||
        capture.producer->purpose != frame_generation::SnapshotPurpose::AndroidPrediction ||
        capture.plan.consumer != upscaling::TemporalConsumer::Sgsr2Sr ||
        capture.producer->inputs.colorEncoding != temporal::ColorEncoding::Sdr ||
        capture.producer->sceneColorCandidate.region.allocation != extent)
        return false;
    last = {capture.producer, now};
    const auto& current = last.images->inputs;
    const auto delta = now - previous.time;
    if (current.resetHistory || !previous.images || delta < std::chrono::milliseconds(23) ||
        delta > std::chrono::milliseconds(40))
        return false;
    const auto& prior = previous.images->inputs;
    if (prior.renderFrameId + 1 != current.renderFrameId || prior.temporalEpoch != current.temporalEpoch ||
        prior.depthConvention != current.depthConvention || previous.images->lineageCanceled ||
        !dlss_fg::SameHistoryConfiguration(prior.plan, current.plan))
        return false;
    if (!EnsureImages(device, extent)) {
        failed = true;
        LOG_WARNING("Android FG: prediction images unavailable");
        return false;
    }
    prediction = std::clamp(std::chrono::duration<float>(kDisplayPeriod) / std::chrono::duration<float>(delta),
        0.4f, 0.6f);
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
        current.depth.region.texture, previous.depth.region.texture};
    for (uint32_t index = 0; index < sources.size(); ++index)
        programs.descriptors->setTexture(index, sources[index], RenderTextureLayout::SHADER_READ);
    programs.descriptors->setTexture(7, field.get(), RenderTextureLayout::GENERAL);
    programs.descriptors->setTexture(8, output.get(), RenderTextureLayout::GENERAL);
    commands.barriers(RenderBarrierStage::COMPUTE, {
        RenderTextureBarrier(original, RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(field.get(), RenderTextureLayout::GENERAL),
        RenderTextureBarrier(output.get(), RenderTextureLayout::GENERAL)});
    const Arguments args{{outputSize.width, outputSize.height}, {fieldSize.width, fieldSize.height},
        {current.qualifiedInputExtent.width, current.qualifiedInputExtent.height},
        current.inputs.depthConvention == temporal::DepthConvention::Reversed ? 1u : 0u, prediction,
        {float(current.inputs.jitter.pixelX), float(current.inputs.jitter.pixelY)},
        {float(previous.inputs.jitter.pixelX), float(previous.inputs.jitter.pixelY)}};

    commands.setComputePipelineLayout(programs.layout.get());
    commands.setComputePushConstants(0, &args);
    commands.setComputeDescriptorSet(programs.descriptors.get(), 0);
    commands.setPipeline(programs.pipelines[0].get());
    commands.dispatch(Groups(fieldSize.width), Groups(fieldSize.height), 1);
    commands.barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(field.get(), RenderTextureLayout::GENERAL));
    commands.setPipeline(programs.pipelines[1].get());
    commands.dispatch(Groups(outputSize.width), Groups(outputSize.height), 1);
    commands.barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(output.get(), RenderTextureLayout::SHADER_READ));
    commands.barriers(RenderBarrierStage::COPY, RenderTextureBarrier(original, RenderTextureLayout::COPY_SOURCE));
    return output.get();
}
}
#endif
