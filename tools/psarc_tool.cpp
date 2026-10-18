// psarc_tool: list / stat / extract PSARC archives.
//   psarc_tool list    <archive.psarc>
//   psarc_tool stats   <archive.psarc>
//   psarc_tool extract <archive.psarc> <out_dir> [name_substring]
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "psarc.h"

namespace fs = std::filesystem;

static std::string Ext(const std::string& name) {
    auto slash = name.find_last_of('/');
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "(none)";
    return name.substr(dot);
}

// Turns an archive path into a safe relative path (no root, no "..").
static fs::path SafeRelative(const std::string& name) {
    fs::path out;
    for (const auto& part : fs::path(name).relative_path()) {
        if (part == ".." || part == ".") continue;
        out /= part;
    }
    return out;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s list|stats|extract <archive.psarc> [out_dir] [filter]\n", argv[0]);
        return 1;
    }
    const std::string cmd = argv[1];
    gr::Psarc arc;
    std::string err;
    if (!arc.Open(argv[2], &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 2;
    }

    if (cmd == "list") {
        for (const auto& e : arc.Entries()) std::printf("%12llu  %s\n", (unsigned long long)e.size, e.name.c_str());
        return 0;
    }
    if (cmd == "stats") {
        struct Agg { uint64_t n = 0, bytes = 0; };
        std::map<std::string, Agg> by_ext;
        uint64_t total = 0;
        for (const auto& e : arc.Entries()) {
            auto& a = by_ext[Ext(e.name)];
            a.n++;
            a.bytes += e.size;
            total += e.size;
        }
        std::printf("compression=%s block=%u files=%zu uncompressed=%.1f MiB\n", arc.Compression().c_str(),
                    arc.BlockSize(), arc.Entries().size(), total / 1048576.0);
        for (const auto& [ext, a] : by_ext)
            std::printf("  %-12s %7llu files %10.1f MiB\n", ext.c_str(), (unsigned long long)a.n, a.bytes / 1048576.0);
        return 0;
    }
    if (cmd == "extract") {
        if (argc < 4) { std::fprintf(stderr, "extract needs <out_dir>\n"); return 1; }
        const fs::path out_dir = argv[3];
        const std::string filter = argc > 4 ? argv[4] : "";
        std::vector<uint8_t> data;
        size_t done = 0, failed = 0;
        for (const auto& e : arc.Entries()) {
            if (!filter.empty() && e.name.find(filter) == std::string::npos) continue;
            if (!arc.Read(e, data, &err)) {
                std::fprintf(stderr, "FAIL %s: %s\n", e.name.c_str(), err.c_str());
                ++failed;
                continue;
            }
            fs::path dst = out_dir / SafeRelative(e.name);
            fs::create_directories(dst.parent_path());
            std::ofstream(dst, std::ios::binary).write(reinterpret_cast<char*>(data.data()), data.size());
            if (++done % 500 == 0) std::fprintf(stderr, "  %zu files...\n", done);
        }
        std::printf("extracted %zu files, %zu failed\n", done, failed);
        return failed ? 3 : 0;
    }
    std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
    return 1;
}
