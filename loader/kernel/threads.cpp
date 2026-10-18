// libkernel threads and synchronisation: scePthread* and the POSIX pthread_* aliases.
// Orbis pthread handles (thread, attr, mutex, cond, rwlock) are pointers to opaque objects, so
// each one maps to a heap-allocated host object. Static initialisers are small integers (0, 1, 2...)
// and are replaced by a real object on first use.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "guest.h"
#include "kernel.h"

namespace gr::kernel {
namespace {

constexpr size_t kMinGuestStack = 2u << 20; // PS4 default is 64 KiB; be generous on PC

struct Thread {
    HANDLE handle = nullptr;
    DWORD tid = 0;
    uint64_t entry = 0, arg = 0, ret = 0;
    char name[32] = {};
    int prio = 700;
    uint64_t affinity = 0x7F;
    bool detached = false;
};

struct ThreadAttr {
    uint64_t stack_size = 0x10000;
    uint64_t stack_addr = 0;
    int detach = 0;
    int prio = 700;
    int policy = 1;
    int inherit = 4;
    uint64_t affinity = 0x7F;
    uint64_t guard = 0x1000;
};

struct Mutex {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<DWORD> owner{0}; // thread id of the holder, 0 = unlocked
    int count = 0;               // recursion depth
    int type = 1;
};
struct MutexAttr {
    int type = 1; // ERRORCHECK
    int protocol = 0;
};
struct Cond {
    CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
};
struct RwLock {
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<DWORD> writer{0};
};

thread_local Thread* t_self = nullptr;
thread_local std::unordered_map<uint32_t, uint64_t> t_specific;
std::atomic<uint32_t> g_next_key{1};
std::mutex g_lazy_init;
std::recursive_mutex g_once;

Thread* Self() {
    if (!t_self) { // thread not created through scePthreadCreate (e.g. the main thread)
        t_self = new Thread;
        t_self->tid = GetCurrentThreadId();
        std::snprintf(t_self->name, sizeof t_self->name, "host-%lu", t_self->tid);
    }
    return t_self;
}

bool IsStaticInit(uint64_t v) { return v < 0x10000; }

template <class T> T* Lazy(uint64_t handle_ptr) {
    if (!handle_ptr) return nullptr;
    uint64_t v = Load<uint64_t>(handle_ptr);
    if (!IsStaticInit(v)) return Ptr<T>(v);
    std::lock_guard<std::mutex> lk(g_lazy_init);
    v = Load<uint64_t>(handle_ptr);
    if (IsStaticInit(v)) {
        v = reinterpret_cast<uint64_t>(new T);
        Store<uint64_t>(handle_ptr, v);
    }
    return Ptr<T>(v);
}

// Each operation returns a plain errno; the registration layer converts it for sce*/posix names.
int ThreadCreate(uint64_t out, uint64_t attr_pp, uint64_t entry, uint64_t arg, uint64_t name) {
    const ThreadAttr def;
    const ThreadAttr* a = attr_pp && Load<uint64_t>(attr_pp) ? Ptr<ThreadAttr>(Load<uint64_t>(attr_pp)) : &def;
    auto* t = new Thread;
    t->entry = entry;
    t->arg = arg;
    t->prio = a->prio;
    t->detached = a->detach != 0;
    if (name) std::snprintf(t->name, sizeof t->name, "%s", Ptr<const char>(name));
    const size_t stack = a->stack_size > kMinGuestStack ? size_t(a->stack_size) : kMinGuestStack;
    t->handle = StartGuestThread(
        [t] {
            t_self = t;
            t->ret = CallGuest(t->entry, {t->arg});
        },
        stack, reinterpret_cast<uint32_t*>(&t->tid), t->name);
    if (!t->handle) { delete t; return kEAGAIN; }
    Store<uint64_t>(out, reinterpret_cast<uint64_t>(t));
    TraceNote("thread created: '%s' entry=0x%llx tid=%lu", t->name, (unsigned long long)entry, t->tid);
    return 0;
}

int ThreadJoin(uint64_t thread, uint64_t ret_out) {
    auto* t = Ptr<Thread>(thread);
    if (!t || !t->handle) return kESRCH;
    WaitForSingleObject(t->handle, INFINITE);
    Store<uint64_t>(ret_out, t->ret);
    return 0;
}

// Mutex types: 1 ERRORCHECK (default), 2 RECURSIVE, 3 NORMAL, 4 ADAPTIVE (behaves like NORMAL).
constexpr int kMutexErrorcheck = 1, kMutexRecursive = 2;

int MutexInit(uint64_t m_pp, uint64_t attr_pp) {
    auto* m = new Mutex;
    if (attr_pp && Load<uint64_t>(attr_pp)) m->type = Ptr<MutexAttr>(Load<uint64_t>(attr_pp))->type;
    Store<uint64_t>(m_pp, reinterpret_cast<uint64_t>(m));
    return 0;
}
std::atomic<int> g_mutex_anomalies{0};

int MutexAcquire(Mutex* m, bool try_only) {
    const DWORD me = GetCurrentThreadId();
    if (m->owner == me) {
        if (m->type == kMutexRecursive) { m->count++; return 0; }
        if (!try_only && g_mutex_anomalies++ < 8)
            TraceNote("mutex: thread %lu re-locked a non-recursive mutex %p (type %d) -> EDEADLK", me, (void*)m, m->type);
        if (m->type == kMutexErrorcheck || try_only) return try_only ? kEBUSY : kEDEADLK;
    }
    if (try_only) {
        if (!TryAcquireSRWLockExclusive(&m->lock)) return kEBUSY;
    } else {
        AcquireSRWLockExclusive(&m->lock);
    }
    m->owner = me;
    m->count = 1;
    return 0;
}
int MutexLock(uint64_t m_pp) {
    auto* m = Lazy<Mutex>(m_pp);
    return m ? MutexAcquire(m, false) : kEINVAL;
}
int MutexTryLock(uint64_t m_pp) {
    auto* m = Lazy<Mutex>(m_pp);
    return m ? MutexAcquire(m, true) : kEINVAL;
}
int MutexUnlock(uint64_t m_pp) {
    auto* m = Lazy<Mutex>(m_pp);
    if (!m) return kEINVAL;
    if (m->owner != GetCurrentThreadId()) {
        if (g_mutex_anomalies++ < 8)
            TraceNote("mutex: thread %lu unlocked mutex %p owned by %lu (type %d)", GetCurrentThreadId(), (void*)m,
                      DWORD(m->owner), m->type);
        if (m->type == kMutexErrorcheck || m->type == kMutexRecursive || m->owner == 0) return kEPERM;
        // NORMAL/ADAPTIVE unlocked by another thread: undefined in POSIX but tolerated on PS4.
    }
    if (m->type == kMutexRecursive && --m->count > 0) return 0;
    m->owner = 0;
    m->count = 0;
    ReleaseSRWLockExclusive(&m->lock);
    return 0;
}
int MutexDestroy(uint64_t m_pp) {
    const uint64_t v = Load<uint64_t>(m_pp);
    if (!IsStaticInit(v)) delete Ptr<Mutex>(v);
    Store<uint64_t>(m_pp, 0);
    return 0;
}

int CondWait(uint64_t c_pp, uint64_t m_pp, DWORD ms) {
    auto* c = Lazy<Cond>(c_pp);
    auto* m = Lazy<Mutex>(m_pp);
    if (!c || !m) return kEINVAL;
    if (m->owner != GetCurrentThreadId()) return kEPERM;
    // The wait releases the mutex completely (even a recursively held one) and restores it after.
    const int saved = m->count;
    m->owner = 0;
    m->count = 0;
    const BOOL ok = SleepConditionVariableSRW(&c->cv, &m->lock, ms, 0);
    const DWORD err = ok ? 0 : GetLastError();
    m->owner = GetCurrentThreadId();
    m->count = saved;
    if (!ok) return err == ERROR_TIMEOUT ? kETIMEDOUT : kEINVAL;
    return 0;
}

DWORD AbsTimespecToMs(uint64_t ts) {
    // pthread_cond_timedwait uses CLOCK_REALTIME absolute time.
    const int64_t sec = Load<int64_t>(ts), nsec = Load<int64_t>(ts + 8);
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t now100ns = ((int64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) - 116444736000000000LL;
    const int64_t target100ns = sec * 10000000LL + nsec / 100;
    const int64_t diff = target100ns - now100ns;
    return diff <= 0 ? 0 : DWORD(diff / 10000);
}

int RwRead(uint64_t pp, bool try_only) {
    auto* l = Lazy<RwLock>(pp);
    if (!l) return kEINVAL;
    if (try_only) return TryAcquireSRWLockShared(&l->lock) ? 0 : kEBUSY;
    AcquireSRWLockShared(&l->lock);
    return 0;
}
int RwWrite(uint64_t pp, bool try_only) {
    auto* l = Lazy<RwLock>(pp);
    if (!l) return kEINVAL;
    if (try_only) {
        if (!TryAcquireSRWLockExclusive(&l->lock)) return kEBUSY;
    } else {
        AcquireSRWLockExclusive(&l->lock);
    }
    l->writer = GetCurrentThreadId();
    return 0;
}
int RwUnlock(uint64_t pp) {
    auto* l = Lazy<RwLock>(pp);
    if (!l) return kEINVAL;
    if (l->writer == GetCurrentThreadId()) {
        l->writer = 0;
        ReleaseSRWLockExclusive(&l->lock);
    } else {
        ReleaseSRWLockShared(&l->lock);
    }
    return 0;
}

int Once(uint64_t once, uint64_t routine) {
    // ScePthreadOnce { int32 state; ScePthreadMutex mutex; }  state 0 = not run yet
    if (Load<int32_t>(once) != 0) return 0;
    std::lock_guard<std::recursive_mutex> lk(g_once);
    if (Load<int32_t>(once) == 0) {
        Store<int32_t>(once, 2); // in progress (recursive calls see it as done)
        CallGuest(routine);
        Store<int32_t>(once, 1);
    }
    return 0;
}

} // namespace

void InitMainThread() {
    Thread* t = Self();
    std::snprintf(t->name, sizeof t->name, "main");
}


void RegisterThreads(Hle& h) {
    // ---- threads
    BOTH("scePthreadCreate", "pthread_create", return ThreadCreate(a[0], a[1], a[2], a[3], a[4]););
    POSIX("pthread_create_name_np", return ThreadCreate(a[0], a[1], a[2], a[3], a[4]););
    BOTH("scePthreadJoin", "pthread_join", return ThreadJoin(a[0], a[1]););
    BOTH("scePthreadDetach", "pthread_detach", {
        if (auto* t = Ptr<Thread>(a[0])) t->detached = true;
        return 0;
    });
    h.Register("scePthreadSelf", [](const uint64_t*) -> uint64_t { return reinterpret_cast<uint64_t>(Self()); });
    h.Register("pthread_self", [](const uint64_t*) -> uint64_t { return reinterpret_cast<uint64_t>(Self()); });
    h.Register("scePthreadEqual", [](const uint64_t* a) -> uint64_t { return a[0] == a[1]; });
    h.Register("pthread_equal", [](const uint64_t* a) -> uint64_t { return a[0] == a[1]; });
    auto exit_fn = [](const uint64_t* a) -> uint64_t {
        Self()->ret = a[0];
        TraceNote("thread '%s' exited", Self()->name);
        ExitThread(0);
    };
    h.Register("scePthreadExit", exit_fn);
    h.Register("pthread_exit", exit_fn);
    h.Register("scePthreadGetthreadid", [](const uint64_t*) -> uint64_t { return Self()->tid; });
    h.Register("scePthreadYield", [](const uint64_t*) -> uint64_t { SwitchToThread(); return 0; });
    h.Register("pthread_yield", [](const uint64_t*) -> uint64_t { SwitchToThread(); return 0; });
    h.Register("sched_yield", [](const uint64_t*) -> uint64_t { SwitchToThread(); return 0; });
    SCE("scePthreadGetprio", {
        auto* t = Ptr<Thread>(a[0]);
        if (!t) return kESRCH;
        Store<int32_t>(a[1], t->prio);
        return 0;
    });
    SCE("scePthreadSetprio", {
        if (auto* t = Ptr<Thread>(a[0])) t->prio = int(a[1]);
        return 0;
    });
    SCE("scePthreadGetaffinity", {
        auto* t = Ptr<Thread>(a[0]);
        Store<uint64_t>(a[1], t ? t->affinity : 0x7F);
        return 0;
    });
    SCE("scePthreadSetaffinity", {
        if (auto* t = Ptr<Thread>(a[0])) t->affinity = a[1];
        return 0;
    });
    SCE("scePthreadRename", {
        if (auto* t = Ptr<Thread>(a[0])) std::snprintf(t->name, sizeof t->name, "%s", Ptr<const char>(a[1]));
        return 0;
    });
    SCE("scePthreadGetname", {
        auto* t = Ptr<Thread>(a[0]);
        if (!t || !a[1]) return kEINVAL;
        std::memcpy(Ptr<char>(a[1]), t->name, sizeof t->name);
        return 0;
    });
    SCE("scePthreadSetschedparam", return 0;);
    SCE("scePthreadGetschedparam", {
        Store<int32_t>(a[1], 1);
        Store<int32_t>(a[2], Ptr<Thread>(a[0]) ? Ptr<Thread>(a[0])->prio : 700);
        return 0;
    });

    // ---- thread attributes
    BOTH("scePthreadAttrInit", "pthread_attr_init", {
        Store<uint64_t>(a[0], reinterpret_cast<uint64_t>(new ThreadAttr));
        return 0;
    });
    BOTH("scePthreadAttrDestroy", "pthread_attr_destroy", {
        delete Ptr<ThreadAttr>(Load<uint64_t>(a[0]));
        Store<uint64_t>(a[0], 0);
        return 0;
    });
#define ATTR_SET(sce_name, posix_name, field)                                                   \
    BOTH(sce_name, posix_name, {                                                                \
        auto* at = Ptr<ThreadAttr>(Load<uint64_t>(a[0]));                                       \
        if (!at) return kEINVAL;                                                                \
        at->field = decltype(at->field)(a[1]);                                                  \
        return 0;                                                                               \
    })
    ATTR_SET("scePthreadAttrSetstacksize", "pthread_attr_setstacksize", stack_size);
    ATTR_SET("scePthreadAttrSetdetachstate", "pthread_attr_setdetachstate", detach);
    ATTR_SET("scePthreadAttrSetinheritsched", "pthread_attr_setinheritsched", inherit);
    ATTR_SET("scePthreadAttrSetschedpolicy", "pthread_attr_setschedpolicy", policy);
    ATTR_SET("scePthreadAttrSetaffinity", "pthread_attr_setaffinity", affinity);
    ATTR_SET("scePthreadAttrSetguardsize", "pthread_attr_setguardsize", guard);
    ATTR_SET("scePthreadAttrSetstackaddr", "pthread_attr_setstackaddr", stack_addr);
    BOTH("scePthreadAttrSetschedparam", "pthread_attr_setschedparam", {
        auto* at = Ptr<ThreadAttr>(Load<uint64_t>(a[0]));
        if (!at) return kEINVAL;
        at->prio = Load<int32_t>(a[1]);
        return 0;
    });
#define ATTR_GET(sce_name, posix_name, field, type)                                             \
    BOTH(sce_name, posix_name, {                                                                \
        auto* at = Ptr<ThreadAttr>(Load<uint64_t>(a[0]));                                       \
        if (!at) return kEINVAL;                                                                \
        Store<type>(a[1], type(at->field));                                                     \
        return 0;                                                                               \
    })
    ATTR_GET("scePthreadAttrGetstacksize", "pthread_attr_getstacksize", stack_size, uint64_t);
    ATTR_GET("scePthreadAttrGetdetachstate", "pthread_attr_getdetachstate", detach, int32_t);
    ATTR_GET("scePthreadAttrGetaffinity", "pthread_attr_getaffinity", affinity, uint64_t);
    ATTR_GET("scePthreadAttrGetstackaddr", "pthread_attr_getstackaddr", stack_addr, uint64_t);
    SCE("scePthreadAttrGetstack", {
        auto* at = Ptr<ThreadAttr>(Load<uint64_t>(a[0]));
        if (!at) return kEINVAL;
        Store<uint64_t>(a[1], at->stack_addr);
        Store<uint64_t>(a[2], at->stack_size);
        return 0;
    });
    SCE("scePthreadAttrGet", { // fill an attr object describing an existing thread
        auto* t = Ptr<Thread>(a[0]);
        auto* at = Ptr<ThreadAttr>(Load<uint64_t>(a[1]));
        if (!t || !at) return kEINVAL;
        at->prio = t->prio;
        at->affinity = t->affinity;
        at->detach = t->detached;
        return 0;
    });

    // ---- mutexes
    BOTH("scePthreadMutexInit", "pthread_mutex_init", return MutexInit(a[0], a[1]););
    BOTH("scePthreadMutexDestroy", "pthread_mutex_destroy", return MutexDestroy(a[0]););
    BOTH("scePthreadMutexLock", "pthread_mutex_lock", return MutexLock(a[0]););
    BOTH("scePthreadMutexTrylock", "pthread_mutex_trylock", return MutexTryLock(a[0]););
    BOTH("scePthreadMutexUnlock", "pthread_mutex_unlock", return MutexUnlock(a[0]););
    SCE("scePthreadMutexTimedlock", { // usec timeout; approximated with polling
        for (uint64_t waited = 0;; waited += 1000) {
            if (MutexTryLock(a[0]) == 0) return 0;
            if (waited >= a[1]) return kETIMEDOUT;
            Sleep(1);
        }
    });
    BOTH("scePthreadMutexattrInit", "pthread_mutexattr_init", {
        Store<uint64_t>(a[0], reinterpret_cast<uint64_t>(new MutexAttr));
        return 0;
    });
    BOTH("scePthreadMutexattrDestroy", "pthread_mutexattr_destroy", {
        delete Ptr<MutexAttr>(Load<uint64_t>(a[0]));
        Store<uint64_t>(a[0], 0);
        return 0;
    });
    BOTH("scePthreadMutexattrSettype", "pthread_mutexattr_settype", {
        if (auto* at = Ptr<MutexAttr>(Load<uint64_t>(a[0]))) at->type = int(a[1]);
        return 0;
    });
    BOTH("scePthreadMutexattrSetprotocol", "pthread_mutexattr_setprotocol", {
        if (auto* at = Ptr<MutexAttr>(Load<uint64_t>(a[0]))) at->protocol = int(a[1]);
        return 0;
    });

    // ---- condition variables
    BOTH("scePthreadCondInit", "pthread_cond_init", {
        Store<uint64_t>(a[0], reinterpret_cast<uint64_t>(new Cond));
        return 0;
    });
    BOTH("scePthreadCondDestroy", "pthread_cond_destroy", {
        const uint64_t v = Load<uint64_t>(a[0]);
        if (!IsStaticInit(v)) delete Ptr<Cond>(v);
        Store<uint64_t>(a[0], 0);
        return 0;
    });
    BOTH("scePthreadCondWait", "pthread_cond_wait", return CondWait(a[0], a[1], INFINITE););
    SCE("scePthreadCondTimedwait", return CondWait(a[0], a[1], DWORD(a[2] / 1000));); // relative usec
    POSIX("pthread_cond_timedwait", return CondWait(a[0], a[1], AbsTimespecToMs(a[2])););
    BOTH("scePthreadCondSignal", "pthread_cond_signal", {
        if (auto* c = Lazy<Cond>(a[0])) WakeConditionVariable(&c->cv);
        return 0;
    });
    BOTH("scePthreadCondBroadcast", "pthread_cond_broadcast", {
        if (auto* c = Lazy<Cond>(a[0])) WakeAllConditionVariable(&c->cv);
        return 0;
    });
    BOTH("scePthreadCondattrInit", "pthread_condattr_init", {
        Store<uint64_t>(a[0], 1);
        return 0;
    });
    BOTH("scePthreadCondattrDestroy", "pthread_condattr_destroy", return 0;);

    // ---- rwlocks
    BOTH("scePthreadRwlockInit", "pthread_rwlock_init", {
        Store<uint64_t>(a[0], reinterpret_cast<uint64_t>(new RwLock));
        return 0;
    });
    BOTH("scePthreadRwlockDestroy", "pthread_rwlock_destroy", {
        const uint64_t v = Load<uint64_t>(a[0]);
        if (!IsStaticInit(v)) delete Ptr<RwLock>(v);
        Store<uint64_t>(a[0], 0);
        return 0;
    });
    BOTH("scePthreadRwlockRdlock", "pthread_rwlock_rdlock", return RwRead(a[0], false););
    BOTH("scePthreadRwlockTryrdlock", "pthread_rwlock_tryrdlock", return RwRead(a[0], true););
    BOTH("scePthreadRwlockWrlock", "pthread_rwlock_wrlock", return RwWrite(a[0], false););
    BOTH("scePthreadRwlockTrywrlock", "pthread_rwlock_trywrlock", return RwWrite(a[0], true););
    BOTH("scePthreadRwlockUnlock", "pthread_rwlock_unlock", return RwUnlock(a[0]););

    // ---- thread-specific data / once
    BOTH("scePthreadKeyCreate", "pthread_key_create", {
        Store<uint32_t>(a[0], g_next_key.fetch_add(1)); // destructors not run yet (phase 2)
        return 0;
    });
    BOTH("scePthreadKeyDelete", "pthread_key_delete", return 0;);
    h.Register("scePthreadGetspecific", [](const uint64_t* a) -> uint64_t {
        auto it = t_specific.find(uint32_t(a[0]));
        return it == t_specific.end() ? 0 : it->second;
    });
    h.Register("pthread_getspecific", [](const uint64_t* a) -> uint64_t {
        auto it = t_specific.find(uint32_t(a[0]));
        return it == t_specific.end() ? 0 : it->second;
    });
    BOTH("scePthreadSetspecific", "pthread_setspecific", {
        t_specific[uint32_t(a[0])] = a[1];
        return 0;
    });
    BOTH("scePthreadOnce", "pthread_once", return Once(a[0], a[1]););
}

} // namespace gr::kernel
