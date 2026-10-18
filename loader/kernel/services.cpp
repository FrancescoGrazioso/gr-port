// System services the boot path depends on: PlayGo (everything installed locally), DiscMap,
// SystemService/UserService parameters and ScreenShot.
#include "guest.h"
#include "kernel.h"

namespace gr::kernel {
namespace {

constexpr uint64_t kDiscMapErrorNoBitmapInfo = 0x81100004;
constexpr uint8_t kPlayGoLocusLocalFast = 3;
constexpr uint32_t kPlayGoInstallSpeedFull = 2;

// SCE_SYSTEM_SERVICE_PARAM_ID_*
int32_t SystemParam(uint32_t id) {
    switch (id) {
    case 1: return 1;    // LANG: English (US)
    case 2: return 1;    // DATE_FORMAT: DD/MM/YYYY
    case 3: return 1;    // TIME_FORMAT: 24h
    case 4: return 60;   // TIME_ZONE: UTC+1 (minutes)
    case 5: return 0;    // SUMMERTIME
    case 7: return 0;    // GAME_PARENTAL_LEVEL: off
    case 1000: return 1; // ENTER_BUTTON_ASSIGN: cross
    default: return 0;
    }
}

} // namespace

void RegisterServices(Hle& h) {
    // ---- PlayGo: the whole game is installed, every chunk is local
    h.Register("scePlayGoInitialize", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("scePlayGoTerminate", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("scePlayGoOpen", [](const uint64_t* a) -> uint64_t { Store<int32_t>(a[0], 1); return 0; });
    h.Register("scePlayGoClose", [](const uint64_t*) -> uint64_t { return 0; });
    // (handle, const u16* chunkIds, u32 n, u8* outLoci)
    h.Register("scePlayGoGetLocus", [](const uint64_t* a) -> uint64_t {
        for (uint64_t i = 0; i < a[2]; ++i) Store<uint8_t>(a[3] + i, kPlayGoLocusLocalFast);
        return 0;
    });
    // (handle, ScePlayGoToDo* out, u32 n, u32* outEntries)
    h.Register("scePlayGoGetToDoList", [](const uint64_t* a) -> uint64_t { Store<uint32_t>(a[3], 0); return 0; });
    h.Register("scePlayGoSetToDoList", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("scePlayGoGetInstallSpeed", [](const uint64_t* a) -> uint64_t {
        Store<uint32_t>(a[1], kPlayGoInstallSpeedFull);
        return 0;
    });
    h.Register("scePlayGoSetInstallSpeed", [](const uint64_t*) -> uint64_t { return 0; });
    // (handle, ids, n, ScePlayGoEta* out)  eta 0 = done
    h.Register("scePlayGoGetEta", [](const uint64_t* a) -> uint64_t { Store<int64_t>(a[3], 0); return 0; });
    // (handle, ids, n, ScePlayGoProgress* {u64 progressSize; u64 totalSize})
    h.Register("scePlayGoGetProgress", [](const uint64_t* a) -> uint64_t {
        Store<uint64_t>(a[3], 1);
        Store<uint64_t>(a[3] + 8, 1);
        return 0;
    });
    h.Register("scePlayGoGetLanguageMask", [](const uint64_t* a) -> uint64_t { Store<uint64_t>(a[1], ~0ull); return 0; });
    h.Register("scePlayGoSetLanguageMask", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("scePlayGoPrefetch", [](const uint64_t*) -> uint64_t { return 0; });

    // ---- DiscMap (used by libSceFios2): no disc bitmap, every request is served from the HDD
    h.Register("sceDiscMapIsRequestOnHDD", [](const uint64_t*) -> uint64_t { return kDiscMapErrorNoBitmapInfo; });
    h.Register("sceDiscMapGetPackageSize", [](const uint64_t*) -> uint64_t { return kDiscMapErrorNoBitmapInfo; });
    // Func_8A828CAEE7EDD5E9(const char* path, i64 offset, i64 nbytes, int* flags, int* ret1, int* ret2)
    h.RegisterNid("ioKMruft1ek", [](const uint64_t* a) -> uint64_t {
        Store<int32_t>(a[3], 0);
        Store<int32_t>(a[4], 0);
        Store<int32_t>(a[5], 0);
        return 0;
    });

    // ---- SystemService / UserService
    h.Register("sceSystemServiceParamGetInt", [](const uint64_t* a) -> uint64_t {
        Store<int32_t>(a[1], SystemParam(uint32_t(a[0])));
        return 0;
    });
    h.Register("sceSystemServiceParamGetString", [](const uint64_t* a) -> uint64_t {
        if (a[1] && a[2]) Ptr<char>(a[1])[0] = 0;
        return 0;
    });
    // SceSystemServiceStatus { i32 eventNum; bool isSystemUiOverlaid, isInBackgroundExecution, isCpuMode7CpuNormal,
    //                          isGameLiveStreamingOnAir, isOutOfVrPlayArea; u8 reserved[] }
    h.Register("sceSystemServiceGetStatus", [](const uint64_t* a) -> uint64_t {
        std::memset(Ptr<void>(a[0]), 0, 0x88);
        return 0;
    });
    h.Register("sceSystemServiceReceiveEvent", [](const uint64_t*) -> uint64_t { return 0x80A10003; }); // no event
    h.Register("sceSystemServiceHideSplashScreen", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceSystemServiceDisableMusicPlayer", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceSystemServiceReenableMusicPlayer", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceSystemServicePowerTick", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceUserServiceInitialize", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceUserServiceTerminate", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("sceUserServiceGetInitialUser", [](const uint64_t* a) -> uint64_t { Store<int32_t>(a[0], 1); return 0; });
    // (userId, char* name, size_t size)
    h.Register("sceUserServiceGetUserName", [](const uint64_t* a) -> uint64_t {
        if (a[1] && a[2]) std::snprintf(Ptr<char>(a[1]), a[2], "%s", "Kat");
        return 0;
    });
    // SceUserServiceLoginUserIdList { i32 userId[4]; }  (-1 = invalid)
    h.Register("sceUserServiceGetLoginUserIdList", [](const uint64_t* a) -> uint64_t {
        const int32_t ids[4] = {1, -1, -1, -1};
        std::memcpy(Ptr<void>(a[0]), ids, sizeof ids);
        return 0;
    });
    h.Register("sceUserServiceGetEvent", [](const uint64_t*) -> uint64_t { return 0x80960007; }); // no event

    // ---- ScreenShot
    for (const char* n : {"sceScreenShotEnable", "sceScreenShotDisable", "sceScreenShotSetParam",
                          "sceScreenShotSetOverlayImage", "sceScreenShotSetOverlayImageWithOrigin"})
        h.Register(n, [](const uint64_t*) -> uint64_t { return 0; });
}

} // namespace gr::kernel
