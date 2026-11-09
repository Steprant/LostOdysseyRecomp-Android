#pragma once

#if defined(LO_GPU_PLUME)
#include "temporal_frame_inputs.h"

#include <cmath>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

namespace plume { struct RenderDevice; struct RenderCommandList; struct RenderTexture; }

// Qualcomm SGSR2 temporal upscaler (2-pass compute) on the renderer's SR route:
// the same scene-copy inputs as FSR (jittered RGBA8 colour, R32 reverse depth,
// backward render-pixel motion, motion invalidity), recorded with plume into
// the isolated SR command list. Experimental; Vulkan only.
namespace gpu::sgsr2 {

enum class Status : uint8_t { Ready, Unavailable, NeedsReconfigure, Failed, InputUnavailable };

struct Config {
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t outputWidth = 0, outputHeight = 0;
    uint64_t deviceEpoch = 0;
    bool operator==(const Config&) const = default;
};

struct Attempt {
    Status status = Status::Unavailable;
    uint64_t useId = 0;
};

// The colour input's format, as the caller sees it (SMAA crops into a stage
// of the same format; 10-bit and float inputs blend into a float stage).
enum class ColorFormat : uint8_t { Rgba8, Rgb10a2, Rgba16f, Other };

// Per-frame values the controller cannot derive from the textures.
struct FrameValues {
    float cameraFovAngleHor = 1.0f; // tan(horizontal fov / 2)
    bool sameCamera = false;        // view-projection unchanged since the last frame
    bool smaa = false;              // Quality: SMAA 1x on the SDR colour input before SGSR2 (8-bit, 10-bit or float)
    float edgeClip = NAN;           // depth-edge history clip weight; NaN: LO_SGSR2_EDGE_CLIP or 1
    float kernelScale = 1.0f;       // 0.5..1: narrows the Lanczos window's bias (wider, softer kernel) below 1
    bool softFilter = false;        // Soft filter: Gaussian kernel without negative lobes (WP7.3)
    ColorFormat colorFormat = ColorFormat::Rgba8;
};

// Render size for an FsrQuality-style ratio (Quality 1.5, Balanced 1.7,
// Performance 2.0, NativeAA 1.0), each side at least one pixel.
resolution::Size RenderSizeFor(resolution::Size output, upscaling::FsrQuality quality);

class Controller {
public:
    Controller();
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    Status EnsureSession(plume::RenderDevice& device, const Config& config);
    // Opens, records and closes the isolated list. Inputs are SHADER_READ,
    // output (RGBA16F storage, output size) GENERAL before and after.
    // With FrameValues::smaa and an SDR colour (8-bit, 10-bit or float), SMAA's three passes run
    // first in the same list, on a copy of the colour's render region.
    Attempt RecordIsolated(plume::RenderCommandList& commands, const Config& config,
        const temporal::TemporalFrameInputs& inputs, const FrameValues& frame, plume::RenderTexture& output);
    void OnBatchSubmitted(uint64_t useId, uint64_t serial);
    void OnBatchDiscarded(uint64_t useId);
    void ReleaseCompletedThrough(uint64_t serial);
    bool HasFeatureState() const;
    void ReleaseFeatureAfterGpuDrain();
    void ShutdownAfterGpuDrain();
    void AbandonUsesAfterDeviceLoss();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gpu::sgsr2
#endif
