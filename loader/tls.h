#pragma once
// Guest thread-local storage.
//
// Orbis follows the FreeBSD x86-64 TLS "variant II" layout: the thread pointer (TP) is the fs
// base, [TP] holds TP itself, and each initially-loaded module's static TLS block lives at
// TP - offset(module). Guest code reads TP with `mov reg, fs:[0]`. Windows x64 user code cannot
// set the fs base, so at load time every such instruction is rewritten in place into
// `mov reg, gs:[0x1480 + 8*slot]` (same 9-byte encoding: prefix 64->65, new disp32), which reads
// TP straight from the TEB TlsSlots array of a Windows TLS slot we own. Zero runtime overhead.
#include <cstdint>
#include <string>
#include <vector>

namespace gr {

struct LoadedModule;

struct TlsPatchStats {
    uint64_t patched = 0;
    uint64_t skipped_other_disp = 0; // fs:[disp != 0] forms we do not rewrite (reported)
};

class GuestTls {
public:
    bool Init(std::string* err); // allocates the Windows TLS slot
    uint32_t Slot() const { return slot_; }

    // Registers a module's PT_TLS template; must be called before any thread is set up.
    void AddModule(LoadedModule& m);
    // Rewrites fs:[0] reads inside the module's executable segments.
    TlsPatchStats PatchModule(LoadedModule& m);

    // Creates the TCB + static TLS blocks for the calling host thread and publishes TP.
    uint64_t SetupCurrentThread();
    static uint64_t CurrentTp();
    // __tls_get_addr for a (module index, offset) pair.
    uint64_t GetAddr(uint64_t module_index, uint64_t offset) const;

    static GuestTls* Instance() { return instance_; }

private:
    struct Block {
        uint32_t module_index;
        uint64_t init_image; // host address of the initialised part
        uint64_t init_size, mem_size, align;
        uint64_t tp_offset;  // block start = TP - tp_offset
    };
    static GuestTls* instance_;
    uint32_t slot_ = 0;
    std::vector<Block> blocks_;
    uint64_t static_size_ = 0;
};

} // namespace gr
