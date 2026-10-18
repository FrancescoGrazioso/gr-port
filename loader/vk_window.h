#pragma once
#include <functional>
#include <string>

namespace gr {

struct WindowOptions {
    std::string title = "Gravity Rush - gr-port";
    int width = 1280;
    int height = 720;
    int max_frames = 0; // 0 = run until closed (used by automated smoke tests)
    std::function<std::string()> title_fn; // polled twice a second to refresh the title
};

// Opens a Win32 window with a Vulkan swapchain and presents a cleared frame each vsync.
// Returns 0 on clean exit, non-zero (and fills *err) when Vulkan could not be initialised.
int RunVulkanWindow(const WindowOptions& opt, std::string* err);

} // namespace gr
