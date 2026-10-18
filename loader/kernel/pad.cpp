// libScePad: DualShock 4 emulation from XInput controllers, the keyboard, and a scripted input
// timeline (for automated boot tests).
//
// Keyboard (only while the game window is focused):
//   arrows = d-pad, WASD = left stick, IJKL = right stick,
//   Space = Cross, Backspace = Circle, F = Square, R = Triangle,
//   Q = L1, E = R1, Z = L2, C = R2, Enter = Options, Tab = touchpad click
#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "guest.h"
#include "kernel.h"

namespace gr::kernel {
namespace {

enum : uint32_t {
    kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80, kL2 = 0x100,
    kR2 = 0x200, kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000, kCross = 0x4000, kSquare = 0x8000,
    kTouchPad = 0x100000,
};

struct ScriptStep {
    double start_s, duration_s;
    uint32_t buttons;
};
std::mutex g_pad_m;
std::vector<ScriptStep> g_script;
LARGE_INTEGER g_t0{};

double SecondsSinceStart() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return double(c.QuadPart - g_t0.QuadPart) / double(f.QuadPart);
}

uint32_t ButtonFromName(const std::string& n) {
    static const std::pair<const char*, uint32_t> kNames[] = {
        {"L3", kL3}, {"R3", kR3}, {"OPTIONS", kOptions}, {"START", kOptions}, {"UP", kUp}, {"RIGHT", kRight},
        {"DOWN", kDown}, {"LEFT", kLeft}, {"L2", kL2}, {"R2", kR2}, {"L1", kL1}, {"R1", kR1},
        {"TRIANGLE", kTriangle}, {"CIRCLE", kCircle}, {"CROSS", kCross}, {"SQUARE", kSquare}, {"TOUCHPAD", kTouchPad}};
    for (const auto& [name, bit] : kNames)
        if (n == name) return bit;
    return 0;
}

bool OurWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

struct PadState {
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
};

void ApplyStick(int16_t x, int16_t y, uint8_t* ox, uint8_t* oy) {
    const int dead = 7849;
    auto conv = [&](int v, bool invert) {
        if (v > -dead && v < dead) return uint8_t(128);
        int s = invert ? -v : v;
        int r = 128 + s / 256;
        return uint8_t(r < 0 ? 0 : r > 255 ? 255 : r);
    };
    *ox = conv(x, false);
    *oy = conv(y, true); // XInput +Y is up, DS4 0 is up
}

PadState Poll() {
    PadState s;
    // XInput (first connected controller)
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        XINPUT_STATE xs{};
        if (XInputGetState(i, &xs) != ERROR_SUCCESS) continue;
        const WORD b = xs.Gamepad.wButtons;
        if (b & XINPUT_GAMEPAD_DPAD_UP) s.buttons |= kUp;
        if (b & XINPUT_GAMEPAD_DPAD_DOWN) s.buttons |= kDown;
        if (b & XINPUT_GAMEPAD_DPAD_LEFT) s.buttons |= kLeft;
        if (b & XINPUT_GAMEPAD_DPAD_RIGHT) s.buttons |= kRight;
        if (b & XINPUT_GAMEPAD_START) s.buttons |= kOptions;
        if (b & XINPUT_GAMEPAD_BACK) s.buttons |= kTouchPad;
        if (b & XINPUT_GAMEPAD_LEFT_THUMB) s.buttons |= kL3;
        if (b & XINPUT_GAMEPAD_RIGHT_THUMB) s.buttons |= kR3;
        if (b & XINPUT_GAMEPAD_LEFT_SHOULDER) s.buttons |= kL1;
        if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) s.buttons |= kR1;
        if (b & XINPUT_GAMEPAD_A) s.buttons |= kCross;
        if (b & XINPUT_GAMEPAD_B) s.buttons |= kCircle;
        if (b & XINPUT_GAMEPAD_X) s.buttons |= kSquare;
        if (b & XINPUT_GAMEPAD_Y) s.buttons |= kTriangle;
        s.l2 = xs.Gamepad.bLeftTrigger;
        s.r2 = xs.Gamepad.bRightTrigger;
        if (s.l2 > 30) s.buttons |= kL2;
        if (s.r2 > 30) s.buttons |= kR2;
        ApplyStick(xs.Gamepad.sThumbLX, xs.Gamepad.sThumbLY, &s.lx, &s.ly);
        ApplyStick(xs.Gamepad.sThumbRX, xs.Gamepad.sThumbRY, &s.rx, &s.ry);
        break;
    }
    // Keyboard
    if (OurWindowFocused()) {
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        const std::pair<int, uint32_t> keys[] = {
            {VK_UP, kUp}, {VK_DOWN, kDown}, {VK_LEFT, kLeft}, {VK_RIGHT, kRight}, {VK_SPACE, kCross},
            {VK_BACK, kCircle}, {'F', kSquare}, {'R', kTriangle}, {'Q', kL1}, {'E', kR1}, {'Z', kL2}, {'C', kR2},
            {VK_RETURN, kOptions}, {VK_TAB, kTouchPad}};
        for (const auto& [vk, bit] : keys)
            if (down(vk)) s.buttons |= bit;
        if (down('Z')) s.l2 = 255;
        if (down('C')) s.r2 = 255;
        if (down('A')) s.lx = 0;
        if (down('D')) s.lx = 255;
        if (down('W')) s.ly = 0;
        if (down('S')) s.ly = 255;
        if (down('J')) s.rx = 0;
        if (down('L')) s.rx = 255;
        if (down('I')) s.ry = 0;
        if (down('K')) s.ry = 255;
    }
    // Scripted input
    {
        std::lock_guard<std::mutex> lk(g_pad_m);
        const double t = SecondsSinceStart();
        for (const auto& st : g_script)
            if (t >= st.start_s && t < st.start_s + st.duration_s) s.buttons |= st.buttons;
    }
    return s;
}

// ScePadData (120 bytes)
void FillPadData(uint64_t dst) {
    const PadState s = Poll();
    uint8_t d[120] = {};
    std::memcpy(d + 0, &s.buttons, 4);
    d[4] = s.lx; d[5] = s.ly; d[6] = s.rx; d[7] = s.ry; d[8] = s.l2; d[9] = s.r2;
    const float quat_w = 1.0f, accel_y = 1.0f; // identity orientation, 1 g of gravity
    std::memcpy(d + 24, &quat_w, 4);
    std::memcpy(d + 32, &accel_y, 4);
    d[76] = 1; // connected
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t ts = uint64_t(c.QuadPart);
    std::memcpy(d + 80, &ts, 8);
    d[104] = 1; // connectedCount
    std::memcpy(Ptr<void>(dst), d, sizeof d);
}

} // namespace

// "5:OPTIONS,8.5:CROSS:0.3" -> press OPTIONS at 5 s, CROSS at 8.5 s for 0.3 s (default 0.2 s).
void SetPadScript(const std::string& script) {
    std::lock_guard<std::mutex> lk(g_pad_m);
    size_t pos = 0;
    while (pos < script.size()) {
        const size_t end = std::min(script.find(',', pos), script.size());
        const std::string item = script.substr(pos, end - pos);
        const size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
        if (c1 != std::string::npos) {
            ScriptStep st{std::atof(item.substr(0, c1).c_str()), 0.2,
                          ButtonFromName(item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1))};
            if (c2 != std::string::npos) st.duration_s = std::atof(item.substr(c2 + 1).c_str());
            if (st.buttons) g_script.push_back(st);
        }
        pos = end + 1;
    }
    std::printf("pad script: %zu steps\n", g_script.size());
}

void RegisterPad(Hle& h) {
    QueryPerformanceCounter(&g_t0);
    h.Register("scePadInit", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("scePadOpen", [](const uint64_t*) -> uint64_t { return 1; });
    h.Register("scePadClose", [](const uint64_t*) -> uint64_t { return 0; });
    h.Register("scePadGetHandle", [](const uint64_t*) -> uint64_t { return 1; });
    // int scePadRead(handle, ScePadData* data, int num): returns entries written
    h.Register("scePadRead", [](const uint64_t* a) -> uint64_t {
        if (!a[1] || int(a[2]) < 1) return 0x80920001;
        FillPadData(a[1]);
        return 1;
    });
    h.Register("scePadReadState", [](const uint64_t* a) -> uint64_t {
        FillPadData(a[1]);
        return 0;
    });
    // ScePadControllerInformation
    h.Register("scePadGetControllerInformation", [](const uint64_t* a) -> uint64_t {
        uint8_t info[28] = {};
        const float density = 44.86f;
        const uint16_t rx = 1920, ry = 943;
        std::memcpy(info, &density, 4);
        std::memcpy(info + 4, &rx, 2);
        std::memcpy(info + 6, &ry, 2);
        info[8] = 13;  // dead zone left
        info[9] = 13;  // dead zone right
        info[10] = 0;  // connection type: local
        info[11] = 1;  // connected count
        info[12] = 1;  // connected
        std::memcpy(Ptr<void>(a[1]), info, sizeof info);
        return 0;
    });
    for (const char* n : {"scePadSetVibration", "scePadSetLightBar", "scePadResetLightBar", "scePadResetOrientation",
                          "scePadSetTiltCorrectionState", "scePadSetMotionSensorState",
                          "scePadSetAngularVelocityDeadbandState"})
        h.Register(n, [](const uint64_t*) -> uint64_t { return 0; });
}

} // namespace gr::kernel
