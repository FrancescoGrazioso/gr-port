#pragma once
// Development overlay for the game's PSARC archives (see docs/MODDING_AND_TESTING.md).
//
// The original archives are never written. When an overlay is enabled, opening an archive that has
// overridden entries yields a *virtual* file: the original bytes, a patched copy of the archive header
// (TOC + block table) and the replacement data appended past the original end. Everything lives in
// memory for the lifetime of the process.
//
// Overrides come from two opt-in sources only:
//   - loose files under a mods directory (--mods DIR): DIR/<path inside the archive>, e.g.
//     DIR/lua/game/param/init.lua; these are the user's own files, the repository ships none;
//   - built-in text patches applied to the user's own game files at load time (--warp MISSION),
//     and --skip-opening, expressed as search/replace rules so that no game code is stored in the
//     repository.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gr::kernel {

struct ArchiveOverlay {
    uint64_t orig_size = 0;        // size of the original file
    uint64_t blob_base = 0;        // virtual offset of the appended data
    std::vector<uint8_t> header;   // patched bytes [0, header.size())
    std::vector<uint8_t> blob;     // replacement entries, at [blob_base, blob_base + blob.size())
    uint64_t Size() const { return blob_base + blob.size(); }
};

// Options (main.cpp). All are off by default.
void SetOverlayModsDir(const std::filesystem::path& dir);
void SetOverlayWarp(const std::string& mission);
void SetOverlaySkipOpening(bool on);
bool OverlayEnabled();

// Overlay for an archive under /app0, or nullptr when it has no overridden entries (built once, cached).
std::shared_ptr<const ArchiveOverlay> GetArchiveOverlay(const std::filesystem::path& host);

// Reads [offset, offset + n) of the virtual file into dst; returns the bytes read.
uint64_t OverlayRead(const ArchiveOverlay& ov, void* file_handle, uint64_t offset, uint8_t* dst, uint64_t n);

} // namespace gr::kernel
