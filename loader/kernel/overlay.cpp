// Development overlay for PSARC archives: see overlay.h and docs/MODDING_AND_TESTING.md.
#include "overlay.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>

#include "guest.h"
#include "psarc.h"

namespace fs = std::filesystem;

namespace gr::kernel {
namespace {

std::mutex g_mu;
fs::path g_mods;
std::string g_warp;
bool g_skip_opening = false;
std::map<std::wstring, std::shared_ptr<const ArchiveOverlay>> g_cache;

uint64_t Be(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

void PutBe(uint8_t* p, int n, uint64_t v) {
    for (int i = n - 1; i >= 0; --i, v >>= 8) p[i] = uint8_t(v);
}

// Built-in text patches. Each rule replaces the text from `from` up to and including the next `to`
// in one Lua file of the user's own game data; only the search strings and the replacement live in
// this repository. A rule whose text is not found (another game version) is reported and skipped.
struct TextRule {
    const char* file;
    const char* from;
    const char* to;
    std::string (*make)(const std::string& original);
};

// --warp: the new-game branch of Fn_Common_InitMissionTable() (missionscript_common.lua) starts the
// prologue header "ep01_h". The patch wraps that block in a lookup of the requested mission in the
// game's own main_mission table and sets the state the way Fn_Common_SetRetryMission() does for a
// retry: the previous entry becomes the current, active mission and the episode is the target's.
// Story flags and unlocked abilities of skipped missions are NOT set, so this is for testing scenes,
// not for playing.
std::string MakeWarp(const std::string& original) {
    return "do local w = \"" + g_warp + "\" local t = nil "
           "for i = 2, table.getn(main_mission) do if main_mission[i][INDEX_NAME] == w then t = i break end end "
           "if t ~= nil then local p = main_mission[t - 1] print(\"gr-port warp: \" .. w) "
           "GameDatabase:set(\"GameWorld\", \"CurrentMission\", p[INDEX_NAME]) "
           "GameDatabase:set(\"GameWorld\", \"CurrentType\", p[INDEX_TYPE]) "
           "GameDatabase:set(\"GameWorld\", \"MissionTable\", p[INDEX_NAME], MISSION_ON) "
           "if main_mission[t][INDEX_EPISODE] ~= EP_NA then GameDatabase:set(\"GameWorld\", \"CurrentEpisode\", main_mission[t][INDEX_EPISODE]) end "
           "else print(\"gr-port warp: unknown mission \" .. w) " +
           original + " end end";
}

// --skip-opening: Fn_Opening() (event/opening.lua) plays the falling-apple demos (cut A, the apple
// taps, cut C) and the comic cut D before the game proper. The patch removes those demos and keeps
// everything else (waiting for the player and HUD, Savedata:setupNewGame(), creating the mother).
std::string MakeSkipCutAC(const std::string&) {
    return "print(\"gr-port: opening skipped\") actor_apple01:setActive(false) camera:setActive(false)";
}
std::string MakeSkipCutD(const std::string&) { return ""; }

std::vector<TextRule> ActiveRules() {
    std::vector<TextRule> r;
    if (!g_warp.empty())
        r.push_back({"lua/game/param/missionscript_common.lua",
                     "GameDatabase:set(\"GameWorld\", \"CurrentMission\",\"ep01_h\")",
                     "GameDatabase:set(\"GameWorld\", \"CurrentEpisode\", EP_01)", MakeWarp});
    if (g_skip_opening) {
        r.push_back({"lua/game/event/opening.lua", "Sandbox:run(Opening_cutA_Demo)",
                     "Sandbox:waitForEnd(Opening_cutC_Demo)", MakeSkipCutAC});
        r.push_back({"lua/game/event/opening.lua", "Sandbox:run(Opening_cutD_Demo)",
                     "Sandbox:waitForEnd(Opening_cutD_Demo)", MakeSkipCutD});
    }
    return r;
}

bool ApplyPatches(const std::string& name, std::vector<uint8_t>& data) {
    bool any = false;
    std::string s(data.begin(), data.end());
    for (const TextRule& rule : ActiveRules()) {
        if (name != rule.file) continue;
        const size_t p = s.find(rule.from);
        const size_t q0 = p == std::string::npos ? p : s.find(rule.to, p);
        if (q0 == std::string::npos) {
            TraceNote("overlay: patch text not found in %s (different game version?)", rule.file);
            continue;
        }
        const size_t q = q0 + std::strlen(rule.to);
        s.replace(p, q - p, rule.make(s.substr(p, q - p)));
        any = true;
    }
    if (any) data.assign(s.begin(), s.end());
    return any;
}

bool IsPatched(const std::string& name) {
    for (const TextRule& rule : ActiveRules())
        if (name == rule.file) return true;
    return false;
}

bool ReadWhole(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

std::shared_ptr<const ArchiveOverlay> Build(const fs::path& host) {
    Psarc arc;
    std::string err;
    if (!arc.Open(host.string(), &err)) return nullptr;
    if (arc.Compression() != "zlib") return nullptr;

    FILE* f = _wfopen(host.wstring().c_str(), L"rb");
    if (!f) return nullptr;
    uint8_t h[32];
    std::vector<uint8_t> header;
    if (std::fread(h, 1, 32, f) == 32) {
        header.resize(size_t(Be(h + 12, 4)));
        std::memcpy(header.data(), h, 32);
        if (std::fread(header.data() + 32, 1, header.size() - 32, f) != header.size() - 32) header.clear();
    }
    _fseeki64(f, 0, SEEK_END);
    const uint64_t file_size = uint64_t(_ftelli64(f));
    std::fclose(f);
    if (header.size() < 32) return nullptr;

    const uint32_t count = uint32_t(Be(h + 20, 4)), bs = uint32_t(Be(h + 24, 4));
    int width = 1;
    while ((1ull << (8 * width)) < bs) ++width;
    const size_t blocks_at = 32 + size_t(count) * 30;
    const size_t nblocks = (header.size() - blocks_at) / width;

    auto ov = std::make_shared<ArchiveOverlay>();
    ov->orig_size = file_size;
    ov->blob_base = (file_size + 0xFFFF) & ~0xFFFFull;
    const auto& entries = arc.Entries();
    for (size_t k = 0; k < entries.size(); ++k) {
        const Psarc::Entry& e = entries[k];
        std::vector<uint8_t> data;
        const char* source = nullptr;
        std::error_code ec;
        if (!g_mods.empty() && fs::is_regular_file(g_mods / fs::path(e.name), ec)) {
            if (ReadWhole(g_mods / fs::path(e.name), data)) source = "mods";
        } else if (IsPatched(e.name)) {
            if (arc.Read(e, data, &err) && ApplyPatches(e.name, data)) source = "built-in patch";
        }
        if (!source) continue;
        // The replacement reuses the entry's block-table slots, stored uncompressed (a full block has
        // size 0 = block size, the last one its own length), so it may not need more blocks than the
        // original entry.
        const uint64_t old_blocks = std::max<uint64_t>(1, (e.size + bs - 1) / bs);
        const uint64_t new_blocks = std::max<uint64_t>(1, (data.size() + bs - 1) / bs);
        if (new_blocks > old_blocks || e.first_block + new_blocks > nblocks || data.empty()) {
            TraceNote("overlay: %s: replacement needs %llu blocks, the entry has %llu (skipped)", e.name.c_str(),
                      (unsigned long long)new_blocks, (unsigned long long)old_blocks);
            continue;
        }
        uint8_t* te = header.data() + 32 + (k + 1) * 30; // TOC entry (index 0 is the manifest)
        PutBe(te + 20, 5, data.size());
        PutBe(te + 25, 5, ov->blob_base + ov->blob.size());
        for (uint64_t b = 0; b < new_blocks; ++b) {
            const uint64_t len = std::min<uint64_t>(bs, data.size() - b * bs);
            PutBe(header.data() + blocks_at + (e.first_block + b) * width, width, len == bs ? 0 : len);
        }
        ov->blob.insert(ov->blob.end(), data.begin(), data.end());
        ov->blob.resize((ov->blob.size() + 15) & ~size_t(15));
        TraceNote("overlay: %s: %s replaced from %s (%llu -> %zu bytes)", host.filename().string().c_str(),
                  e.name.c_str(), source, (unsigned long long)e.size, data.size());
    }
    if (ov->blob.empty()) return nullptr;
    ov->header = std::move(header);
    return ov;
}

} // namespace

void SetOverlayModsDir(const fs::path& dir) { g_mods = dir; }

void SetOverlayWarp(const std::string& mission) {
    // mission names are identifiers (ep03_a, free_eps1, ...); anything else is rejected
    for (char c : mission)
        if (!(std::isalnum(uint8_t(c)) || c == '_')) {
            std::printf("--warp: invalid mission name '%s'\n", mission.c_str());
            return;
        }
    g_warp = mission;
}

void SetOverlaySkipOpening(bool on) { g_skip_opening = on; }

bool OverlayEnabled() { return !g_mods.empty() || !g_warp.empty() || g_skip_opening; }

std::shared_ptr<const ArchiveOverlay> GetArchiveOverlay(const fs::path& host) {
    if (!OverlayEnabled()) return nullptr;
    std::lock_guard<std::mutex> lk(g_mu);
    const std::wstring key = host.wstring();
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    return g_cache[key] = Build(host);
}

uint64_t OverlayRead(const ArchiveOverlay& ov, void* file_handle, uint64_t offset, uint8_t* dst, uint64_t n) {
    const uint64_t end = std::min(ov.Size(), offset + n);
    uint64_t pos = offset;
    while (pos < end) {
        uint64_t chunk;
        if (pos < ov.header.size()) {
            chunk = std::min<uint64_t>(end, ov.header.size()) - pos;
            std::memcpy(dst, ov.header.data() + pos, chunk);
        } else if (pos < ov.orig_size) {
            chunk = std::min(end, ov.orig_size) - pos;
            OVERLAPPED o{};
            o.Offset = DWORD(pos);
            o.OffsetHigh = DWORD(pos >> 32);
            DWORD got = 0;
            if (!ReadFile(HANDLE(file_handle), dst, DWORD(chunk), &got, &o) || got != chunk) return pos - offset + got;
        } else if (pos < ov.blob_base) {
            chunk = std::min(end, ov.blob_base) - pos;
            std::memset(dst, 0, chunk);
        } else {
            chunk = end - pos;
            std::memcpy(dst, ov.blob.data() + (pos - ov.blob_base), chunk);
        }
        dst += chunk;
        pos += chunk;
    }
    return pos > offset ? pos - offset : 0;
}

} // namespace gr::kernel
