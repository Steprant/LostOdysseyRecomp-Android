// Steprant 2.0 WP1 stage 1a and WP-A.3 on a real Vulkan device (lavapipe).
// Default: a pass that resumes the framebuffer the previous pass ended on,
// after a barrier on another texture only, counts as mergeable with the bytes
// it loaded again; a resume after a barrier on its own attachment does not.
// With LO_VK_DIAG_DISCARD_DEAD_STORES=1: a store that the next pass cleared
// unread is predicted dead and stored with DONT_CARE the next time; a store
// that something sampled is never discarded.
#include <plume_render_interface.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace plume { std::unique_ptr<RenderInterface> CreateVulkanInterface(); }

static int failures = 0;
static void Check(bool ok, const char* what)
{
    std::printf("  %s: %s\n", what, ok ? "PASS" : "FAIL");
    failures += !ok;
}

int main()
{
    using namespace plume;
    constexpr uint32_t size = 64;
    constexpr uint64_t bytes = uint64_t(size) * size * 4;
    const char* mode = std::getenv("LO_VK_DIAG_DISCARD_DEAD_STORES");
    const bool experiment = mode && std::strcmp(mode, "1") == 0;
    auto api = CreateVulkanInterface();
    if (!api) { std::puts("SKIP: no Vulkan"); return 0; }
    auto device = api->createDevice();
    auto queue = device->createCommandQueue(RenderCommandListType::DIRECT);
    auto target = device->createTexture(RenderTextureDesc::ColorTarget(size, size, RenderFormat::R8G8B8A8_UNORM));
    auto sampled = device->createTexture(RenderTextureDesc::ColorTarget(size, size, RenderFormat::R8G8B8A8_UNORM));
    auto other = device->createTexture(RenderTextureDesc::ColorTarget(size, size, RenderFormat::R8G8B8A8_UNORM));
    const RenderTexture* targets[] = {target.get()};
    const RenderTexture* sampledTargets[] = {sampled.get()};
    auto fb = device->createFramebuffer(RenderFramebufferDesc(targets, 1));
    auto sampledFb = device->createFramebuffer(RenderFramebufferDesc(sampledTargets, 1));
    auto commands = queue->createCommandList();
    auto fence = device->createCommandFence();
    const RenderRect corner{0, 0, 8, 8};
    const RenderRect farCorner{int32_t(size) - 8, int32_t(size) - 8, int32_t(size), int32_t(size)};

    const auto run = [&](auto record) {
        commands->setPassProfiling(true);
        commands->begin();
        record();
        commands->end();
        const RenderCommandList* lists[] = {commands.get()};
        queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0, fence.get());
        queue->waitForCommandFence(fence.get());
        RenderPassProfile profile;
        if (!commands->takePassProfile(profile)) std::puts("  (no profile)");
        return profile;
    };

    if (!experiment) {
        const auto profile = run([&] {
            commands->barriers(RenderBarrierStage::GRAPHICS, std::vector<RenderTextureBarrier>{
                RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE),
                RenderTextureBarrier(other.get(), RenderTextureLayout::COLOR_WRITE)});
            commands->setFramebuffer(fb.get());
            commands->clearColor(0, RenderColor(1, 0, 0, 1), &corner, 1);
            // A barrier on another texture ends the pass (barrier_other)...
            commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(other.get(), RenderTextureLayout::SHADER_READ));
            // ...and the next clear resumes the same framebuffer: mergeable.
            commands->clearColor(0, RenderColor(0, 1, 0, 1), &farCorner, 1);
            // A barrier on its own attachment: the resume is not mergeable.
            commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::SHADER_READ));
            commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE));
            commands->clearColor(0, RenderColor(0, 0, 1, 1), &corner, 1);
            commands->setFramebuffer(nullptr);
        });
        std::printf("  mergeable=%u merge_bytes=%llu passes=%zu\n", profile.whatIfMergeablePasses,
            static_cast<unsigned long long>(profile.whatIfMergeBytes), profile.passes.size());
        Check(profile.whatIfMergeablePasses == 1, "one resume after an unrelated barrier is mergeable");
        // With render areas the resumed pass may cover the far corner only;
        // the what-if charges the attachment's full size (upper bound).
        Check(profile.whatIfMergeBytes == bytes, "its reload is charged at the attachment size");
        Check(profile.diagDiscardedStores == 0, "no discarded store without the diagnostic switch");
    } else {
        // Three "frames" of the same pattern on `target`: stored, then the
        // next pass clears it unread. `sampled` is stored and then read.
        uint32_t discarded = 0;
        for (int frame = 0; frame < 3; ++frame) {
            const auto profile = run([&] {
                commands->barriers(RenderBarrierStage::GRAPHICS, std::vector<RenderTextureBarrier>{
                    RenderTextureBarrier(target.get(), RenderTextureLayout::COLOR_WRITE),
                    RenderTextureBarrier(sampled.get(), RenderTextureLayout::COLOR_WRITE)});
                // `target` is cleared whole by both of its passes: every store
                // of it is replaced unread. `sampled` keeps loadOp LOAD.
                commands->setFramebuffer(fb.get());
                commands->clearColor(0, RenderColor(1, 0, 0, 1));
                commands->setFramebuffer(sampledFb.get());
                commands->clearColor(0, RenderColor(0, 1, 0, 1), &corner, 1);
                commands->setFramebuffer(nullptr);
                commands->barriers(RenderBarrierStage::GRAPHICS, RenderTextureBarrier(sampled.get(), RenderTextureLayout::SHADER_READ));
                // The next pass on `target` clears it at load: its store was dead.
                commands->setFramebuffer(fb.get());
                commands->clearColor(0, RenderColor(0, 0, 0, 1));
                commands->setFramebuffer(nullptr);
            });
            std::printf("  frame %d: discarded stores=%u bytes=%llu\n", frame, profile.diagDiscardedStores,
                static_cast<unsigned long long>(profile.diagDiscardedStoreBytes));
            // Frame 0: only the second pass (the first store was just seen
            // replaced); later frames: both passes of `target`.
            Check(profile.diagDiscardedStores == (frame == 0 ? 1u : 2u), "dead stores of target discarded as predicted");
            Check(profile.diagDiscardedStoreBytes == uint64_t(profile.diagDiscardedStores) * bytes, "their bytes");
            discarded += profile.diagDiscardedStores;
        }
        // `sampled` is read after each store: never discarded.
        Check(discarded == 5, "nothing but the dead stores of target is discarded");
    }
    if (failures) { std::printf("FAIL: %d checks\n", failures); return 1; }
    std::puts(experiment ? "PASS frame graph diagnostic dead-store discard" : "PASS frame graph what-if");
    return 0;
}
