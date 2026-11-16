#pragma once
#include <plume_vulkan.h>

namespace gpu::submission {
// Plume's virtual begin/end return void. Observe native results before callers
// record commands or publish an executable list, retaining its bookkeeping ABI.
// Forgets every binding plume tracks to skip redundant commands, including
// the last viewport and scissor, which a new command buffer does not have.
inline void ClearBindings(plume::VulkanCommandList& list) {
    list.invalidateExternalCommandState();
}
inline VkResult BeginCommands(plume::VulkanCommandList& list) {
    if (!list.vk || list.recording || list.externalCommandsOpen || list.activeRenderPass)
        return VK_ERROR_UNKNOWN;
    const auto reset = vkResetCommandBuffer(list.vk, 0);
    if (reset != VK_SUCCESS) return reset;
    // Steprant 2.1.5: the previous recording's parallel chunks are no longer
    // in flight (a list is begun again only after its fence).
    list.resetParallelRecording();
    ClearBindings(list);
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    const auto result = vkBeginCommandBuffer(list.vk, &info);
    list.recording = result == VK_SUCCESS;
    if (list.recording) {
        list.startPassProfile();
        // SR4 G-2: the frame graph keeps the list's commands until it ends.
        list.beginCapture();
    }
    return result;
}
inline VkResult EndCommands(plume::VulkanCommandList& list) {
    if (!list.vk || !list.recording || list.externalCommandsOpen) return VK_ERROR_UNKNOWN;
    // Steprant 2.1.5: the replay that ends the list may record in parallel;
    // it returns once every chunk is recorded (see appendSubmitBuffers).
    list.flushCapture(true);
    list.capturing = false;
    list.endActiveRenderPass();
    list.finishPassProfile();
    auto result = vkEndCommandBuffer(list.vk);
    if (result == VK_SUCCESS && list.parallelResult != VK_SUCCESS) result = list.parallelResult;
    if (result == VK_SUCCESS) {
        ClearBindings(list);
        list.recording = false;
        list.externalCommandsOpen = false;
    }
    return result;
}
} // namespace gpu::submission
