// Win32 window + Vulkan swapchain (presenter). It runs on the host main thread and blits the most
// recent frame the guest flipped; until the first frame exists it clears to a slowly pulsing colour.
//
// Normally it shares the renderer's device and blits the renderer's output image directly. When the
// window cannot be presented from that GPU (e.g. the monitor is attached to the integrated GPU while
// rendering happens on the discrete one) it creates its own device on a GPU that can present, and the
// renderer hands it each frame through host memory instead.
#include "vk_window.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#include "gpu/renderer.h"
#include "gpu/vk_device.h"

namespace gr {
namespace {

bool g_resized = false;
bool g_quit = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE: g_resized = true; return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE: g_quit = true; DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool SurfaceUsable(VkPhysicalDevice gpu, VkSurfaceKHR surface) {
    VkSurfaceCapabilitiesKHR caps{};
    return vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps) == VK_SUCCESS;
}

struct Presenter {
    vk::Device& dev = vk::Device::Get();
    // device used for presenting: the shared one, or our own on another GPU (cross mode)
    bool cross = false;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    std::mutex own_queue_m;
    std::string gpu_name;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // cross mode: host frame → staging buffer → image
    gpu::CpuFrame frame;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void* staging_ptr = nullptr;
    VkDeviceSize staging_size = 0;
    VkImage cimg = VK_NULL_HANDLE;
    VkDeviceMemory cimg_mem = VK_NULL_HANDLE;
    uint32_t cimg_w = 0, cimg_h = 0;
    VkFormat cimg_fmt = VK_FORMAT_UNDEFINED;
    bool cimg_valid = false, staging_dirty = false;

    std::mutex& QueueMutex() { return cross ? own_queue_m : dev.queue_mutex(); }

    bool Fail(std::string* err, const char* what, VkResult r = VK_SUCCESS) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s (VkResult %d)", what, int(r));
        *err = buf;
        return false;
    }

    bool CreateOwnDevice(std::string* err) {
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(dev.instance(), &n, nullptr);
        std::vector<VkPhysicalDevice> gpus(n);
        vkEnumeratePhysicalDevices(dev.instance(), &n, gpus.data());
        for (auto g : gpus) {
            if (!SurfaceUsable(g, surface)) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qf(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(g, &qn, qf.data());
            for (uint32_t i = 0; i < qn; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(g, i, surface, &present);
                if (!present || !(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
                const float prio = 1.0f;
                VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
                qci.queueFamilyIndex = i;
                qci.queueCount = 1;
                qci.pQueuePriorities = &prio;
                VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
                f13.synchronization2 = VK_TRUE;
                const char* ext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
                VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
                dci.pNext = &f13;
                dci.queueCreateInfoCount = 1;
                dci.pQueueCreateInfos = &qci;
                dci.enabledExtensionCount = 1;
                dci.ppEnabledExtensionNames = &ext;
                if (VkResult r = vkCreateDevice(g, &dci, nullptr, &device); r != VK_SUCCESS) {
                    device = VK_NULL_HANDLE;
                    break;
                }
                gpu = g;
                family = i;
                vkGetDeviceQueue(device, family, 0, &queue);
                VkPhysicalDeviceProperties p;
                vkGetPhysicalDeviceProperties(g, &p);
                gpu_name = p.deviceName;
                return true;
            }
        }
        return Fail(err, "no GPU can present to this window");
    }

    bool Init(HWND hwnd, std::string* err) {
        if (!dev.ok()) return Fail(err, dev.Error().c_str());
        VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        sci.hinstance = GetModuleHandleW(nullptr);
        sci.hwnd = hwnd;
        if (VkResult r = vkCreateWin32SurfaceKHR(dev.instance(), &sci, nullptr, &surface); r != VK_SUCCESS)
            return Fail(err, "vkCreateWin32SurfaceKHR failed", r);
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(dev.gpu(), dev.queue_family(), surface, &present);
        if (present && SurfaceUsable(dev.gpu(), surface)) {
            gpu = dev.gpu();
            device = dev.device();
            queue = dev.queue();
            family = dev.queue_family();
            gpu_name = dev.gpu_name();
        } else {
            cross = true;
            if (!CreateOwnDevice(err)) return false;
            gpu::RendererEnableCpuFrames(true);
            std::printf("Vulkan: %s cannot present to this window; presenting on %s (frames copied via host memory)\n",
                        dev.gpu_name().c_str(), gpu_name.c_str());
        }
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = family;
        vkCreateCommandPool(device, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cai, &cmd);
        VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &semi, nullptr, &acquired);
        vkCreateSemaphore(device, &semi, nullptr, &rendered);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device, &fci, nullptr, &fence);
        return CreateSwapchain(err);
    }

    bool CreateSwapchain(std::string* err) {
        VkSurfaceCapabilitiesKHR caps{};
        if (VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps); r != VK_SUCCESS)
            return Fail(err, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed", r);
        if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) return true; // minimised
        uint32_t n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr);
        std::vector<VkSurfaceFormatKHR> fmts(n);
        vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, fmts.data());
        VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
        // Guest frames are sRGB-encoded: an sRGB swapchain keeps the blit colour-exact.
        for (const auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && chosen.format != VK_FORMAT_B8G8R8A8_SRGB) chosen = f;
        for (const auto& f : fmts)
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB) chosen = f;
        extent = caps.currentExtent;
        VkSwapchainKHR old = swapchain;
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        ci.surface = surface;
        ci.minImageCount = std::max(caps.minImageCount, 2u);
        if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = extent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        ci.clipped = VK_TRUE;
        ci.oldSwapchain = old;
        if (VkResult r = vkCreateSwapchainKHR(device, &ci, nullptr, &swapchain); r != VK_SUCCESS)
            return Fail(err, "vkCreateSwapchainKHR failed", r);
        if (old) vkDestroySwapchainKHR(device, old, nullptr);
        vkGetSwapchainImagesKHR(device, swapchain, &n, nullptr);
        images.resize(n);
        vkGetSwapchainImagesKHR(device, swapchain, &n, images.data());
        return true;
    }

    void WaitIdle() {
        std::lock_guard<std::mutex> lk(QueueMutex());
        vkDeviceWaitIdle(device);
    }

    static void Barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(c, &dep);
    }

    // Cross mode: make sure the staging buffer / image match the latest host frame (fence already waited).
    void PrepareCrossResources() {
        if (!gpu::CopyCpuFrame(frame)) return;
        const VkDeviceSize need = VkDeviceSize(frame.pixels.size());
        if (need > staging_size) {
            if (staging) { vkDestroyBuffer(device, staging, nullptr); vkFreeMemory(device, staging_mem, nullptr); }
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = need;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            vkCreateBuffer(device, &bci, nullptr, &staging);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(device, staging, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = vk::Device::FindMemoryType(
                gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(device, &ai, nullptr, &staging_mem);
            vkBindBufferMemory(device, staging, staging_mem, 0);
            vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0, &staging_ptr);
            staging_size = need;
        }
        std::memcpy(staging_ptr, frame.pixels.data(), size_t(need));
        if (!cimg || cimg_w != frame.width || cimg_h != frame.height || cimg_fmt != frame.format) {
            if (cimg) { vkDestroyImage(device, cimg, nullptr); vkFreeMemory(device, cimg_mem, nullptr); }
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = frame.format;
            ci.extent = {frame.width, frame.height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            vkCreateImage(device, &ci, nullptr, &cimg);
            VkMemoryRequirements req;
            vkGetImageMemoryRequirements(device, cimg, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = vk::Device::FindMemoryType(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            vkAllocateMemory(device, &ai, nullptr, &cimg_mem);
            vkBindImageMemory(device, cimg, cimg_mem, 0);
            cimg_w = frame.width;
            cimg_h = frame.height;
            cimg_fmt = frame.format;
        }
        staging_dirty = true;
    }

    void BlitFrame(VkCommandBuffer c, VkImage src, uint32_t fw, uint32_t fh, VkImage dst) {
        const float sx = float(extent.width) / fw, sy = float(extent.height) / fh;
        const float s = std::min(sx, sy);
        const int32_t w = int32_t(fw * s), h = int32_t(fh * s);
        const int32_t x = (int32_t(extent.width) - w) / 2, y = (int32_t(extent.height) - h) / 2;
        VkClearColorValue black{{0, 0, 0, 1}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(c, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        // clear and blit both write the swapchain image: without this barrier they may overlap and
        // leave tiles of the clear colour on top of the frame (seen as black blocks on RDNA3)
        Barrier(c, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {int32_t(fw), int32_t(fh), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {x, y, 0};
        blit.dstOffsets[1] = {x + w, y + h, 1};
        vkCmdBlitImage(c, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_LINEAR);
    }

    bool Frame(float t, std::string* err) {
        if (!swapchain) return true;
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        uint32_t idx = 0;
        VkResult r = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { g_resized = true; return true; }
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return Fail(err, "vkAcquireNextImageKHR failed", r);
        vkResetFences(device, 1, &fence);
        if (cross) PrepareCrossResources();

        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        Barrier(cmd, images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        bool drew = false;
        gpu::OutputFrame out;
        if (cross) {
            if (staging_dirty) {
                Barrier(cmd, cimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy bc{};
                bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                bc.imageExtent = {cimg_w, cimg_h, 1};
                vkCmdCopyBufferToImage(cmd, staging, cimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
                Barrier(cmd, cimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                staging_dirty = false;
                cimg_valid = true;
            }
            if (cimg_valid) {
                BlitFrame(cmd, cimg, cimg_w, cimg_h, images[idx]);
                drew = true;
            }
        } else if (gpu::AcquireOutputFrame(&out)) {
            // the renderer keeps its output image in TRANSFER_SRC layout
            BlitFrame(cmd, out.image, out.width, out.height, images[idx]);
            drew = true;
        }
        if (!drew) {
            const float k = 0.5f + 0.5f * std::sin(t * 1.5f);
            VkClearColorValue color{{0.10f + 0.15f * k, 0.06f + 0.05f * k, 0.22f + 0.10f * k, 1.0f}};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cmd, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        }
        Barrier(cmd, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &rendered;
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &rendered;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &idx;
        VkResult sr;
        {
            std::lock_guard<std::mutex> lk(QueueMutex());
            sr = vkQueueSubmit(queue, 1, &si, fence);
            if (sr == VK_SUCCESS) r = vkQueuePresentKHR(queue, &pi);
        }
        if (!cross) gpu::ReleaseOutputFrame();
        if (sr != VK_SUCCESS) return Fail(err, "vkQueueSubmit failed", sr);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_resized = true;
        else if (r != VK_SUCCESS) return Fail(err, "vkQueuePresentKHR failed", r);
        return true;
    }

    void Shutdown() {
        if (!dev.ok() || !device) {
            if (surface) vkDestroySurfaceKHR(dev.instance(), surface, nullptr);
            return;
        }
        WaitIdle();
        if (cross) gpu::RendererEnableCpuFrames(false);
        if (staging) { vkDestroyBuffer(device, staging, nullptr); vkFreeMemory(device, staging_mem, nullptr); }
        if (cimg) { vkDestroyImage(device, cimg, nullptr); vkFreeMemory(device, cimg_mem, nullptr); }
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (acquired) vkDestroySemaphore(device, acquired, nullptr);
        if (rendered) vkDestroySemaphore(device, rendered, nullptr);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
        if (cross) vkDestroyDevice(device, nullptr);
        if (surface) vkDestroySurfaceKHR(dev.instance(), surface, nullptr);
    }
};

} // namespace

int RunVulkanWindow(const WindowOptions& opt, std::string* err) {
    const HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /*IDC_ARROW*/);
    wc.lpszClassName = L"GrPortWindow";
    RegisterClassExW(&wc);

    RECT rc{0, 0, opt.width, opt.height};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    std::wstring title(opt.title.begin(), opt.title.end());
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
    if (!hwnd) { *err = "CreateWindowEx failed"; return 1; }
    ShowWindow(hwnd, SW_SHOW);

    Presenter p;
    if (!p.Init(hwnd, err)) {
        p.Shutdown();
        DestroyWindow(hwnd);
        return 2;
    }
    std::printf("Vulkan: presenting on %s, %ux%u, %zu swapchain images\n", p.gpu_name.c_str(), p.extent.width,
                p.extent.height, p.images.size());
    std::fflush(stdout);

    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    int frames = 0, rc_code = 0;
    g_quit = false;
    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_quit = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_quit) break;
        if (g_resized) {
            g_resized = false;
            p.WaitIdle();
            if (!p.CreateSwapchain(err)) { rc_code = 3; break; }
        }
        QueryPerformanceCounter(&now);
        const float t = float(now.QuadPart - start.QuadPart) / float(freq.QuadPart);
        if (!p.Frame(t, err)) { rc_code = 3; break; }
        if (opt.title_fn && (frames % 30) == 0) {
            const std::string s = opt.title_fn();
            SetWindowTextW(hwnd, std::wstring(s.begin(), s.end()).c_str());
        }
        ++frames;
        if (opt.max_frames && frames >= opt.max_frames) break;
    }
    p.Shutdown();
    if (IsWindow(hwnd)) DestroyWindow(hwnd);
    if (frames) std::printf("presented %d frames\n", frames);
    return rc_code;
}

} // namespace gr
