// libkernel file I/O over a small virtual filesystem:
//   /app0/...      -> game directory (uroot), READ-ONLY: the original files are never modified
//   /savedata0/... -> <exe dir>/savedata/<dir mounted by sceSaveDataMount>
//   /data, /temp0  -> <exe dir>/savedata/{data,temp}
#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "guest.h"
#include "kernel.h"

namespace fs = std::filesystem;

namespace gr::kernel {
namespace {

// FreeBSD open flags
constexpr uint32_t O_ACCMODE_ = 3, O_WRONLY_ = 1, O_RDWR_ = 2, O_APPEND_ = 0x8, O_CREAT_ = 0x200, O_TRUNC_ = 0x400,
                   O_EXCL_ = 0x800, O_DIRECTORY_ = 0x20000;

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool is_dir = false;
    std::string guest_path;
    fs::path host_path;
    std::vector<std::string> entries; // directory listing
    size_t dir_pos = 0;
};

std::mutex g_files;
std::vector<File*> g_fds(3, nullptr); // 0..2 = stdio

bool Resolve(const std::string& guest, fs::path* host, bool* read_only) {
    auto under = [&](const char* prefix, const fs::path& root) {
        const size_t n = std::strlen(prefix);
        if (guest.compare(0, n, prefix) != 0 || (guest.size() > n && guest[n] != '/')) return false;
        *host = root / fs::path(guest.substr(std::min(guest.size(), n + 1))).relative_path();
        return true;
    };
    const Runtime& rt = Rt();
    *read_only = false;
    if (under("/app0", rt.app0) || under("/hostapp", rt.app0)) { *read_only = true; return true; }
    if (under("/savedata0", rt.savedata_mount.empty() ? rt.savedata / "unmounted" : rt.savedata_mount)) return true;
    if (under("/data", rt.savedata / "data")) return true;
    if (under("/temp0", rt.savedata / "temp")) return true;
    if (under("/temp", rt.savedata / "temp")) return true;
    return false;
}

int ErrnoFromWin(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return kENOENT;
    case ERROR_ACCESS_DENIED: return kEACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return kEEXIST;
    case ERROR_DIR_NOT_EMPTY: return kENOTEMPTY;
    default: return kEIO;
    }
}

// Returns fd >= 0 or -errno.
int Open(const std::string& path, uint32_t flags) {
    fs::path host;
    bool ro = false;
    if (!Resolve(path, &host, &ro)) {
        TraceNote("open: unmapped path '%s'", path.c_str());
        return -kENOENT;
    }
    const uint32_t acc = flags & O_ACCMODE_;
    const bool wants_write = acc == O_WRONLY_ || acc == O_RDWR_ || (flags & (O_CREAT_ | O_TRUNC_ | O_APPEND_));
    if (ro && wants_write) return -kEROFS;

    auto* f = new File;
    f->guest_path = path;
    f->host_path = host;
    std::error_code ec;
    if (fs::is_directory(host, ec)) {
        f->is_dir = true;
        f->entries = {".", ".."};
        for (const auto& e : fs::directory_iterator(host, ec)) f->entries.push_back(e.path().filename().string());
    } else {
        if (flags & O_DIRECTORY_) { delete f; return -kENOTDIR; }
        if (wants_write) fs::create_directories(host.parent_path(), ec);
        DWORD access = acc == O_WRONLY_ ? GENERIC_WRITE : acc == O_RDWR_ ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
        DWORD disp = OPEN_EXISTING;
        if (flags & O_CREAT_) disp = (flags & O_EXCL_) ? CREATE_NEW : (flags & O_TRUNC_) ? CREATE_ALWAYS : OPEN_ALWAYS;
        else if (flags & O_TRUNC_) disp = TRUNCATE_EXISTING;
        f->h = CreateFileW(host.wstring().c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f->h == INVALID_HANDLE_VALUE) {
            const int e = ErrnoFromWin(GetLastError());
            delete f;
            return -e;
        }
        if (flags & O_APPEND_) SetFilePointer(f->h, 0, nullptr, FILE_END);
    }
    std::lock_guard<std::mutex> lk(g_files);
    for (size_t i = 3; i < g_fds.size(); ++i)
        if (!g_fds[i]) { g_fds[i] = f; return int(i); }
    g_fds.push_back(f);
    return int(g_fds.size() - 1);
}

File* Get(uint64_t fd) {
    std::lock_guard<std::mutex> lk(g_files);
    return fd < g_fds.size() ? g_fds[fd] : nullptr;
}

int Close(uint64_t fd) {
    std::lock_guard<std::mutex> lk(g_files);
    if (fd < 3) return 0;
    if (fd >= g_fds.size() || !g_fds[fd]) return kEBADF;
    if (g_fds[fd]->h != INVALID_HANDLE_VALUE) CloseHandle(g_fds[fd]->h);
    delete g_fds[fd];
    g_fds[fd] = nullptr;
    return 0;
}

// Returns bytes or -errno.
int64_t Read(uint64_t fd, uint64_t buf, uint64_t n, int64_t offset /* -1 = current */) {
    if (fd == 0) return 0;
    File* f = Get(fd);
    if (!f) return -kEBADF;
    if (f->is_dir) return -kEISDIR;
    DWORD got = 0;
    OVERLAPPED ov{};
    if (offset >= 0) {
        ov.Offset = DWORD(offset);
        ov.OffsetHigh = DWORD(uint64_t(offset) >> 32);
    }
    if (!ReadFile(f->h, Ptr<void>(buf), DWORD(n), &got, offset >= 0 ? &ov : nullptr) &&
        GetLastError() != ERROR_HANDLE_EOF)
        return -kEIO;
    return got;
}

int64_t Write(uint64_t fd, uint64_t buf, uint64_t n, int64_t offset) {
    if (fd == 1 || fd == 2) {
        // Guest console output, each line prefixed with the time since start ("[guest 12.34s]").
        // Lines also go into the trace file so they can be correlated with HLE calls.
        static std::mutex m;
        static std::string line;
        static const ULONGLONG t0 = GetTickCount64();
        std::lock_guard<std::mutex> lk(m);
        const char* p = Ptr<const char>(buf);
        for (uint64_t i = 0; i < n; ++i) {
            if (p[i] == '\n') {
                TraceNote("[guest %7.2fs] %s", double(GetTickCount64() - t0) / 1000.0, line.c_str());
                line.clear();
            } else if (p[i] != '\r') {
                line += p[i];
            }
        }
        return int64_t(n);
    }
    File* f = Get(fd);
    if (!f) return -kEBADF;
    DWORD put = 0;
    OVERLAPPED ov{};
    if (offset >= 0) {
        ov.Offset = DWORD(offset);
        ov.OffsetHigh = DWORD(uint64_t(offset) >> 32);
    }
    if (!WriteFile(f->h, Ptr<void>(buf), DWORD(n), &put, offset >= 0 ? &ov : nullptr)) return -kEIO;
    return put;
}

int64_t Seek(uint64_t fd, int64_t off, int whence) {
    File* f = Get(fd);
    if (!f || f->is_dir) return -kEBADF;
    LARGE_INTEGER d, out;
    d.QuadPart = off;
    const DWORD method = whence == 1 ? FILE_CURRENT : whence == 2 ? FILE_END : FILE_BEGIN;
    if (!SetFilePointerEx(f->h, d, &out, method)) return -kEINVAL;
    return out.QuadPart;
}

// FreeBSD struct stat (120 bytes).
void FillStat(uint64_t st, uint64_t size, bool dir, uint64_t mtime_sec) {
    uint8_t s[120] = {};
    const uint16_t mode = dir ? 0040000 | 0755 : 0100000 | 0644;
    const uint16_t nlink = 1;
    std::memcpy(s + 8, &mode, 2);
    std::memcpy(s + 10, &nlink, 2);
    for (int off : {24, 40, 56, 104}) std::memcpy(s + off, &mtime_sec, 8);
    const int64_t sz = int64_t(size), blocks = int64_t((size + 511) / 512);
    const uint32_t blksize = 0x10000;
    std::memcpy(s + 72, &sz, 8);
    std::memcpy(s + 80, &blocks, 8);
    std::memcpy(s + 88, &blksize, 4);
    std::memcpy(Ptr<void>(st), s, sizeof s);
}

int StatPath(const std::string& path, uint64_t st) {
    fs::path host;
    bool ro;
    if (!Resolve(path, &host, &ro)) return kENOENT;
    std::error_code ec;
    const auto status = fs::status(host, ec);
    if (ec || !fs::exists(status)) return kENOENT;
    const bool dir = fs::is_directory(status);
    const uint64_t size = dir ? 0 : fs::file_size(host, ec);
    FillStat(st, size, dir, 1500000000);
    return 0;
}

int StatFd(uint64_t fd, uint64_t st) {
    File* f = Get(fd);
    if (fd <= 2) { FillStat(st, 0, false, 0); return 0; }
    if (!f) return kEBADF;
    if (f->is_dir) { FillStat(st, 0, true, 1500000000); return 0; }
    LARGE_INTEGER sz;
    GetFileSizeEx(f->h, &sz);
    FillStat(st, uint64_t(sz.QuadPart), false, 1500000000);
    return 0;
}

// FreeBSD struct dirent: u32 fileno; u16 reclen; u8 type; u8 namlen; char name[256]
int64_t GetDents(uint64_t fd, uint64_t buf, uint64_t len) {
    File* f = Get(fd);
    if (!f || !f->is_dir) return -kEBADF;
    uint64_t used = 0;
    while (f->dir_pos < f->entries.size()) {
        const std::string& name = f->entries[f->dir_pos];
        const uint16_t reclen = uint16_t((8 + name.size() + 1 + 7) & ~size_t(7));
        if (used + reclen > len) break;
        uint8_t* p = Ptr<uint8_t>(buf + used);
        std::memset(p, 0, reclen);
        const uint32_t ino = uint32_t(f->dir_pos + 1);
        std::memcpy(p, &ino, 4);
        std::memcpy(p + 4, &reclen, 2);
        std::error_code ec;
        p[6] = fs::is_directory(f->host_path / name, ec) ? 4 : 8; // DT_DIR / DT_REG
        p[7] = uint8_t(name.size());
        std::memcpy(p + 8, name.data(), name.size());
        used += reclen;
        f->dir_pos++;
    }
    return int64_t(used);
}

inline uint64_t SceRet(int64_t v) { return v < 0 ? SceError(int(-v)) : uint64_t(v); }
inline uint64_t PosixRet(int64_t v) { return v < 0 ? PosixFail(int(-v)) : uint64_t(v); }

} // namespace

void RegisterFiles(Hle& h) {
    h.Register("sceKernelOpen", [](const uint64_t* a) -> uint64_t {
        const int fd = Open(Str(a[0]), uint32_t(a[1]));
        TraceNote("open '%s' flags=0x%x -> %d", Str(a[0]).c_str(), unsigned(a[1]), fd);
        return SceRet(fd);
    });
    for (const char* n : {"open", "_open"})
        h.Register(n, [](const uint64_t* a) -> uint64_t { return PosixRet(Open(Str(a[0]), uint32_t(a[1]))); });
    SCE("sceKernelClose", return Close(a[0]););
    for (const char* n : {"close", "_close"})
        h.Register(n, [](const uint64_t* a) -> uint64_t { const int e = Close(a[0]); return e ? PosixFail(e) : 0; });

    h.Register("sceKernelRead", [](const uint64_t* a) -> uint64_t { return SceRet(Read(a[0], a[1], a[2], -1)); });
    for (const char* n : {"read", "_read"})
        h.Register(n, [](const uint64_t* a) -> uint64_t { return PosixRet(Read(a[0], a[1], a[2], -1)); });
    h.Register("sceKernelPread", [](const uint64_t* a) -> uint64_t { return SceRet(Read(a[0], a[1], a[2], int64_t(a[3]))); });
    h.Register("sceKernelWrite", [](const uint64_t* a) -> uint64_t { return SceRet(Write(a[0], a[1], a[2], -1)); });
    for (const char* n : {"write", "_write"})
        h.Register(n, [](const uint64_t* a) -> uint64_t { return PosixRet(Write(a[0], a[1], a[2], -1)); });
    h.Register("sceKernelPwrite", [](const uint64_t* a) -> uint64_t { return SceRet(Write(a[0], a[1], a[2], int64_t(a[3]))); });
    h.Register("sceKernelLseek", [](const uint64_t* a) -> uint64_t { return SceRet(Seek(a[0], int64_t(a[1]), int(a[2]))); });
    h.Register("lseek", [](const uint64_t* a) -> uint64_t { return PosixRet(Seek(a[0], int64_t(a[1]), int(a[2]))); });

    SCE("sceKernelStat", return StatPath(Str(a[0]), a[1]););
    h.Register("stat", [](const uint64_t* a) -> uint64_t { const int e = StatPath(Str(a[0]), a[1]); return e ? PosixFail(e) : 0; });
    SCE("sceKernelFstat", return StatFd(a[0], a[1]););
    h.Register("fstat", [](const uint64_t* a) -> uint64_t { const int e = StatFd(a[0], a[1]); return e ? PosixFail(e) : 0; });

    h.Register("sceKernelGetdents", [](const uint64_t* a) -> uint64_t { return SceRet(GetDents(a[0], a[1], a[2])); });
    h.Register("sceKernelGetdirentries", [](const uint64_t* a) -> uint64_t {
        const int64_t r = GetDents(a[0], a[1], a[2]);
        if (r >= 0) Store<int64_t>(a[3], 0);
        return SceRet(r);
    });

    SCE("sceKernelMkdir", {
        fs::path p;
        bool ro;
        if (!Resolve(Str(a[0]), &p, &ro)) return kENOENT;
        if (ro) return kEROFS;
        std::error_code ec;
        if (fs::exists(p, ec)) return kEEXIST;
        return fs::create_directories(p, ec) ? 0 : kEIO;
    });
    SCE("sceKernelRmdir", {
        fs::path p;
        bool ro;
        if (!Resolve(Str(a[0]), &p, &ro)) return kENOENT;
        if (ro) return kEROFS;
        std::error_code ec;
        return fs::remove(p, ec) ? 0 : kENOENT;
    });
    SCE("sceKernelUnlink", {
        fs::path p;
        bool ro;
        if (!Resolve(Str(a[0]), &p, &ro)) return kENOENT;
        if (ro) return kEROFS;
        std::error_code ec;
        return fs::remove(p, ec) ? 0 : kENOENT;
    });
    SCE("sceKernelRename", {
        fs::path p1, p2;
        bool ro1 = false, ro2 = false;
        if (!Resolve(Str(a[0]), &p1, &ro1) || !Resolve(Str(a[1]), &p2, &ro2)) return kENOENT;
        if (ro1 || ro2) return kEROFS;
        std::error_code ec;
        fs::rename(p1, p2, ec);
        return ec ? kEIO : 0;
    });
    SCE("sceKernelFsync", {
        if (File* f = Get(a[0]); f && f->h != INVALID_HANDLE_VALUE) FlushFileBuffers(f->h);
        return 0;
    });
    SCE("sceKernelFtruncate", {
        File* f = Get(a[0]);
        if (!f || f->h == INVALID_HANDLE_VALUE) return kEBADF;
        FILE_END_OF_FILE_INFO info{};
        info.EndOfFile.QuadPart = int64_t(a[1]);
        return SetFileInformationByHandle(f->h, FileEndOfFileInfo, &info, sizeof info) ? 0 : kEIO;
    });
}

} // namespace gr::kernel
