// Steprant 2.0, Super Roadmap 4.0 M-4: random command sequences through Plume
// on a real Vulkan device (lavapipe in CI), checked against a CPU model.
//
// Each sequence works like the renderer: it binds one of four targets, clears
// rectangles in it, copies rectangles between the targets with the barriers
// the renderer records around a copy (to COPY_SOURCE / COPY_DEST, then back),
// moves other targets to SHADER_READ or COLOR_WRITE and unbinds. Plume is free
// to keep a pass open across those commands (deferred resolves and texture
// copies, deferred target transitions, partial render areas, zero clears of
// undefined contents); the final contents of every target must still be the
// ones the commands give in their recorded order. The CPU model computes them
// exactly (clear colours are multiples of 51/255, copies move bytes).
//
// The sequences run in child processes, one per configuration of Plume's
// switches (they are read once per process): the defaults, every deferral
// off, texture copies immediate, and the whole framebuffer as render area.
// Any byte that differs from the model fails the test with the seed and the
// first differing pixel. LO_PASS_SEQUENCE_SEEDS sets the number of seeds.
#include <plume_render_interface.h>
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

using namespace plume;
namespace {
constexpr uint32_t kSize = 32;
constexpr int kTargets = 4;
constexpr int kOps = 48;

struct Model {
    std::array<std::vector<uint32_t>, kTargets> pixels;
    Model() { for (auto& p : pixels) p.assign(kSize * kSize, 0); }
    void Fill(int t, const RenderRect& r, uint32_t value) {
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x) pixels[t][y * kSize + x] = value;
    }
    void Copy(int dst, int src, const RenderBox& box, int dx, int dy) {
        const auto source = pixels[src];
        for (int y = box.top; y < box.bottom; ++y)
            for (int x = box.left; x < box.right; ++x)
                pixels[dst][(dy + y - box.top) * kSize + (dx + x - box.left)] = source[y * kSize + x];
    }
};

uint32_t Pack(const std::array<uint8_t, 4>& c) { return uint32_t(c[0]) | (uint32_t(c[1]) << 8) | (uint32_t(c[2]) << 16) | (uint32_t(c[3]) << 24); }

struct Sequence {
    RenderDevice* device;
    RenderCommandQueue* queue;
    RenderCommandList* commands;
    RenderCommandFence* fence;
    std::array<std::unique_ptr<RenderTexture>, kTargets> textures;
    std::array<std::unique_ptr<RenderFramebuffer>, kTargets> framebuffers;
    std::array<std::unique_ptr<RenderBuffer>, kTargets> readbacks;
    std::array<RenderTextureLayout, kTargets> layout{};
    int bound = -1;

    void Barrier(int t, RenderTextureLayout to, RenderBarrierStages stages) {
        commands->barriers(stages, RenderTextureBarrier(textures[t].get(), to));
        layout[t] = to;
    }
    void Bind(int t) {
        if (layout[t] != RenderTextureLayout::COLOR_WRITE) Barrier(t, RenderTextureLayout::COLOR_WRITE, RenderBarrierStage::GRAPHICS);
        commands->setFramebuffer(framebuffers[t].get());
        bound = t;
    }
};

RenderRect RandomRect(std::mt19937& rng, bool allowFull) {
    if (allowFull && rng() % 4 == 0) return RenderRect{0, 0, int32_t(kSize), int32_t(kSize)};
    const int x0 = int(rng() % kSize), y0 = int(rng() % kSize);
    const int x1 = x0 + 1 + int(rng() % (kSize - x0)), y1 = y0 + 1 + int(rng() % (kSize - y0));
    return RenderRect{x0, y0, x1, y1};
}

std::array<uint8_t, 4> RandomColor(std::mt19937& rng) {
    std::array<uint8_t, 4> c{};
    for (auto& v : c) v = uint8_t(51 * (rng() % 6));
    return c;
}

// Records one sequence and its model; returns false on a Plume failure.
void Record(Sequence& s, Model& model, uint32_t seed) {
    std::mt19937 rng(seed);
    auto clear = [&](int t, const RenderRect& rect, const std::array<uint8_t, 4>& c) {
        s.commands->clearColor(0, RenderColor(c[0] / 255.0f, c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f), &rect, 1);
        model.Fill(t, rect, Pack(c));
    };
    // Every target starts defined: a full clear in its own pass.
    for (int t = 0; t < kTargets; ++t) {
        s.layout[t] = RenderTextureLayout::UNKNOWN;
        s.Bind(t);
        clear(t, RenderRect{0, 0, int32_t(kSize), int32_t(kSize)}, RandomColor(rng));
    }
    for (int op = 0; op < kOps; ++op) {
        const uint32_t kind = rng() % 10;
        if (kind < 2) {
            s.Bind(int(rng() % kTargets));
        } else if (kind < 5) {
            if (s.bound < 0) s.Bind(int(rng() % kTargets));
            clear(s.bound, RandomRect(rng, true), RandomColor(rng));
        } else if (kind < 8) {
            const int src = int(rng() % kTargets);
            int dst = int(rng() % (kTargets - 1));
            if (dst >= src) ++dst;
            const RenderRect r = RandomRect(rng, false);
            const RenderBox box{r.left, r.top, r.right, r.bottom, 0, 1};
            const int w = r.right - r.left, h = r.bottom - r.top;
            const int dx = int(rng() % (kSize - w + 1)), dy = int(rng() % (kSize - h + 1));
            // The renderer's resolve: one call for both, or one each.
            if (rng() % 2) {
                s.commands->barriers(RenderBarrierStage::COPY, std::vector<RenderTextureBarrier>{
                    RenderTextureBarrier(s.textures[src].get(), RenderTextureLayout::COPY_SOURCE),
                    RenderTextureBarrier(s.textures[dst].get(), RenderTextureLayout::COPY_DEST)});
                s.layout[src] = RenderTextureLayout::COPY_SOURCE;
                s.layout[dst] = RenderTextureLayout::COPY_DEST;
            } else {
                s.Barrier(src, RenderTextureLayout::COPY_SOURCE, RenderBarrierStage::COPY);
                s.Barrier(dst, RenderTextureLayout::COPY_DEST, RenderBarrierStage::COPY);
            }
            s.commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(s.textures[dst].get()),
                RenderTextureCopyLocation::Subresource(s.textures[src].get()), uint32_t(dx), uint32_t(dy), 0, &box);
            model.Copy(dst, src, box, dx, dy);
            // Back: the bound target to COLOR_WRITE (always, before it is
            // cleared again), the others to SHADER_READ or left in place.
            for (const int t : {dst, src}) {
                if (t == s.bound) s.Barrier(t, RenderTextureLayout::COLOR_WRITE, RenderBarrierStage::GRAPHICS);
                else if (rng() % 3 != 0) s.Barrier(t, RenderTextureLayout::SHADER_READ, RenderBarrierStage::GRAPHICS);
            }
        } else if (kind < 9) {
            const int t = int(rng() % kTargets);
            if (t != s.bound)
                s.Barrier(t, rng() % 2 ? RenderTextureLayout::SHADER_READ : RenderTextureLayout::COLOR_WRITE, RenderBarrierStage::GRAPHICS);
        } else {
            s.commands->setFramebuffer(nullptr);
            s.bound = -1;
        }
    }
    s.commands->setFramebuffer(nullptr);
    s.bound = -1;
    for (int t = 0; t < kTargets; ++t) {
        s.Barrier(t, RenderTextureLayout::COPY_SOURCE, RenderBarrierStage::COPY);
        s.commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(s.readbacks[t].get(), RenderFormat::R8G8B8A8_UNORM, kSize, kSize, 1, kSize),
            RenderTextureCopyLocation::Subresource(s.textures[t].get()));
    }
}

// One configuration: every seed, compared with the model. Returns failures.
int RunAll(uint32_t seeds, const char* name) {
    auto api = CreateVulkanInterface();
    if (!api) { std::printf("SKIP: no Vulkan\n"); return -1; }
    auto device = api->createDevice();
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    Sequence s{device.get(), queue.get(), commands.get(), fence.get()};
    for (int t = 0; t < kTargets; ++t) {
        s.textures[t] = device->createTexture(RenderTextureDesc::ColorTarget(kSize, kSize, RenderFormat::R8G8B8A8_UNORM));
        const RenderTexture* colors[] = {s.textures[t].get()};
        s.framebuffers[t] = device->createFramebuffer(RenderFramebufferDesc(colors, 1));
        s.readbacks[t] = device->createBuffer(RenderBufferDesc::ReadbackBuffer(kSize * kSize * 4));
    }
    int failures = 0;
    uint64_t deferred = 0, textureDeferred = 0, passes = 0;
    for (uint32_t seed = 1; seed <= seeds; ++seed) {
        Model model;
        commands->setPassProfiling(true);
        commands->begin();
        Record(s, model, seed);
        commands->end();
        const RenderCommandList* lists[] = {commands.get()};
        queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
        queue->waitForCommandFence(fence.get());
        RenderPassProfile profile;
        if (commands->takePassProfile(profile)) {
            deferred += profile.copiesDeferred;
            textureDeferred += profile.textureCopiesDeferred;
            passes += profile.passes.size();
        }
        for (int t = 0; t < kTargets && failures < 4; ++t) {
            const auto* data = static_cast<const uint32_t*>(s.readbacks[t]->map());
            for (uint32_t i = 0; i < kSize * kSize; ++i) {
                if (data[i] != model.pixels[t][i]) {
                    std::printf("FAIL [%s]: seed %u target %d pixel (%u,%u) = %08x, model %08x\n", name, seed, t,
                        i % kSize, i / kSize, data[i], model.pixels[t][i]);
                    ++failures;
                    break;
                }
            }
            s.readbacks[t]->unmap();
        }
    }
    std::printf("[%s] %u sequences, %llu passes, %llu deferred copies (%llu texture copies), %d failures\n", name, seeds,
        (unsigned long long)passes, (unsigned long long)deferred, (unsigned long long)textureDeferred, failures);
    return failures;
}
}

int main() {
    const char* count = std::getenv("LO_PASS_SEQUENCE_SEEDS");
    const uint32_t seeds = count ? uint32_t(std::strtoul(count, nullptr, 10)) : 300;
    struct Config { const char* name; std::vector<std::pair<const char*, const char*>> env; };
    const Config configs[] = {
        {"defaults", {}},
        {"no deferral", {{"LO_VK_DEFER_COPY", "0"}}},
        {"texture copies immediate", {{"LO_VK_DEFER_TEXTURE_COPY", "0"}}},
        {"full render area", {{"LO_VK_RENDER_AREA", "0"}}},
        {"keep DONT_CARE", {{"LO_VK_ZERO_UNDEFINED", "0"}}},
    };
    int failed = 0;
    for (const auto& config : configs) {
        std::fflush(stdout);
        const pid_t child = fork();
        if (child == 0) {
            // Plume reads its switches once per process: set them before any call.
            for (const auto& [key, value] : config.env) setenv(key, value, 1);
            const int result = RunAll(seeds, config.name);
            std::fflush(stdout);
            _exit(result < 0 ? 77 : (result == 0 ? 0 : 1));
        }
        int status = 0;
        waitpid(child, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 77) { std::printf("SKIP: no Vulkan\n"); return 0; }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) ++failed;
    }
    if (failed) { std::printf("FAIL: %d configuration(s) differ from the model\n", failed); return 1; }
    std::printf("PASS: random pass sequences match the CPU model in every configuration\n");
    return 0;
}
