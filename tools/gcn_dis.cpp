// gcn_dis: disassembles captured GCN shader programs.
//   gcn_dis <shader.bin>...          full listing of each file
//   gcn_dis --stats <shader.bin>...  opcode histogram over all files (what the translator must cover)
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "gcn.h"
#include "self.h"

int main(int argc, char** argv) {
    bool stats = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--stats") stats = true;
        else files.push_back(a);
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: %s [--stats] <shader.bin>...\n", argv[0]);
        return 1;
    }
    std::map<std::string, int> hist;
    for (const auto& f : files) {
        std::vector<uint8_t> data;
        if (!gr::ReadWholeFile(f, data)) { std::fprintf(stderr, "cannot read %s\n", f.c_str()); continue; }
        data.resize((data.size() + 3) & ~size_t(3));
        const auto prog = gr::gcn::Decode(reinterpret_cast<const uint32_t*>(data.data()), uint32_t(data.size()));
        if (!stats) std::printf("== %s (%zu instructions)\n", f.c_str(), prog.size());
        for (const auto& in : prog) {
            if (stats) {
                hist[std::string(gr::gcn::FmtName(in.fmt)) + " " + gr::gcn::OpName(in) +
                     (std::string(gr::gcn::OpName(in)) == "?" ? " #" + std::to_string(in.op) : "")]++;
            } else {
                std::printf("  %04x  %s\n", in.pc, gr::gcn::Disassemble(in).c_str());
            }
        }
    }
    if (stats)
        for (const auto& [k, v] : hist) std::printf("%6d  %s\n", v, k.c_str());
    return 0;
}
