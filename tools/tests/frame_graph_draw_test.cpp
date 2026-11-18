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
//
// Steprant 2.1.5: the frame graph with its replay recorded in parallel, the
// chunks as small as possible: every chunk is a new command buffer that
// must bind again the pipeline, descriptor set, push constants, viewport
// and scissor its draws use. Equal bytes, and the replays must have been
// split into several chunks.
//
// Steprant 2.4.2: the capture leaves out set binds analyze() finds repeated
// and layout binds of the layout in effect. The frames bind sets again where
// they are bound, at a second index above the sampled one, and switch
// between two compatible layouts. Every configuration must make exactly the
// direct recording's descriptor set binds in the driver, the unfiltered frame
// graph must find the same dead stores, and the filter must have left binds
// out.
#include <plume_render_interface.h>
#include <stdafx.h>
#include <gpu/shader/dxc_compiler.h>
#include <gpu/geometry_prepare.h>
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
struct Counts { uint64_t deadStores = 0, parallelReplays = 0, chunks = 0, descriptorBinds = 0, filteredSets = 0, filteredLayouts = 0, indexBinds = 0, filteredDrawState = 0; };

std::vector<uint8_t> RunFrames(uint32_t seeds, Counts& counts)
{
    auto api = CreateVulkanInterface();
    if (!api) return {};
    auto device = api->createDevice();
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto list = queue->createCommandList();
    // Only takes effect where LO_VK_PARALLEL_RECORD allows it.
    list->setParallelRecording(true);
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
    // Two sets (the shaders sample set 0), and a second layout of the same
    // definition: compatible, but a layout change for Plume.
    RenderPipelineLayoutBuilder layoutBuilder;
    layoutBuilder.begin(false, false);
    layoutBuilder.addPushConstant(0, 0, sizeof(Push), RenderShaderStageFlag::VERTEX | RenderShaderStageFlag::PIXEL);
    layoutBuilder.addDescriptorSet(setBuilder);
    layoutBuilder.addDescriptorSet(setBuilder);
    layoutBuilder.end();
    auto layout = layoutBuilder.create(device.get());
    auto otherLayout = layoutBuilder.create(device.get());
    if (!otherLayout) return {};
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

    // Two buffers and two different six-index slices in each. Direct recording
    // uses the original slice view. The graph uses whole-buffer views and
    // firstIndex, including negative baseVertex offsets for both buffers.
    constexpr uint32_t indices[2][12] = {
        {1,2,3,4,5,6, 1,2,3,1,2,3},
        {5,6,7,5,6,7, 2,3,4,5,6,7}
    };
    std::array<std::unique_ptr<RenderBuffer>, 2> indexBuffers;
    for (unsigned i = 0; i < indexBuffers.size(); ++i) {
        indexBuffers[i] = device->createBuffer(RenderBufferDesc::UploadBuffer(sizeof(indices[i]), RenderBufferFlag::INDEX));
        if (!indexBuffers[i]) return {};
        auto* mapped = indexBuffers[i]->map();
        if (!mapped) return {};
        std::memcpy(mapped, indices[i], sizeof(indices[i]));
        indexBuffers[i]->unmap();
    }
    const char* graph = std::getenv("LO_VK_FRAME_GRAPH");
    const bool commonView = graph && std::strcmp(graph, "0") != 0;

    std::vector<uint8_t> out;
    counts = Counts{};
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
        unsigned drawNumber = seed;
        auto draw = [&]() {
            if ((++drawNumber % 5) == 0) {
                list->drawInstanced(6, 1, 0, 0);
                return;
            }
            const unsigned buffer = (drawNumber / 4) % 2;
            const uint32_t sliceOffset = (drawNumber % 2) * 6u * sizeof(uint32_t);
            const auto range = gpu::geometry_prepare::IndexRangeForDraw(sliceOffset, 6,
                commonView ? sizeof(indices[buffer]) : 0);
            const RenderIndexBufferView view(indexBuffers[buffer]->at(range.byteOffset), range.byteSize, RenderFormat::R32_UINT);
            list->setIndexBuffer(&view);
            list->drawIndexedInstanced(6, 1, range.firstIndex, -int32_t(buffer + 1), 0);
        };
        auto randomRect = [&](float* r) {
            const int x0 = int(rng() % kSize), y0 = int(rng() % kSize);
            r[0] = float(x0); r[1] = float(y0);
            r[2] = float(x0 + 1 + int(rng() % (kSize - x0))); r[3] = float(y0 + 1 + int(rng() % (kSize - y0)));
        };
        auto randomColor = [&](float* c) { for (int i = 0; i < 4; ++i) c[i] = float(51 * (rng() % 6)) / 255.0f; };
        auto attached = [&](int t) { return (boundIndex == t) || (boundIndex == 3 && t == 0); };

        // Binds as the renderer makes them: the layout and the sets again
        // before draws, mostly unchanged.
        const RenderPipelineLayout* currentLayout = layout.get();
        auto rebindLayout = [&]() {
            if (rng() % 6 == 0) currentLayout = (currentLayout == layout.get()) ? otherLayout.get() : layout.get();
            list->setGraphicsPipelineLayout(currentLayout);
        };
        auto bindUnused = [&]() {
            // Set 1 is not read: any set that is not an attachment is valid there.
            for (int tries = 0; tries < 3; ++tries) {
                const int t = int(rng() % kColors);
                if (attached(t) || layoutOf[t] != RenderTextureLayout::SHADER_READ) continue;
                list->setGraphicsDescriptorSet(sets[t].get(), 1);
                return;
            }
        };

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
                // Draws inside a scissor (partial render areas, SR4 G-4).
                if (rng() % 2) {
                    float r[4]; randomRect(r);
                    const RenderRect scissor{int32_t(r[0]), int32_t(r[1]), int32_t(r[2]), int32_t(r[3])};
                    list->setScissors(&scissor, 1);
                } else {
                    const RenderRect scissor{0, 0, int32_t(kSize), int32_t(kSize)};
                    list->setScissors(&scissor, 1);
                }
                Push push{};
                randomRect(push.rect); randomColor(push.color);
                push.depth = float(rng() % 8) / 8.0f;
                list->setPipeline(boundIndex == 3 ? depthPipeline.get() : colorPipeline.get());
                if (rng() % 2) rebindLayout();
                if (rng() % 3 == 0) bindUnused();
                list->setGraphicsPushConstants(0, &push);
                draw();
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
                rebindLayout();
                if (rng() % 2) bindUnused();
                const uint32_t binds = 1 + rng() % 3;
                for (uint32_t b = 0; b < binds; ++b) {
                    list->setGraphicsDescriptorSet(sets[source].get(), 0);
                    if (rng() % 3 == 0) bindUnused();
                }
                list->setGraphicsPushConstants(0, &push);
                draw();
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
        if (list->takePassProfile(profile)) {
            counts.deadStores += profile.frameGraphDeadStores;
            counts.parallelReplays += profile.parallelReplays;
            counts.chunks += profile.parallelChunks;
            counts.descriptorBinds += profile.descriptorBinds;
            counts.filteredSets += profile.frameGraphFilteredSetBinds;
            counts.filteredLayouts += profile.frameGraphFilteredLayoutBinds;
            counts.indexBinds += profile.indexBinds;
            counts.filteredDrawState += profile.frameGraphFilteredDrawState;
        }
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
        // Steprant 2.4.2: compared with the one above (same dead stores).
        {"frame graph, poisoned, every bind captured", {{"LO_VK_FRAME_GRAPH", "1"}, {"LO_VK_FRAME_GRAPH_POISON", "1"}, {"LO_FRAME_GRAPH_SET_FILTER", "0"}}},
        {"frame graph, poisoned, no deferral", {{"LO_VK_FRAME_GRAPH", "1"}, {"LO_VK_FRAME_GRAPH_POISON", "1"}, {"LO_VK_DEFER_COPY", "0"}}},
        {"frame graph, poisoned, full render area", {{"LO_VK_FRAME_GRAPH", "1"}, {"LO_VK_FRAME_GRAPH_POISON", "1"}, {"LO_VK_RENDER_AREA", "0"}}},
        {"frame graph, parallel recording, poisoned", {{"LO_VK_FRAME_GRAPH", "1"}, {"LO_VK_FRAME_GRAPH_POISON", "1"}, {"LO_VK_PARALLEL_RECORD", "1"},
            {"LO_VK_PARALLEL_RECORD_MIN", "1"}, {"LO_VK_PARALLEL_RECORD_CHUNK", "1"}}},
        {"frame graph, parallel recording, 3 helpers, chunks of 3", {{"LO_VK_FRAME_GRAPH", "1"}, {"LO_VK_FRAME_GRAPH_POISON", "1"}, {"LO_VK_PARALLEL_RECORD", "1"},
            {"LO_VK_PARALLEL_RECORD_MIN", "1"}, {"LO_VK_PARALLEL_RECORD_CHUNK", "3"}, {"LO_VK_PARALLEL_RECORD_THREADS", "3"}}},
    };
    std::vector<std::vector<uint8_t>> results;
    uint64_t directBinds = 0, directIndexBinds = 0, filteredDeadStores = 0;
    int failed = 0;
    for (const auto& config : configs) {
        int pipe[2];
        if (::pipe(pipe) != 0) return 1;
        std::fflush(stdout);
        const pid_t child = fork();
        if (child == 0) {
            ::close(pipe[0]);
            for (const auto& [key, value] : config.env) setenv(key, value, 1);
            Counts counts;
            const auto bytes = RunFrames(seeds, counts);
            std::printf("[%s] %u frames, %llu dead stores, %llu replays recorded in parallel, %llu chunks, %llu set binds in the driver, "
                "%llu set and %llu layout binds left out, %llu index binds, %llu redundant draw states filtered\n", config.name, seeds,
                (unsigned long long)counts.deadStores, (unsigned long long)counts.parallelReplays, (unsigned long long)counts.chunks,
                (unsigned long long)counts.descriptorBinds, (unsigned long long)counts.filteredSets, (unsigned long long)counts.filteredLayouts,
                (unsigned long long)counts.indexBinds, (unsigned long long)counts.filteredDrawState);
            std::fflush(stdout);
            const char* parallel = std::getenv("LO_VK_PARALLEL_RECORD");
            const bool split = counts.parallelReplays == seeds && counts.chunks >= counts.parallelReplays * 4;
            const uint64_t header[8] = {bytes.size(), counts.deadStores, (parallel && std::strcmp(parallel, "1") == 0 && !split) ? 1u : 0u,
                counts.descriptorBinds, counts.filteredSets, counts.filteredLayouts, counts.indexBinds, counts.filteredDrawState};
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
        uint64_t header[8] = {};
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
            if (header[2] != 0) { std::printf("FAIL [%s]: the replays were not split into chunks\n", config.name); ++failed; }
            // Steprant 2.4.2: the same set binds reach the driver as in the
            // direct recording, and the filter left binds out where it is on.
            if (header[3] != directBinds) {
                std::printf("FAIL [%s]: %llu set binds in the driver, the direct recording made %llu\n", config.name,
                    (unsigned long long)header[3], (unsigned long long)directBinds);
                ++failed;
            }
            const bool filtered = std::strstr(config.name, "every bind captured") == nullptr;
            if (filtered && (header[4] == 0 || header[5] == 0)) { std::printf("FAIL [%s]: no bind left out\n", config.name); ++failed; }
            if (!filtered && (header[4] != 0 || header[5] != 0)) { std::printf("FAIL [%s]: binds left out with the filter off\n", config.name); ++failed; }
            if (filtered ? (header[7] == 0 || header[6] >= directIndexBinds) : (header[7] != 0 || header[6] != directIndexBinds)) {
                std::printf("FAIL [%s]: unexpected index bind / filtered draw-state counts\n", config.name);
                ++failed;
            }
            if (!filtered && header[1] != filteredDeadStores) {
                std::printf("FAIL [%s]: %llu dead stores, %llu with the filter\n", config.name,
                    (unsigned long long)header[1], (unsigned long long)filteredDeadStores);
                ++failed;
            }
        } else {
            directBinds = header[3];
            directIndexBinds = header[6];
            if (directBinds == 0) { std::printf("FAIL [%s]: no set bind reached the driver\n", config.name); ++failed; }
        }
        if (results.size() == 1) filteredDeadStores = header[1];
        results.push_back(std::move(bytes));
    }
    if (failed) { std::printf("FAIL: %d configuration(s)\n", failed); return 1; }
    std::printf("PASS: frames with draws match the direct recording with every frame graph discard poisoned\n");
    return 0;
}
