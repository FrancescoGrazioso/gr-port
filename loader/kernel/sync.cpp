// libkernel synchronisation objects: semaphores, event flags and event queues (kqueue-like).
// Handles given to the guest are pointers to host objects.
#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include "equeue.h"
#include "guest.h"
#include "kernel.h"

namespace gr::kernel {
namespace {

using Clock = std::chrono::steady_clock;

// Timeouts are passed as `u32* micros` (nullptr = wait forever); on timeout the kernel writes 0 back.
template <class Pred> bool WaitFor(std::condition_variable& cv, std::unique_lock<std::mutex>& lk, uint64_t timeout_ptr,
                                   Pred pred) {
    if (!timeout_ptr) {
        cv.wait(lk, pred);
        return true;
    }
    const auto deadline = Clock::now() + std::chrono::microseconds(Load<uint32_t>(timeout_ptr));
    const bool ok = cv.wait_until(lk, deadline, pred);
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count();
    Store<uint32_t>(timeout_ptr, ok && left > 0 ? uint32_t(left) : 0);
    return ok;
}

struct Sema {
    std::mutex m;
    std::condition_variable cv;
    int32_t count = 0, max = 0;
    char name[32] = {};
};

struct EventFlag {
    std::mutex m;
    std::condition_variable cv;
    uint64_t bits = 0;
    char name[32] = {};
};

// wait modes
constexpr uint32_t kEvfAnd = 1, kEvfOr = 2, kEvfClearAll = 0x10, kEvfClearPat = 0x20;

bool EvfSatisfied(uint64_t bits, uint64_t pattern, uint32_t mode) {
    return (mode & kEvfAnd) ? (bits & pattern) == pattern : (bits & pattern) != 0;
}
void EvfConsume(EventFlag* f, uint64_t pattern, uint32_t mode) {
    if (mode & kEvfClearAll) f->bits = 0;
    else if (mode & kEvfClearPat) f->bits &= ~pattern;
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Event queue (shared with VideoOut/GNM later through equeue.h)
void EventQueue::Add(uint64_t ident, int16_t filter, uint64_t udata, bool edge_triggered) {
    std::lock_guard<std::mutex> lk(m);
    for (auto& r : regs)
        if (r.ident == ident && r.filter == filter) { r.udata = udata; return; }
    regs.push_back({ident, filter, udata, edge_triggered, false, 0});
}

bool EventQueue::Remove(uint64_t ident, int16_t filter) {
    std::lock_guard<std::mutex> lk(m);
    for (size_t i = 0; i < regs.size(); ++i)
        if (regs[i].ident == ident && regs[i].filter == filter) {
            regs.erase(regs.begin() + long(i));
            return true;
        }
    return false;
}

bool EventQueue::Trigger(uint64_t ident, int16_t filter, int64_t data, uint64_t udata_override, bool use_override) {
    std::lock_guard<std::mutex> lk(m);
    for (auto& r : regs) {
        if (r.ident != ident || r.filter != filter) continue;
        r.triggered = true;
        r.data = data;
        if (use_override) r.udata = udata_override;
        cv.notify_all();
        return true;
    }
    return false;
}

int EventQueue::Wait(uint64_t events, int max_events, uint64_t out_count, uint64_t timeout_ptr) {
    std::unique_lock<std::mutex> lk(m);
    auto ready = [&] {
        for (auto& r : regs)
            if (r.triggered) return true;
        return false;
    };
    const bool ok = WaitFor(cv, lk, timeout_ptr, ready);
    int n = 0;
    for (auto& r : regs) {
        if (!r.triggered || n >= max_events) continue;
        // struct kevent { u64 ident; i16 filter; u16 flags; u32 fflags; i64 data; u64 udata; }
        uint8_t ev[32] = {};
        std::memcpy(ev, &r.ident, 8);
        std::memcpy(ev + 8, &r.filter, 2);
        std::memcpy(ev + 16, &r.data, 8);
        std::memcpy(ev + 24, &r.udata, 8);
        std::memcpy(Ptr<void>(events + uint64_t(n) * 32), ev, 32);
        if (r.edge || r.filter == kFilterUser) r.triggered = false;
        ++n;
    }
    Store<int32_t>(out_count, n);
    return ok || n ? 0 : kETIMEDOUT;
}

void RegisterSync(Hle& h) {
    // ---- semaphores: (SceKernelSema* out, const char* name, u32 attr, int init, int max, void* opt)
    SCE("sceKernelCreateSema", {
        auto* s = new Sema;
        s->count = int32_t(a[3]);
        s->max = int32_t(a[4]);
        if (a[1]) std::snprintf(s->name, sizeof s->name, "%s", Ptr<const char>(a[1]));
        Store<uint64_t>(a[0], reinterpret_cast<uint64_t>(s));
        return 0;
    });
    SCE("sceKernelDeleteSema", delete Ptr<Sema>(a[0]); return 0;);
    SCE("sceKernelWaitSema", { // (sema, int need, u32* timeout)
        auto* s = Ptr<Sema>(a[0]);
        if (!s) return kESRCH;
        const int32_t need = int32_t(a[1]);
        std::unique_lock<std::mutex> lk(s->m);
        if (!WaitFor(s->cv, lk, a[2], [&] { return s->count >= need; })) return kETIMEDOUT;
        s->count -= need;
        return 0;
    });
    SCE("sceKernelPollSema", {
        auto* s = Ptr<Sema>(a[0]);
        if (!s) return kESRCH;
        std::lock_guard<std::mutex> lk(s->m);
        if (s->count < int32_t(a[1])) return kEBUSY;
        s->count -= int32_t(a[1]);
        return 0;
    });
    SCE("sceKernelSignalSema", {
        auto* s = Ptr<Sema>(a[0]);
        if (!s) return kESRCH;
        std::lock_guard<std::mutex> lk(s->m);
        s->count += int32_t(a[1]);
        if (s->max > 0 && s->count > s->max) s->count = s->max;
        s->cv.notify_all();
        return 0;
    });
    SCE("sceKernelCancelSema", { // (sema, int set_count, int* waiters)
        auto* s = Ptr<Sema>(a[0]);
        if (!s) return kESRCH;
        std::lock_guard<std::mutex> lk(s->m);
        if (int32_t(a[1]) >= 0) s->count = int32_t(a[1]);
        Store<int32_t>(a[2], 0);
        s->cv.notify_all();
        return 0;
    });

    // ---- event flags: (SceKernelEventFlag* out, const char* name, u32 attr, u64 init, void* opt)
    SCE("sceKernelCreateEventFlag", {
        auto* f = new EventFlag;
        f->bits = a[3];
        if (a[1]) std::snprintf(f->name, sizeof f->name, "%s", Ptr<const char>(a[1]));
        Store<uint64_t>(a[0], reinterpret_cast<uint64_t>(f));
        return 0;
    });
    SCE("sceKernelDeleteEventFlag", delete Ptr<EventFlag>(a[0]); return 0;);
    SCE("sceKernelSetEventFlag", {
        auto* f = Ptr<EventFlag>(a[0]);
        if (!f) return kESRCH;
        std::lock_guard<std::mutex> lk(f->m);
        f->bits |= a[1];
        f->cv.notify_all();
        return 0;
    });
    SCE("sceKernelClearEventFlag", { // bits &= mask
        auto* f = Ptr<EventFlag>(a[0]);
        if (!f) return kESRCH;
        std::lock_guard<std::mutex> lk(f->m);
        f->bits &= a[1];
        return 0;
    });
    SCE("sceKernelWaitEventFlag", { // (flag, u64 pattern, u32 mode, u64* result, u32* timeout)
        auto* f = Ptr<EventFlag>(a[0]);
        if (!f) return kESRCH;
        const uint64_t pattern = a[1];
        const uint32_t mode = uint32_t(a[2]);
        std::unique_lock<std::mutex> lk(f->m);
        const bool ok = WaitFor(f->cv, lk, a[4], [&] { return EvfSatisfied(f->bits, pattern, mode); });
        Store<uint64_t>(a[3], f->bits);
        if (!ok) return kETIMEDOUT;
        EvfConsume(f, pattern, mode);
        return 0;
    });
    SCE("sceKernelPollEventFlag", { // (flag, u64 pattern, u32 mode, u64* result)
        auto* f = Ptr<EventFlag>(a[0]);
        if (!f) return kESRCH;
        std::lock_guard<std::mutex> lk(f->m);
        Store<uint64_t>(a[3], f->bits);
        if (!EvfSatisfied(f->bits, a[1], uint32_t(a[2]))) return kEBUSY;
        EvfConsume(f, a[1], uint32_t(a[2]));
        return 0;
    });

    // ---- event queues
    SCE("sceKernelCreateEqueue", { // (SceKernelEqueue* out, const char* name)
        auto* q = new EventQueue;
        if (a[1]) std::snprintf(q->name, sizeof q->name, "%s", Ptr<const char>(a[1]));
        Store<uint64_t>(a[0], reinterpret_cast<uint64_t>(q));
        return 0;
    });
    SCE("sceKernelDeleteEqueue", delete Ptr<EventQueue>(a[0]); return 0;);
    SCE("sceKernelAddUserEvent", {
        if (!a[0]) return kEBADF;
        Ptr<EventQueue>(a[0])->Add(a[1], kFilterUser, 0, false);
        return 0;
    });
    SCE("sceKernelAddUserEventEdge", {
        if (!a[0]) return kEBADF;
        Ptr<EventQueue>(a[0])->Add(a[1], kFilterUser, 0, true);
        return 0;
    });
    SCE("sceKernelDeleteUserEvent", {
        if (!a[0]) return kEBADF;
        return Ptr<EventQueue>(a[0])->Remove(a[1], kFilterUser) ? 0 : kENOENT;
    });
    SCE("sceKernelTriggerUserEvent", { // (eq, ident, void* udata)
        if (!a[0]) return kEBADF;
        return Ptr<EventQueue>(a[0])->Trigger(a[1], kFilterUser, 0, a[2], true) ? 0 : kENOENT;
    });
    SCE("sceKernelWaitEqueue", { // (eq, SceKernelEvent* ev, int num, int* out, u32* timeout)
        if (!a[0]) return kEBADF;
        return Ptr<EventQueue>(a[0])->Wait(a[1], int(a[2]), a[3], a[4]);
    });
    h.Register("sceKernelGetEventId", [](const uint64_t* a) -> uint64_t { return Load<uint64_t>(a[0]); });
    h.Register("sceKernelGetEventFilter", [](const uint64_t* a) -> uint64_t { return uint64_t(int64_t(Load<int16_t>(a[0] + 8))); });
    h.Register("sceKernelGetEventData", [](const uint64_t* a) -> uint64_t { return Load<uint64_t>(a[0] + 16); });
    h.Register("sceKernelGetEventUserData", [](const uint64_t* a) -> uint64_t { return Load<uint64_t>(a[0] + 24); });
}

} // namespace gr::kernel
