#include "gpu/renderdoc.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <renderdoc_app.h>

namespace gr::gpu {
namespace {

RENDERDOC_API_1_6_0* g_api = nullptr;
bool g_capturing = false;

std::filesystem::path ExePath() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();
}

// Install directory from the .rdc file association (same lookup as shadPS4), else the default path.
std::filesystem::path RenderDocDll() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\RenderDoc.RDCCapture.1\\DefaultIcon\\", 0, KEY_READ,
                      &key) == ERROR_SUCCESS) {
        wchar_t value[MAX_PATH] = {};
        DWORD size = sizeof value;
        const LONG r = RegQueryValueExW(key, L"", nullptr, nullptr, reinterpret_cast<LPBYTE>(value), &size);
        RegCloseKey(key);
        if (r == ERROR_SUCCESS) return std::filesystem::path(value).parent_path() / "renderdoc.dll";
    }
    return L"C:\\Program Files\\RenderDoc\\renderdoc.dll";
}

} // namespace

int64_t RenderDocTargetSubmission() {
    static const int64_t target = [] { const char* e = std::getenv("GR_RENDERDOC"); return e ? std::atoll(e) : -1; }();
    return target;
}

void LoadRenderDoc() {
    static bool done = false;
    if (done) return;
    done = true;
    HMODULE mod = GetModuleHandleW(L"renderdoc.dll"); // launched from the RenderDoc GUI
    if (!mod && RenderDocTargetSubmission() >= 0) mod = LoadLibraryW(RenderDocDll().c_str());
    if (!mod) {
        if (RenderDocTargetSubmission() >= 0) std::fprintf(stderr, "renderdoc: renderdoc.dll not found\n");
        return;
    }
    auto get_api = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
    if (!get_api || get_api(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_api)) != 1) {
        g_api = nullptr;
        return;
    }
    g_api->SetCaptureKeys(nullptr, 0); // captures are per GPU submission, not present-to-present
    g_api->UnloadCrashHandler();
    const std::string tmpl = (ExePath() / "rdoc" / "gr").string();
    std::filesystem::create_directories(ExePath() / "rdoc");
    g_api->SetCaptureFilePathTemplate(tmpl.c_str());
    std::fprintf(stderr, "renderdoc: loaded, capturing submission %lld to %s_*.rdc\n",
                 static_cast<long long>(RenderDocTargetSubmission()), tmpl.c_str());
}

void RenderDocStartCapture() {
    if (!g_api || g_capturing) return;
    g_api->StartFrameCapture(nullptr, nullptr);
    g_capturing = true;
}

void RenderDocEndCapture() {
    if (!g_api || !g_capturing) return;
    g_capturing = false;
    if (g_api->EndFrameCapture(nullptr, nullptr)) std::fprintf(stderr, "renderdoc: capture written\n");
    else std::fprintf(stderr, "renderdoc: capture failed\n");
}

} // namespace gr::gpu
