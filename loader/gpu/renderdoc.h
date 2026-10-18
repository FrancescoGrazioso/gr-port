#pragma once
// RenderDoc in-application API (optional). With GR_RENDERDOC=<submission> set, renderdoc.dll is
// loaded before the Vulkan instance exists and that GPU submission is captured to
// <exe>/rdoc/gr_*.rdc. Started from the RenderDoc GUI, the already-injected DLL is used instead.
#include <cstdint>

namespace gr::gpu {

void LoadRenderDoc();               // call before vkCreateInstance
int64_t RenderDocTargetSubmission(); // -1 = no capture requested
void RenderDocStartCapture();
void RenderDocEndCapture();         // after the captured work was submitted

} // namespace gr::gpu
