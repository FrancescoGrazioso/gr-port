#pragma once
// Orbis NIDs: symbol names in Sony ELFs are replaced by an 11-char hash ("NID").
// NID = base64(reverse(first 8 bytes of SHA1(name || suffix))) with '/' -> '-'.
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace gr {

std::string ComputeNid(std::string_view symbol_name);

// Decodes the "#X" library/module index fields that follow a NID in symbol names.
uint16_t DecodeNidIndex(std::string_view s);

// NID -> human name database, loaded from text files with one symbol name per line.
class NidDatabase {
public:
    size_t LoadNamesFile(const std::string& path); // returns names added
    void AddName(std::string_view name);
    const std::string* Lookup(std::string_view nid) const;
    size_t size() const { return map_.size(); }

private:
    std::unordered_map<std::string, std::string> map_;
};

} // namespace gr
