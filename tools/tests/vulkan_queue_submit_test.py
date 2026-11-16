"""Exercise Plume's production queue submission against a fake vkQueueSubmit.

Uses the pinned Vulkan headers and patched Plume source, but no loader, GPU,
window system or game assets. ASan checks that every declared wait mask is
readable; the fake Vulkan entry point also checks command/semaphore/fence data.
"""
import argparse
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PLUME = ROOT / 'thirdparty/plume'

PRELUDE = r'''
#include <vulkan/vulkan_core.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <vector>

static void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Handle(uintptr_t value) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(value);
    else return static_cast<T>(value);
}
static unsigned submits = 0, errors = 0;
static uint32_t expectedWaits = 0, expectedSignals = 0;
static VkFence expectedFence = VK_NULL_HANDLE;
static VkResult submitResult = VK_SUCCESS;
static std::vector<VkCommandBuffer> expectedBuffers;

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count,
                                            const VkSubmitInfo* info, VkFence fence) {
    ++submits;
    Check(queue == Handle<VkQueue>(1) && count == 1, "wrong queue or batch count");
    Check(info->sType == VK_STRUCTURE_TYPE_SUBMIT_INFO && !info->pNext, "wrong submit structure");
    Check(info->waitSemaphoreCount == expectedWaits, "wrong wait count");
    // Read the entire declared array before inspecting the masks. On the
    // original code, ASan reports a stack overread when there is a second wait.
    std::vector<VkPipelineStageFlags> stages;
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i)
        stages.push_back(info->pWaitDstStageMask[i]);
    for (uint32_t i = 0; i < info->waitSemaphoreCount; ++i) {
        Check(info->pWaitSemaphores[i] == Handle<VkSemaphore>(10 + i), "wrong wait semaphore");
        // The generic API accepts graphics, compute and copy lists. This mask
        // covers their first accesses and is valid for each queue family.
        Check(stages[i] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, "wait excludes compute/transfer operations");
    }
    if (!expectedWaits)
        Check(!info->pWaitSemaphores && !info->pWaitDstStageMask, "stale wait pointers");
    Check(info->commandBufferCount == expectedBuffers.size(), "wrong command count");
    for (unsigned i = 0; i < expectedBuffers.size(); ++i)
        Check(info->pCommandBuffers[i] == expectedBuffers[i], "wrong command buffer");
    Check(info->signalSemaphoreCount == expectedSignals, "wrong signal count");
    for (uint32_t i = 0; i < expectedSignals; ++i)
        Check(info->pSignalSemaphores[i] == Handle<VkSemaphore>(20 + i), "wrong signal semaphore");
    if (!expectedSignals) Check(!info->pSignalSemaphores, "stale signal pointer");
    Check(fence == expectedFence, "wrong signal fence");
    return submitResult;
}
#define PLUME_LOG_ERROR(...) (++errors)
namespace plume {
// Minimal host objects; the submission method below is unmodified production
// code. All Vulkan structures and flags come from the actual pinned headers.
struct RenderCommandList {};
struct VulkanCommandList : RenderCommandList {
    VkCommandBuffer vk;
    // Steprant 2.1.5: SubmitVulkan submits the list, then its parallel chunks.
    std::vector<VkCommandBuffer> chainedCommandBuffers;
    void appendSubmitBuffers(std::vector<VkCommandBuffer>& buffers) const {
        buffers.emplace_back(vk);
        buffers.insert(buffers.end(), chainedCommandBuffers.begin(), chainedCommandBuffers.end());
    }
};
struct RenderCommandSemaphore {};
struct VulkanCommandSemaphore : RenderCommandSemaphore { VkSemaphore vk; };
struct RenderCommandFence {};
struct VulkanCommandFence : RenderCommandFence { VkFence vk; };
struct Queue { VkQueue vk = Handle<VkQueue>(1); std::mutex* mutex; };
struct VulkanCommandQueue {
    Queue* queue;
    void executeCommandLists(const RenderCommandList**, uint32_t,
        RenderCommandSemaphore**, uint32_t, RenderCommandSemaphore**, uint32_t,
        RenderCommandFence*);
};
'''

TAIL = r'''
}
int main() try {
    using namespace plume;
    std::mutex mutex;
    Queue nativeQueue{Handle<VkQueue>(1), &mutex};
    VulkanCommandQueue queue{&nativeQueue};
    std::array<VulkanCommandList, 2> commands;
    std::array<const RenderCommandList*, 2> commandPointers;
    for (unsigned i = 0; i < commands.size(); ++i) {
        commands[i].vk = Handle<VkCommandBuffer>(30 + i);
        commandPointers[i] = &commands[i];
    }
    std::array<VulkanCommandSemaphore, 8> waits;
    std::array<RenderCommandSemaphore*, 8> waitPointers;
    for (unsigned i = 0; i < waits.size(); ++i) {
        waits[i].vk = Handle<VkSemaphore>(10 + i);
        waitPointers[i] = &waits[i];
    }
    std::array<VulkanCommandSemaphore, 2> signals;
    std::array<RenderCommandSemaphore*, 2> signalPointers;
    for (unsigned i = 0; i < signals.size(); ++i) {
        signals[i].vk = Handle<VkSemaphore>(20 + i);
        signalPointers[i] = &signals[i];
    }
    VulkanCommandFence fence;
    fence.vk = Handle<VkFence>(40);
    // Growing, shrinking and clearing the thread-local arrays must not leave
    // stale entries. Include submissions with and without signals or a fence.
    expectedBuffers = {Handle<VkCommandBuffer>(30), Handle<VkCommandBuffer>(31)};
    for (const uint32_t count : {8, 1, 0, 3, 2, 0}) {
        expectedWaits = count;
        expectedSignals = count % 3;
        expectedFence = count % 2 ? fence.vk : VK_NULL_HANDLE;
        queue.executeCommandLists(commandPointers.data(), 2,
            count ? waitPointers.data() : nullptr, count,
            expectedSignals ? signalPointers.data() : nullptr, expectedSignals,
            expectedFence ? &fence : nullptr);
    }
    Check(submits == 6 && errors == 0, "successful submissions reported errors");
    // Steprant 2.1.5: a list's parallel chunks follow it, before the next list.
    commands[0].chainedCommandBuffers = {Handle<VkCommandBuffer>(50), Handle<VkCommandBuffer>(51)};
    expectedBuffers = {Handle<VkCommandBuffer>(30), Handle<VkCommandBuffer>(50), Handle<VkCommandBuffer>(51), Handle<VkCommandBuffer>(31)};
    expectedWaits = expectedSignals = 0;
    expectedFence = VK_NULL_HANDLE;
    queue.executeCommandLists(commandPointers.data(), 2, nullptr, 0, nullptr, 0, nullptr);
    Check(submits == 7 && errors == 0, "chunked submission reported errors");
    submitResult = VK_ERROR_DEVICE_LOST;
    queue.executeCommandLists(commandPointers.data(), 2, nullptr, 0, nullptr, 0, nullptr);
    Check(submits == 8 && errors == 1, "failed submission not reported");
    std::puts("PASS: production Vulkan submit: 0/1/2/3/8 wait masks, generic queue stages, array reuse, commands, parallel chunks in order, signals, fences, device-loss reporting");
} catch (const std::exception& error) {
    std::fprintf(stderr, "FAIL: %s\n", error.what());
    return 1;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='g++')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = (PLUME / 'plume_vulkan.cpp').read_text()
    begin = '    void VulkanCommandQueue::executeCommandLists('
    end = '    void VulkanCommandQueue::waitForCommandFence('
    if source.count(begin) != 1 or source.count(end) != 1:
        raise RuntimeError('Production extraction boundary changed')
    method = source[source.index(begin):source.index(end)]
    cpp = out / 'vulkan_queue_submit.cpp'
    cpp.write_text(PRELUDE + method + TAIL)
    flags = ['-std=c++20', '-pthread', '-I' + str(PLUME / 'contrib/Vulkan-Headers/include')]
    if args.sanitize:
        flags += ['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined']
    else:
        flags += ['-O2']
    binary = out / 'vulkan_queue_submit'
    build = subprocess.run([args.cxx, *flags, str(cpp), '-o', str(binary)],
                           capture_output=True, text=True, timeout=120)
    (out / 'build.log').write_text(build.stdout + build.stderr)
    if build.returncode:
        print(build.stdout + build.stderr, end='')
        return build.returncode
    run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    (out / 'run.log').write_text(run.stdout + run.stderr)
    print(run.stdout + run.stderr, end='')
    return run.returncode


if __name__ == '__main__':
    raise SystemExit(main())
