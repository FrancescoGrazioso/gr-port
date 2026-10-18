#include "hle.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "guest.h"
#include "nid.h"

namespace gr {

Hle* Hle::instance_ = nullptr;

Hle::Hle() {
    instance_ = this;
    code_cap_ = 4 << 20;
    code_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, code_cap_, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
}

Hle::~Hle() {
    for (auto* t : thunks_) delete t;
    for (void* p : data_blocks_) VirtualFree(p, 0, MEM_RELEASE);
    if (code_) VirtualFree(code_, 0, MEM_RELEASE);
    instance_ = nullptr;
}

void Hle::Register(const std::string& name, HleFunc fn) { impl_[ComputeNid(name)] = fn; }

size_t Hle::ImplementedThunkCount() const {
    size_t n = 0;
    for (auto* t : thunks_) n += t->func != nullptr;
    return n;
}

uint8_t* Hle::AllocCode(size_t n) {
    n = (n + 15) & ~size_t(15);
    if (!code_ || code_used_ + n > code_cap_) return nullptr;
    uint8_t* p = code_ + code_used_;
    code_used_ += n;
    return p;
}

namespace {
struct Emitter {
    uint8_t* p;
    void B(std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) *p++ = b;
    }
    void Q(uint64_t v) {
        std::memcpy(p, &v, 8);
        p += 8;
    }
};
} // namespace

uint64_t Hle::GetFunctionThunk(const std::string& nid, const std::string& name, const std::string& library) {
    const std::string key = nid + "#" + library;
    if (auto it = thunk_by_key_.find(key); it != thunk_by_key_.end()) return it->second;

    auto* info = new ThunkInfo;
    info->nid = nid;
    info->name = name;
    info->library = library;
    if (auto it = impl_.find(nid); it != impl_.end()) info->func = it->second;
    const uint64_t index = thunks_.size();
    thunks_.push_back(info);

    uint8_t* code = AllocCode(192);
    if (!code) return 0;
    Emitter e{code};
    // args[0..5]  = SysV integer registers (rdi, rsi, rdx, rcx, r8, r9)
    // args[6], [7], [9] = the first three stack arguments, args[8] = the guest return address
    // args[10..13] = low 64 bits of xmm0..xmm3 (float/double arguments; use ArgF/ArgD)
    // Guest code does not always call with the ABI-mandated 16-byte stack alignment (hand-written
    // code, context switches), so the frame is aligned explicitly through rbp:
    //   [rbp+8] = return address, [rbp+0x10..] = stack arguments.
    auto disp32 = [&](uint32_t d) {
        for (int i = 0; i < 4; ++i) e.B({uint8_t(d >> (8 * i))});
    };
    auto load_rbp = [&](uint8_t d) { e.B({0x48, 0x8B, 0x45, d}); };                  // mov rax, [rbp+d8]
    auto store_rsp = [&](uint32_t d) { e.B({0x48, 0x89, 0x84, 0x24}); disp32(d); }; // mov [rsp+d], rax
    auto store_xmm = [&](uint8_t modrm, uint32_t d) { e.B({0x66, 0x0F, 0xD6, modrm, 0x24}); disp32(d); }; // movq
    e.B({0x55});                               // push rbp
    e.B({0x48, 0x89, 0xE5});                   // mov  rbp, rsp
    e.B({0x48, 0x83, 0xE4, 0xF0});             // and  rsp, -16
    e.B({0x48, 0x81, 0xEC}); disp32(0xA0);     // sub  rsp, 0xA0   (shadow space + args[0..13])
    e.B({0x48, 0x89, 0x7C, 0x24, 0x28});       // mov  [rsp+0x28], rdi
    e.B({0x48, 0x89, 0x74, 0x24, 0x30});       // mov  [rsp+0x30], rsi
    e.B({0x48, 0x89, 0x54, 0x24, 0x38});       // mov  [rsp+0x38], rdx
    e.B({0x48, 0x89, 0x4C, 0x24, 0x40});       // mov  [rsp+0x40], rcx
    e.B({0x4C, 0x89, 0x44, 0x24, 0x48});       // mov  [rsp+0x48], r8
    e.B({0x4C, 0x89, 0x4C, 0x24, 0x50});       // mov  [rsp+0x50], r9
    load_rbp(0x10); store_rsp(0x58);           // args[6] = stack arg 0
    load_rbp(0x18); store_rsp(0x60);           // args[7] = stack arg 1
    load_rbp(0x08); store_rsp(0x68);           // args[8] = return address
    load_rbp(0x20); store_rsp(0x70);           // args[9] = stack arg 2
    store_xmm(0x84, 0x78);                     // args[10] = xmm0
    store_xmm(0x8C, 0x80);                     // args[11] = xmm1
    store_xmm(0x94, 0x88);                     // args[12] = xmm2
    store_xmm(0x9C, 0x90);                     // args[13] = xmm3
    e.B({0x48, 0x8D, 0x54, 0x24, 0x28});       // lea  rdx, [rsp+0x28]   (MS arg2 = args)
    e.B({0x48, 0xB9}); e.Q(index);             // mov  rcx, index        (MS arg1)
    e.B({0x48, 0xB8}); e.Q(reinterpret_cast<uint64_t>(&Hle::Dispatch));
    e.B({0xFF, 0xD0});                         // call rax
    e.B({0x48, 0x89, 0xEC});                   // mov  rsp, rbp
    e.B({0x5D});                               // pop  rbp
    e.B({0xC3});                               // ret  (result already in rax)
    if (e.p - code > 192) { std::fprintf(stderr, "HLE thunk overflow (%td bytes)\n", e.p - code); std::abort(); }
    const uint64_t addr = reinterpret_cast<uint64_t>(code);
    thunk_by_key_[key] = addr;
    return addr;
}

uint64_t Hle::GetDataObject(const std::string& nid, const std::string& name, size_t size) {
    if (auto it = data_by_nid_.find(nid); it != data_by_nid_.end()) return it->second;
    void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    data_blocks_.push_back(p);
    if (name == "__stack_chk_guard") {
        std::random_device rd;
        const uint64_t guard = (uint64_t(rd()) << 32) | rd();
        std::memcpy(p, &guard, 8);
    }
    const uint64_t addr = reinterpret_cast<uint64_t>(p);
    data_by_nid_[nid] = addr;
    return addr;
}

uint64_t Hle::Dispatch(uint64_t index, const uint64_t* args) {
    Hle* self = instance_;
    if (!self || index >= self->thunks_.size()) return 0;
    ThunkInfo* t = self->thunks_[index];
    const uint64_t n = t->calls.fetch_add(1);
    const char* name = t->name.empty() ? t->nid.c_str() : t->name.c_str();
    if (t->func) {
        TraceEnter(t->library.c_str(), name, args[8], args[0]);
        const uint64_t ret = t->func(args);
        TraceLeave();
        TraceCall(t->library.c_str(), name, args, ret, true);
        return ret;
    }
    if (n < 3) {
        std::printf("[stub] %s::%s (%s) args=%llx,%llx,%llx\n", t->library.c_str(), name, t->nid.c_str(),
                    (unsigned long long)args[0], (unsigned long long)args[1], (unsigned long long)args[2]);
    }
    TraceCall(t->library.c_str(), name, args, 0, false);
    return 0;
}

namespace {
// Printable C string at a guest address (for logging pointer arguments), or empty.
std::string PeekString(uint64_t p) {
    if (p < 0x10000) return {};
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        return {};
    const uint64_t end = reinterpret_cast<uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
    std::string s;
    for (uint64_t q = p; q < end && s.size() < 80; ++q) {
        const char c = *reinterpret_cast<const char*>(q);
        if (c == 0) return s.size() >= 3 ? s : std::string();
        if (c < 0x20 || c > 0x7E) return {};
        s += c;
    }
    return {};
}
} // namespace

void Hle::LleLog(const LleInfo* info, const uint64_t* args) {
    char line[512];
    int n = std::snprintf(line, sizeof line, "LLE  %s::%s(%llx, %llx, %llx, %llx)", info->library.c_str(),
                          info->name.c_str(), (unsigned long long)args[0], (unsigned long long)args[1],
                          (unsigned long long)args[2], (unsigned long long)args[3]);
    for (int i = 0; i < 4 && n > 0 && n < int(sizeof line) - 100; ++i) {
        const std::string s = PeekString(args[i]);
        if (!s.empty()) n += std::snprintf(line + n, sizeof line - n, " a%d=\"%s\"", i, s.c_str());
    }
    TraceLine(line, args[24]);
}

uint64_t Hle::GetLleTraceThunk(uint64_t target, const std::string& name, const std::string& library) {
    if (auto it = lle_thunks_.find(target); it != lle_thunks_.end()) return it->second;
    auto* info = new LleInfo{name, library};
    lle_infos_.push_back(info);
    uint8_t* code = AllocCode(320);
    if (!code) return target;
    Emitter e{code};
    auto disp32 = [&](uint32_t d) {
        for (int i = 0; i < 4; ++i) e.B({uint8_t(d >> (8 * i))});
    };
    // Aligned frame through rbp (the guest may call with any alignment): shadow space,
    // rdi..r9 at 0x28..0x50, rax at 0x58, xmm0..7 at 0x60..0xD0, return address copy at 0xE8 (args[24]).
    e.B({0x55});                               // push rbp
    e.B({0x48, 0x89, 0xE5});                   // mov rbp, rsp
    e.B({0x48, 0x83, 0xE4, 0xF0});             // and rsp, -16
    e.B({0x48, 0x81, 0xEC}); disp32(0xF0);
    e.B({0x4C, 0x8B, 0x5D, 0x08});             // mov r11, [rbp+8]   (return address)
    e.B({0x4C, 0x89, 0x9C, 0x24}); disp32(0xE8); // mov [rsp+0xE8], r11
    e.B({0x48, 0x89, 0x7C, 0x24, 0x28});       // mov [rsp+0x28], rdi
    e.B({0x48, 0x89, 0x74, 0x24, 0x30});       // mov [rsp+0x30], rsi
    e.B({0x48, 0x89, 0x54, 0x24, 0x38});       // mov [rsp+0x38], rdx
    e.B({0x48, 0x89, 0x4C, 0x24, 0x40});       // mov [rsp+0x40], rcx
    e.B({0x4C, 0x89, 0x44, 0x24, 0x48});       // mov [rsp+0x48], r8
    e.B({0x4C, 0x89, 0x4C, 0x24, 0x50});       // mov [rsp+0x50], r9
    e.B({0x48, 0x89, 0x44, 0x24, 0x58});       // mov [rsp+0x58], rax  (al = vector-arg count for varargs)
    for (uint8_t i = 0; i < 8; ++i) {          // movdqu [rsp+0x60+16*i], xmm_i
        e.B({0xF3, 0x0F, 0x7F, uint8_t(0x84 | (i << 3)), 0x24}); disp32(0x60 + 16u * i);
    }
    e.B({0x48, 0x8D, 0x54, 0x24, 0x28});       // lea rdx, [rsp+0x28]
    e.B({0x48, 0xB9}); e.Q(reinterpret_cast<uint64_t>(info));
    e.B({0x48, 0xB8}); e.Q(reinterpret_cast<uint64_t>(&Hle::LleLog));
    e.B({0xFF, 0xD0});                         // call rax
    for (uint8_t i = 0; i < 8; ++i) {          // movdqu xmm_i, [rsp+0x60+16*i]
        e.B({0xF3, 0x0F, 0x6F, uint8_t(0x84 | (i << 3)), 0x24}); disp32(0x60 + 16u * i);
    }
    e.B({0x48, 0x8B, 0x7C, 0x24, 0x28});       // restore rdi
    e.B({0x48, 0x8B, 0x74, 0x24, 0x30});       // rsi
    e.B({0x48, 0x8B, 0x54, 0x24, 0x38});       // rdx
    e.B({0x48, 0x8B, 0x4C, 0x24, 0x40});       // rcx
    e.B({0x4C, 0x8B, 0x44, 0x24, 0x48});       // r8
    e.B({0x4C, 0x8B, 0x4C, 0x24, 0x50});       // r9
    e.B({0x48, 0x8B, 0x44, 0x24, 0x58});       // rax
    e.B({0x48, 0x89, 0xEC});                   // mov rsp, rbp
    e.B({0x5D});                               // pop rbp
    e.B({0x49, 0xBB}); e.Q(target);            // mov r11, target   (r11 is scratch in SysV)
    e.B({0x41, 0xFF, 0xE3});                   // jmp r11
    if (e.p - code > 320) { std::fprintf(stderr, "LLE thunk overflow (%td bytes)\n", e.p - code); std::abort(); }
    const uint64_t addr = reinterpret_cast<uint64_t>(code);
    lle_thunks_[target] = addr;
    return addr;
}

} // namespace gr
