#include "linker.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <sstream>

#include "self.h"
#include "vmem.h"

namespace gr {
namespace {
constexpr uint64_t kPage = 0x4000; // PS4 page size; also a multiple of the host 4K page

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

const char* RelocName(uint32_t t) {
    switch (t) {
    case R_X86_64_64: return "R_X86_64_64";
    case R_X86_64_GLOB_DAT: return "GLOB_DAT";
    case R_X86_64_JUMP_SLOT: return "JUMP_SLOT";
    case R_X86_64_RELATIVE: return "RELATIVE";
    case R_X86_64_DTPMOD64: return "DTPMOD64";
    case R_X86_64_DTPOFF64: return "DTPOFF64";
    case R_X86_64_TPOFF64: return "TPOFF64";
    default: return "other";
    }
}
} // namespace

Linker::~Linker() {
    for (auto& m : modules_)
        if (m->mapping) VirtualFree(m->mapping, 0, MEM_RELEASE);
}

bool Linker::Load(const std::string& path, std::string* err) {
    auto m = std::make_unique<LoadedModule>();
    m->path = path;
    std::vector<uint8_t> file;
    if (!ReadWholeFile(path, file)) { *err = "cannot read " + path; return false; }
    if (!SelfToElf(file, m->elf, nullptr, err)) return false;
    if (!m->oe.Parse(m->elf, err)) return false;
    m->name = m->oe.module_info.empty() ? path : m->oe.module_info[0].name;

    uint64_t min_vaddr = 0;
    const uint64_t span = m->oe.VirtualSpan(&min_vaddr);
    m->size = AlignUp(span, kPage);
    m->mapping = reinterpret_cast<uint8_t*>(
        GuestVirtualAlloc(0, m->size, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, false, kGuestModuleBase));
    if (!m->mapping) { *err = "VirtualAlloc failed for " + path; return false; }
    m->base = reinterpret_cast<uint64_t>(m->mapping) - min_vaddr;

    for (const auto& p : m->oe.phdrs) {
        if (p.p_type == PT_LOAD || p.p_type == PT_SCE_RELRO) {
            if (p.p_offset + p.p_filesz > m->elf.size()) { *err = "segment outside file"; return false; }
            std::memcpy(reinterpret_cast<void*>(m->base + p.p_vaddr), m->elf.data() + p.p_offset, p.p_filesz);
        } else if (p.p_type == PT_TLS) {
            m->tls_index = next_tls_++;
        }
    }
    for (const auto& s : m->oe.symbols) {
        if (!s.defined() || s.library_id == 0xFFFF) continue;
        const auto* lib = m->oe.FindLibrary(s.library_id, true);
        if (!lib) continue;
        const std::string key = s.nid + "#" + lib->name;
        m->exports[key] = m->base + s.value;
        global_exports_.emplace(key, std::make_pair(m->base + s.value, m->name));
    }
    modules_.push_back(std::move(m));
    return true;
}

uint64_t Linker::ResolveImport(const LoadedModule& m, const OrbisSymbol& s, bool* was_lle) {
    const auto* lib = m.oe.FindLibrary(s.library_id, false);
    const std::string libname = lib ? lib->name : "lib#" + std::to_string(s.library_id);
    *was_lle = false;
    if (auto it = global_exports_.find(s.nid + "#" + libname); it != global_exports_.end()) {
        *was_lle = true;
        const auto& [addr, module] = it->second;
        if (module != m.name && std::find(trace_lle_.begin(), trace_lle_.end(), module) != trace_lle_.end()) {
            const std::string* name = names_.Lookup(s.nid);
            if (s.type != 1) return hle_.GetLleTraceThunk(addr, name ? *name : s.nid, libname);
        }
        return addr;
    }
    const std::string* name = names_.Lookup(s.nid);
    if (s.type == 1 /*STT_OBJECT*/) {
        stats_.data_objects++;
        return hle_.GetDataObject(s.nid, name ? *name : std::string());
    }
    return hle_.GetFunctionThunk(s.nid, name ? *name : std::string(), libname);
}

void Linker::Relocate(LoadedModule& m, const Elf64_Rela& r) {
    const uint32_t type = uint32_t(r.r_info);
    const uint32_t sym_index = uint32_t(r.r_info >> 32);
    stats_.relocs_by_type[type]++;
    auto* where = reinterpret_cast<uint64_t*>(m.base + r.r_offset);

    auto symbol_value = [&](bool* imported) -> uint64_t {
        *imported = false;
        if (sym_index == 0 || sym_index >= m.oe.symbols.size()) return 0; // STN_UNDEF: S = 0
        const auto& s = m.oe.symbols[sym_index];
        if (s.defined()) return m.base + s.value;
        if (s.library_id == 0xFFFF && s.bind == 2 /*STB_WEAK*/) return 0; // e.g. module_start/stop
        *imported = true;
        bool lle = false;
        const uint64_t v = ResolveImport(m, s, &lle);
        if (lle) stats_.imports_lle++;
        else if (s.type != 1 && hle_.IsImplemented(s.nid)) stats_.imports_hle++;
        else if (s.type != 1) stats_.imports_stub++;
        return v;
    };

    bool imported = false;
    switch (type) {
    case R_X86_64_NONE: break;
    case R_X86_64_RELATIVE: *where = m.base + r.r_addend; break;
    case R_X86_64_64: *where = symbol_value(&imported) + r.r_addend; break;
    case R_X86_64_GLOB_DAT:
    case R_X86_64_JUMP_SLOT: *where = symbol_value(&imported); break;
    case R_X86_64_DTPMOD64: *where = m.tls_index; break;
    case R_X86_64_DTPOFF64: {
        const uint64_t v = sym_index < m.oe.symbols.size() ? m.oe.symbols[sym_index].value : 0;
        *where = v + r.r_addend;
        break;
    }
    default: stats_.unsupported++; break; // TPOFF64 etc.: needs the TLS layout (phase 1)
    }
}

void Linker::LinkAll() {
    stats_ = {};
    for (auto& m : modules_) {
        for (const auto& r : m->oe.rela) Relocate(*m, r);
        for (const auto& r : m->oe.jmprel) Relocate(*m, r);
    }
}

void Linker::Protect() {
    for (auto& m : modules_) {
        for (const auto& p : m->oe.phdrs) {
            if (p.p_type != PT_LOAD && p.p_type != PT_SCE_RELRO) continue;
            DWORD prot = PAGE_READONLY;
            if (p.p_type == PT_LOAD && (p.p_flags & 1)) prot = PAGE_EXECUTE_READ;
            else if (p.p_type == PT_LOAD && (p.p_flags & 2)) prot = PAGE_READWRITE;
            const uint64_t start = (m->base + p.p_vaddr) & ~(kPage - 1);
            const uint64_t end = AlignUp(m->base + p.p_vaddr + p.p_memsz, kPage);
            DWORD old;
            VirtualProtect(reinterpret_cast<void*>(start), end - start, prot, &old);
        }
        FlushInstructionCache(GetCurrentProcess(), m->mapping, m->size);
    }
}

uint64_t Linker::EntryPoint() const {
    return modules_.empty() ? 0 : modules_[0]->base + modules_[0]->oe.ehdr.e_entry;
}

std::string Linker::Report() const {
    std::ostringstream o;
    o << "== modules\n";
    for (const auto& m : modules_) {
        char line[512];
        std::snprintf(line, sizeof line, "%-22s %-15s base=0x%012llx size=0x%09llx exports=%-5zu tls=%u init=0x%llx\n",
                      m->name.c_str(), ElfTypeName(m->oe.ehdr.e_type), (unsigned long long)m->base,
                      (unsigned long long)m->size, m->exports.size(), m->tls_index,
                      (unsigned long long)(m->oe.init ? m->base + m->oe.init : 0));
        o << line;
    }
    o << "== relocations\n";
    for (const auto& [t, n] : stats_.relocs_by_type) o << "  " << RelocName(t) << " (" << t << "): " << n << "\n";
    o << "== import bindings (relocation sites)\n"
      << "  LLE (bundled guest module): " << stats_.imports_lle << "\n"
      << "  HLE implemented:            " << stats_.imports_hle << "\n"
      << "  stub (logs + returns 0):    " << stats_.imports_stub << "\n"
      << "  data objects:               " << stats_.data_objects << "\n"
      << "  unsupported reloc types:    " << stats_.unsupported << "\n";

    std::map<std::string, std::vector<const ThunkInfo*>> by_lib;
    for (const auto* t : hle_.Thunks()) by_lib[t->library].push_back(t);
    o << "== host-side functions by library (" << hle_.ThunkCount() << " unique, " << hle_.ImplementedThunkCount()
      << " implemented)\n";
    for (const auto& [lib, v] : by_lib) {
        size_t impl = std::count_if(v.begin(), v.end(), [](const ThunkInfo* t) { return t->func != nullptr; });
        o << "  " << lib << ": " << v.size() << " (" << impl << " implemented)\n";
        for (const auto* t : v)
            o << "      " << (t->func ? "[HLE] " : "[stub] ") << t->nid << "  " << (t->name.empty() ? "?" : t->name)
              << "\n";
    }
    return o.str();
}

} // namespace gr
