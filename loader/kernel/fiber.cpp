// libSceFiber on top of Windows fibers. Windows fibers share the thread's TLS slots, so the guest
// thread pointer (read through the patched gs: slot) stays correct across switches.
#include <windows.h>

#include <cstdio>

#include "guest.h"
#include "kernel.h"

namespace gr::kernel {
namespace {

constexpr uint64_t kFiberMagic = 0x4752464942455221ull; // stored in the guest SceFiber object
constexpr size_t kFiberStack = 2u << 20;
constexpr uint64_t kErrInvalid = 0x80590002, kErrState = 0x80590006, kErrPermission = 0x80590005;

struct Fiber {
    uint64_t guest = 0; // guest SceFiber*
    uint64_t entry = 0, arg_on_init = 0;
    uint64_t resume_arg = 0; // argument delivered when this fiber is (re)entered
    void* win = nullptr;
    void* caller = nullptr; // thread fiber to return to
    char name[32] = {};
    bool running = false;
};

thread_local void* t_thread_fiber = nullptr;
thread_local Fiber* t_current = nullptr;
thread_local uint64_t t_return_arg = 0;

Fiber* FromGuest(uint64_t g) {
    if (!g || Load<uint64_t>(g) != kFiberMagic) return nullptr;
    return Ptr<Fiber>(Load<uint64_t>(g + 8));
}

void EnsureThreadFiber() {
    if (!t_thread_fiber) t_thread_fiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);
}

void WINAPI FiberProc(void* p) {
    auto* f = static_cast<Fiber*>(p);
    // void entry(u64 argOnInitialize, u64 argOnRun): must never return on PS4.
    CallGuest(f->entry, {f->arg_on_init, f->resume_arg});
    TraceNote("fiber '%s' returned from its entry function", f->name);
    t_return_arg = 0;
    f->running = false;
    for (;;) SwitchToFiber(f->caller);
}

// void* entry, u64 argOnInit, void* addrContext, u64 sizeContext, opt*
uint64_t Initialize(const uint64_t* a) {
    if (!a[0] || !a[2]) return kErrInvalid;
    auto* f = new Fiber;
    f->guest = a[0];
    f->entry = a[2];
    f->arg_on_init = a[3];
    if (a[1]) std::snprintf(f->name, sizeof f->name, "%s", Ptr<const char>(a[1]));
    f->win = CreateFiberEx(kFiberStack, kFiberStack, FIBER_FLAG_FLOAT_SWITCH, FiberProc, f);
    if (!f->win) { delete f; return kErrInvalid; }
    Store<uint64_t>(a[0], kFiberMagic);
    Store<uint64_t>(a[0] + 8, reinterpret_cast<uint64_t>(f));
    return 0;
}

} // namespace

void RegisterFiber(Hle& h) {
    // (SceFiber*, const char* name, entry, u64 argOnInit, void* ctx, u64 ctxSize, const opt*[, u32 build])
    h.Register("sceFiberInitialize", Initialize);
    h.Register("_sceFiberInitializeImpl", Initialize);
    h.Register("sceFiberOptParamInitialize", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceFiberFinalize", [](const uint64_t* a) -> uint64_t {
        Fiber* f = FromGuest(a[0]);
        if (!f) return kErrInvalid;
        if (f->running) return kErrState;
        DeleteFiber(f->win);
        Store<uint64_t>(a[0], 0);
        delete f;
        return 0;
    });
    // (SceFiber*, u64 argOnRunTo, u64* argOnReturn): run from a plain thread until the fiber returns
    h.Register("sceFiberRun", [](const uint64_t* a) -> uint64_t {
        Fiber* f = FromGuest(a[0]);
        if (!f) return kErrInvalid;
        if (t_current) return kErrPermission; // must be called from a thread, not a fiber
        if (f->running) return kErrState;
        EnsureThreadFiber();
        f->caller = t_thread_fiber;
        f->resume_arg = a[1];
        f->running = true;
        t_current = f;
        SwitchToFiber(f->win);
        t_current = nullptr;
        Store<uint64_t>(a[2], t_return_arg);
        return 0;
    });
    // (SceFiber* target, u64 argOnRunTo, u64* argOnRun): fiber -> fiber
    h.Register("sceFiberSwitch", [](const uint64_t* a) -> uint64_t {
        Fiber* target = FromGuest(a[0]);
        Fiber* self = t_current;
        if (!target || !self) return kErrPermission;
        if (target->running) return kErrState;
        target->caller = self->caller;
        target->resume_arg = a[1];
        target->running = true;
        self->running = false;
        t_current = target;
        SwitchToFiber(target->win);
        // resumed: someone switched back to us
        t_current = self;
        self->running = true;
        Store<uint64_t>(a[2], self->resume_arg);
        return 0;
    });
    // (u64 argOnReturn, u64* argOnRun)
    h.Register("sceFiberReturnToThread", [](const uint64_t* a) -> uint64_t {
        Fiber* self = t_current;
        if (!self) return kErrPermission;
        t_return_arg = a[0];
        self->running = false;
        SwitchToFiber(self->caller);
        // resumed by a later sceFiberRun
        t_current = self;
        self->running = true;
        Store<uint64_t>(a[1], self->resume_arg);
        return 0;
    });
    h.Register("sceFiberGetSelf", [](const uint64_t* a) -> uint64_t {
        if (!t_current) return kErrPermission;
        Store<uint64_t>(a[0], t_current->guest);
        return 0;
    });
    // SceFiberInfo { u64 size; entry; argOnInit; addrContext; sizeContext; char name[32]; u64 sizeContextMargin; }
    h.Register("sceFiberGetInfo", [](const uint64_t* a) -> uint64_t {
        Fiber* f = FromGuest(a[0]);
        if (!f || !a[1]) return kErrInvalid;
        Store<uint64_t>(a[1] + 8, f->entry);
        Store<uint64_t>(a[1] + 16, f->arg_on_init);
        std::memcpy(Ptr<char>(a[1] + 40), f->name, 32);
        return 0;
    });
    h.Register("sceFiberRename", [](const uint64_t* a) -> uint64_t {
        if (Fiber* f = FromGuest(a[0])) std::snprintf(f->name, sizeof f->name, "%s", Ptr<const char>(a[1]));
        return 0;
    });
    h.Register("sceFiberStartContextSizeCheck", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceFiberStopContextSizeCheck", [](const uint64_t*) -> uint64_t { return 0; });
}

} // namespace gr::kernel
