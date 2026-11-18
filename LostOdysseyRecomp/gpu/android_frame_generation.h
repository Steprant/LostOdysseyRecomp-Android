#pragma once

#include "frame_generation_composite.h"
#include <chrono>

namespace gpu::android_fg
{
using Clock = std::chrono::steady_clock;

inline constexpr uint32_t kSourceFps = 30;
inline constexpr uint32_t kOutputFps = 60;

class Generator
{
    using Snapshot = std::shared_ptr<frame_generation::ProducerSnapshot>;
    struct Sample {
        Snapshot images;
        Clock::time_point time;
    };
    Sample last;
    // These leases outlive submission, even if the producer moves to a new
    // scene or frame generation is turned off before the present fence.
    Snapshot currentInput, previousInput;
    std::unique_ptr<plume::RenderPipelineLayout> layout;
    std::unique_ptr<plume::RenderSampler> sampler;
    std::unique_ptr<plume::RenderDescriptorSet> descriptors;
    std::array<std::unique_ptr<plume::RenderPipeline>, 3> pipelines;
    std::unique_ptr<plume::RenderTexture> field, output;
    resolution::Size fieldSize{}, outputSize{};
    bool failed = false;
    bool active = false;

    bool CreatePipelines(plume::RenderDevice& device);
    bool EnsureImages(plume::RenderDevice& device, resolution::Size extent, resolution::Size render);

public:
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
