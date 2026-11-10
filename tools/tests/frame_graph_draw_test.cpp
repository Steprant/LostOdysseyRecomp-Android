// Steprant 2.0, Super Roadmap 4.0 G-2 1c on a real Vulkan device (lavapipe):
// random frames with draws, checked against the same frames recorded
// directly.
//
// The pass sequence test has clears and copies only. Here the frames also
// draw: rectangles blended over a target (the blend reads the attachment),
// rectangles that sample another target through a descriptor set (a read the
// frame graph only sees through the set), and depth-tested rectangles on a
// colour + depth framebuffer whose pass may end on a barrier and resume (the
// depth of the resumed pass decides which colour lands). Each configuration
// runs in its own process; the frame graph runs with every discarded
// attachment cleared to magenta (or depth 0.25) after its pass, so a store it
// wrongly thought dead changes the bytes. Every target of every frame must
// equal the direct recording byte for byte, and the frame graph must have
// discarded something. LO_FRAME_GRAPH_DRAW_SEEDS sets the number of frames.
#include <plume_render_interface.h>
#include <stdafx.h>
#include <gpu/shader/dxc_compiler.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }

namespace {
using namespace plume;
constexpr uint32_t kSize = 32;
constexpr int kColors = 3;
constexpr int kOps = 40;

constexpr char kVs[] = R"(
struct Push { float4 rect; float4 color; float depth; uint mode; };
[[vk::push_constant]] ConstantBuffer<Push> push;
float4 main(uint id : SV_VertexID) : SV_Position
{
    const uint corner[6] = { 0, 1, 2, 2, 1, 3 };
    const uint c = corner[id];
    const float2 p = float2((c & 1) ? push.rect.z : push.rect.x, (c & 2) ? push.rect.w : push.rect.y);
    return float4(p / 32.0 * 2.0 - 1.0, push.depth, 1.0);
}
)";
constexpr char kColorPs[] = R"(
struct Push { float4 rect; float4 color; float depth; uint mode; };
[[vk::push_constant]] ConstantBuffer<Push> push;
float4 main(float4 position : SV_Position) : SV_Target0
{
    return push.color;
}
)";
constexpr char kSamplePs[] = R"(
struct Push { float4 rect; float4 color; float depth; uint mode; };
[[vk::push_constant]] ConstantBuffer<Push> push;
[[vk::binding(0, 0)]] Texture2D<float4> source;
float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 p = int2(position.xy);
    return source.Load(int3(31 - p.x, p.y, 0)) * push.color;
}
)";

struct Push { float rect[4]; float color[4]; float depth; uint32_t mode; uint32_t pad[2]; };

std::unique_ptr<RenderShader> Compile(RenderDevice* device, const char* source, const char* profile)
{
    auto compiled = xenos::CompileCachedHlsl(source, "main", profile, xenos::ShaderBinaryFormat::Spirv);
    if (!compiled.ok) { std::printf("compile %s: %s\n", profile, compiled.errors.c_str()); return nullptr; }
    return device->createShader(compiled.bytecode.data(), compiled.bytecode.size(), "main", RenderShaderFormat::SPIRV);
}

// Records and runs the frames; returns the bytes of every target of every
// frame, or empty on a failure.
std::vector<uint8_t> RunFrames(uint32_t seeds, uint64_t& deadStores)
{
    auto api = CreateVulkanInterface();
    if (!api) return {};
    auto device = api->createDevice();
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto list = queue->createCommandList();
    auto fence = device->createCommandFence();
    std::array<std::unique_ptr<RenderTexture>, kColors> colors;
    std::array<std::unique_ptr<RenderFramebuffer>, kColors> colorFbs;
    std::array<std::unique_ptr<RenderDescriptorSet>, kColors> sets;
    std::array<std::unique_ptr<RenderBuffer>, kColors + 1> readbacks;
    RenderDescriptorSetBuilder setBuilder;
    setBuilder.begin(); setBuilder.addTexture(0); setBuilder.end();
    for (int i = 0; i < kColors; ++i) {
        colors[i] = device->createTexture(RenderTextureDesc::Texture2D(kSize, kSize, 1, RenderFormat::R8G8B8A8_UNORM, RenderTextureFlag::RENDER_TARGET));
        const RenderTexture* attachments[] = {colors[i].get()};
        colorFbs[i] = device->createFramebuffer(RenderFramebufferDesc(attachments, 1));
        sets[i] = setBuilder.create(device.get());
        sets[i]->setTexture(0, colors[i].get(), RenderTextureLayout::SHADER_READ);
        readbacks[i] = device->createBuffer(RenderBufferDesc::ReadbackBuffer(kSize * kSize * 4));
    }
    auto depth = device->createTexture(RenderTextureDesc::DepthTarget(kSize, kSize, RenderFormat::D32_FLOAT));
    readbacks[kColors] = device->createBuffer(RenderBufferDesc::ReadbackBuffer(kSize * kSize * 4));
    const RenderTexture* depthColor[] = {colors[0].get()};
    auto depthFb = device->createFramebuffer(RenderFramebufferDesc(depthColor, 1, depth.get()));
    RenderPipelineLayoutBuilder layoutBuilder;
    layoutBuilder.begin(false, false);
    layoutBuilder.addPushConstant(0, 0, sizeof(Push), RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
    layoutBuilder.addDescriptorSet(setBuilder);
    layoutBuilder.end();
    auto layout = layoutBuilder.create(device.get());
    auto vs = Compile(device.get(), kVs, "vs_6_0");
    auto colorPs = Compile(device.get(), kColorPs, "ps_6_0");
    auto samplePs = Compile(device.get(), kSamplePs, "ps_6_0");
    if (!vs || !colorPs || !samplePs || !layout) return {};
    auto pipeline = [&](RenderShader* ps, bool withDepth) {
        RenderGraphicsPipelineDesc desc;
        desc.pipelineLayout = layout.get();
        desc.vertexShader = vs.get();
        desc.pixelShader = ps;
        desc.renderTargetFormat[0] = RenderFormat::R8G8B8A8_UNORM;
        desc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
        desc.renderTargetCount = 1;
        desc.cullMode = RenderCullMode::NONE;
        desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        if (withDepth) {
            desc.depthTargetFormat = RenderFormat::D32_FLOAT;
            desc.depthEnabled = true;
            desc.depthWriteEnabled = true;
            desc.depthFunction = RenderComparisonFunction::LESS;
        }
        return device->createGraphicsPipeline(desc);
    };
    auto colorPipeline = pipeline(colorPs.get(), false);
    auto samplePipeline = pipeline(samplePs.get(), false);
    auto depthPipeline = pipeline(colorPs.get(), true);
    if (!colorPipeline || !samplePipeline || !depthPipeline) return {};

    std::vector<uint8_t> out;
    deadStores = 0;
    for (uint32_t seed = 1; seed <= seeds; ++seed) {
        std::mt19937 rng(seed * 7919u);
        std::array<RenderTextureLayout, kColors> layoutOf{};
        RenderTextureLayout depthLayout = RenderTextureLayout::UNKNOWN;
        const RenderFramebuffer* bound = nullptr;
        int boundIndex = -1; // 0..2 colour fbs, 3 the depth fb
        auto barrier = [&](int t, RenderTextureLayout to) {
            if (layoutOf[t] == to) return;
            list->barriers(to == RenderTextureLayout::COPY_SOURCE || to == RenderTextureLayout::COPY_DEST ? RenderBarrierStage::COPY : RenderBarrierStage::GRAPHICS,
                RenderTextureBarrier(colors[t].get(), to));
            layoutOf[t] = to;
        };
        auto bind = [&](int which) {
            if (which == 3) {
                barrier(0, RenderTextureLayout::COLOR_WRITE);
                if (depthLayout != RenderTextureLayout::DEPTH_WRITE) {
                    list->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(depth.get(), RenderTextureLayout::DEPTH_WRITE));
                    depthLayout = RenderTextureLayout::DEPTH_WRITE;
                }
                bound = depthFb.get();
            } else {
                barrier(which, RenderTextureLayout::COLOR_WRITE);
                bound = colorFbs[which].get();
            }
            list->setFramebuffer(bound);
            boundIndex = which;
            const RenderViewport viewport(0, 0, kSize, kSize);
            const RenderRect scissor(0, 0, kSize, kSize);
            list->setViewports(&viewport, 1);
            list->setScissors(&scissor, 1);
        };
        auto randomRect = [&](float* r) {
            const int x0 = int(rng() % kSize), y0 = int(rng() % kSize);
            r[0] = float(x0); r[1] = float(y0);
            r[2] = float(x0 + 1 + int(rng() % (kSize - x0))); r[3] = float(y0 + 1 + int(rng() % (kSize - y0)));
        };
        auto randomColor = [&](float* c) { for (int i = 0; i < 4; ++i) c[i] = float(51 * (rng() % 6)) / 255.0f; };
        auto attached = [&](int t) { return (boundIndex == t) || (boundIndex == 3 && t == 0); };

        list->setPassProfiling(true);
        list->begin();
        list->setGraphicsPipelineLayout(layout.get());
        for (int t = 0; t < kColors; ++t) {
            layoutOf[t] = RenderTextureLayout::UNKNOWN;
            bind(t);
            float c[4]; randomColor(c);
            list->clearColor(0, RenderColor(c[0], c[1], c[2], c[3]));
        }
        bind(3);
        list->clearDepthStencil(true, false, 1.0f, 0);
        for (int op = 0; op < kOps; ++op) {
            const uint32_t kind = rng() % 12;
            if (kind < 2) {
                bind(int(rng() % 4));
            } else if (kind < 4) {
                if (boundIndex < 0) bind(int(rng() % 4));
                float c[4]; randomColor(c);
                if (rng() % 3 == 0) list->clearColor(0, RenderColor(c[0], c[1], c[2], c[3]));
                else {
                    float r[4]; randomRect(r);
                    const RenderRect rect{int32_t(r[0]), int32_t(r[1]), int32_t(r[2]), int32_t(r[3])};
                    list->clearColor(0, RenderColor(c[0], c[1], c[2], c[3]), &rect, 1);
                }
                if (boundIndex == 3 && rng() % 2) list->clearDepthStencil(true, false, 1.0f, 0);
            } else if (kind < 7) {
                if (boundIndex < 0) bind(int(rng() % 4));
                Push push{};
                randomRect(push.rect); randomColor(push.color);
                push.depth = float(rng() % 8) / 8.0f;
                list->setPipeline(boundIndex == 3 ? depthPipeline.get() : colorPipeline.get());
                list->setGraphicsPushConstants(0, &push);
                list->drawInstanced(6, 1, 0, 0);
            } else if (kind < 9) {
                // Sample another target, mirrored.
                if (boundIndex < 0) bind(int(rng() % 3));
                if (boundIndex == 3) continue;
                int source = int(rng() % kColors);
                if (attached(source)) continue;
                barrier(source, RenderTextureLayout::SHADER_READ);
                Push push{};
                randomRect(push.rect); randomColor(push.color);
                list->setPipeline(samplePipeline.get());
                list->setGraphicsDescriptorSet(sets[source].get(), 0);
                list->setGraphicsPushConstants(0, &push);
                list->drawInstanced(6, 1, 0, 0);
            } else if (kind < 10) {
                // A copy between two targets that are not bound.
                const int src = int(rng() % kColors);
                const int dst = (src + 1 + int(rng() % (kColors - 1))) % kColors;
                if (attached(src) || attached(dst)) continue;
                barrier(src, RenderTextureLayout::COPY_SOURCE);
                barrier(dst, RenderTextureLayout::COPY_DEST);
                const RenderBox box(0, 0, int32_t(kSize / 2), int32_t(kSize / 2));
                list->copyTextureRegion(RenderTextureCopyLocation::Subresource(colors[dst].get()), RenderTextureCopyLocation::Subresource(colors[src].get()),
                    kSize / 2, kSize / 2, 0, &box);
                barrier(src, RenderTextureLayout::SHADER_READ);
                barrier(dst, RenderTextureLayout::SHADER_READ);
            } else if (kind < 11) {
                // A barrier on a target that is not bound (it may end the pass).
                const int t = int(rng() % kColors);
                if (!attached(t)) barrier(t, rng() % 2 ? RenderTextureLayout::SHADER_READ : RenderTextureLayout::COLOR_WRITE);
            } else {
                list->setFramebuffer(nullptr);
                bound = nullptr;
                boundIndex = -1;
            }
        }
        list->setFramebuffer(nullptr);
        for (int t = 0; t < kColors; ++t) {
            barrier(t, RenderTextureLayout::COPY_SOURCE);
            list->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readbacks[t].get(), RenderFormat::R8G8B8A8_UNORM, kSize, kSize, 1, kSize),
                RenderTextureCopyLocation::Subresource(colors[t].get()));
        }
        list->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(depth.get(), RenderTextureLayout::COPY_SOURCE));
        list->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readbacks[kColors].get(), RenderFormat::D32_FLOAT, kSize, kSize, 1, kSize),
            RenderTextureCopyLocation::Subresource(depth.get()));
        list->end();
        const RenderCommandList* lists[] = {list.get()};
        queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
        queue->waitForCommandFence(fence.get());
        RenderPassProfile profile;
        if (list->takePassProfile(profile)) deadStores += profile.frameGraphDeadStores;
        for (auto& readback : readbacks) {
            const auto* bytes = static_cast<const uint8_t*>(readback->map());
            out.insert(out.end(), bytes, bytes + kSize * kSize * 4);
            readback->unmap();
        }
    }
    return out;
}
}

int main()
{
    const char* count = std::getenv("LO_FRAME_GRAPH_DRAW_SEEDS");
    const uint32_t seeds = count ? uint32_t(std::strtoul(count, nullptr, 10)) : 400;
    struct Config { const char* name; std::vector<std::pair<const char*, const char*>> env; };
    const Config configs[] = {
        {"direct", {{"LO_VK_FRAME_GRAPH", "0"}}},
        {"frame graph, poisoned discards", {{"LO_VK_FRAME_GRAPH", "1"}, {"LO_VK_FRAME_GRAPH_POISON", "1"}}},
        {"frame graph, poisoned, no deferral", {{"LO_VK_FRAME_GRAPH", "1"}, {"LO_VK_FRAME_GRAPH_POISON", "1"}, {"LO_VK_DEFER_COPY", "0"}}},
        {"frame graph, poisoned, full render area", {{"LO_VK_FRAME_GRAPH", "1"}, {"LO_VK_FRAME_GRAPH_POISON", "1"}, {"LO_VK_RENDER_AREA", "0"}}},
    };
    std::vector<std::vector<uint8_t>> results;
    int failed = 0;
    for (const auto& config : configs) {
        int pipe[2];
        if (::pipe(pipe) != 0) return 1;
        std::fflush(stdout);
        const pid_t child = fork();
        if (child == 0) {
            ::close(pipe[0]);
            for (const auto& [key, value] : config.env) setenv(key, value, 1);
            uint64_t dead = 0;
            const auto bytes = RunFrames(seeds, dead);
            std::printf("[%s] %u frames, %llu dead stores\n", config.name, seeds, (unsigned long long)dead);
            std::fflush(stdout);
            const uint64_t header[2] = {bytes.size(), dead};
            ssize_t ignored = ::write(pipe[1], header, sizeof(header));
            for (size_t done = 0; done < bytes.size();) {
                const ssize_t n = ::write(pipe[1], bytes.data() + done, bytes.size() - done);
                if (n <= 0) break;
                done += size_t(n);
            }
            (void)ignored;
            ::close(pipe[1]);
            _exit(bytes.empty() ? 77 : 0);
        }
        ::close(pipe[1]);
        uint64_t header[2] = {};
        std::vector<uint8_t> bytes;
        if (::read(pipe[0], header, sizeof(header)) == ssize_t(sizeof(header))) {
            bytes.resize(header[0]);
            for (size_t done = 0; done < bytes.size();) {
                const ssize_t n = ::read(pipe[0], bytes.data() + done, bytes.size() - done);
                if (n <= 0) break;
                done += size_t(n);
            }
        }
        ::close(pipe[0]);
        int status = 0;
        waitpid(child, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 77) { std::printf("SKIP: no Vulkan or no shader compiler\n"); return 0; }
        if (!results.empty()) {
            const auto& reference = results.front();
            if (bytes != reference) {
                size_t first = 0;
                while (first < bytes.size() && first < reference.size() && bytes[first] == reference[first]) ++first;
                const size_t perFrame = (kColors + 1) * kSize * kSize * 4;
                std::printf("FAIL [%s]: differs from the direct recording at frame %zu target %zu byte %zu\n", config.name,
                    first / perFrame + 1, (first % perFrame) / (kSize * kSize * 4), first % (kSize * kSize * 4));
                ++failed;
            }
            if (header[1] == 0) { std::printf("FAIL [%s]: no dead store to test\n", config.name); ++failed; }
        }
        results.push_back(std::move(bytes));
    }
    if (failed) { std::printf("FAIL: %d configuration(s)\n", failed); return 1; }
    std::printf("PASS: frames with draws match the direct recording with every frame graph discard poisoned\n");
    return 0;
}
