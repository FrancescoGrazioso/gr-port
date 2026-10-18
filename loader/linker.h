#pragma once
// Maps Orbis modules (eboot.bin + bundled *.prx) into host memory and links them.
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "elf_orbis.h"
#include "hle.h"
#include "nid.h"

namespace gr {

struct LoadedModule {
    std::string path;
    std::string name; // module name from DT_SCE_MODULE_INFO
    std::vector<uint8_t> elf;
    OrbisElf oe;
    uint8_t* mapping = nullptr; // allocation start
    uint64_t base = 0;          // value added to p_vaddr
    uint64_t size = 0;
    uint32_t tls_index = 0;     // 1-based, 0 = no TLS segment
    std::unordered_map<std::string, uint64_t> exports; // "nid#library" -> absolute address
};

struct LinkStats {
    std::map<uint32_t, uint64_t> relocs_by_type;
    uint64_t imports_lle = 0;   // satisfied by another loaded guest module
    uint64_t imports_hle = 0;   // satisfied by a host implementation
    uint64_t imports_stub = 0;  // logging stub
    uint64_t data_objects = 0;  // imported variables backed by host storage
    uint64_t unsupported = 0;   // relocation types not handled yet
};

class Linker {
public:
    Linker(Hle& hle, const NidDatabase& names) : hle_(hle), names_(names) {}
    ~Linker();

    bool Load(const std::string& path, std::string* err);
    // Imports resolved to these guest modules go through logging thunks (--trace-lle).
    void TraceLleModules(std::vector<std::string> names) { trace_lle_ = std::move(names); }
    void LinkAll();
    void Protect(); // apply final page protections (after relocation)

    const std::vector<std::unique_ptr<LoadedModule>>& Modules() const { return modules_; }
    const LinkStats& Stats() const { return stats_; }
    std::string Report() const;
    uint64_t EntryPoint() const; // eboot entry (not executed in phase 0)

private:
    uint64_t ResolveImport(const LoadedModule& m, const OrbisSymbol& s, bool* was_lle);
    void Relocate(LoadedModule& m, const Elf64_Rela& r);

    Hle& hle_;
    const NidDatabase& names_;
    std::vector<std::unique_ptr<LoadedModule>> modules_;
    std::unordered_map<std::string, std::pair<uint64_t, std::string>> global_exports_; // key -> (addr, module)
    std::vector<std::string> trace_lle_;
    LinkStats stats_;
    uint32_t next_tls_ = 1;
};

} // namespace gr
