#include "tls.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "linker.h"
#include "vmem.h"

namespace gr {

GuestTls* GuestTls::instance_ = nullptr;

namespace {
constexpr uint32_t kTebTlsSlots = 0x1480; // TEB::TlsSlots on x64
constexpr uint64_t kTcbSize = 0x100;

uint64_t AlignUp(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }
} // namespace

bool GuestTls::Init(std::string* err) {
    const DWORD s = TlsAlloc();
    if (s == TLS_OUT_OF_INDEXES || s >= 64) {
        *err = "no TLS slot below 64 available (needed for the in-place fs: patch)";
        return false;
    }
    slot_ = s;
    instance_ = this;
    return true;
}

void GuestTls::AddModule(LoadedModule& m) {
    for (const auto& p : m.oe.phdrs) {
        if (p.p_type != PT_TLS) continue;
        Block b{};
        b.module_index = m.tls_index;
        b.init_image = m.base + p.p_vaddr;
        b.init_size = p.p_filesz;
        b.mem_size = p.p_memsz;
        b.align = std::max<uint64_t>(p.p_align, 1);
        // FreeBSD variant II: offset = round(previous_offset + size, align)
        b.tp_offset = AlignUp(static_size_ + b.mem_size, b.align);
        static_size_ = b.tp_offset;
        blocks_.push_back(b);
    }
}

TlsPatchStats GuestTls::PatchModule(LoadedModule& m) {
    TlsPatchStats st;
    const uint32_t disp = kTebTlsSlots + 8 * slot_;
    for (const auto& p : m.oe.phdrs) {
        if (p.p_type != PT_LOAD || !(p.p_flags & 1)) continue;
        auto* code = reinterpret_cast<uint8_t*>(m.base + p.p_vaddr);
        const uint64_t n = p.p_filesz;
        for (uint64_t i = 0; i + 9 <= n; ++i) {
            if (code[i] != 0x64) continue;
            // mov r64, fs:[disp32]   64 (48|4C) 8B (00 rrr 100) 25 disp32
            if ((code[i + 1] == 0x48 || code[i + 1] == 0x4C) && code[i + 2] == 0x8B && (code[i + 3] & 0xC7) == 0x04 &&
                code[i + 4] == 0x25) {
                uint32_t d;
                std::memcpy(&d, code + i + 5, 4);
                if (d != 0) {
                    st.skipped_other_disp++;
                    continue;
                }
                code[i] = 0x65;
                std::memcpy(code + i + 5, &disp, 4);
                st.patched++;
                i += 8;
            } else if (i + 11 <= n && code[i + 1] == 0x48 && code[i + 2] == 0xA1) {
                // mov rax, fs:[moffs64]  64 48 A1 imm64
                uint64_t a;
                std::memcpy(&a, code + i + 3, 8);
                if (a != 0) continue;
                code[i] = 0x65;
                const uint64_t a2 = disp;
                std::memcpy(code + i + 3, &a2, 8);
                st.patched++;
                i += 10;
            }
        }
    }
    return st;
}

uint64_t GuestTls::SetupCurrentThread() {
    if (uint64_t tp = CurrentTp()) return tp;
    const uint64_t static_area = AlignUp(static_size_, 64);
    auto* mem = reinterpret_cast<uint8_t*>(
        GuestVirtualAlloc(0, static_area + kTcbSize, 0, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    const uint64_t tp = reinterpret_cast<uint64_t>(mem) + static_area;
    for (const auto& b : blocks_) {
        auto* dst = reinterpret_cast<uint8_t*>(tp - b.tp_offset);
        std::memcpy(dst, reinterpret_cast<const void*>(b.init_image), b.init_size);
        // the rest of the block is already zero (fresh VirtualAlloc)
    }
    *reinterpret_cast<uint64_t*>(tp) = tp; // tcb_self: what `mov reg, fs:[0]` returns
    TlsSetValue(slot_, reinterpret_cast<void*>(tp));
    return tp;
}

uint64_t GuestTls::CurrentTp() {
    return instance_ ? reinterpret_cast<uint64_t>(TlsGetValue(instance_->slot_)) : 0;
}

uint64_t GuestTls::GetAddr(uint64_t module_index, uint64_t offset) const {
    const uint64_t tp = CurrentTp();
    for (const auto& b : blocks_)
        if (b.module_index == module_index) return tp - b.tp_offset + offset;
    return 0;
}

} // namespace gr
