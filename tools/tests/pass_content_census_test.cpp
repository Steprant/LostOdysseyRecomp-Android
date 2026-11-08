// Roadmap 1.1: Plume's content census on a real Vulkan device (lavapipe in CI).
// A colour store that a later barrier reads, a depth store that the next pass
// clears unread, and a load of a texture nothing had written must each land
// in the matching RenderPassProfile::ContentFlow bucket.
#include <plume_render_interface.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }

int main()
{
    using namespace plume;
    constexpr uint32_t size = 64;
    constexpr uint64_t bytes = uint64_t(size) * size * 4;
    auto api = CreateVulkanInterface();
    if (!api) { std::puts("SKIP: no Vulkan"); return 0; }
    auto device = api->createDevice();
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto color = device->createTexture(RenderTextureDesc::ColorTarget(size, size, RenderFormat::R8G8B8A8_UNORM));
    auto fresh = device->createTexture(RenderTextureDesc::ColorTarget(size, size, RenderFormat::R8G8B8A8_UNORM));
    auto depth = device->createTexture(RenderTextureDesc::DepthTarget(size, size, RenderFormat::D32_FLOAT));
    const RenderTexture* colors[] = {color.get()};
    const RenderTexture* freshColors[] = {fresh.get()};
    auto fb = device->createFramebuffer(RenderFramebufferDesc(colors, 1, depth.get()));
    auto freshFb = device->createFramebuffer(RenderFramebufferDesc(freshColors, 1));
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();

    commands->setPassProfiling(true);
    commands->begin();
    // Pass 1: clears both at load and stores both.
    commands->barriers(RenderBarrierStage::GRAPHICS, std::vector<RenderTextureBarrier>{
        RenderTextureBarrier(color.get(), RenderTextureLayout::COLOR_WRITE),
        RenderTextureBarrier(depth.get(), RenderTextureLayout::DEPTH_WRITE)});
    commands->setFramebuffer(fb.get());
    commands->clearColor(0, RenderColor(0, 0, 0, 1));
    commands->clearDepthStencil(true, false, 1.0f, 0);
    commands->setFramebuffer(nullptr);
    // The colour is sampled: its store was needed.
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(color.get(), RenderTextureLayout::SHADER_READ));
    // Pass 2: loads the colour, clears the depth nobody read.
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(color.get(), RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(fb.get());
    commands->clearDepthStencil(true, false, 1.0f, 0);
    commands->setFramebuffer(nullptr);
    // Pass 3: a partial clear loads a texture nothing had written.
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(fresh.get(), RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(freshFb.get());
    const RenderRect corner{0, 0, 8, 8};
    commands->clearColor(0, RenderColor(1, 0, 0, 1), &corner, 1);
    // With render areas (LO_VK_RENDER_AREA, default on) pass 3 covers only
    // that corner; a clear of the opposite corner ends it and pass 4 covers
    // the whole framebuffer, loading what pass 3 stored.
    const RenderRect farCorner{int32_t(size) - 8, int32_t(size) - 8, int32_t(size), int32_t(size)};
    commands->clearColor(0, RenderColor(0, 1, 0, 1), &farCorner, 1);
    // Sampling a texture the pass does not render to still ends it, and
    // reads the colour pass 2 stored.
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(color.get(), RenderTextureLayout::SHADER_READ));
    commands->setFramebuffer(nullptr);
    commands->end();
    const RenderCommandList* lists[] = {commands.get()};
    queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());

    RenderPassProfile profile;
    if (!commands->takePassProfile(profile)) { std::puts("SKIP: no timestamp support"); return 0; }
    const auto& f = profile.flow;
    std::printf("passes=%zu color: read=%llu loaded=%llu cleared=%llu valid=%llu undefined=%llu depth: read=%llu loaded=%llu cleared=%llu\n",
        profile.passes.size(),
        (unsigned long long)f.storeRead[0], (unsigned long long)f.storeLoaded[0], (unsigned long long)f.storeCleared[0],
        (unsigned long long)f.loadValid[0], (unsigned long long)f.loadUndefined[0],
        (unsigned long long)f.storeRead[1], (unsigned long long)f.storeLoaded[1], (unsigned long long)f.storeCleared[1]);
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } };
    const char* discardSetting = std::getenv("LO_VK_DISCARD_UNDEFINED");
    const bool discard = !discardSetting || std::strcmp(discardSetting, "0") != 0;
    const char* areaSetting = std::getenv("LO_VK_RENDER_AREA");
    const bool area = !areaSetting || std::strcmp(areaSetting, "0") != 0;
    const uint64_t cornerBytes = area ? 8 * 8 * 4 : bytes;
    expect(profile.passes.size() == (area ? 4u : 3u), "pass count");
    expect(f.storeRead[0] == 2 * bytes, "both colour stores read by sampling barriers");
    expect(f.loadValid[0] == (area ? 2 * bytes : bytes), "pass 2 (and pass 4) load written colour");
    expect(f.storeCleared[1] == bytes, "depth store cleared unread");
    expect(f.storeRead[1] == 0 && f.storeLoaded[1] == 0, "depth never consumed");
    // Pass 3's texture left an unknown layout and nothing wrote it: Plume
    // loads it with DONT_CARE (LO_VK_DISCARD_UNDEFINED=0 keeps LOAD), and
    // only over its render area.
    expect(f.loadUndefined[0] == (discard ? 0 : cornerBytes), "load of a never-written texture");
    expect(profile.passes.size() >= 3 && profile.passes[2].loadBytes == (discard ? 0 : cornerBytes), "pass 3 load bytes");
    expect(profile.passes.size() >= 3 && profile.passes[1].loadBytes == bytes, "pass 2 still loads the defined colour");
    if (area) expect(profile.passes.size() == 4 && profile.passes[3].loadBytes == bytes && profile.passes[2].storeBytes == cornerBytes,
        "pass 3 stores its corner, pass 4 loads the union");
    size_t clearedDepth = 0;
    for (const auto& event : profile.flowEvents)
        clearedDepth += event.depth && event.width == size && event.colors == 1 && event.bucket == RenderPassProfile::FlowBucket::Cleared;
    expect(clearedDepth == 1, "the cleared depth store is charged to the 64x64 colour+depth pass");
    using End = RenderPassProfile::PassEnd;
    expect(profile.passEnds[size_t(End::Framebuffer)] == 2, "passes 1 and 2 end at the framebuffer change");
    expect(profile.passEnds[size_t(End::RenderArea)] == (area ? 1u : 0u), "pass 3 ends when a clear leaves its area");
    expect(profile.passEnds[size_t(End::BarrierOther)] == 1, "the last pass ends at a barrier on another texture");
    expect(profile.barrierEndLayouts[1][uint32_t(RenderTextureLayout::COLOR_WRITE)][uint32_t(RenderTextureLayout::SHADER_READ)] == 1,
        "that barrier is recorded as another texture going from colour write to shader read");
    if (failures) return 1;
    std::puts("PASS: content census buckets (read, loaded, cleared unread, undefined load)");
    return 0;
}
