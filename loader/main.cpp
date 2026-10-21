// gr_port — loader.
//
//   gr_port <uroot_dir> [--run] [--no-window] [--timeout S] [--trace N]
//                       [--names file.txt] [--report out.txt] [--frames N]
//
// Loads eboot.bin and the bundled sce_module/*.prx, applies relocations, binds imports
// (bundled module > HLE > logging stub), patches guest TLS accesses and opens the Vulkan window.
// With --run (phase 1.1) it also starts the required prx modules and calls the eboot entry point
// on a guest thread, logging every HLE call to gr_port_trace.txt.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "gpu/capture.h"
#include "gpu/renderer.h"
#include "gpu/gpu.h"
#include "gpu/videoout.h"
#include "guest.h"
#include "guest_bp.h"
#include "vmem.h"
#include "hle.h"
#include "kernel/kernel.h"
#include "kernel/overlay.h"
#include "kernel/audio_out.h"
#include "linker.h"
#include "nid.h"
#include "profiler.h"
#include "tls.h"
#include "vk_window.h"

namespace fs = std::filesystem;

static fs::path ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

static bool SelfTest(gr::Hle& hle) {
    const uint64_t add = hle.GetFunctionThunk(gr::ComputeNid("gr_selftest_add"), "gr_selftest_add", "selftest");
    const uint64_t stub = hle.GetFunctionThunk("AAAAAAAAAAA", "selftest_stub", "selftest");
    const uint64_t r1 = gr::CallGuest(add, {40, 2});
    const uint64_t r2 = gr::CallGuest(stub, {1, 2});
    const bool ok = r1 == 42 && r2 == 0;
    std::printf("ABI thunk self-test: add(40,2)=%llu stub()=%llu -> %s\n", (unsigned long long)r1,
                (unsigned long long)r2, ok ? "OK" : "FAILED");
    return ok;
}

// Modules the eboot needs (transitively), in an order where dependencies start first.
static std::vector<gr::LoadedModule*> StartOrder(const gr::Linker& linker) {
    std::map<std::string, gr::LoadedModule*> by_name;
    for (const auto& m : linker.Modules()) by_name[m->name] = m.get();
    std::vector<gr::LoadedModule*> order;
    std::set<std::string> visiting, done;
    std::function<void(gr::LoadedModule*)> visit = [&](gr::LoadedModule* m) {
        if (done.count(m->name) || visiting.count(m->name)) return;
        visiting.insert(m->name);
        for (const auto& dep : m->oe.needed_modules)
            if (auto it = by_name.find(dep.name); it != by_name.end()) visit(it->second);
        done.insert(m->name);
        order.push_back(m);
    };
    for (const auto& dep : linker.Modules()[0]->oe.needed_modules)
        if (auto it = by_name.find(dep.name); it != by_name.end()) visit(it->second);
    return order;
}

static void RunGuest(gr::Linker& linker, gr::Hle& hle) {
    gr::kernel::InitMainThread();
    for (gr::LoadedModule* m : StartOrder(linker)) {
        const uint64_t init = m->base + (m->oe.has_init ? m->oe.init : m->oe.ehdr.e_entry);
        gr::TraceNote("starting module %s (init 0x%llx)", m->name.c_str(), (unsigned long long)init);
        const uint64_t r = gr::CallGuest(init, {0, 0, 0});
        gr::TraceNote("module %s init returned 0x%llx", m->name.c_str(), (unsigned long long)r);
    }
    // eboot entry: void _start(EntryParams* params, void (*exit_handler)(void))
    struct EntryParams {
        int32_t argc;
        uint32_t pad;
        const char* argv[3];
    };
    static const char kArgv0[] = "eboot.bin";
    static EntryParams params{1, 0, {kArgv0, nullptr, nullptr}};
    const uint64_t exit_handler = hle.GetFunctionThunk(gr::ComputeNid("gr_exit_handler"), "gr_exit_handler", "gr");
    gr::InstallGuestBreakpoints(gr::kGuestModuleBase); // the eboot is the first module mapped there
    gr::TraceNote("calling eboot entry 0x%llx", (unsigned long long)linker.EntryPoint());
    const uint64_t r = gr::CallGuest(linker.EntryPoint(), {reinterpret_cast<uint64_t>(&params), exit_handler});
    gr::TraceNote("eboot entry returned 0x%llx", (unsigned long long)r);
}

static void PrintCallSummary(const gr::Hle& hle) {
    std::vector<const gr::ThunkInfo*> called;
    for (const auto* t : hle.Thunks())
        if (t->calls > 0) called.push_back(t);
    std::sort(called.begin(), called.end(), [](auto* x, auto* y) { return x->calls > y->calls; });
    size_t stubs = 0;
    for (const auto* t : called) stubs += t->func == nullptr;
    std::printf("== HLE calls so far: %zu distinct functions called, %zu of them still stubs\n", called.size(), stubs);
    for (const auto* t : called)
        std::printf("  %8llu  %s %s::%s\n", (unsigned long long)t->calls.load(), t->func ? "HLE " : "STUB",
                    t->library.c_str(), t->name.empty() ? t->nid.c_str() : t->name.c_str());
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <uroot_dir> [--run] [--no-window] [--timeout S] [--trace N] [--names f.txt]"
                     " [--report out.txt] [--frames N]\n"
                     "  [--mute]\n"
                     "  testing: [--skip-opening] [--warp MISSION] [--mods DIR] [--savedata DIR]"
                     " [--dump-at F1,F2,...] [--dump-dir DIR] [--exit-after-dumps]\n"
                     "  (see docs/DEBUGGING.md and docs/MODDING_AND_TESTING.md)\n",
                     argv[0]);
        return 1;
    }
    const fs::path root = fs::absolute(argv[1]);
    fs::path names_path = ExeDir() / "orbis_names.txt";
    fs::path report_path = "gr_port_report.txt";
    bool window = true, run = false;
    int frames = 0, timeout_s = 30;
    uint64_t trace_lines = 200000;
    std::string input_script;
    double trace_after = 0;
    int64_t capture_submission = -2; // -2 = capture disabled, -1 = dump shaders only
    uint32_t dump_every = 0, dump_first = 0;
    std::vector<uint32_t> dump_at;
    fs::path dump_dir = ExeDir() / "frames";
    bool exit_after_dumps = false;
    bool gpu_enabled = true;
    fs::path savedata_dir = ExeDir() / "savedata";
    std::vector<std::string> trace_lle;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--names" && i + 1 < argc) names_path = argv[++i];
        else if (a == "--report" && i + 1 < argc) report_path = argv[++i];
        else if (a == "--no-window") window = false;
        else if (a == "--run") run = true;
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--timeout" && i + 1 < argc) timeout_s = std::atoi(argv[++i]);
        else if (a == "--trace" && i + 1 < argc) trace_lines = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--input" && i + 1 < argc) input_script = argv[++i];
        else if (a == "--trace-after" && i + 1 < argc) trace_after = std::atof(argv[++i]);
        else if (a == "--capture" && i + 1 < argc) capture_submission = std::atoll(argv[++i]);
        else if (a == "--dump-frames" && i + 1 < argc) dump_every = uint32_t(std::atoi(argv[++i]));
        else if (a == "--dump-first" && i + 1 < argc) dump_first = uint32_t(std::atoi(argv[++i]));
        else if (a == "--dump-at" && i + 1 < argc) {
            for (const char* p = argv[++i]; *p;) { // comma-separated flip numbers
                char* end = nullptr;
                const unsigned long v = std::strtoul(p, &end, 10);
                if (end == p) break;
                dump_at.push_back(uint32_t(v));
                p = end;
                while (*p == ',') ++p;
            }
            std::sort(dump_at.begin(), dump_at.end());
        }
        else if (a == "--dump-dir" && i + 1 < argc) dump_dir = fs::absolute(argv[++i]);
        else if (a == "--exit-after-dumps") exit_after_dumps = true;
        else if (a == "--no-gpu") gpu_enabled = false;
        // development / testing (docs/MODDING_AND_TESTING.md): nothing on disk is modified
        else if (a == "--mods" && i + 1 < argc) gr::kernel::SetOverlayModsDir(fs::absolute(argv[++i]));
        else if (a == "--warp" && i + 1 < argc) gr::kernel::SetOverlayWarp(argv[++i]);
        else if (a == "--skip-opening") gr::kernel::SetOverlaySkipOpening(true);
        else if (a == "--mute") gr::kernel::AudioOutSetMuted(true);
        else if (a == "--savedata" && i + 1 < argc) savedata_dir = fs::absolute(argv[++i]);
        else if (a == "--trace-lle" && i + 1 < argc) {
            std::string list = argv[++i];
            for (size_t p = 0; p < list.size();) {
                const size_t c = std::min(list.find(',', p), list.size());
                trace_lle.push_back(list.substr(p, c - p));
                p = c + 1;
            }
        }
    }
    setvbuf(stdout, nullptr, _IONBF, 0);

    gr::NidDatabase names;
    const size_t n_names = names.LoadNamesFile(names_path.string());
    std::printf("NID database: %zu names (%s)\n", n_names, names_path.string().c_str());

    // HLE implementations must be registered before linking: thunks bind at creation time.
    gr::Hle hle;
    gr::RegisterPhase0Hle(hle);
    gr::kernel::RegisterMisc(hle);
    gr::kernel::RegisterMemory(hle);
    gr::kernel::RegisterThreads(hle);
    gr::kernel::RegisterFiles(hle);
    gr::kernel::RegisterLibcInternal(hle);
    gr::kernel::RegisterSync(hle);
    gr::kernel::RegisterServices(hle);
    gr::kernel::RegisterFiber(hle);
    gr::kernel::RegisterAudio(hle);
    gr::kernel::RegisterPad(hle);
    gr::kernel::RegisterAvPlayer(hle);
    gr::kernel::RegisterFont(hle);
    gr::kernel::RegisterSaveData(hle);
    if (!input_script.empty()) gr::kernel::SetPadScript(input_script);
    gr::gpu::RegisterGnmDriver(hle);
    gr::videoout::Register(hle);
    hle.Register("gr_exit_handler", [](const uint64_t*) -> uint64_t {
        gr::TraceNote("guest invoked the exit handler");
        return 0;
    });
    if (!SelfTest(hle)) return 4;

    gr::Linker linker(hle, names);
    std::string err;
    if (!linker.Load((root / "eboot.bin").string(), &err)) {
        std::fprintf(stderr, "eboot: %s\n", err.c_str());
        return 2;
    }
    std::vector<fs::path> prx;
    if (fs::exists(root / "sce_module"))
        for (const auto& e : fs::directory_iterator(root / "sce_module"))
            if (e.path().extension() == ".prx") prx.push_back(e.path());
    std::sort(prx.begin(), prx.end());
    for (const auto& p : prx)
        if (!linker.Load(p.string(), &err)) std::fprintf(stderr, "warning: %s: %s\n", p.string().c_str(), err.c_str());
    linker.TraceLleModules(trace_lle);
    linker.LinkAll();

    gr::GuestTls tls;
    if (!tls.Init(&err)) {
        std::fprintf(stderr, "TLS: %s\n", err.c_str());
        return 5;
    }
    uint64_t patched = 0, skipped = 0;
    for (const auto& m : linker.Modules()) {
        tls.AddModule(*m);
        const auto st = tls.PatchModule(*m);
        patched += st.patched;
        skipped += st.skipped_other_disp;
    }
    linker.Protect();
    std::printf("TLS: slot %u, %llu fs:[0] reads patched, %llu other fs: forms left untouched\n", tls.Slot(),
                (unsigned long long)patched, (unsigned long long)skipped);

    gr::Runtime& rt = gr::Rt();
    rt.app0 = root;
    rt.savedata = savedata_dir;
    rt.linker = &linker;
    rt.tls = &tls;
    const auto& eboot = *linker.Modules()[0];
    for (const auto& p : eboot.oe.phdrs)
        if (p.p_type == gr::PT_SCE_PROCPARAM) rt.proc_param = eboot.base + p.p_vaddr;

    const std::string report = linker.Report();
    std::ofstream(report_path) << report;
    const auto cut = report.find("== host-side functions");
    std::printf("%s", report.substr(0, cut).c_str());
    const auto& st = linker.Stats();
    std::printf("full report: %s\n", fs::absolute(report_path).string().c_str());

    HANDLE guest = nullptr;
    if (capture_submission != -2) gr::gpu::CaptureConfigure((ExeDir() / "capture").string(), capture_submission);
    {
        gr::gpu::RendererConfig rc;
        rc.enabled = gpu_enabled;
        rc.dump_every = dump_every;
        rc.dump_first = dump_first;
        rc.dump_at = dump_at;
        rc.exit_after_dumps = exit_after_dumps && !dump_at.empty();
        if (dump_every || !dump_at.empty()) rc.dump_dir = dump_dir.string();
        gr::gpu::RendererConfigure(rc);
    }
    if (run) {
        gr::InstallCrashHandler();
        gr::TraceOpen("gr_port_trace.txt", trace_lines, trace_after);
        std::printf("== running guest (trace: %s)\n", fs::absolute("gr_port_trace.txt").string().c_str());
        guest = gr::StartGuestThread([&] { RunGuest(linker, hle); }, 16u << 20, nullptr, "main");
    } else {
        std::printf("guest entry point 0x%llx (not executed: pass --run)\n", (unsigned long long)linker.EntryPoint());
    }

    int rc = 0;
    if (window) {
        gr::WindowOptions wo;
        char title[256];
        std::snprintf(title, sizeof title, "Gravity Rush - gr-port | %zu modules | imports LLE %llu / HLE %llu / stub %llu",
                      linker.Modules().size(), (unsigned long long)st.imports_lle, (unsigned long long)st.imports_hle,
                      (unsigned long long)st.imports_stub);
        wo.title = title;
        wo.max_frames = frames;
        if (run)
            wo.title_fn = [] {
                const auto fi = gr::videoout::Current();
                char t[256];
                std::snprintf(t, sizeof t, "Gravity Rush - gr-port | guest flips %llu | vblank %llu | frame %ux%u",
                              (unsigned long long)fi.flip_count, (unsigned long long)fi.vblank_count, fi.width, fi.height);
                return std::string(t);
            };
        rc = gr::RunVulkanWindow(wo, &err);
        if (rc) std::fprintf(stderr, "Vulkan window: %s\n", err.c_str());
    } else if (guest) {
        if (WaitForSingleObject(guest, DWORD(timeout_s) * 1000) == WAIT_TIMEOUT) {
            std::printf("== guest still running after %d s (timeout); thread states:\n", timeout_s);
            gr::DumpGuestThreads();
        } else {
            std::printf("== guest main thread finished\n");
        }
    }
    if (run) std::printf("%s\n", gr::gpu::StatsSummary().c_str());
    if (run) std::printf("%s\n", gr::gpu::RendererStats().c_str());
    if (run) PrintCallSummary(hle);
    if (run) std::printf("%s\n", gr::ProfileReport().c_str());
    std::fflush(stdout);
    ExitProcess(UINT(rc)); // guest threads may still be running
}
