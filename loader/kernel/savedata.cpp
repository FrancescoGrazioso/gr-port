// libSceSaveData (+ SaveDataDialog, CommonDialog, MsgDialog, ErrorDialog) and an offline
// libSceNpTrophy.
//
// Save directories live in <exe dir>/savedata/<dirName>/ and are mounted as /savedata0 through the
// VFS in files.cpp. "Save data memory" (the small system save) is <exe dir>/savedata/sdmemory.bin.
// Dialogs complete immediately with an affirmative result; trophies are accepted and ignored.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "guest.h"
#include "kernel.h"

namespace fs = std::filesystem;

namespace gr::kernel {
namespace {

constexpr uint64_t kErrNotMounted = 0x809F0004, kErrNotFound = 0x809F0008, kErrParameter = 0x809F0001,
                   kErrExists = 0x809F0007;
constexpr uint32_t kMountRdOnly = 0x1, kMountRdWr = 0x2, kMountCreate = 0x4, kMountCreate2 = 0x20;
constexpr uint32_t kStatusCreated = 1;
constexpr int32_t kDialogStatusFinished = 3;

std::mutex g_sd;
std::string g_mounted_dir;

fs::path Root() { return Rt().savedata; }

std::string DirName(uint64_t p) { // SceSaveDataDirName { char data[32]; }
    if (!p) return {};
    char buf[33] = {};
    std::memcpy(buf, Ptr<const char>(p), 32);
    return buf;
}

bool Matches(const std::string& pattern, const std::string& name) { // '%' = any sequence
    if (pattern.empty()) return true;
    const size_t pct = pattern.find('%');
    if (pct == std::string::npos) return pattern == name;
    return name.compare(0, pct, pattern, 0, pct) == 0;
}

fs::path MemoryFile() { return Root() / "sdmemory.bin"; }

// Ensures the save-data-memory file exists with at least `size` bytes.
void EnsureMemory(uint64_t size) {
    std::error_code ec;
    fs::create_directories(Root(), ec);
    const fs::path f = MemoryFile();
    if (!fs::exists(f, ec) || fs::file_size(f, ec) < size) {
        std::vector<char> zeros(size_t(size), 0);
        std::fstream s(f, std::ios::binary | std::ios::in | std::ios::out);
        if (!s) s.open(f, std::ios::binary | std::ios::out);
        s.seekp(0, std::ios::end);
        const auto have = uint64_t(s.tellp());
        if (have < size) s.write(zeros.data(), std::streamsize(size - have));
    }
}

void ImmediateDialog(Hle& h, const char* prefix) {
    const std::string p = prefix;
    for (const char* suffix : {"Initialize", "Terminate", "Close"})
        h.Register(p + suffix, [](const uint64_t*) -> uint64_t { return 0; });
    h.Register(p + "Open", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register(p + "UpdateStatus", [](const uint64_t*) -> uint64_t { return uint64_t(kDialogStatusFinished); });
    h.Register(p + "GetStatus", [](const uint64_t*) -> uint64_t { return uint64_t(kDialogStatusFinished); });
}

} // namespace

void RegisterSaveData(Hle& h) {
    for (const char* n : {"sceSaveDataInitialize", "sceSaveDataInitialize2", "sceSaveDataInitialize3", "sceSaveDataTerminate"})
        h.Register(n, [](const uint64_t*) -> uint64_t { return 0; });

    // (userId, size_t memorySize, SceSaveDataParam* param)
    h.Register("sceSaveDataSetupSaveDataMemory", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_sd);
        EnsureMemory(a[1]);
        return 0;
    });
    // (userId, void* buf, size_t size, i64 offset)
    h.Register("sceSaveDataGetSaveDataMemory", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_sd);
        EnsureMemory(a[3] + a[2]);
        std::ifstream s(MemoryFile(), std::ios::binary);
        s.seekg(std::streamoff(a[3]));
        s.read(Ptr<char>(a[1]), std::streamsize(a[2]));
        return 0;
    });
    // (userId, const void* buf, size_t size, i64 offset)
    h.Register("sceSaveDataSetSaveDataMemory", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_sd);
        EnsureMemory(a[3] + a[2]);
        std::fstream s(MemoryFile(), std::ios::binary | std::ios::in | std::ios::out);
        s.seekp(std::streamoff(a[3]));
        s.write(Ptr<const char>(a[1]), std::streamsize(a[2]));
        TraceNote("savedata: system save memory written (%llu bytes)", (unsigned long long)a[2]);
        return 0;
    });

    // SceSaveDataDirNameSearchCond { i32 userId; pad; titleId*; dirName* (pattern); u32 key; u32 order; ... }
    // SceSaveDataDirNameSearchResult { u32 hitNum; pad; SceSaveDataDirName* dirNames; u32 dirNamesNum;
    //                                  u32 setNum; SceSaveDataParam* params; SceSaveDataSearchInfo* infos; }
    h.Register("sceSaveDataDirNameSearch", [](const uint64_t* a) -> uint64_t {
        if (!a[0] || !a[1]) return kErrParameter;
        std::lock_guard<std::mutex> lk(g_sd);
        const std::string pattern = DirName(Load<uint64_t>(a[0] + 16));
        std::vector<std::string> hits;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(Root(), ec))
            if (e.is_directory() && e.path().filename() != "unmounted" && e.path().filename() != "data" &&
                e.path().filename() != "temp" && Matches(pattern, e.path().filename().string()))
                hits.push_back(e.path().filename().string());
        const uint64_t res = a[1];
        const uint64_t names = Load<uint64_t>(res + 8);
        const uint32_t cap = Load<uint32_t>(res + 16);
        uint32_t set = 0;
        for (const auto& name : hits) {
            if (!names || set >= cap) break;
            char buf[32] = {};
            std::snprintf(buf, sizeof buf, "%s", name.c_str());
            std::memcpy(Ptr<void>(names + 32ull * set), buf, 32);
            ++set;
        }
        Store<uint32_t>(res, uint32_t(hits.size()));
        Store<uint32_t>(res + 20, set);
        TraceNote("savedata: search '%s' -> %zu hit(s)", pattern.c_str(), hits.size());
        return 0;
    });

    // SceSaveDataMount { i32 userId; pad; titleId*; dirName*; fingerprint*; u64 blocks; u32 mountMode; ... }
    // SceSaveDataMountResult { char mountPoint[16]; u64 requiredBlocks; u32 unused; u32 mountStatus; ... }
    h.Register("sceSaveDataMount", [](const uint64_t* a) -> uint64_t {
        if (!a[0] || !a[1]) return kErrParameter;
        std::lock_guard<std::mutex> lk(g_sd);
        const std::string dir = DirName(Load<uint64_t>(a[0] + 16));
        const uint32_t mode = Load<uint32_t>(a[0] + 40);
        const fs::path path = Root() / dir;
        std::error_code ec;
        uint32_t status = 0;
        if (!fs::exists(path, ec)) {
            if (!(mode & (kMountCreate | kMountCreate2))) {
                TraceNote("savedata: mount '%s' -> not found", dir.c_str());
                return kErrNotFound;
            }
            fs::create_directories(path, ec);
            status = kStatusCreated;
        } else if ((mode & kMountCreate) && !(mode & kMountCreate2) && !(mode & kMountRdWr) && !(mode & kMountRdOnly)) {
            return kErrExists;
        }
        g_mounted_dir = dir;
        Rt().savedata_mount = path;
        uint8_t r[64] = {};
        std::memcpy(r, "/savedata0", 11);
        std::memcpy(r + 28, &status, 4);
        std::memcpy(Ptr<void>(a[1]), r, sizeof r);
        TraceNote("savedata: mounted '%s' (mode 0x%x)%s", dir.c_str(), mode, status ? " [created]" : "");
        return 0;
    });
    h.Register("sceSaveDataUmount", [](const uint64_t*) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_sd);
        if (g_mounted_dir.empty()) return kErrNotMounted;
        TraceNote("savedata: unmounted '%s'", g_mounted_dir.c_str());
        g_mounted_dir.clear();
        Rt().savedata_mount.clear();
        return 0;
    });
    // (const SceSaveDataMountPoint*, u32 paramType, const void* buf, size_t size)
    h.Register("sceSaveDataSetParam", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_sd);
        if (g_mounted_dir.empty()) return kErrNotMounted;
        if (a[2] && a[3]) {
            std::ofstream(Root() / g_mounted_dir / "sce_param.bin", std::ios::binary)
                .write(Ptr<const char>(a[2]), std::streamsize(a[3]));
        }
        return 0;
    });
    // (const SceSaveDataMountPoint*, const SceSaveDataIcon* {void* buf; size_t bufSize; size_t dataSize; ...})
    h.Register("sceSaveDataSaveIcon", [](const uint64_t* a) -> uint64_t {
        std::lock_guard<std::mutex> lk(g_sd);
        if (g_mounted_dir.empty()) return kErrNotMounted;
        if (a[1] && Load<uint64_t>(a[1]) && Load<uint64_t>(a[1] + 16))
            std::ofstream(Root() / g_mounted_dir / "icon0.png", std::ios::binary)
                .write(Ptr<const char>(Load<uint64_t>(a[1])), std::streamsize(Load<uint64_t>(a[1] + 16)));
        return 0;
    });
    // SceSaveDataDelete { i32 userId; pad; titleId*; dirName*; ... }
    h.Register("sceSaveDataDelete", [](const uint64_t* a) -> uint64_t {
        if (!a[0]) return kErrParameter;
        std::lock_guard<std::mutex> lk(g_sd);
        const std::string dir = DirName(Load<uint64_t>(a[0] + 16));
        if (dir.empty()) return kErrParameter;
        std::error_code ec;
        fs::remove_all(Root() / dir, ec);
        TraceNote("savedata: deleted '%s'", dir.c_str());
        return 0;
    });

    // ---- dialogs: finish immediately
    ImmediateDialog(h, "sceSaveDataDialog");
    ImmediateDialog(h, "sceMsgDialog");
    ImmediateDialog(h, "sceErrorDialog");
    h.Register("sceSaveDataDialogIsReadyToDisplay", [](const uint64_t*) -> uint64_t { return 1; });
    // SceSaveDataDialogResult { i32 mode; i32 result; i32 buttonId; ... } : OK, button YES
    h.Register("sceSaveDataDialogGetResult", [](const uint64_t* a) -> uint64_t {
        if (a[0]) {
            Store<int32_t>(a[0] + 4, 0);
            Store<int32_t>(a[0] + 8, 1);
        }
        return 0;
    });
    // SceMsgDialogResult { i32 mode; i32 result; i32 buttonId; } : OK, button YES
    h.Register("sceMsgDialogGetResult", [](const uint64_t* a) -> uint64_t {
        if (a[0]) {
            Store<int32_t>(a[0] + 4, 0);
            Store<int32_t>(a[0] + 8, 1);
        }
        return 0;
    });
    h.Register("sceCommonDialogInitialize", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceCommonDialogIsUsed", [](const uint64_t*) -> uint64_t { return 0; });

    // ---- trophies: offline, every call succeeds, nothing is unlocked or reported
    h.Register("sceNpTrophyCreateContext", [](const uint64_t* a) -> uint64_t { Store<int32_t>(a[0], 1); return 0; });
    h.Register("sceNpTrophyCreateHandle", [](const uint64_t* a) -> uint64_t { Store<int32_t>(a[0], 1); return 0; });
    for (const char* n : {"sceNpTrophyRegisterContext", "sceNpTrophyDestroyHandle", "sceNpTrophyDestroyContext",
                          "sceNpTrophyAbortHandle"})
        h.Register(n, [](const uint64_t*) -> uint64_t { return 0; });
    // (ctx, handle, trophyId, i32* platinumId)
    h.Register("sceNpTrophyUnlockTrophy", [](const uint64_t* a) -> uint64_t {
        TraceNote("trophy: unlock request for trophy %d (ignored offline)", int(a[2]));
        Store<int32_t>(a[3], -1);
        return 0;
    });
    // GetGameInfo(ctx, handle, details*, data*) / GetGroupInfo(ctx, handle, group, details*, data*) /
    // GetTrophyInfo(ctx, handle, trophyId, details*, data*): zeroed records (keep the leading size field)
    h.Register("sceNpTrophyGetGameInfo", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceNpTrophyGetGroupInfo", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceNpTrophyGetTrophyInfo", [](const uint64_t*) -> uint64_t { return 0; });
    for (const char* n : {"sceNpTrophyGetGameIcon", "sceNpTrophyGetGroupIcon", "sceNpTrophyGetTrophyIcon"})
        h.Register(n, [](const uint64_t*) -> uint64_t { return 0x80551604; }); // icon not found
}

} // namespace gr::kernel
