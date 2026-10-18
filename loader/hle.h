#pragma once
// HLE registry and SysV->MS-x64 call thunks.
//
// Guest (PS4) code uses the System V AMD64 ABI; this host is built with MSVC (MS x64 ABI).
// Every import that is not satisfied by a bundled guest module gets a small machine-code
// thunk that spills the six SysV integer argument registers into an array and calls a
// host dispatcher with (thunk_index, args). The dispatcher calls the registered HLE
// function, or logs the call and returns 0 for a stub.
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace gr {

using HleFunc = uint64_t (*)(const uint64_t* args);

struct ThunkInfo {
    std::string nid;
    std::string name;    // human name if known
    std::string library;
    HleFunc func = nullptr; // nullptr = unimplemented stub
    std::atomic<uint64_t> calls{0};
};

class Hle {
public:
    Hle();
    ~Hle();

    // Registers an implementation by symbol name (the NID is derived from it).
    void Register(const std::string& name, HleFunc fn);
    void RegisterNid(const std::string& nid, HleFunc fn) { impl_[nid] = fn; } // for symbols with no known name
    bool IsImplemented(const std::string& nid) const { return impl_.count(nid) != 0; }

    // Returns a callable address for an imported function (one thunk per nid+library).
    uint64_t GetFunctionThunk(const std::string& nid, const std::string& name, const std::string& library);
    // Returns writable storage for an imported data object (e.g. __stack_chk_guard).
    uint64_t GetDataObject(const std::string& nid, const std::string& name, size_t size = 0x100);
    // Wraps a guest-to-guest (LLE) call: logs name + arguments, then tail-jumps to `target`.
    uint64_t GetLleTraceThunk(uint64_t target, const std::string& name, const std::string& library);

    size_t ThunkCount() const { return thunks_.size(); }
    size_t ImplementedThunkCount() const;
    const std::vector<ThunkInfo*>& Thunks() const { return thunks_; }


    static Hle* Instance() { return instance_; }
    static uint64_t Dispatch(uint64_t index, const uint64_t* args); // MS ABI, called from thunks

private:
    uint8_t* AllocCode(size_t n);

    static Hle* instance_;
    std::unordered_map<std::string, HleFunc> impl_; // nid -> fn
    std::unordered_map<std::string, uint64_t> thunk_by_key_;
    std::unordered_map<std::string, uint64_t> data_by_nid_;
    std::vector<ThunkInfo*> thunks_;
    uint8_t* code_ = nullptr;
    size_t code_used_ = 0, code_cap_ = 0;
    std::vector<void*> data_blocks_;
    struct LleInfo {
        std::string name, library;
    };
    std::unordered_map<uint64_t, uint64_t> lle_thunks_; // target -> thunk
    std::vector<LleInfo*> lle_infos_;
    static void LleLog(const LleInfo* info, const uint64_t* args);
};

void RegisterPhase0Hle(Hle& hle); // hle_functions.cpp

} // namespace gr
