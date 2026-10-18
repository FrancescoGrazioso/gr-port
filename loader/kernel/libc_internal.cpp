// libSceLibcInternal: the C runtime the bundled Sony prx modules import. Mapped onto the host CRT.
// printf-family functions decode the System V va_list directly (it differs from the MS one).
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "guest.h"
#include "kernel.h"

namespace gr::kernel {
namespace {

// System V AMD64 va_list element.
struct SysVVaList {
    uint32_t gp_offset; // < 48: next integer arg in reg_save_area
    uint32_t fp_offset; // < 176: next xmm arg in reg_save_area
    uint64_t overflow_arg_area;
    uint64_t reg_save_area;
};

uint64_t NextInt(SysVVaList* va) {
    if (va->gp_offset < 48) {
        const uint64_t v = Load<uint64_t>(va->reg_save_area + va->gp_offset);
        va->gp_offset += 8;
        return v;
    }
    const uint64_t v = Load<uint64_t>(va->overflow_arg_area);
    va->overflow_arg_area += 8;
    return v;
}
double NextDouble(SysVVaList* va) {
    if (va->fp_offset < 176) {
        const double v = Load<double>(va->reg_save_area + va->fp_offset);
        va->fp_offset += 16;
        return v;
    }
    const double v = Load<double>(va->overflow_arg_area);
    va->overflow_arg_area += 8;
    return v;
}

// Formats `fmt` pulling arguments from a SysV va_list. Each conversion is delegated to the host
// snprintf with the argument converted to the right host type.
std::string Format(const char* fmt, SysVVaList* va) {
    std::string out;
    char spec[32], buf[512];
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') { out += *p; continue; }
        const char* start = p++;
        if (*p == '%') { out += '%'; continue; }
        std::string flags;
        while (*p && std::strchr("-+ #0", *p)) flags += *p++;
        std::string width, prec;
        if (*p == '*') { width = std::to_string(int32_t(NextInt(va))); ++p; }
        while (*p >= '0' && *p <= '9') width += *p++;
        if (*p == '.') {
            ++p;
            prec = ".";
            if (*p == '*') { prec += std::to_string(int32_t(NextInt(va))); ++p; }
            while (*p >= '0' && *p <= '9') prec += *p++;
        }
        int lng = 0; // 0 int, 1 long/ll/z/j/t (64-bit), -1 short, -2 char
        bool long_double = false;
        while (*p && std::strchr("hlLqjzt", *p)) {
            if (*p == 'h') lng = lng == -1 ? -2 : -1;
            else if (*p == 'L') long_double = true;
            else lng = 1;
            ++p;
        }
        const char conv = *p;
        if (!conv) break;
        const std::string base = flags + width + prec;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v = int64_t(NextInt(va));
            if (lng == 0) v = int32_t(v); else if (lng == -1) v = int16_t(v); else if (lng == -2) v = int8_t(v);
            std::snprintf(spec, sizeof spec, "%%%slld", base.c_str());
            std::snprintf(buf, sizeof buf, spec, (long long)v);
            out += buf;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = NextInt(va);
            if (lng == 0) v = uint32_t(v); else if (lng == -1) v = uint16_t(v); else if (lng == -2) v = uint8_t(v);
            std::snprintf(spec, sizeof spec, "%%%sll%c", base.c_str(), conv);
            std::snprintf(buf, sizeof buf, spec, (unsigned long long)v);
            out += buf;
            break;
        }
        case 'c':
            std::snprintf(spec, sizeof spec, "%%%sc", base.c_str());
            std::snprintf(buf, sizeof buf, spec, int(NextInt(va)));
            out += buf;
            break;
        case 's': {
            const uint64_t s = NextInt(va);
            std::snprintf(spec, sizeof spec, "%%%ss", base.c_str());
            const int need = std::snprintf(nullptr, 0, spec, s ? Ptr<const char>(s) : "(null)");
            std::string tmp(size_t(need) + 1, '\0');
            std::snprintf(tmp.data(), tmp.size(), spec, s ? Ptr<const char>(s) : "(null)");
            tmp.resize(size_t(need));
            out += tmp;
            break;
        }
        case 'p':
            std::snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)NextInt(va));
            out += buf;
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            const double v = long_double ? NextDouble(va) : NextDouble(va); // long double unsupported
            std::snprintf(spec, sizeof spec, "%%%s%c", base.c_str(), conv);
            std::snprintf(buf, sizeof buf, spec, v);
            out += buf;
            break;
        }
        case 'n': NextInt(va); break;
        default: out.append(start, p + 1); break;
        }
    }
    return out;
}

uint64_t WriteBounded(uint64_t dst, uint64_t n, const std::string& s) {
    if (dst && n) {
        const size_t c = std::min<size_t>(s.size(), size_t(n - 1));
        std::memcpy(Ptr<char>(dst), s.data(), c);
        Ptr<char>(dst)[c] = 0;
    }
    return s.size();
}

// Builds a va_list for a variadic call whose fixed arguments consumed `fixed` integer registers.
// Float varargs (passed in xmm registers) are not visible to the thunk and print as 0.
struct RegVaList {
    uint64_t save[22] = {}; // 6 gp + 8 xmm*2
    uint64_t overflow[2];
    SysVVaList va;
    RegVaList(const uint64_t* a, uint32_t fixed) {
        for (int i = 0; i < 6; ++i) save[i] = a[i];
        overflow[0] = a[6];
        overflow[1] = a[7];
        va = {fixed * 8, 48, reinterpret_cast<uint64_t>(overflow), reinterpret_cast<uint64_t>(save)};
    }
};

// FreeBSD struct tm: 9 ints, pad, long tm_gmtoff, char* tm_zone (56 bytes)
thread_local uint8_t t_tm[56];
uint64_t StoreTm(const std::tm& t, uint64_t dst) {
    uint8_t* p = dst ? Ptr<uint8_t>(dst) : t_tm;
    std::memset(p, 0, 56);
    const int v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    std::memcpy(p, v, sizeof v);
    return reinterpret_cast<uint64_t>(p);
}
std::tm LoadTm(uint64_t src) {
    std::tm t{};
    int v[9];
    std::memcpy(v, Ptr<void>(src), sizeof v);
    t.tm_sec = v[0]; t.tm_min = v[1]; t.tm_hour = v[2]; t.tm_mday = v[3]; t.tm_mon = v[4];
    t.tm_year = v[5]; t.tm_wday = v[6]; t.tm_yday = v[7]; t.tm_isdst = v[8];
    return t;
}

} // namespace

void RegisterLibcInternal(Hle& h) {
    h.Register("memset", [](const uint64_t* a) -> uint64_t { std::memset(Ptr<void>(a[0]), int(a[1]), a[2]); return a[0]; });
    h.Register("memcpy", [](const uint64_t* a) -> uint64_t { std::memcpy(Ptr<void>(a[0]), Ptr<void>(a[1]), a[2]); return a[0]; });
    h.Register("memmove", [](const uint64_t* a) -> uint64_t { std::memmove(Ptr<void>(a[0]), Ptr<void>(a[1]), a[2]); return a[0]; });
    h.Register("memcmp", [](const uint64_t* a) -> uint64_t { return uint64_t(int64_t(std::memcmp(Ptr<void>(a[0]), Ptr<void>(a[1]), a[2]))); });
    h.Register("strlen", [](const uint64_t* a) -> uint64_t { return std::strlen(Ptr<const char>(a[0])); });
    h.Register("strcmp", [](const uint64_t* a) -> uint64_t { return uint64_t(int64_t(std::strcmp(Ptr<const char>(a[0]), Ptr<const char>(a[1])))); });
    h.Register("strncmp", [](const uint64_t* a) -> uint64_t { return uint64_t(int64_t(std::strncmp(Ptr<const char>(a[0]), Ptr<const char>(a[1]), a[2]))); });
    h.Register("strcpy", [](const uint64_t* a) -> uint64_t { std::strcpy(Ptr<char>(a[0]), Ptr<const char>(a[1])); return a[0]; });
    h.Register("strncpy", [](const uint64_t* a) -> uint64_t { std::strncpy(Ptr<char>(a[0]), Ptr<const char>(a[1]), a[2]); return a[0]; });
    h.Register("malloc", [](const uint64_t* a) -> uint64_t { return reinterpret_cast<uint64_t>(_aligned_malloc(a[0] ? a[0] : 1, 16)); });
    h.Register("free", [](const uint64_t* a) -> uint64_t { _aligned_free(Ptr<void>(a[0])); return 0; });
    h.Register("calloc", [](const uint64_t* a) -> uint64_t {
        void* p = _aligned_malloc(a[0] * a[1] ? a[0] * a[1] : 1, 16);
        std::memset(p, 0, a[0] * a[1]);
        return reinterpret_cast<uint64_t>(p);
    });
    h.Register("realloc", [](const uint64_t* a) -> uint64_t { return reinterpret_cast<uint64_t>(_aligned_realloc(Ptr<void>(a[0]), a[1] ? a[1] : 1, 16)); });
    h.Register("memalign", [](const uint64_t* a) -> uint64_t {
        return reinterpret_cast<uint64_t>(_aligned_malloc(a[1] ? a[1] : 1, a[0] < 16 ? 16 : a[0]));
    });

    // int vsnprintf(char*, size_t, const char*, va_list)
    h.Register("vsnprintf", [](const uint64_t* a) -> uint64_t {
        return WriteBounded(a[0], a[1], Format(Ptr<const char>(a[2]), Ptr<SysVVaList>(a[3])));
    });
    h.Register("vsprintf", [](const uint64_t* a) -> uint64_t {
        return WriteBounded(a[0], ~0ull, Format(Ptr<const char>(a[1]), Ptr<SysVVaList>(a[2])));
    });
    h.Register("vprintf", [](const uint64_t* a) -> uint64_t {
        const std::string s = Format(Ptr<const char>(a[0]), Ptr<SysVVaList>(a[1]));
        std::fputs(s.c_str(), stdout);
        return s.size();
    });
    h.Register("printf", [](const uint64_t* a) -> uint64_t {
        RegVaList v(a, 1);
        const std::string s = Format(Ptr<const char>(a[0]), &v.va);
        std::fputs(s.c_str(), stdout);
        return s.size();
    });
    h.Register("snprintf", [](const uint64_t* a) -> uint64_t {
        RegVaList v(a, 3);
        return WriteBounded(a[0], a[1], Format(Ptr<const char>(a[2]), &v.va));
    });
    h.Register("sprintf", [](const uint64_t* a) -> uint64_t {
        RegVaList v(a, 2);
        return WriteBounded(a[0], ~0ull, Format(Ptr<const char>(a[1]), &v.va));
    });
    h.Register("puts", [](const uint64_t* a) -> uint64_t { std::puts(Ptr<const char>(a[0])); return 0; });
    h.Register("fflush", [](const uint64_t*) -> uint64_t { std::fflush(stdout); return 0; });
    h.Register("__cxa_finalize", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("__cxa_atexit", [](const uint64_t*) -> uint64_t { return 0; }); // static dtors never run at exit

    // time
    h.Register("time", [](const uint64_t* a) -> uint64_t {
        const int64_t t = int64_t(std::time(nullptr));
        Store<int64_t>(a[0], t);
        return uint64_t(t);
    });
    h.Register("gmtime", [](const uint64_t* a) -> uint64_t {
        std::tm t{};
        const std::time_t tt = std::time_t(Load<int64_t>(a[0]));
        gmtime_s(&t, &tt);
        return StoreTm(t, 0);
    });
    h.Register("gmtime_s", [](const uint64_t* a) -> uint64_t { // FreeBSD-style gmtime_s(const time_t*, struct tm*)
        std::tm t{};
        const std::time_t tt = std::time_t(Load<int64_t>(a[0]));
        gmtime_s(&t, &tt);
        return StoreTm(t, a[1]);
    });
    h.Register("localtime", [](const uint64_t* a) -> uint64_t {
        std::tm t{};
        const std::time_t tt = std::time_t(Load<int64_t>(a[0]));
        localtime_s(&t, &tt);
        return StoreTm(t, 0);
    });
    h.Register("localtime_s", [](const uint64_t* a) -> uint64_t {
        std::tm t{};
        const std::time_t tt = std::time_t(Load<int64_t>(a[0]));
        localtime_s(&t, &tt);
        return StoreTm(t, a[1]);
    });
    h.Register("mktime", [](const uint64_t* a) -> uint64_t {
        std::tm t = LoadTm(a[0]);
        return uint64_t(int64_t(std::mktime(&t)));
    });
}

} // namespace gr::kernel
