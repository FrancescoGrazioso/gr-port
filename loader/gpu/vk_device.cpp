#include "gpu/vk_device.h"
#include "gpu/renderdoc.h"

#include <cstdio>
#include <vector>

#define GR_VK_DEFINE(fn) PFN_##fn fn = nullptr;
GR_VK_GLOBAL_FUNCS(GR_VK_DEFINE)
GR_VK_INSTANCE_FUNCS(GR_VK_DEFINE)
GR_VK_DEVICE_FUNCS(GR_VK_DEFINE)
#undef GR_VK_DEFINE

namespace gr::vk {

Device& Device::Get() {
    static Device d;
    return d;
}

Device::Device() {
    if (!Init() && error_.empty()) error_ = "Vulkan initialisation failed";
    if (!error_.empty()) std::fprintf(stderr, "vulkan: %s\n", error_.c_str());
}

bool Device::Init() {
    gr::gpu::LoadRenderDoc(); // must hook Vulkan before the instance is created
    lib_ = LoadLibraryW(L"vulkan-1.dll");
    if (!lib_) { error_ = "vulkan-1.dll not found"; return false; }
    auto get_instance_proc =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib_, "vkGetInstanceProcAddr"));
#define GR_LOAD_GLOBAL(fn) fn = reinterpret_cast<PFN_##fn>(get_instance_proc(nullptr, #fn));
    GR_VK_GLOBAL_FUNCS(GR_LOAD_GLOBAL)
#undef GR_LOAD_GLOBAL

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gr-port";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* exts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    if (vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) { error_ = "vkCreateInstance failed"; return false; }
#define GR_LOAD_INSTANCE(fn) fn = reinterpret_cast<PFN_##fn>(get_instance_proc(instance_, #fn));
    GR_VK_INSTANCE_FUNCS(GR_LOAD_INSTANCE)
#undef GR_LOAD_INSTANCE

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance_, &n, gpus.data());
    int best = -1;
    for (auto g : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(g, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qf.data());
        for (uint32_t i = 0; i < qn; ++i) {
            if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) {
                best = score;
                gpu_ = g;
                queue_family_ = i;
                gpu_name_ = p.deviceName;
            }
            break;
        }
    }
    if (!gpu_) { error_ = "no Vulkan 1.3 GPU with a graphics queue"; return false; }
    vkGetPhysicalDeviceMemoryProperties(gpu_, &mem_props_);

    // Features: core 1.0 subset + 1.2/1.3 features the renderer relies on.
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feats.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(gpu_, &feats);
    if (!f13.dynamicRendering || !f13.synchronization2) { error_ = "GPU lacks dynamic rendering / sync2"; return false; }

    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;
    e13.shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation;
    e13.subgroupSizeControl = f13.subgroupSizeControl;   // compute: wave64-sized subgroups
    e13.computeFullSubgroups = f13.computeFullSubgroups;
    subgroup_size_control_ = f13.subgroupSizeControl && f13.computeFullSubgroups;
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.pNext = &e13;
    e12.scalarBlockLayout = f12.scalarBlockLayout;
    e12.shaderFloat16 = f12.shaderFloat16;
    e12.storageBuffer8BitAccess = f12.storageBuffer8BitAccess;
    e12.uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess;
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    e11.pNext = &e12;
    e11.storageBuffer16BitAccess = f11.storageBuffer16BitAccess;
    VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enable.pNext = &e11;
    auto& b = feats.features;
    auto& e = enable.features;
    e.independentBlend = b.independentBlend;
    e.textureCompressionBC = b.textureCompressionBC;
    e.samplerAnisotropy = b.samplerAnisotropy;
    e.fragmentStoresAndAtomics = b.fragmentStoresAndAtomics;
    e.vertexPipelineStoresAndAtomics = b.vertexPipelineStoresAndAtomics;
    e.shaderClipDistance = b.shaderClipDistance;
    e.depthClamp = b.depthClamp;
    e.imageCubeArray = b.imageCubeArray;
    e.fillModeNonSolid = b.fillModeNonSolid;
    e.shaderInt64 = b.shaderInt64;
    e.shaderInt16 = b.shaderInt16;
    e.shaderImageGatherExtended = b.shaderImageGatherExtended;
    e.dualSrcBlend = b.dualSrcBlend;
    e.logicOp = b.logicOp;
    e.shaderStorageImageWriteWithoutFormat = b.shaderStorageImageWriteWithoutFormat;
    {
        VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &p13;
        vkGetPhysicalDeviceProperties2(gpu_, &p2);
        wave64_ = subgroup_size_control_ && p13.minSubgroupSize <= 64 && p13.maxSubgroupSize >= 64 &&
                  (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &enable;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = dev_exts;
    if (VkResult r = vkCreateDevice(gpu_, &dci, nullptr, &device_); r != VK_SUCCESS) {
        error_ = "vkCreateDevice failed (" + std::to_string(int(r)) + ")";
        device_ = VK_NULL_HANDLE;
        return false;
    }
    // Loader trampolines (instance-level lookup) dispatch per device, so the same entry points also
    // serve the presenter's own device when the display hangs off another GPU.
#define GR_LOAD_DEVICE(fn) fn = reinterpret_cast<PFN_##fn>(get_instance_proc(instance_, #fn));
    GR_VK_DEVICE_FUNCS(GR_LOAD_DEVICE)
#undef GR_LOAD_DEVICE
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    return true;
}

uint32_t Device::FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

uint32_t Device::FindMemoryType(VkPhysicalDevice gpu, uint32_t type_bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

Buffer Device::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    vkCreateBuffer(device_, &ci, nullptr, &b.buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, host_visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                                                         : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &ai, nullptr, &b.memory);
    vkBindBufferMemory(device_, b.buffer, b.memory, 0);
    if (host_visible) vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped);
    return b;
}

void Device::DestroyBuffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

Image Device::CreateImage(VkFormat fmt, uint32_t w, uint32_t h, uint32_t mips, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect, VkImageCreateFlags flags) {
    Image img;
    img.format = fmt;
    img.width = w;
    img.height = h;
    img.mips = mips;
    img.aspect = aspect;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = flags;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ci, nullptr, &img.image) != VK_SUCCESS) return {};
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &ai, nullptr, &img.memory);
    vkBindImageMemory(device_, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, mips, 0, 1};
    vkCreateImageView(device_, &vi, nullptr, &img.view);
    return img;
}

void Device::DestroyImage(Image& img) {
    if (img.view) vkDestroyImageView(device_, img.view, nullptr);
    if (img.image) vkDestroyImage(device_, img.image, nullptr);
    if (img.memory) vkFreeMemory(device_, img.memory, nullptr);
    img = {};
}

void Device::Transition(VkCommandBuffer cmd, Image& img, VkImageLayout to) {
    if (img.layout == to) return;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, img.mips, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    img.layout = to;
}

} // namespace gr::vk
