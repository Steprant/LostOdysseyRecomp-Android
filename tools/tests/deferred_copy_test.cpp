// Roadmap 1.4c: Plume defers a copy out of a colour attachment of the active
// render pass (an EDRAM resolve) until the pass ends, instead of ending it.
// Checks on a real Vulkan device (lavapipe in CI) that
//  - the pass is not split by the copy and its layout transitions,
//  - the copy still reads the attachment as it was when the copy was
//    recorded when a later clear touches it (the clear runs the copy first),
//  - LO_VK_DEFER_COPY=0 copies at once.
#include <plume_render_interface.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }

using namespace plume;
static constexpr uint32_t kSize = 32;

static uint32_t Pixel(const uint8_t* data, uint32_t x, uint32_t y)
{
    uint32_t value;
    std::memcpy(&value, data + (size_t(y) * kSize + x) * 4, 4);
    return value;
}

int main()
{
    auto api = CreateVulkanInterface();
    if (!api) { std::puts("SKIP: no Vulkan"); return 0; }
    auto device = api->createDevice();
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto color = device->createTexture(RenderTextureDesc::ColorTarget(kSize, kSize, RenderFormat::R8G8B8A8_UNORM));
    auto dst = device->createTexture(RenderTextureDesc::ColorTarget(kSize, kSize, RenderFormat::R8G8B8A8_UNORM));
    auto readback = device->createBuffer(RenderBufferDesc::ReadbackBuffer(kSize * kSize * 4));
    const RenderTexture* colors[] = {color.get()};
    auto fb = device->createFramebuffer(RenderFramebufferDesc(colors, 1));
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    const char* setting = std::getenv("LO_VK_DEFER_COPY");
    const bool defer = !setting || std::strcmp(setting, "0") != 0;

    commands->setPassProfiling(true);
    commands->begin();
    commands->barriers(RenderBarrierStage::GRAPHICS, std::vector<RenderTextureBarrier>{
        RenderTextureBarrier(color.get(), RenderTextureLayout::COLOR_WRITE),
        RenderTextureBarrier(dst.get(), RenderTextureLayout::COLOR_WRITE)});
    // Pass 1: a partial clear (red) opens the pass on the attachment.
    commands->setFramebuffer(fb.get());
    const RenderRect all{0, 0, int32_t(kSize), int32_t(kSize)};
    const RenderRect half{0, 0, int32_t(kSize), int32_t(kSize / 2)};
    commands->clearColor(0, RenderColor(1, 0, 0, 1), &half, 1);
    // Resolve the top half like the renderer does: source to COPY_SOURCE,
    // destination to COPY_DEST, copy, destination to SHADER_READ, source
    // back to COLOR_WRITE. Deferred: no pass ends here.
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(color.get(), RenderTextureLayout::COPY_SOURCE));
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(dst.get(), RenderTextureLayout::COPY_DEST));
    const RenderBox box{0, 0, int32_t(kSize), int32_t(kSize / 2), 0, 1};
    commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(dst.get()), RenderTextureCopyLocation::Subresource(color.get()), 0, 0, 0, &box);
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(dst.get(), RenderTextureLayout::SHADER_READ));
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(color.get(), RenderTextureLayout::COLOR_WRITE));
    // A clear over the copied rectangle (green) must not reach the copy:
    // it runs the deferred copy first.
    commands->clearColor(0, RenderColor(0, 1, 0, 1), &all, 1);
    commands->setFramebuffer(nullptr);
    // Read the destination back.
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(dst.get(), RenderTextureLayout::COPY_SOURCE));
    commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::R8G8B8A8_UNORM, kSize, kSize, 1, kSize),
        RenderTextureCopyLocation::Subresource(dst.get()));
    commands->end();
    const RenderCommandList* lists[] = {commands.get()};
    queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());

    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++failures; } };
    const auto* data = static_cast<const uint8_t*>(readback->map());
    expect(Pixel(data, 3, 3) == 0xff0000ffu, "the copy holds the red written before it, not the later green");
    readback->unmap();
    RenderPassProfile profile;
    if (commands->takePassProfile(profile)) {
        std::printf("passes=%zu deferred=%u flushed_early=%u copy_ends=%u\n", profile.passes.size(), profile.copiesDeferred,
            profile.deferredFlushedEarly, profile.passEnds[size_t(RenderPassProfile::PassEnd::Copy)]);
        expect(profile.copiesDeferred == (defer ? 1u : 0u), "the resolve copy is deferred (or not, when disabled)");
        expect(profile.deferredFlushedEarly == (defer ? 1u : 0u), "the overlapping clear runs it early");
    }

    // Second list: nothing touches the copied rectangle after the copy, so
    // the copy waits for the pass to end and the pass is not split.
    commands->begin();
    commands->barriers(RenderBarrierStage::GRAPHICS, std::vector<RenderTextureBarrier>{
        RenderTextureBarrier(color.get(), RenderTextureLayout::COLOR_WRITE),
        RenderTextureBarrier(dst.get(), RenderTextureLayout::COLOR_WRITE)});
    commands->setFramebuffer(fb.get());
    commands->clearColor(0, RenderColor(0, 0, 1, 1), &half, 1);
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(color.get(), RenderTextureLayout::COPY_SOURCE));
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(dst.get(), RenderTextureLayout::COPY_DEST));
    commands->copyTextureRegion(RenderTextureCopyLocation::Subresource(dst.get()), RenderTextureCopyLocation::Subresource(color.get()), 0, 0, 0, &box);
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(dst.get(), RenderTextureLayout::SHADER_READ));
    commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(color.get(), RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(nullptr);
    commands->barriers(RenderBarrierStage::COPY, RenderTextureBarrier(dst.get(), RenderTextureLayout::COPY_SOURCE));
    commands->copyTextureRegion(RenderTextureCopyLocation::PlacedFootprint(readback.get(), RenderFormat::R8G8B8A8_UNORM, kSize, kSize, 1, kSize),
        RenderTextureCopyLocation::Subresource(dst.get()));
    commands->end();
    queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
    queue->waitForCommandFence(fence.get());
    data = static_cast<const uint8_t*>(readback->map());
    expect(Pixel(data, 3, 3) == 0xffff0000u, "the deferred copy holds the blue of its rectangle");
    readback->unmap();
    if (commands->takePassProfile(profile)) {
        std::printf("passes=%zu deferred=%u flushed_early=%u\n", profile.passes.size(), profile.copiesDeferred, profile.deferredFlushedEarly);
        expect(profile.passes.size() == 1, "one pass: the copy did not split it");
        expect(profile.copiesDeferred == (defer ? 1u : 0u) && profile.deferredFlushedEarly == 0, "deferred to the end of the pass");
    }
    if (failures) return 1;
    std::printf("PASS: deferred resolve copies (%s)\n", defer ? "on" : "off");
    return 0;
}
