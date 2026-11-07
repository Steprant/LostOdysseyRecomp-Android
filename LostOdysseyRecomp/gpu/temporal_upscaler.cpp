#include "temporal_upscaler.h"

#if defined(LO_GPU_PLUME)
#include "dlss_ngx.h"
#include "fsr_upscaler.h"
#include "fsr_projection.h"
#include "xess_upscaler.h"
#include "sgsr2_upscaler.h"
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <string_view>

#if LO_PLATFORM_MACOS
#include <os/logger.h>
#include <cstdio>
#include <cstdlib>
namespace plume {
    struct RenderCommandList; struct RenderTexture;
    bool EncodeMetalFxTemporalScale(RenderCommandList* commandList, const RenderTexture* color,
        const RenderTexture* depth, const RenderTexture* motion, const RenderTexture* output,
        uint32_t inputWidth, uint32_t inputHeight, float jitterX, float jitterY,
        float motionScaleX, float motionScaleY, bool depthReversed, bool reset, const char** failure);
    void EncodeMetalQueueSignal(RenderCommandList* commandList);
    void EncodeMetalQueueWait(RenderCommandList* commandList);
}
#endif

namespace gpu {
namespace {
SrResultStatus Convert(dlss::SrStatus status) {
    switch (status) {
    case dlss::SrStatus::Executable: return SrResultStatus::Ready;
    case dlss::SrStatus::Bypass: return SrResultStatus::Unavailable;
    case dlss::SrStatus::NeedsReconfigure: return SrResultStatus::NeedsReconfigure;
    case dlss::SrStatus::Failed: return SrResultStatus::Failed;
    case dlss::SrStatus::DeviceLost: return SrResultStatus::DeviceLost;
    }
    return SrResultStatus::Failed;
}
SrResultStatus Convert(fsr::Status status) {
    switch (status) {
    case fsr::Status::Ready: return SrResultStatus::Ready;
    case fsr::Status::Unavailable: return SrResultStatus::Unavailable;
    case fsr::Status::NeedsReconfigure: return SrResultStatus::NeedsReconfigure;
    case fsr::Status::Failed: return SrResultStatus::Failed;
    case fsr::Status::DeviceLost: return SrResultStatus::DeviceLost;
    case fsr::Status::InputUnavailable: return SrResultStatus::InputUnavailable;
    }
    return SrResultStatus::Failed;
}
fsr::Config FsrConfig(const SrRequest& request) {
    return {request.inputs.color.width, request.inputs.color.height, request.plan.output.width,
        request.plan.output.height, request.plan.fsrQuality, request.plan.deviceEpoch};
}
fsr::FrameMetadata FsrMetadata(const SrRequest& request) {
    fsr::FrameMetadata frame{};
    frame.enableSharpening = request.options.fsrSharpening;
    frame.sharpness = request.options.fsrSharpness;
    if (!request.inputs.cameraValid || !request.inputs.color.height) return frame;
    const auto projection = fsr::DeriveProjection(request.inputs.cameraViewProjection,
        double(request.inputs.color.width) / request.inputs.color.height);
    if (!projection) return frame;
    frame.cameraValid = true;
    frame.cameraNear = FLT_MAX;
    frame.cameraFar = projection->nearDistance;
    frame.verticalFovRadians = projection->verticalFovRadians;
    // P1 uses the SDK default unit scale. Guest world units are not calibrated.
    frame.viewSpaceToMetersFactor = 1.0f;
    frame.frameTimeDeltaMilliseconds = request.inputs.frameTimeDeltaMilliseconds;
    frame.depthScale = projection->depthScale;
    frame.depthBias = projection->depthBias;
    return frame;
}
#ifdef _WIN32
SrResultStatus Convert(xess::Status status) {
    switch (status) {
    case xess::Status::Ready: return SrResultStatus::Ready;
    case xess::Status::Unavailable: return SrResultStatus::Unavailable;
    case xess::Status::NeedsReconfigure: return SrResultStatus::NeedsReconfigure;
    case xess::Status::Failed: return SrResultStatus::Failed;
    case xess::Status::DeviceLost: return SrResultStatus::DeviceLost;
    case xess::Status::InputUnavailable: return SrResultStatus::InputUnavailable;
    }
    return SrResultStatus::Failed;
}
xess::Config XessConfig(const SrRequest& request) {
    return {request.inputs.color.width, request.inputs.color.height, request.plan.output.width,
        request.plan.output.height, request.plan.fsrQuality, request.plan.deviceEpoch,
        request.inputs.depthConvention == temporal::DepthConvention::Reversed};
}
#endif
SrResultStatus Convert(sgsr2::Status status) {
    switch (status) {
    case sgsr2::Status::Ready: return SrResultStatus::Ready;
    case sgsr2::Status::Unavailable: return SrResultStatus::Unavailable;
    case sgsr2::Status::NeedsReconfigure: return SrResultStatus::NeedsReconfigure;
    case sgsr2::Status::Failed: return SrResultStatus::Failed;
    case sgsr2::Status::InputUnavailable: return SrResultStatus::InputUnavailable;
    }
    return SrResultStatus::Failed;
}
sgsr2::Config Sgsr2Config(const SrRequest& request) {
    return {request.inputs.color.width, request.inputs.color.height, request.plan.output.width,
        request.plan.output.height, request.plan.deviceEpoch};
}
// XeSS is compiled for D3D12 only; other backends never acquire a use.
SrResult XessUnavailable(const SrRequest& request) {
    SrResult result{};
    result.requestedProvider = request.plan.requestedUpscaler;
    result.actualProvider = upscaling::Upscaler::Xess;
    return result;
}
dlss::SrConfig DlssConfig(const SrRequest& request) {
    dlss::SrConfig config{};
    config.renderExtent = {request.inputs.color.width, request.inputs.color.height};
    config.outputExtent = {request.plan.output.width, request.plan.output.height};
    config.quality = request.plan.dlssQuality;
    config.deviceEpoch = request.plan.deviceEpoch;
    config.depthInverted = request.inputs.depthConvention == temporal::DepthConvention::Reversed;
    config.colorSpace = request.inputs.colorEncoding == temporal::ColorEncoding::Sdr ?
        dlss::SrColorSpace::DisplayEncoded : dlss::SrColorSpace::Linear;
    config.neuralRenderingPasses = uint8_t(std::min(request.options.dlssNeuralRenderingPasses,
        dlss::kMaxNeuralRenderingPasses));
    config.neuralRenderingPreset = uint8_t(std::min(request.options.dlssNeuralRenderingPreset, 3u));
    // LO_DLSS_PRESET=J|K|L|M replaces the menu's model for developer comparisons.
    static const uint8_t presetOverride = [] {
        const char* value = std::getenv("LO_DLSS_PRESET");
        const int letter = value && value[0] && !value[1] ? std::toupper(static_cast<unsigned char>(value[0])) : 0;
        return uint8_t(letter >= 'J' && letter <= 'M' ? letter - 'A' + 1 : 0);
    }();
    config.renderPreset = presetOverride ? presetOverride :
        request.options.dlssModel == 1 ? dlss::kRenderPresetL : dlss::kRenderPresetM;
    return config;
}
}

TemporalUpscaler::TemporalUpscaler(dlss::Controller& controller)
    : TemporalUpscaler(&controller) {}
TemporalUpscaler::TemporalUpscaler(dlss::Controller* controller)
    : dlss_(controller), fsr_(std::make_unique<fsr::Controller>()), xess_(std::make_unique<xess::Controller>()),
      sgsr2_(std::make_unique<sgsr2::Controller>()) {}
TemporalUpscaler::~TemporalUpscaler() = default;

upscaling::OutputSizing TemporalUpscaler::QuerySizing(const plume::VulkanInterface& api,
    const plume::VulkanDevice& device, const upscaling::SizingKey& key) {
    if (CanQueryNgxSizing(key) && dlss_)
        return dlss_->QueryOutputSizing(api, device, key);
    upscaling::OutputSizing unavailable{};
    unavailable.key = key;
    for (uint32_t i = 0; i < unavailable.modes.size(); ++i) {
        auto& mode = unavailable.modes[i];
        mode.state = upscaling::SizingState::Unavailable;
        if (key.provider == upscaling::Upscaler::Fsr) {
            if (const auto size = fsr::RecommendedRenderSize({key.outputWidth, key.outputHeight}, upscaling::FsrQuality(i))) {
                mode.state = upscaling::SizingState::Ready;
                mode.optimal = mode.minimum = mode.maximum = *size;
            }
        }
        // SGSR2 has no SDK presets: the FSR ratios pick the render extent.
        if (key.provider == upscaling::Upscaler::Sgsr2 && key.outputWidth && key.outputHeight) {
            mode.state = upscaling::SizingState::Ready;
            mode.optimal = mode.minimum = mode.maximum =
                sgsr2::RenderSizeFor({key.outputWidth, key.outputHeight}, upscaling::FsrQuality(i));
        }
    }
    return unavailable;
}

SrResult TemporalUpscaler::Prepare(plume::VulkanDevice& device, const SrRequest& request) {
    SrResult result{};
    result.requestedProvider = request.plan.requestedUpscaler;
    if (!ValidSrRequest(request)) return result;
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Xess) return XessUnavailable(request);
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Sgsr2) {
        result.actualProvider = upscaling::Upscaler::Sgsr2;
        result.status = Convert(sgsr2_->EnsureSession(device, Sgsr2Config(request)));
        return result;
    }
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Fsr) {
        result.actualProvider = upscaling::Upscaler::Fsr;
        result.status = FsrMetadata(request).cameraValid ? Convert(fsr_->EnsureSession(device, FsrConfig(request))) :
            SrResultStatus::InputUnavailable;
        return result;
    }
    result.actualProvider = upscaling::Upscaler::Dlss;
    result.status = dlss_ ? Convert(dlss_->EnsureSession(device)) : SrResultStatus::Unavailable;
    return result;
}

SrResult TemporalUpscaler::RecordIsolated(plume::VulkanCommandList& commands,
    const SrRequest& request, plume::VulkanTexture& output, dlss::EvaluateCapture* capture) {
    SrResult result{};
    result.requestedProvider = request.plan.requestedUpscaler;
    if (!ValidSrRequest(request)) return result;
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Xess) return XessUnavailable(request);
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Sgsr2) {
        const auto& inputs = request.inputs;
        sgsr2::FrameValues frame{};
        // Quality: SMAA 1x on the RGBA8 SDR input (G3; LO_SGSR2_SMAA=0, the
        // Android diagnostics switch, turns it off). The input's edges are
        // then anti-aliased before accumulation, so Quality keeps the
        // reference kernel; without SMAA it widens it (kernelScale 0.8) for
        // a softer image. Balanced and Performance keep the reference and no
        // SMAA. No sharpening pass. LO_SGSR2_KERNEL_SCALE=0.5..1 overrides.
        static const bool smaaEnabled = [] {
            const char* value = std::getenv("LO_SGSR2_SMAA");
            return !(value && std::string_view(value) == "0");
        }();
        static const float kernelOverride = [] {
            const char* value = std::getenv("LO_SGSR2_KERNEL_SCALE");
            return value && *value ? float(std::clamp(std::atof(value), 0.5, 1.0)) : -1.0f;
        }();
        const bool quality = request.plan.fsrQuality == upscaling::FsrQuality::Quality;
        frame.smaa = smaaEnabled && quality && inputs.color.texture &&
            static_cast<const plume::VulkanTexture*>(inputs.color.texture)->desc.format == plume::RenderFormat::R8G8B8A8_UNORM;
        frame.kernelScale = kernelOverride > 0.0f ? kernelOverride : quality && !frame.smaa ? 0.8f : 1.0f;
        if (inputs.cameraValid && inputs.color.height) {
            const double aspect = double(inputs.color.width) / inputs.color.height;
            if (const auto projection = fsr::DeriveProjection(inputs.cameraViewProjection, aspect))
                frame.cameraFovAngleHor = float(std::tan(projection->verticalFovRadians * 0.5) * aspect);
            if (sgsr2Camera_ && sgsr2CameraFrame_ + 1 == inputs.renderFrameId) {
                double difference = 0;
                for (size_t i = 0; i < inputs.cameraViewProjection.size(); ++i)
                    difference += std::abs(inputs.cameraViewProjection[i] - (*sgsr2Camera_)[i]);
                frame.sameCamera = difference < 1e-5;
            }
            sgsr2Camera_ = inputs.cameraViewProjection;
            sgsr2CameraFrame_ = inputs.renderFrameId;
        } else sgsr2Camera_.reset();
        const auto attempt = sgsr2_->RecordIsolated(commands, Sgsr2Config(request), inputs, frame, output);
        result.actualProvider = upscaling::Upscaler::Sgsr2;
        result.status = Convert(attempt.status);
        if (attempt.useId) result.token = {upscaling::Upscaler::Sgsr2, request.plan.deviceEpoch,
            attempt.useId, request.plan.requestSignature, request.plan.geometryEpoch};
        return result;
    }
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Fsr) {
        const auto attempt = fsr_->RecordIsolated(commands, FsrConfig(request), request.inputs,
            FsrMetadata(request), output, capture);
        result.actualProvider = upscaling::Upscaler::Fsr;
        result.status = Convert(attempt.status);
        result.rawResult = attempt.sdkResult;
        result.rawVkResult = attempt.vkResult;
        if (attempt.useId) result.token = {upscaling::Upscaler::Fsr, request.plan.deviceEpoch,
            attempt.useId, request.plan.requestSignature, request.plan.geometryEpoch};
        return result;
    }
    if (!dlss_) return result;
    dlss_->SetNeuralRenderingTuning(request.options.dlssNeuralRenderingTuning);
    const auto attempt = dlss_->RecordIsolated(commands, DlssConfig(request), request.inputs, output, capture);
    result.actualProvider = upscaling::Upscaler::Dlss;
    result.status = Convert(attempt.status);
    result.rawResult = attempt.rawNgxResult;
    result.rawVkResult = attempt.rawVkResult;
    if (attempt.useId) result.token = {upscaling::Upscaler::Dlss, request.plan.deviceEpoch, attempt.useId,
        request.plan.requestSignature, request.plan.geometryEpoch};
    return result;
}

#ifdef _WIN32
upscaling::OutputSizing TemporalUpscaler::QuerySizing(const plume::D3D12Device& device, const upscaling::SizingKey& key) {
    if (CanQueryNgxSizing(key) && dlss_)
        return dlss_->QueryOutputSizing(device, key);
    if (key.provider == upscaling::Upscaler::Xess)
        return xess_->QuerySizing(device, key);
    upscaling::OutputSizing unavailable{};
    unavailable.key = key;
    for (uint32_t i = 0; i < unavailable.modes.size(); ++i) {
        auto& mode = unavailable.modes[i];
        mode.state = upscaling::SizingState::Unavailable;
        if (key.provider == upscaling::Upscaler::Fsr) {
            if (const auto size = fsr::RecommendedRenderSize({key.outputWidth, key.outputHeight}, upscaling::FsrQuality(i))) {
                mode.state = upscaling::SizingState::Ready;
                mode.optimal = mode.minimum = mode.maximum = *size;
            }
        }
    }
    return unavailable;
}

SrResult TemporalUpscaler::Prepare(plume::D3D12Device& device, const SrRequest& request) {
    SrResult result{};
    result.requestedProvider = request.plan.requestedUpscaler;
    if (!ValidSrRequest(request)) return result;
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Xess) {
        result.actualProvider = upscaling::Upscaler::Xess;
        result.status = Convert(xess_->EnsureSession(device, XessConfig(request)));
        return result;
    }
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Fsr) {
        result.actualProvider = upscaling::Upscaler::Fsr;
        result.status = FsrMetadata(request).cameraValid ? Convert(fsr_->EnsureSession(device, FsrConfig(request))) :
            SrResultStatus::InputUnavailable;
        return result;
    }
    result.actualProvider = upscaling::Upscaler::Dlss;
    result.status = dlss_ ? Convert(dlss_->EnsureSession(device)) : SrResultStatus::Unavailable;
    return result;
}

SrResult TemporalUpscaler::RecordIsolated(plume::D3D12CommandList& commands,
    const SrRequest& request, plume::D3D12Texture& output, dlss::EvaluateCapture* capture) {
    SrResult result{};
    result.requestedProvider = request.plan.requestedUpscaler;
    if (!ValidSrRequest(request)) return result;
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Xess) {
        const auto attempt = xess_->RecordIsolated(commands, XessConfig(request), request.inputs, output);
        result.actualProvider = upscaling::Upscaler::Xess;
        result.status = Convert(attempt.status);
        result.rawResult = attempt.sdkResult;
        result.rawVkResult = attempt.hrResult;
        if (attempt.useId) result.token = {upscaling::Upscaler::Xess, request.plan.deviceEpoch,
            attempt.useId, request.plan.requestSignature, request.plan.geometryEpoch};
        return result;
    }
    if (request.plan.requestedUpscaler == upscaling::Upscaler::Fsr) {
        const auto attempt = fsr_->RecordIsolated(commands, FsrConfig(request), request.inputs,
            FsrMetadata(request), output, capture);
        result.actualProvider = upscaling::Upscaler::Fsr;
        result.status = Convert(attempt.status);
        result.rawResult = attempt.sdkResult;
        result.rawVkResult = attempt.vkResult;
        if (attempt.useId) result.token = {upscaling::Upscaler::Fsr, request.plan.deviceEpoch,
            attempt.useId, request.plan.requestSignature, request.plan.geometryEpoch};
        return result;
    }
    if (!dlss_) return result;
    dlss_->SetNeuralRenderingTuning(request.options.dlssNeuralRenderingTuning);
    const auto attempt = dlss_->RecordIsolated(commands, DlssConfig(request), request.inputs, output, capture);
    result.actualProvider = upscaling::Upscaler::Dlss;
    result.status = Convert(attempt.status);
    result.rawResult = attempt.rawNgxResult;
    result.rawVkResult = attempt.rawVkResult;
    if (attempt.useId) result.token = {upscaling::Upscaler::Dlss, request.plan.deviceEpoch, attempt.useId,
        request.plan.requestSignature, request.plan.geometryEpoch};
    return result;
}

#endif

#if LO_PLATFORM_MACOS
// MetalFX has no fixed quality modes; the FSR ratios pick the render extent.
upscaling::OutputSizing TemporalUpscaler::QuerySizing(const plume::RenderDevice&, const upscaling::SizingKey& key) {
    upscaling::OutputSizing sizing{};
    sizing.key = key;
    for (uint32_t i = 0; i < sizing.modes.size(); ++i) {
        auto& mode = sizing.modes[i];
        mode.state = upscaling::SizingState::Unavailable;
        if (key.provider != upscaling::Upscaler::MetalFx || !key.outputWidth || !key.outputHeight) continue;
        static constexpr double kRatio[] = {1.5, 1.7, 2.0, 1.0};
        const resolution::Size size{std::max(1u, uint32_t(key.outputWidth / kRatio[i] + 0.5)),
            std::max(1u, uint32_t(key.outputHeight / kRatio[i] + 0.5))};
        mode.state = upscaling::SizingState::Ready;
        mode.optimal = mode.minimum = mode.maximum = size;
    }
    return sizing;
}

SrResult TemporalUpscaler::Prepare(plume::RenderDevice&, const SrRequest& request) {
    SrResult result{};
    result.requestedProvider = request.plan.requestedUpscaler;
    if (!ValidSrRequest(request) || request.plan.requestedUpscaler != upscaling::Upscaler::MetalFx) return result;
    result.actualProvider = upscaling::Upscaler::MetalFx;
    result.status = SrResultStatus::Ready;
    return result;
}

SrResult TemporalUpscaler::RecordIsolated(plume::RenderCommandList& commands, const SrRequest& request,
    plume::RenderTexture& output, dlss::EvaluateCapture*) {
    SrResult result{};
    result.requestedProvider = request.plan.requestedUpscaler;
    if (!ValidSrRequest(request) || request.plan.requestedUpscaler != upscaling::Upscaler::MetalFx) return result;
    result.actualProvider = upscaling::Upscaler::MetalFx;
    const auto& inputs = request.inputs;
    const auto& color = inputs.color;
    // MetalFX reads every input from (0, 0) with one content extent.
    if (color.x || color.y || inputs.depth.x || inputs.depth.y || inputs.motion.x || inputs.motion.y ||
        inputs.depth.width < color.width || inputs.depth.height < color.height ||
        inputs.motion.width < color.width || inputs.motion.height < color.height ||
        !temporal::KnownDepthConvention(inputs.depthConvention)) {
        result.status = SrResultStatus::InputUnavailable;
        return result;
    }
    // Motion vectors are backward, unjittered render pixels, +Y down (motion_vector.h),
    // which is MetalFX's pixel convention; jitter is passed as FSR receives it (the
    // opposite sign visibly blurs static detail). The environment overrides exist to
    // check the conventions on screen: LO_METALFX_MV_SCALE="x,y", LO_METALFX_JITTER_SIGN=-1|1.
    static const auto overrides = [] {
        struct { float mvX = 1.0f, mvY = 1.0f, jitterSign = 1.0f; } values;
        if (const char* scale = std::getenv("LO_METALFX_MV_SCALE")) std::sscanf(scale, "%f,%f", &values.mvX, &values.mvY);
        if (const char* sign = std::getenv("LO_METALFX_JITTER_SIGN")) values.jitterSign = std::atof(sign) < 0 ? -1.0f : 1.0f;
        return values;
    }();
    const bool frameGap = !metalFxLastFrame_ || inputs.renderFrameId != metalFxLastFrame_ + 1;
    metalFxLastFrame_ = inputs.renderFrameId;
    const char* failure = "";
    // The isolated list is the provider's own: open it, record, close it. The
    // renderer's prefix list signalled; its continuation waits for this signal.
    commands.begin();
    plume::EncodeMetalQueueWait(&commands);
    const bool encoded = plume::EncodeMetalFxTemporalScale(&commands, color.texture, inputs.depth.texture,
        inputs.motion.texture, &output, color.width, color.height,
        overrides.jitterSign * float(inputs.jitter.pixelX), overrides.jitterSign * float(inputs.jitter.pixelY),
        overrides.mvX, overrides.mvY, inputs.depthConvention == temporal::DepthConvention::Reversed,
        inputs.resetHistory || frameGap, &failure);
    if (encoded) plume::EncodeMetalQueueSignal(&commands);
    commands.end();
    if (!encoded) {
        metalFxLastFrame_ = 0;
        LOG_WARNING("MetalFX: temporal scaling failed step={} input={}x{} output={}x{}", failure,
            color.width, color.height, request.plan.output.width, request.plan.output.height);
    }
    result.status = encoded ? SrResultStatus::Ready : SrResultStatus::Failed;
    return result;
}
#endif

void TemporalUpscaler::OnSubmitted(SrUseToken token, uint64_t checkedSerial) {
    if (dlss_) RouteSubmitted(token, checkedSerial, *dlss_);
    if (token.provider == upscaling::Upscaler::Fsr && token.useId && checkedSerial) fsr_->OnBatchSubmitted(token.useId, checkedSerial);
    if (token.provider == upscaling::Upscaler::Xess && token.useId && checkedSerial) xess_->OnBatchSubmitted(token.useId, checkedSerial);
    if (token.provider == upscaling::Upscaler::Sgsr2 && token.useId && checkedSerial) sgsr2_->OnBatchSubmitted(token.useId, checkedSerial);
}
void TemporalUpscaler::OnDiscarded(SrUseToken token) {
    if (dlss_) RouteDiscarded(token, *dlss_);
    if (token.provider == upscaling::Upscaler::Fsr && token.useId) fsr_->OnBatchDiscarded(token.useId);
    if (token.provider == upscaling::Upscaler::Xess && token.useId) xess_->OnBatchDiscarded(token.useId);
    if (token.provider == upscaling::Upscaler::Sgsr2 && token.useId) sgsr2_->OnBatchDiscarded(token.useId);
}
void TemporalUpscaler::ReleaseCompleted(uint64_t serial) {
    if (!serial) return;
    if (dlss_) dlss_->ReleaseCompletedThrough(serial);
    fsr_->ReleaseCompletedThrough(serial);
    xess_->ReleaseCompletedThrough(serial);
    sgsr2_->ReleaseCompletedThrough(serial);
}
bool TemporalUpscaler::HasFeatureState() const {
    return (dlss_ && dlss_->HasFeatureState()) || fsr_->HasFeatureState() || xess_->HasFeatureState() ||
        sgsr2_->HasFeatureState();
}
void TemporalUpscaler::ReleaseFeatureAfterGpuDrain() {
    if (dlss_) dlss_->ReleaseFeatureAfterGpuDrain();
    fsr_->ReleaseFeatureAfterGpuDrain(); xess_->ReleaseFeatureAfterGpuDrain(); sgsr2_->ReleaseFeatureAfterGpuDrain();
    sgsr2Camera_.reset();
}
void TemporalUpscaler::ShutdownAfterGpuDrain() {
    if (dlss_) dlss_->ShutdownAfterGpuDrain();
    fsr_->ShutdownAfterGpuDrain(); xess_->ShutdownAfterGpuDrain(); sgsr2_->ShutdownAfterGpuDrain();
    sgsr2Camera_.reset();
}
bool TemporalUpscaler::ShutdownComplete() const {
    return (!dlss_ || dlss_->ShutdownComplete()) && !fsr_->HasFeatureState() && !xess_->HasFeatureState() &&
        !sgsr2_->HasFeatureState();
}
void TemporalUpscaler::AbandonAfterDeviceLoss() {
    if (dlss_) dlss_->AbandonUsesAfterDeviceLoss();
    fsr_->AbandonUsesAfterDeviceLoss(); xess_->AbandonUsesAfterDeviceLoss(); sgsr2_->AbandonUsesAfterDeviceLoss();
}
} // namespace gpu
#endif
