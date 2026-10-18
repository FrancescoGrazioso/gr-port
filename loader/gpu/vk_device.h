#pragma once
// Process-wide Vulkan device shared by the GPU renderer (gpu thread) and the presenter (window).
// Vulkan is loaded at runtime from vulkan-1.dll; every entry point used by the project is a
// global function pointer declared here.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdint>
#include <mutex>
#include <string>

#define GR_VK_GLOBAL_FUNCS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define GR_VK_INSTANCE_FUNCS(X)                                                                           \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                   \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2)                                     \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)                    \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)                       \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR)             \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                  \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define GR_VK_DEVICE_FUNCS(X)                                                                             \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit) X(vkQueueWaitIdle)        \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)  \
    X(vkQueuePresentKHR) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)       \
    X(vkFreeCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer)         \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences)      \
    X(vkResetFences) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory)                  \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)           \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)               \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler)                     \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout)                            \
    X(vkDestroyPipelineLayout) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout)             \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCmdPipelineBarrier) X(vkCmdPipelineBarrier2)    \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdCopyBufferToImage)                      \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkCmdBeginRendering)                  \
    X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed)    \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdPushConstants) X(vkCmdPushDescriptorSetKHR)             \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable)      \
    X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable)                    \
    X(vkCmdBindDescriptorSets) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool)    \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdSetStencilOp) X(vkCmdSetStencilCompareMask)  \
    X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetBlendConstants) X(vkCmdCopyBuffer)      \
    X(vkCmdFillBuffer) X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetDepthBiasEnable) X(vkCmdSetRasterizerDiscardEnable) \
    X(vkCmdSetPrimitiveRestartEnable) X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCreatePipelineCache)    \
    X(vkGetPipelineCacheData) X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery)  \
    X(vkGetQueryPoolResults)

#define GR_VK_DECLARE(fn) extern PFN_##fn fn;
GR_VK_GLOBAL_FUNCS(GR_VK_DECLARE)
GR_VK_INSTANCE_FUNCS(GR_VK_DECLARE)
GR_VK_DEVICE_FUNCS(GR_VK_DECLARE)
#undef GR_VK_DECLARE

namespace gr::vk {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0, mips = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

class Device {
public:
    static Device& Get(); // created on first use (thread-safe)
    bool ok() const { return device_ != VK_NULL_HANDLE; }
    const std::string& Error() const { return error_; }

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice gpu() const { return gpu_; }
    VkDevice device() const { return device_; }
    uint32_t queue_family() const { return queue_family_; }
    const std::string& gpu_name() const { return gpu_name_; }
    bool wave64() const { return wave64_; } // compute pipelines can require 64-wide subgroups

    // The single queue is shared by renderer and presenter: hold this lock around submit/present.
    std::mutex& queue_mutex() { return queue_m_; }
    VkQueue queue() const { return queue_; }

    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const;
    static uint32_t FindMemoryType(VkPhysicalDevice gpu, uint32_t type_bits, VkMemoryPropertyFlags flags);
    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible);
    void DestroyBuffer(Buffer& b);
    Image CreateImage(VkFormat fmt, uint32_t w, uint32_t h, uint32_t mips, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, VkImageCreateFlags flags = 0);
    void DestroyImage(Image& img);

    // Records an image layout transition (full subresource range).
    static void Transition(VkCommandBuffer cmd, Image& img, VkImageLayout to);

private:
    Device();
    bool Init();

    HMODULE lib_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkPhysicalDeviceMemoryProperties mem_props_{};
    std::string gpu_name_, error_;
    bool subgroup_size_control_ = false, wave64_ = false;
    std::mutex queue_m_;
};

} // namespace gr::vk
