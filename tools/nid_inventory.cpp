// nid_inventory: lists modules, libraries, imports and exports of Orbis ELF/SELF files.
//   nid_inventory [--names file.txt]... [--csv out.csv] <file1> [file2 ...]
//   nid_inventory --nid <symbol_name>      (prints the NID of a name)
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "elf_orbis.h"
#include "nid.h"
#include "self.h"

int main(int argc, char** argv) {
    gr::NidDatabase db;
    std::string csv_path;
    std::vector<std::string> inputs;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--nid" && i + 1 < argc) {
            std::printf("%s  %s\n", gr::ComputeNid(argv[i + 1]).c_str(), argv[i + 1]);
            return 0;
        } else if (a == "--names" && i + 1 < argc) {
            const size_t n = db.LoadNamesFile(argv[++i]);
            std::fprintf(stderr, "loaded %zu names from %s\n", n, argv[i]);
        } else if (a == "--csv" && i + 1 < argc) {
            csv_path = argv[++i];
        } else {
            inputs.push_back(a);
        }
    }
    if (inputs.empty()) {
        std::fprintf(stderr, "usage: %s [--names f.txt] [--csv out.csv] <elf/self>...\n", argv[0]);
        return 1;
    }
    std::ofstream csv;
    if (!csv_path.empty()) {
        csv.open(csv_path);
        csv << "file,direction,module,library,nid,name,kind\n";
    }

    size_t total_imports = 0, total_named = 0;
    for (const auto& path : inputs) {
        std::vector<uint8_t> file, elf;
        std::string err;
        gr::OrbisElf oe;
        if (!gr::ReadWholeFile(path, file) || !gr::SelfToElf(file, elf, nullptr, &err) || !oe.Parse(elf, &err)) {
            std::fprintf(stderr, "%s: %s\n", path.c_str(), err.empty() ? "read error" : err.c_str());
            continue;
        }
        std::printf("=== %s  (%s, original name '%s')\n", path.c_str(), gr::ElfTypeName(oe.ehdr.e_type),
                    oe.original_filename.c_str());
        if (!oe.fingerprint_hex.empty()) std::printf("fingerprint %s\n", oe.fingerprint_hex.c_str());
        for (const auto& m : oe.module_info)
            std::printf("this module: %s v%d.%d id=%u\n", m.name.c_str(), m.version >> 8, m.version & 0xFF, m.id);
        for (const auto& m : oe.needed_modules)
            std::printf("needs module: %-28s v%d.%d id=%u\n", m.name.c_str(), m.version >> 8, m.version & 0xFF, m.id);

        // Group symbols by (direction, library).
        struct Group { size_t total = 0, named = 0; std::vector<std::string> lines; };
        std::map<std::string, Group> groups;
        for (const auto& s : oe.symbols) {
            if (s.library_id == 0xFFFF) continue; // locals / unnamed
            const auto* lib = oe.FindLibrary(s.library_id, s.defined());
            const auto* mod = oe.FindModule(s.module_id);
            const std::string libname = lib ? lib->name : "lib#" + std::to_string(s.library_id);
            const std::string modname = mod ? mod->name : "mod#" + std::to_string(s.module_id);
            const char* dir = s.defined() ? "export" : "import";
            const std::string* name = db.Lookup(s.nid);
            const char* kind = s.type == 2 ? "func" : s.type == 1 ? "object" : s.type == 6 ? "tls" : "other";
            auto& g = groups[std::string(dir) + " " + libname + " (" + modname + ")"];
            g.total++;
            if (name) g.named++;
            g.lines.push_back(s.nid + "  " + (name ? *name : std::string("?")) + "  [" + kind + "]");
            if (!s.defined()) {
                total_imports++;
                if (name) total_named++;
            }
            if (csv.is_open())
                csv << path << ',' << dir << ',' << modname << ',' << libname << ',' << s.nid << ','
                    << (name ? *name : "") << ',' << kind << '\n';
        }
        for (const auto& [key, g] : groups) {
            std::printf("  %-60s %5zu symbols, %5zu named\n", key.c_str(), g.total, g.named);
        }
        std::printf("  relocations: rela=%zu jmprel=%zu\n", oe.rela.size(), oe.jmprel.size());
    }
    std::printf("TOTAL imports: %zu (named via database: %zu, NID db size %zu)\n", total_imports, total_named,
                db.size());
    return 0;
}
