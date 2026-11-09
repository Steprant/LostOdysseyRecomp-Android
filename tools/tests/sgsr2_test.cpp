// Headless Vulkan fixture for the SGSR2 controller (lavapipe in CI): the
// production shaders, pipelines and lifecycle run on synthetic inputs, then
// the output is read back and checked for finite, in-range, convergent values.
#include <gpu/sgsr2_upscaler.h>
#include <gpu/sgsr2_status.h>
#include <plume_render_interface.h>
#include <stdafx.h>
#include <cmath>
#include <cstring>
#include <functional>

namespace plume
{
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace {
using namespace plume;
constexpr uint32_t kRenderW = 64, kRenderH = 36, kOutW = 128, kOutH = 72;

float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float value;
    if (exponent == 0) value = std::ldexp(float(mantissa), -24);
    else if (exponent == 31) value = mantissa ? NAN : INFINITY;
    else value = std::ldexp(float(mantissa | 1024), int(exponent) - 25);
    return sign ? -value : value;
}
uint16_t FloatToHalf(float f)
{
    if (f == 0.0f) return 0;
    uint32_t bits; std::memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    int exponent = int((bits >> 23) & 255) - 127 + 15;
    const uint32_t mantissa = (bits >> 13) & 1023;
    if (exponent <= 0) return uint16_t(sign);
    if (exponent >= 31) return uint16_t(sign | 0x7c00);
    return uint16_t(sign | (exponent << 10) | mantissa);
}

// A continuous scene in output pixels: a horizontal gradient (red), a fine
// sinusoid with a 5-output-pixel period, above the render Nyquist limit
// (green) and a hard vertical edge at a non-integer position (blue). Or a
// flat colour, which must come back unchanged.
struct Scene {
    bool flat = false;
    static float Channel(int c, double ox, double oy)
    {
        if (c == 0) return float(std::clamp(ox / kOutW, 0.0, 1.0));
        if (c == 1) return float(0.5 + 0.4 * std::sin(6.283185307 * (ox + 0.6 * oy) / 5.0));
        return ox >= 63.3 ? 0.9f : 0.1f;
    }
    // Rasterised render pixel (x, y) with the frame's content displacement
    // (jx, jy) in render pixels: point-sampled at its jittered centre.
    uint32_t Pixel(uint32_t x, uint32_t y, double jx, double jy) const
    {
        if (flat) return 0xff000000u | (192u << 16) | (128u << 8) | 64u;
        const double ox = (x + 0.5 - jx) * kOutW / kRenderW, oy = (y + 0.5 - jy) * kOutH / kRenderH;
        uint32_t p = 0xff000000u;
        for (int c = 0; c < 3; ++c) p |= uint32_t(std::lround(Channel(c, ox, oy) * 255)) << (8 * c);
        return p;
    }
    // The ideal output pixel with the content displaced by (dx, dy) render pixels.
    float Ideal(uint32_t ox, uint32_t oy, int c, double dx = 0, double dy = 0) const
    {
        if (flat) return (c == 0 ? 64 : c == 1 ? 128 : 192) / 255.0f;
        return std::lround(Channel(c, ox + 0.5 - dx * kOutW / kRenderW, oy + 0.5 - dy * kOutH / kRenderH) * 255) / 255.0f;
    }
};

struct Fixture {
    RenderDevice* device = nullptr;
    std::unique_ptr<RenderCommandQueue> queue;
    std::unique_ptr<RenderCommandList> setup, sr;
    std::unique_ptr<RenderCommandFence> fence;
    std::unique_ptr<RenderTexture> color, colorHdr, depth, motion, invalidity, output;
    std::unique_ptr<RenderBuffer> upload, readback;
    static constexpr uint64_t kColor = 0, kDepth = 64 * 1024, kMotion = 128 * 1024, kInvalid = 192 * 1024;

    bool Init(RenderDevice* d)
    {
        device = d;
        queue = device->createCommandQueue(RenderCommandListType::DIRECT);
        if (!queue) return false;
        setup = queue->createCommandList();
        sr = queue->createCommandList();
        fence = device->createCommandFence();
        const auto tex = [&](uint32_t w, uint32_t h, RenderFormat format, RenderTextureFlags flags = RenderTextureFlag::NONE) {
            return device->createTexture(RenderTextureDesc::Texture2D(w, h, 1, format, flags));
        };
        color = tex(kRenderW, kRenderH, RenderFormat::R8G8B8A8_UNORM);
        colorHdr = tex(kRenderW, kRenderH, RenderFormat::R16G16B16A16_FLOAT);
        depth = tex(kRenderW, kRenderH, RenderFormat::R32_FLOAT);
        motion = tex(kRenderW, kRenderH, RenderFormat::R16G16_FLOAT);
        invalidity = tex(kRenderW, kRenderH, RenderFormat::R8_UNORM);
        output = tex(kOutW, kOutH, RenderFormat::R16G16B16A16_FLOAT,
            RenderTextureFlag::STORAGE | RenderTextureFlag::UNORDERED_ACCESS);
        upload = device->createBuffer(RenderBufferDesc::UploadBuffer(256 * 1024));
        readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(kOutW * kOutH * 8));
        return setup && sr && fence && color && colorHdr && depth && motion && invalidity && output && upload && readback;
    }

    void Submit(RenderCommandList* list)
    {
        const RenderCommandList* lists[] = {list};
        queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
        queue->waitForCommandFence(fence.get());
    }

    // Uploads the scene; motion is (mx, my) render pixels everywhere and the
    // left `invalidColumns` columns are flagged invalid.
    // Optional per-pixel overrides of the scene colour and depth.
    std::function<uint32_t(uint32_t, uint32_t)> colorFn;
    std::function<float(uint32_t, uint32_t)> depthFn;

    bool Upload(const Scene& scene, float mx, float my, uint32_t invalidColumns, bool first, double jx = 0, double jy = 0)
    {
        auto* bytes = static_cast<uint8_t*>(upload->map());
        if (!bytes) return false;
        for (uint32_t y = 0; y < kRenderH; ++y)
            for (uint32_t x = 0; x < kRenderW; ++x) {
                reinterpret_cast<uint32_t*>(bytes + kColor)[y * kRenderW + x] = colorFn ? colorFn(x, y) : scene.Pixel(x, y, jx, jy);
                // A flat wall (reverse Z), plus a nearer box in the lower right.
                reinterpret_cast<float*>(bytes + kDepth)[y * kRenderW + x] = depthFn ? depthFn(x, y) :
                    x >= 48 && y >= 24 ? 0.2f : 0.05f;
                auto* mv = reinterpret_cast<uint16_t*>(bytes + kMotion) + 2 * (y * kRenderW + x);
                mv[0] = FloatToHalf(mx); mv[1] = FloatToHalf(my);
                bytes[kInvalid + y * 256 + x] = x < invalidColumns ? 255 : 0;
            }
        upload->unmap();
        setup->begin();
        struct Item { RenderTexture* texture; RenderFormat format; uint32_t rowPixels; uint64_t offset; };
        const Item items[] = {
            {color.get(), RenderFormat::R8G8B8A8_UNORM, kRenderW, kColor},
            {depth.get(), RenderFormat::R32_FLOAT, kRenderW, kDepth},
            {motion.get(), RenderFormat::R16G16_FLOAT, kRenderW, kMotion},
            {invalidity.get(), RenderFormat::R8_UNORM, 256, kInvalid}};
        for (const auto& item : items) {
            setup->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(item.texture, RenderTextureLayout::COPY_DEST));
            setup->copyTextureRegion(RenderTextureCopyLocation::Subresource(item.texture),
                RenderTextureCopyLocation::PlacedFootprint(upload.get(), item.format, kRenderW, kRenderH, 1,
                    item.rowPixels, item.offset));
            setup->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(item.texture, RenderTextureLayout::SHADER_READ));
        }
        if (first) setup->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(output.get(), RenderTextureLayout::GENERAL));
        setup->end();
        Submit(setup.get());
        return true;
    }

    // Fills the HDR colour image with one linear colour.
    bool UploadHdr(const float rgb[3])
    {
        auto* half = static_cast<uint16_t*>(upload->map());
        if (!half) return false;
        for (uint32_t i = 0; i < kRenderW * kRenderH; ++i)
            for (int c = 0; c < 4; ++c) half[i * 4 + c] = FloatToHalf(c < 3 ? rgb[c] : 1.0f);
        upload->unmap();
        setup->begin();
        setup->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(colorHdr.get(), RenderTextureLayout::COPY_DEST));
        setup->copyTextureRegion(RenderTextureCopyLocation::Subresource(colorHdr.get()),
            RenderTextureCopyLocation::PlacedFootprint(upload.get(), RenderFormat::R16G16B16A16_FLOAT, kRenderW, kRenderH, 1, kRenderW));
        setup->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(colorHdr.get(), RenderTextureLayout::SHADER_READ));
        setup->end();
        Submit(setup.get());
        return true;
    }

    // Fills the float colour image with per-pixel SDR values (0..1), for the
    // 10-bit/float SMAA path (2.0, WP7.2.2).
    bool UploadFloatSdr(const std::function<float(uint32_t, uint32_t)>& value)
    {
        auto* half = static_cast<uint16_t*>(upload->map());
        if (!half) return false;
        for (uint32_t y = 0; y < kRenderH; ++y)
            for (uint32_t x = 0; x < kRenderW; ++x)
                for (int c = 0; c < 4; ++c) half[(y * kRenderW + x) * 4 + c] = FloatToHalf(c < 3 ? value(x, y) : 1.0f);
        upload->unmap();
        setup->begin();
        setup->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(colorHdr.get(), RenderTextureLayout::COPY_DEST));
        setup->copyTextureRegion(RenderTextureCopyLocation::Subresource(colorHdr.get()),
            RenderTextureCopyLocation::PlacedFootprint(upload.get(), RenderFormat::R16G16B16A16_FLOAT, kRenderW, kRenderH, 1, kRenderW));
        setup->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(colorHdr.get(), RenderTextureLayout::SHADER_READ));
        setup->end();
        Submit(setup.get());
        return true;
    }

    bool hdr = false;
    bool floatSdr = false; // colorHdr carries SDR values (a float scene target)
    gpu::temporal::TemporalFrameInputs Inputs(uint64_t frame, bool reset, double jx, double jy) const
    {
        gpu::temporal::TemporalFrameInputs inputs{};
        const auto region = [](RenderTexture* texture) {
            gpu::temporal::TextureRegion r{};
            r.texture = texture; r.allocation = {kRenderW, kRenderH}; r.width = kRenderW; r.height = kRenderH;
            return r;
        };
        inputs.color = region(hdr || floatSdr ? colorHdr.get() : color.get());
        inputs.colorEncoding = hdr ? gpu::temporal::ColorEncoding::HdrLinear : gpu::temporal::ColorEncoding::Sdr;
        inputs.depth = region(depth.get());
        inputs.motion = region(motion.get());
        inputs.motionInvalidity = region(invalidity.get());
        inputs.depthConvention = gpu::temporal::DepthConvention::Reversed;
        inputs.renderFrameId = frame;
        inputs.resetHistory = reset;
        inputs.jitter.pixelX = jx; inputs.jitter.pixelY = jy;
        inputs.currentInputsComplete = true;
        return inputs;
    }

    // Returns RGB floats of the output (kOutW * kOutH * 3).
    std::vector<float> Read()
    {
        setup->begin();
        setup->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(output.get(), RenderTextureLayout::COPY_SOURCE));
        setup->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(),
            RenderFormat::R16G16B16A16_FLOAT, kOutW, kOutH, 1, kOutW), RenderTextureCopyLocation::Subresource(output.get()));
        setup->barriers(RenderBarrierStage::COMPUTE, RenderTextureBarrier(output.get(), RenderTextureLayout::GENERAL));
        setup->end();
        Submit(setup.get());
        std::vector<float> rgb(size_t(kOutW) * kOutH * 3);
        const auto* half = static_cast<const uint16_t*>(readback->map());
        for (size_t i = 0; i < size_t(kOutW) * kOutH; ++i)
            for (int c = 0; c < 3; ++c) rgb[i * 3 + c] = HalfToFloat(half[i * 4 + c]);
        readback->unmap();
        return rgb;
    }
};

struct Stats { bool finite = true; float min = 1e9f, max = -1e9f; double mean[3]{}; };
Stats Measure(const std::vector<float>& rgb)
{
    Stats s;
    const size_t pixels = rgb.size() / 3;
    for (size_t i = 0; i < pixels; ++i)
        for (int c = 0; c < 3; ++c) {
            const float v = rgb[i * 3 + c];
            if (!std::isfinite(v)) { s.finite = false; continue; }
            s.min = std::min(s.min, v); s.max = std::max(s.max, v);
            s.mean[c] += v / double(pixels);
        }
    return s;
}
// Per-channel RMSE against the ideal output-resolution image, away from the
// two-pixel border.
std::array<double, 3> Rmse(const Scene& scene, const std::vector<float>& rgb, double dx = 0, double dy = 0)
{
    std::array<double, 3> sum{};
    size_t count = 0;
    for (uint32_t y = 2; y + 2 < kOutH; ++y)
        for (uint32_t x = 2; x + 2 < kOutW; ++x, ++count)
            for (int c = 0; c < 3; ++c) {
                const double d = rgb[(size_t(y) * kOutW + x) * 3 + c] - scene.Ideal(x, y, c, dx, dy);
                sum[c] += d * d;
            }
    for (auto& v : sum) v = std::sqrt(v / double(count));
    return sum;
}
// Bilinear upscale of an unjittered render: the spatial baseline.
std::vector<float> Bilinear(const Scene& scene)
{
    std::vector<float> rgb(size_t(kOutW) * kOutH * 3);
    for (uint32_t y = 0; y < kOutH; ++y)
        for (uint32_t x = 0; x < kOutW; ++x) {
            const double sx = std::clamp((x + 0.5) * kRenderW / kOutW - 0.5, 0.0, kRenderW - 1.0);
            const double sy = std::clamp((y + 0.5) * kRenderH / kOutH - 0.5, 0.0, kRenderH - 1.0);
            const uint32_t x0 = uint32_t(sx), y0 = uint32_t(sy);
            const uint32_t x1 = std::min(x0 + 1, kRenderW - 1), y1 = std::min(y0 + 1, kRenderH - 1);
            const double fx = sx - x0, fy = sy - y0;
            for (int c = 0; c < 3; ++c) {
                const auto at = [&](uint32_t px, uint32_t py) { return ((scene.Pixel(px, py, 0, 0) >> (8 * c)) & 255) / 255.0; };
                rgb[(size_t(y) * kOutW + x) * 3 + c] = float((at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) +
                    (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy);
            }
        }
    return rgb;
}
double MeanAbsDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    double d = 0;
    for (size_t i = 0; i < a.size(); ++i) d += std::abs(a[i] - b[i]);
    return d / double(a.size());
}

bool Check(bool condition, const char* what)
{
    printf("  %s: %s\n", what, condition ? "PASS" : "FAIL");
    return condition;
}

const gpu::sgsr2::Config kConfig{kRenderW, kRenderH, kOutW, kOutH, 1};
bool gSmaa = false;      // SMAA on the input (Quality), off unless a check turns it on
float gEdgeClip = NAN;   // the controller's default unless a check sets it
bool gSoft = false;      // the Soft filter (WP7.3), with its 0.7 window
gpu::sgsr2::ColorFormat gColorFormat = gpu::sgsr2::ColorFormat::Rgba8;
bool gAntiTrail = false;  // G6: transient history not kept
float gKernelScale = 1.0f;

// Records one frame, submits it and retires it, as the renderer does.
gpu::sgsr2::Status RunFrame(Fixture& f, gpu::sgsr2::Controller& controller, uint64_t frame, bool reset,
    double jx, double jy, bool sameCamera, uint64_t& serial)
{
    const auto attempt = controller.RecordIsolated(*f.sr, kConfig, f.Inputs(frame, reset, jx, jy),
        gpu::sgsr2::FrameValues{0.9f, sameCamera, gSmaa, gEdgeClip, gKernelScale, gSoft, gColorFormat, gAntiTrail}, *f.output);
    if (attempt.status != gpu::sgsr2::Status::Ready) return attempt.status;
    controller.OnBatchSubmitted(attempt.useId, ++serial);
    f.Submit(f.sr.get());
    controller.ReleaseCompletedThrough(serial);
    return attempt.status;
}

int Run(RenderDevice* device)
{
    Fixture f;
    if (!f.Init(device)) { printf("fixture init failed\n"); return 1; }
    gpu::sgsr2::Controller controller;
    bool pass = true;
    uint64_t serial = 0;

    pass &= Check(gpu::sgsr2::RenderSizeFor({1920, 1080}, gpu::upscaling::FsrQuality::Performance).width == 960,
        "performance ratio 2.0");
    pass &= Check(controller.EnsureSession(*device, {kOutW + 1, kOutH, kOutW, kOutH, 1}) ==
        gpu::sgsr2::Status::InputUnavailable, "render larger than output rejected");
    if (!Check(controller.EnsureSession(*device, kConfig) == gpu::sgsr2::Status::Ready, "session ready (shaders, pipelines)"))
        return 1;

    // 1. A flat colour must come back unchanged after a reset and stay so.
    Scene flat{true};
    if (!f.Upload(flat, 0, 0, 0, true)) return 1;
    pass &= Check(RunFrame(f, controller, 1, true, 0.25, -0.25, false, serial) == gpu::sgsr2::Status::Ready, "flat frame recorded");
    auto out = f.Read();
    auto s = Measure(out);
    const double expected[3] = {64 / 255.0, 128 / 255.0, 192 / 255.0};
    bool flatOk = s.finite;
    for (int c = 0; c < 3; ++c) flatOk &= std::abs(s.mean[c] - expected[c]) < 0.01;
    printf("  flat mean %.4f %.4f %.4f (expected %.4f %.4f %.4f) min %.4f max %.4f\n",
        s.mean[0], s.mean[1], s.mean[2], expected[0], expected[1], expected[2], s.min, s.max);
    const auto flatDeviation = [&](const std::vector<float>& rgb) {
        double worst = 0;
        for (size_t i = 0; i < rgb.size(); ++i) worst = std::max(worst, std::abs(rgb[i] - expected[i % 3]));
        return worst;
    };
    printf("  flat worst deviation %.5f\n", flatDeviation(out));
    pass &= Check(flatOk && flatDeviation(out) < 0.006, "flat colour preserved on reset");
    for (uint64_t frame = 2; frame <= 8; ++frame)
        RunFrame(f, controller, frame, false, (frame % 2) ? 0.25 : -0.25, (frame % 3) ? -0.25 : 0.25, true, serial);
    out = f.Read();
    s = Measure(out);
    printf("  flat worst deviation after accumulation %.5f\n", flatDeviation(out));
    pass &= Check(s.finite && flatDeviation(out) < 0.006, "flat colour preserved through accumulation");

    // 2. Jittered static scene: finite, in range, closer to the ideal
    // output-resolution image than a bilinear upscale, closer with the
    // renderer's jitter sign than with the flipped one, and stable once
    // accumulated.
    Scene detail{};
    const auto accumulate = [&](uint64_t firstFrame, float sign, std::vector<float>& last, double& delta) {
        std::vector<float> previous;
        for (uint64_t frame = firstFrame; frame < firstFrame + 32; ++frame) {
            const auto j = gpu::temporal::FrameJitter(frame, kRenderW, kRenderH);
            if (!f.Upload(detail, 0, 0, 0, false, j.pixelX, j.pixelY)) return false;
            if (RunFrame(f, controller, frame, frame == firstFrame, sign * j.pixelX, sign * j.pixelY,
                    frame != firstFrame, serial) != gpu::sgsr2::Status::Ready)
                return false;
            if (frame + 2 >= firstFrame + 32) {
                auto current = f.Read();
                if (!previous.empty()) delta = MeanAbsDiff(previous, current);
                previous = std::move(current);
            }
        }
        last = std::move(previous);
        return true;
    };
    std::vector<float> correct, flipped;
    double delta = 1, flippedDelta = 1;
    pass &= Check(accumulate(9, 1.0f, correct, delta), "jittered frames recorded");
    pass &= Check(accumulate(41, -1.0f, flipped, flippedDelta), "flipped-jitter frames recorded");
    if (std::getenv("LO_SGSR2_TEST_DUMP")) {
        for (uint32_t x = 0; x < kOutW; x += 8) {
            double e = 0;
            for (uint32_t y = 4; y < 20; ++y) e += correct[(size_t(y) * kOutW + x) * 3] - detail.Ideal(x, y, 0);
            printf("  red column %u signed error %.4f\n", x, e / 16);
        }
    }
    s = Measure(correct);
    const auto good = Rmse(detail, correct), bad = Rmse(detail, flipped), base = Rmse(detail, Bilinear(detail));
    printf("  rmse r/g/b: sgsr2 %.4f %.4f %.4f  flipped %.4f %.4f %.4f  bilinear %.4f %.4f %.4f\n",
        good[0], good[1], good[2], bad[0], bad[1], bad[2], base[0], base[1], base[2]);
    printf("  detail min %.4f max %.4f frame_delta %.5f (flipped %.5f)\n", s.min, s.max, delta, flippedDelta);
    pass &= Check(s.finite, "detail output finite");
    pass &= Check(s.min >= 0.0f && s.max <= 1.0f, "detail output in [0, 1]");
    const double goodSum = good[0] + good[1] + good[2], badSum = bad[0] + bad[1] + bad[2];
    pass &= Check(goodSum < base[0] + base[1] + base[2], "sharper than bilinear");
    pass &= Check(goodSum < badSum, "renderer jitter sign beats the flipped sign");
    pass &= Check(delta < 0.02, "static history stable (mean frame-to-frame change)");

    // 2b. SMAA on the input (Quality, G3): a staircase edge, one frame at a
    // time with no history (reset, no jitter), as SGSR2 sees a new scene.
    // Against the area-sampled ideal edge the output must be finite, in
    // range, and smoother than without SMAA; a flat colour stays unchanged.
    {
        // Edge y = 0.45 * x + 8 in render pixels: dark below, light above.
        const auto light = [](double rx, double ry) { return ry < 0.45 * rx + 8.0; };
        f.colorFn = [&](uint32_t x, uint32_t y) {
            const uint32_t v = light(x + 0.5, y + 0.5) ? 220u : 30u;
            return 0xff000000u | (v << 16) | (v << 8) | v;
        };
        f.depthFn = [](uint32_t, uint32_t) { return 0.05f; };
        // Area-sampled ideal at output resolution (8x8 subsamples per pixel).
        std::vector<float> ideal(size_t(kOutW) * kOutH);
        for (uint32_t y = 0; y < kOutH; ++y)
            for (uint32_t x = 0; x < kOutW; ++x) {
                int lit = 0;
                for (int sy = 0; sy < 8; ++sy)
                    for (int sx = 0; sx < 8; ++sx)
                        lit += light((x + (sx + 0.5) / 8.0) * kRenderW / kOutW, (y + (sy + 0.5) / 8.0) * kRenderH / kOutH);
                ideal[size_t(y) * kOutW + x] = float((30 + (220 - 30) * lit / 64.0) / 255.0);
            }
        // Error near the edge only (where the staircase is).
        const auto edgeError = [&](const std::vector<float>& rgb) {
            double sum = 0; size_t count = 0;
            for (uint32_t y = 2; y + 2 < kOutH; ++y)
                for (uint32_t x = 2; x + 2 < kOutW; ++x) {
                    const double ry = (y + 0.5) * kRenderH / kOutH, rx = (x + 0.5) * kRenderW / kOutW;
                    if (std::abs(ry - (0.45 * rx + 8.0)) > 2.0) continue;
                    const double d = rgb[(size_t(y) * kOutW + x) * 3 + 1] - ideal[size_t(y) * kOutW + x];
                    sum += d * d; ++count;
                }
            return std::sqrt(sum / double(std::max<size_t>(count, 1)));
        };
        Scene unused{};
        std::array<double, 2> error{};
        std::array<Stats, 2> stats{};
        for (int run = 0; run < 2; ++run) {
            gSmaa = run == 1;
            if (!f.Upload(unused, 0, 0, 0, false)) return 1;
            pass &= Check(RunFrame(f, controller, 620 + run, true, 0, 0, false, serial) == gpu::sgsr2::Status::Ready,
                run ? "staircase frame with SMAA recorded" : "staircase frame without SMAA recorded");
            const auto out = f.Read();
            error[run] = edgeError(out);
            stats[run] = Measure(out);
        }
        printf("  staircase edge rmse: without SMAA %.4f, with SMAA %.4f (min %.4f max %.4f)\n",
            error[0], error[1], stats[1].min, stats[1].max);
        pass &= Check(stats[1].finite && stats[1].min >= 0.0f && stats[1].max <= 1.0f, "SMAA output finite and in [0, 1]");
        pass &= Check(error[1] < error[0] * 0.9, "SMAA brings the staircase closer to the area-sampled edge");
        f.colorFn = nullptr; f.depthFn = nullptr;
        gSmaa = true;
        if (!f.Upload(flat, 0, 0, 0, false)) return 1;
        for (uint64_t frame = 650; frame < 654; ++frame) RunFrame(f, controller, frame, frame == 650, 0.25, -0.25, true, serial);
        pass &= Check(flatDeviation(f.Read()) < 0.006, "SMAA leaves a flat colour unchanged");
        // The detail scene with SMAA stays as close to the ideal as without.
        std::vector<float> smoothed;
        double smoothedDelta = 1;
        pass &= Check(accumulate(660, 1.0f, smoothed, smoothedDelta), "jittered frames with SMAA recorded");
        const auto withSmaa = Rmse(detail, smoothed);
        printf("  detail rmse with SMAA r/g/b %.4f %.4f %.4f (without %.4f %.4f %.4f) frame_delta %.5f\n",
            withSmaa[0], withSmaa[1], withSmaa[2], good[0], good[1], good[2], smoothedDelta);
        // SMAA reads the above-Nyquist green pattern as edges and blends part
        // of what the jitter would recover (measured +30% rmse on green, red
        // and blue edges better): the softer Quality the player asked for,
        // bounded here so a regression in the pipeline still fails.
        pass &= Check(withSmaa[0] + withSmaa[1] + withSmaa[2] < goodSum * 1.35, "SMAA keeps most of the accumulated detail");
        pass &= Check(withSmaa[0] <= good[0] * 1.05 && withSmaa[2] <= good[2] * 1.3, "SMAA leaves gradients and hard edges close");
        gSmaa = false;

        // 2b-ii. SMAA on a float SDR input (a 16-bit scene target): before
        // 2.0 only RGBA8 inputs had SMAA. Same staircase, float colour.
        f.colorFn = nullptr;
        f.depthFn = [](uint32_t, uint32_t) { return 0.05f; };
        f.floatSdr = true;
        gColorFormat = gpu::sgsr2::ColorFormat::Rgba16f;
        std::array<double, 2> floatError{};
        std::array<Stats, 2> floatStats{};
        for (int run = 0; run < 2; ++run) {
            gSmaa = run == 1;
            if (!f.Upload(unused, 0, 0, 0, false) ||
                !f.UploadFloatSdr([&](uint32_t x, uint32_t y) { return (light(x + 0.5, y + 0.5) ? 220 : 30) / 255.0f; }))
                return 1;
            pass &= Check(RunFrame(f, controller, 700 + run, true, 0, 0, false, serial) == gpu::sgsr2::Status::Ready,
                run ? "float staircase with SMAA recorded" : "float staircase without SMAA recorded");
            const auto out = f.Read();
            floatError[run] = edgeError(out);
            floatStats[run] = Measure(out);
        }
        printf("  float staircase edge rmse: without SMAA %.4f, with SMAA %.4f\n", floatError[0], floatError[1]);
        pass &= Check(gpu::sgsr2::LastStatus().smaaActive, "SMAA reported active on a float SDR input");
        pass &= Check(floatStats[1].finite && floatStats[1].min >= 0.0f && floatStats[1].max <= 1.0f,
            "float-input SMAA output finite and in [0, 1]");
        pass &= Check(floatError[1] < floatError[0] * 0.9, "SMAA smooths the float staircase like the 8-bit one");
        pass &= Check(std::abs(floatError[0] - error[0]) < 0.01, "float and 8-bit inputs agree without SMAA");
        gSmaa = false;
        f.floatSdr = false;
        gColorFormat = gpu::sgsr2::ColorFormat::Rgba8;

        // 2b-iii. Soft filter (WP7.3): no ring around a hard edge. With no
        // history (reset) each output pixel is the kernel-weighted mix of
        // its render neighbours: the Standard (Lanczos-shaped) weight has a
        // negative lobe and can push a pixel past either plateau, the Soft
        // Gaussian cannot.
        const float darkV = 30 / 255.0f, lightV = 220 / 255.0f;
        std::array<double, 2> overshoot{};
        std::array<double, 2> stepError{};
        for (int run = 0; run < 2; ++run) {
            gSoft = run == 1;
            gKernelScale = gSoft ? 0.7f : 1.0f;
            f.colorFn = [&](uint32_t x, uint32_t y) {
                const uint32_t v = light(x + 0.5, y + 0.5) ? 220u : 30u;
                return 0xff000000u | (v << 16) | (v << 8) | v;
            };
            if (!f.Upload(unused, 0, 0, 0, false)) return 1;
            pass &= Check(RunFrame(f, controller, 720 + run, true, 0, 0, false, serial) == gpu::sgsr2::Status::Ready,
                run ? "edge frame with the Soft filter recorded" : "edge frame with the Standard filter recorded");
            const auto out = f.Read();
            const auto st = Measure(out);
            pass &= Check(st.finite, run ? "Soft output finite" : "Standard output finite");
            double worst = 0;
            for (size_t i = 1; i < out.size(); i += 3)
                worst = std::max({worst, double(out[i] - lightV), double(darkV - out[i])});
            overshoot[run] = worst;
            stepError[run] = edgeError(out);
        }
        printf("  edge overshoot past the plateaus: Standard %.4f, Soft %.4f; edge rmse Standard %.4f, Soft %.4f\n",
            overshoot[0], overshoot[1], stepError[0], stepError[1]);
        pass &= Check(overshoot[1] <= 0.002, "the Soft filter adds no halo past either side of an edge");
        pass &= Check(overshoot[1] <= overshoot[0] + 1e-4, "the Soft filter rings no more than Standard");
        pass &= Check(gpu::sgsr2::LastStatus().soft, "Soft filter reported in the status");
        gSoft = false;
        gKernelScale = 1.0f;
        f.colorFn = nullptr; f.depthFn = nullptr;
        // Soft keeps a flat colour flat through accumulation.
        gSoft = true; gKernelScale = 0.7f;
        if (!f.Upload(flat, 0, 0, 0, false)) return 1;
        for (uint64_t frame = 740; frame < 746; ++frame) RunFrame(f, controller, frame, frame == 740, 0.25, -0.25, true, serial);
        pass &= Check(flatDeviation(f.Read()) < 0.006, "the Soft filter keeps a flat colour");
        gSoft = false; gKernelScale = 1.0f;
    }

    // 2b-iv. G6: a bright particle moving over still ground without motion
    // vectors (camera still), as the battle effects do. Without the
    // anti-trail rule the history keeps it where it was; with it the trail
    // is gone, and a still detailed scene keeps its accumulated detail.
    {
        const auto particleAt = [](int frame) { return 10 + frame * 4; };
        std::array<double, 2> trail{};
        for (int run = 0; run < 2; ++run) {
            gAntiTrail = run == 1;
            int last = 0;
            for (int frame = 0; frame < 9; ++frame) {
                const int px = particleAt(frame);
                f.colorFn = [px](uint32_t x, uint32_t y) {
                    const bool particle = int(x) >= px && int(x) < px + 3 && y >= 16 && y < 19;
                    const uint32_t v = particle ? 250u : 60u;
                    return 0xff000000u | (v << 16) | (v << 8) | v;
                };
                f.depthFn = [](uint32_t, uint32_t) { return 0.05f; };
                if (!f.Upload(flat, 0, 0, 0, false, 0, 0)) return 1;
                const double jx = (frame & 1) ? 0.25 : -0.25, jy = (frame & 2) ? 0.25 : -0.25;
                RunFrame(f, controller, 760 + run * 20 + frame, frame == 0, jx, jy, true, serial);
                last = px;
            }
            const auto out = f.Read();
            // Where the particle was two or more frames ago (render x < last - 4),
            // in output pixels, the ground should be back to 60/255.
            double worst = 0;
            for (uint32_t y = 16 * kOutH / kRenderH; y < 19 * kOutH / kRenderH; ++y)
                for (uint32_t x = uint32_t(particleAt(0)) * kOutW / kRenderW; x < uint32_t(last - 4) * kOutW / kRenderW; ++x)
                    worst = std::max(worst, double(out[(size_t(y) * kOutW + x) * 3 + 1]) - 60 / 255.0);
            trail[run] = worst;
        }
        printf("  particle trail on still ground: without anti-trail %.4f, with %.4f\n", trail[0], trail[1]);
        pass &= Check(trail[1] < 0.03, "no visible trail behind a particle without motion vectors");
        pass &= Check(trail[1] <= trail[0], "the anti-trail rule never leaves more trail than the reference box");
        f.colorFn = nullptr; f.depthFn = nullptr;
        // A still detailed scene keeps what the jitter accumulates: the same
        // frame sequence with and without the rule.
        std::vector<float> kept, reference;
        double keptDelta = 1, referenceDelta = 1;
        gAntiTrail = false;
        pass &= Check(accumulate(800, 1.0f, reference, referenceDelta), "jittered reference frames recorded");
        gAntiTrail = true;
        pass &= Check(accumulate(800, 1.0f, kept, keptDelta), "jittered frames with anti-trail recorded");
        const auto withRule = Rmse(detail, kept), without = Rmse(detail, reference);
        printf("  detail rmse with anti-trail r/g/b %.4f %.4f %.4f (same frames without %.4f %.4f %.4f)\n",
            withRule[0], withRule[1], withRule[2], without[0], without[1], without[2]);
        pass &= Check(withRule[0] + withRule[1] + withRule[2] <= (without[0] + without[1] + without[2]) * 1.05,
            "anti-trail keeps the accumulated detail (within 5% of the reference box)");
        pass &= Check(keptDelta < 0.03, "anti-trail keeps a still image stable");
        gAntiTrail = false;
    }

    // 2c. Thin railings and a moving character, as on the device.
    // Railings: one-render-pixel posts nearer than the wall, jittered, still
    // camera. Weighting down the reference's depth-edge clip must bring the
    // posts closer to the ideal output (anti-aliasing on thin geometry).
    {
        // Posts every 8 output pixels, 2.6 output (1.3 render) pixels wide.
        const auto post = [](double ox) { const double m = std::fmod(ox, 8.0); return m >= 2.0 && m < 4.6; };
        const auto railColor = [&](double ox, double oy) { (void)oy; return post(ox) ? 0.85 : 0.15; };
        std::array<double, 2> rmse{};
        int run = 0;
        for (const float edge : {1.0f, 0.35f}) {
            gEdgeClip = edge;
            uint64_t base = 700 + 100 * run;
            double jx = 0, jy = 0;
            f.colorFn = [&](uint32_t x, uint32_t y) {
                const double ox = (x + 0.5 - jx) * kOutW / kRenderW, oy = (y + 0.5 - jy) * kOutH / kRenderH;
                const uint32_t v = uint32_t(std::lround(railColor(ox, oy) * 255));
                return 0xff000000u | (v << 16) | (v << 8) | v;
            };
            f.depthFn = [&](uint32_t x, uint32_t) { return post((x + 0.5 - jx) * kOutW / kRenderW) ? 0.3f : 0.05f; };
            for (uint64_t frame = base; frame < base + 32; ++frame) {
                const auto j = gpu::temporal::FrameJitter(frame, kRenderW, kRenderH);
                jx = j.pixelX; jy = j.pixelY;
                f.Upload(detail, 0, 0, 0, false);
                RunFrame(f, controller, frame, frame == base, j.pixelX, j.pixelY, frame != base, serial);
            }
            const auto out = f.Read();
            double sum = 0; size_t count = 0;
            for (uint32_t y = 2; y + 2 < kOutH; ++y)
                for (uint32_t x = 2; x + 2 < kOutW; ++x, ++count) {
                    const double ideal = std::lround(railColor(x + 0.5, y + 0.5) * 255) / 255.0;
                    const double d = out[(size_t(y) * kOutW + x) * 3 + 1] - ideal;
                    sum += d * d;
                }
            if (std::getenv("LO_SGSR2_TEST_DUMP")) {
                printf("  rail row:");
                for (uint32_t x = 0; x < 48; ++x) printf(" %.2f", out[(size_t(36) * kOutW + x) * 3 + 1]);
                printf("\n");
            }
            rmse[run++] = std::sqrt(sum / double(count));
        }
        printf("  railings rmse: edge clip 1 (default) %.4f, 0.35 %.4f\n", rmse[0], rmse[1]);
        pass &= Check(rmse[0] < 0.12 && rmse[1] < 0.12, "1.3-pixel railings resolve on a still camera");

        // A character: a bright box nearer than the wall moving right 1.5
        // render pixels a frame, with camera-only (zero) motion vectors.
        // Where it was, the wall must come back without a ghost.
        run = 0;
        std::array<double, 2> ghost{};
        for (const float edge : {1.0f, 0.35f}) {
            gEdgeClip = edge;
            uint64_t base = 900 + 100 * run;
            double boxX = 4;
            f.colorFn = [&](uint32_t x, uint32_t y) {
                const bool inBox = x >= boxX && x < boxX + 10 && y >= 10 && y < 26;
                const uint32_t v = inBox ? 240 : 40;
                return 0xff000000u | (v << 16) | (v << 8) | v;
            };
            f.depthFn = [&](uint32_t x, uint32_t y) { return x >= boxX && x < boxX + 10 && y >= 10 && y < 26 ? 0.3f : 0.05f; };
            for (uint64_t frame = base; frame < base + 20; ++frame) {
                boxX = 4 + 1.5 * double(frame - base);
                f.Upload(detail, 0, 0, 0, false);
                RunFrame(f, controller, frame, frame == base, 0, 0, frame != base, serial);
            }
            const auto out = f.Read();
            // Wall pixels the box covered two to four frames ago (nearer ones
            // are inside the Lanczos footprint of its current edge).
            double worst = 0;
            const double boxOut = boxX * kOutW / kRenderW;
            for (uint32_t y = 2 * 12; y < 2 * 24; ++y)
                for (uint32_t x = uint32_t(boxOut - 2 * 6); x < uint32_t(boxOut - 2 * 2.5); ++x)
                    worst = std::max(worst, double(std::abs(out[(size_t(y) * kOutW + x) * 3 + 1] - 40 / 255.0)));
            if (std::getenv("LO_SGSR2_TEST_DUMP")) {
                printf("  row 36 (box out x %.1f..%.1f):", boxOut, boxOut + 20);
                for (uint32_t x = 0; x < kOutW; x += 2) printf(" %.2f", out[(size_t(36) * kOutW + x) * 3 + 1]);
                printf("\n");
            }
            ghost[run++] = worst;
        }
        printf("  moving character trail, worst error: edge clip 1 (default) %.4f, 0.35 %.4f\n", ghost[0], ghost[1]);
        pass &= Check(ghost[0] < 0.1 && ghost[1] < 0.1, "a moving character leaves no ghost trail");
        f.colorFn = nullptr; f.depthFn = nullptr;
        gEdgeClip = NAN;
    }

    // 3. Content panning right and down by (0.75, 0.25) render pixels a
    // frame. The renderer's vectors are backward (previous = current +
    // motion), so (-0.75, -0.25) must track it; the opposite sign must not.
    const auto pan = [&](uint64_t firstFrame, float sign) {
        const double vx = 0.75, vy = 0.25;
        double dx = 0, dy = 0;
        for (uint64_t frame = firstFrame; frame < firstFrame + 24; ++frame) {
            dx = vx * double(frame - firstFrame); dy = vy * double(frame - firstFrame);
            const auto j = gpu::temporal::FrameJitter(frame, kRenderW, kRenderH);
            f.Upload(detail, sign * float(-vx), sign * float(-vy), 0, false, j.pixelX + dx, j.pixelY + dy);
            RunFrame(f, controller, frame, frame == firstFrame, j.pixelX, j.pixelY, false, serial);
        }
        return Rmse(detail, f.Read(), dx, dy);
    };
    const auto tracked = pan(100, 1.0f), reversed = pan(200, -1.0f);
    printf("  pan rmse r/g/b: tracked %.4f %.4f %.4f  reversed %.4f %.4f %.4f\n",
        tracked[0], tracked[1], tracked[2], reversed[0], reversed[1], reversed[2]);
    pass &= Check(tracked[0] + tracked[1] + tracked[2] < 0.75 * (reversed[0] + reversed[1] + reversed[2]),
        "backward motion vectors track a pan");
    // Detail above the render Nyquist limit is not expected to survive a pan
    // (the history is resampled bilinearly each frame): printed, not checked.

    // 3b. Moving camera with an invalid band: still finite and in range.
    if (!f.Upload(detail, 3.5f, -1.25f, 8, false)) return 1;
    for (uint64_t frame = 301; frame <= 308; ++frame) {
        const auto j = gpu::temporal::FrameJitter(frame, kRenderW, kRenderH);
        RunFrame(f, controller, frame, false, j.pixelX, j.pixelY, false, serial);
    }
    s = Measure(f.Read());
    printf("  motion min %.4f max %.4f\n", s.min, s.max);
    pass &= Check(s.finite && s.min >= -0.01f && s.max <= 1.05f, "motion + invalidity output finite and in range");

    // 3c. HDR input keeps the reference tone map: a flat linear colour above
    // 1 comes back within 1%.
    {
        const float hdrColor[3] = {2.0f, 0.5f, 0.25f};
        if (!f.UploadHdr(hdrColor) || !f.Upload(flat, 0, 0, 0, false)) return 1;
        f.hdr = true;
        for (uint64_t frame = 400; frame < 404; ++frame)
            RunFrame(f, controller, frame, frame == 400, 0.25, -0.25, true, serial);
        f.hdr = false;
        out = f.Read();
        double worst = 0;
        for (size_t i = 0; i < out.size(); ++i)
            worst = std::max(worst, double(std::abs(out[i] - hdrColor[i % 3]) / hdrColor[i % 3]));
        printf("  hdr pixel %.4f %.4f %.4f worst relative error %.4f\n", out[0], out[1], out[2], worst);
        pass &= Check(worst < 0.01, "HDR colour preserved through the tone map");
    }

    // 4. Lifecycle: slots run out while batches are unretired, a resize waits
    // for them, and discarding a batch forces a reset without breaking state.
    std::vector<gpu::sgsr2::Attempt> pending;
    gpu::sgsr2::Status last = gpu::sgsr2::Status::Ready;
    for (uint64_t frame = 501; frame <= 505; ++frame) {
        auto attempt = controller.RecordIsolated(*f.sr, kConfig, f.Inputs(frame, false, 0, 0),
            gpu::sgsr2::FrameValues{0.9f, true}, *f.output);
        last = attempt.status;
        if (attempt.status == gpu::sgsr2::Status::Ready) pending.push_back(attempt);
    }
    pass &= Check(pending.size() == 4 && last == gpu::sgsr2::Status::InputUnavailable, "four slots, fifth frame skipped");
    pass &= Check(controller.EnsureSession(*device, {kRenderW / 2, kRenderH / 2, kOutW, kOutH, 1}) ==
        gpu::sgsr2::Status::NeedsReconfigure, "resize waits for pending batches");
    for (const auto& attempt : pending) controller.OnBatchDiscarded(attempt.useId);
    pass &= Check(RunFrame(f, controller, 506, false, 0, 0, true, serial) == gpu::sgsr2::Status::Ready, "recovers after discard");
    s = Measure(f.Read());
    pass &= Check(s.finite && s.max <= 1.05f, "post-discard output finite");
    pass &= Check(controller.EnsureSession(*device, {kRenderW / 2, kRenderH / 2, kOutW, kOutH, 1}) ==
        gpu::sgsr2::Status::Ready, "resize after retirement");
    // The renderer's resize: release after its drain, then a session at the other size.
    controller.ReleaseFeatureAfterGpuDrain();
    pass &= Check(!controller.HasFeatureState(), "release leaves no feature state");
    pass &= Check(controller.EnsureSession(*device, {kOutW, kOutH, kOutW, kOutH, 1}) == gpu::sgsr2::Status::Ready,
        "session at the output size after a release (pipelines kept)");
    controller.ReleaseFeatureAfterGpuDrain();
    pass &= Check(controller.EnsureSession(*device, {kRenderW, kRenderH, kOutW, kOutH, 1}) == gpu::sgsr2::Status::Ready,
        "and back at the render size");
    controller.ShutdownAfterGpuDrain();
    pass &= Check(!controller.HasFeatureState(), "shutdown releases state");

    printf("SGSR2 controller: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
} // namespace

int main()
{
    auto api = plume::CreateVulkanInterface();
    if (!api) { printf("no Vulkan\n"); return 2; }
    auto device = api->createDevice();
    if (!device) { printf("no Vulkan device\n"); return 2; }
    printf("Backend: Vulkan on %s\n", device->getDescription().name.c_str());
    return Run(device.get());
}
