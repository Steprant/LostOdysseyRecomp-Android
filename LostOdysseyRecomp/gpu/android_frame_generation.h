#pragma once

#include "android_frame_generation_budget.h"
#include "frame_generation_composite.h"

#include <future>

namespace gpu::android_fg
{
class Generator
{
    using Snapshot = std::shared_ptr<frame_generation::ProducerSnapshot>;
    struct Sample {
        Snapshot images;
        Clock::time_point time;
    };
    struct Programs {
        std::unique_ptr<plume::RenderPipelineLayout> layout;
        std::unique_ptr<plume::RenderSampler> sampler;
        std::unique_ptr<plume::RenderDescriptorSet> descriptors;
        std::array<std::unique_ptr<plume::RenderPipeline>, 2> pipelines;
    };
    Sample last;
    // These leases remain owned through the optional submission's fence.
    Snapshot currentInput, previousInput;
    Programs programs;
    std::future<Programs> warming;
    std::unique_ptr<plume::RenderTexture> field, output;
    resolution::Size fieldSize{}, outputSize{};
    float prediction = 0.5f;
    bool failed = false;
    bool active = false;

    static Programs CreatePipelines(plume::RenderDevice& device);
    bool EnsureImages(plume::RenderDevice& device, resolution::Size extent);

public:
    bool Warmup(plume::RenderDevice& device);
    bool Warming() const;
    bool Prepare(plume::RenderDevice& device, const frame_generation::CompositeHandoff& capture,
        resolution::Size extent, Clock::time_point now);
    plume::RenderTexture* Record(plume::RenderCommandList& commands, plume::RenderTexture* original);
    void AfterFence() { currentInput.reset(); previousInput.reset(); }
    void Interrupt() { last = {}; active = false; }
    void Presented(bool generated) { active = generated; }
    bool Active() const { return active; }
    bool Unavailable() const { return failed; }
};
}
