// pad.cpp
//   ViGEm's client library, loaded by hand at runtime.
//
//   Why not link it: the proxy has to be a drop-in opengl32.dll with no redistributable and no
//   load-time dependencies. A missing ViGEmClient.dll must degrade to "the rider does nothing",
//   not to "the game won't start", and LoadLibrary gives exactly that.
//
//   The ABI below is ViGEmClient's C API (MIT, https://github.com/nefarius/ViGEmClient). The
//   XUSB_REPORT layout and button bits are the XUSB ones the driver expects; they are repeated
//   here so this file needs no third-party headers, and they must not be reordered.

#include <windows.h>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "log.h"
#include "pad.h"

namespace mxrider {
namespace pad {
namespace {

// ---- ViGEmClient ABI ------------------------------------------------------
struct XUSB_REPORT {
    unsigned short wButtons;
    unsigned char  bLeftTrigger;
    unsigned char  bRightTrigger;
    short          sThumbLX;
    short          sThumbLY;
    short          sThumbRX;
    short          sThumbRY;
};

// Button bits as the XUSB driver reports them (same values as XINPUT_GAMEPAD_*).
constexpr int kButtonDpadUp    = 0x0001;
constexpr int kButtonDpadDown  = 0x0002;
constexpr int kButtonDpadLeft  = 0x0004;
constexpr int kButtonDpadRight = 0x0008;
constexpr int kButtonStart     = 0x0010;
constexpr int kButtonBack      = 0x0020;
constexpr int kButtonLeftThumb = 0x0040;
constexpr int kButtonRightThumb= 0x0080;
constexpr int kButtonLeftShoulder  = 0x0100;
constexpr int kButtonRightShoulder = 0x0200;
constexpr int kButtonGuide     = 0x0400;
constexpr int kButtonA         = 0x1000;
constexpr int kButtonB         = 0x2000;
constexpr int kButtonX         = 0x4000;
constexpr int kButtonY         = 0x8000;

using PVIGEM_CLIENT = void*;
using PVIGEM_TARGET = void*;
using VIGEM_ERROR   = unsigned long;
constexpr VIGEM_ERROR kVigemErrorSuccess = 0x20000000UL;

using PFN_vigem_alloc              = PVIGEM_CLIENT (WINAPI*)();
using PFN_vigem_free               = void (WINAPI*)(PVIGEM_CLIENT);
using PFN_vigem_connect            = VIGEM_ERROR (WINAPI*)(PVIGEM_CLIENT);
using PFN_vigem_disconnect         = void (WINAPI*)(PVIGEM_CLIENT);
using PFN_vigem_target_x360_alloc  = PVIGEM_TARGET (WINAPI*)();
using PFN_vigem_target_free        = void (WINAPI*)(PVIGEM_TARGET);
using PFN_vigem_target_add         = VIGEM_ERROR (WINAPI*)(PVIGEM_CLIENT, PVIGEM_TARGET);
using PFN_vigem_target_remove      = VIGEM_ERROR (WINAPI*)(PVIGEM_CLIENT, PVIGEM_TARGET);
using PFN_vigem_target_x360_update = VIGEM_ERROR (WINAPI*)(PVIGEM_CLIENT, PVIGEM_TARGET, XUSB_REPORT);
using PFN_vigem_x360_get_index = VIGEM_ERROR (WINAPI*)(PVIGEM_CLIENT, PVIGEM_TARGET, unsigned long*);

struct Api {
    HMODULE module = nullptr;
    PFN_vigem_alloc              alloc = nullptr;
    PFN_vigem_free               free = nullptr;
    PFN_vigem_connect            connect = nullptr;
    PFN_vigem_disconnect         disconnect = nullptr;
    PFN_vigem_target_x360_alloc  targetAlloc = nullptr;
    PFN_vigem_target_free        targetFree = nullptr;
    PFN_vigem_target_add         targetAdd = nullptr;
    PFN_vigem_target_remove      targetRemove = nullptr;
    PFN_vigem_target_x360_update targetUpdate = nullptr;
    PFN_vigem_x360_get_index  x360GetIndex = nullptr;  // optional: which slot we landed in
    bool complete() const
    {
        return alloc && free && connect && disconnect && targetAlloc && targetFree &&
               targetAdd && targetRemove && targetUpdate;
    }
};

Api         g_api;
PVIGEM_CLIENT g_client = nullptr;
PVIGEM_TARGET g_target = nullptr;
bool          g_attempted = false;
const char*   g_status = "NO PAD";
volatile LONG g_earlyStarted = 0;   // the early attach thread is one-shot
XUSB_REPORT   g_lastReport = {};    // exactly what we last sent, so we can recognise our own pad

template <class T>
void Bind(HMODULE module, T& slot, const char* name)
{
    slot = reinterpret_cast<T>(GetProcAddress(module, name));
}

// Looks for the client next to this DLL first (where the mod ships it), then anywhere on the
// normal search path.
HMODULE LoadClient()
{
    wchar_t path[1024];
    swprintf_s(path, L"%sViGEmClient.dll", LogDirectory());
    if (HMODULE module = LoadLibraryW(path))
        return module;
    if (HMODULE module = LoadLibraryW(L"ViGEmClient.dll"))
        return module;
    return nullptr;
}

bool BindApi()
{
    if (g_api.module)
        return g_api.complete();
    g_api.module = LoadClient();
    if (!g_api.module) {
        g_status = "NO PAD (ViGEmClient.dll missing)";
        Log("pad: ViGEmClient.dll not found next to the DLL or on the path; the rider will not drive"
            " (this is not an error - the game is untouched)");
        return false;
    }
    Bind(g_api.module, g_api.alloc, "vigem_alloc");
    Bind(g_api.module, g_api.free, "vigem_free");
    Bind(g_api.module, g_api.connect, "vigem_connect");
    Bind(g_api.module, g_api.disconnect, "vigem_disconnect");
    Bind(g_api.module, g_api.targetAlloc, "vigem_target_x360_alloc");
    Bind(g_api.module, g_api.targetFree, "vigem_target_free");
    Bind(g_api.module, g_api.targetAdd, "vigem_target_add");
    Bind(g_api.module, g_api.targetRemove, "vigem_target_remove");
    Bind(g_api.module, g_api.targetUpdate, "vigem_target_x360_update");
    Bind(g_api.module, g_api.x360GetIndex, "vigem_target_x360_get_user_index");
    if (!g_api.complete()) {
        g_status = "NO PAD (client API incomplete)";
        Log("pad: ViGEmClient.dll is missing expected exports; not driving");
        return false;
    }
    return true;
}

int ToTrigger(double value)
{
    if (!(value > 0.0))
        return 0;
    const double scaled = value * 255.0;
    return scaled >= 255.0 ? 255 : static_cast<int>(scaled + 0.5);
}

short ToAxis(double value, double deadzone)
{
    if (!(value > deadzone) && !(value < -deadzone))
        return 0;
    const double scaled = value * 32767.0;
    if (scaled >= 32767.0) return 32767;
    if (scaled <= -32768.0) return -32768;
    return static_cast<short>(scaled);
}

}  // namespace

bool Attach()
{
    const Config& cfg = GetConfig();
    if (g_target)
        return true;
    if (g_attempted && !g_target && !g_api.complete())
        return false;  // don't retry a failing client on every frame

    g_attempted = true;
    if (!BindApi())
        return false;

    g_client = g_api.alloc();
    if (!g_client) {
        g_status = "NO PAD (alloc failed)";
        Log("pad: vigem_alloc failed");
        return false;
    }
    const VIGEM_ERROR connected = g_api.connect(g_client);
    if (connected != kVigemErrorSuccess) {
        g_status = "NO PAD (ViGEmBus driver unavailable)";
        Log("pad: vigem_connect failed (0x%08X); is the ViGEmBus driver installed?", connected);
        g_api.free(g_client);
        g_client = nullptr;
        return false;
    }

    g_target = g_api.targetAlloc();
    if (!g_target) {
        g_status = "NO PAD (target alloc failed)";
        Log("pad: vigem_target_x360_alloc failed");
        g_api.disconnect(g_client);
        g_api.free(g_client);
        g_client = nullptr;
        return false;
    }
    const VIGEM_ERROR added = g_api.targetAdd(g_client, g_target);
    if (added != kVigemErrorSuccess) {
        g_status = "NO PAD (target add failed)";
        Log("pad: vigem_target_add failed (0x%08X)", added);
        g_api.targetFree(g_target);
        g_target = nullptr;
        g_api.disconnect(g_client);
        g_api.free(g_client);
        g_client = nullptr;
        return false;
    }

    g_status = "VIRTUAL PAD";

    // Which XInput slot the pad landed in decides whether the game can see it at all. MX Simulator
    // reads the controller its bindings name (XInputGamepad0), so a pad in any other slot is live
    // and centred as far as the game is concerned: the rider presses buttons nobody reads.
    unsigned long slot = 0xFFFFFFFFul;
    if (g_api.x360GetIndex &&
        g_api.x360GetIndex(g_client, g_target, &slot) == kVigemErrorSuccess) {
        if (slot == 0) {
            Log("pad: virtual pad is XInput slot 0, the slot the game's bindings point at");
        } else {
            Log("pad: virtual pad is XInput slot %lu, not slot 0: another controller is plugged in."
                " The game reads XInputGamepad0 only, so nothing the rider presses arrives - unplug"
                " (or switch off) the other pad, start the game again and press the rider key", slot);
        }
    }
    Log("pad: virtual Xbox 360 pad attached (throttle=RT, front brake=%s trigger, rear brake=B,"
        " clutch=A, steer=left X, lean=left Y)",
        cfg.frontBrakeTrigger == 0 ? "LT (left)" : "RT (right)");
    Release();  // start centred, whatever the game was doing
    return true;
}

// ---- Attaching before the game looks ---------------------------------------
// MX Simulator enumerates controllers once, while it starts up ("XInput roll call" in its own
// lastlog.txt), and never looks again. A pad created when the player presses F7 therefore does
// not exist as far as the game is concerned - which is exactly how the rider came to send full
// throttle into a pad nobody was reading. DllMain starts this thread so the pad exists before
// the game gets that far. It sits centred, sending nothing, until the rider actually drives.

DWORD WINAPI EarlyAttachThread(LPVOID)
{
    Attach();
    return 0;
}

void BeginEarlyAttach()
{
    if (InterlockedExchange(&g_earlyStarted, 1) != 0)
        return;
    HANDLE thread = CreateThread(nullptr, 0, &EarlyAttachThread, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

void Detach()
{
    if (!g_target || !g_client)
        return;
    if (g_api.complete()) {
        g_api.targetRemove(g_client, g_target);
        g_api.targetFree(g_target);
        g_api.disconnect(g_client);
        g_api.free(g_client);
    }
    g_target = nullptr;
    g_client = nullptr;
    g_status = "PAD DETACHED";
    Log("pad: virtual pad detached");
}

bool Attached() { return g_target != nullptr; }

void Submit(const Action& raw)
{
    if (!g_target || !g_client || !g_api.complete())
        return;

    const Config& cfg = GetConfig();
    const Action a = raw.Sanitized();

    XUSB_REPORT report;
    memset(&report, 0, sizeof report);

    // Front and rear brake go to whichever trigger/button the ini assigns, so the layout can be
    // matched to the game's own controller bindings.
    const int front = ToTrigger(a.brakeFront);
    const int throttle = ToTrigger(a.throttle);
    if (cfg.frontBrakeTrigger == 0) {
        report.bLeftTrigger = static_cast<unsigned char>(front);
        report.bRightTrigger = static_cast<unsigned char>(throttle);
    } else {
        report.bRightTrigger = static_cast<unsigned char>(throttle);
        report.bLeftTrigger = static_cast<unsigned char>(front);
    }

    if (a.brakeRear > 0.5)
        report.wButtons |= static_cast<unsigned short>(cfg.rearBrakeButton);
    if (a.clutch > 0.5)
        report.wButtons |= static_cast<unsigned short>(cfg.clutchButton);

    report.sThumbLX = ToAxis(a.steer, cfg.stickDeadzone);
    report.sThumbLY = ToAxis(a.lean, cfg.stickDeadzone);

    g_api.targetUpdate(g_client, g_target, report);
    g_lastReport = report;
}

void Release()
{
    if (!g_target || !g_client || !g_api.complete())
        return;
    XUSB_REPORT report;
    memset(&report, 0, sizeof report);
    g_api.targetUpdate(g_client, g_target, report);
    g_lastReport = report;
}

// ---- The real controller, read-only -------------------------------------
namespace {

typedef DWORD(WINAPI* PFN_XInputGetState)(DWORD, void*);
PFN_XInputGetState g_xinputGetState = nullptr;
bool                g_xinputTried = false;

void BindXInput()
{
    if (g_xinputTried)
        return;
    g_xinputTried = true;
    const wchar_t* const names[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (const wchar_t* name : names) {
        HMODULE module = LoadLibraryW(name);
        if (!module)
            continue;
        g_xinputGetState = reinterpret_cast<PFN_XInputGetState>(GetProcAddress(module, "XInputGetState"));
        if (g_xinputGetState)
            break;
    }
    if (!g_xinputGetState)
        Log("pad: no XInput found; human driving will not be logged");
}

double Trigger(unsigned char value) { return value > 0 ? value / 255.0 : 0.0; }

double Axis(short value)
{
    if (value > 0)
        return value / 32767.0;
    if (value < 0)
        return value / 32768.0;
    return 0.0;
}

}  // namespace

bool ReadHuman(Action& out)
{
    BindXInput();
    out = Action{};
    if (!g_xinputGetState)
        return false;

    // XINPUT_STATE is dwPacketNumber followed by XINPUT_GAMEPAD: 16 bytes in total.
    unsigned char state[16] = {};
    for (DWORD slot = 0; slot < 4; ++slot) {
        if (g_xinputGetState(slot, state) != 0)
            continue;

        // XINPUT_GAMEPAD starts at offset 4: wButtons, bLeftTrigger, bRightTrigger, then the sticks.
        const unsigned short buttons   = *reinterpret_cast<const unsigned short*>(state + 4);
        const unsigned char  leftTrig  = state[6];
        const unsigned char  rightTrig = state[7];
        const short          thumbLX   = *reinterpret_cast<const short*>(state + 8);
        const short          thumbLY   = *reinterpret_cast<const short*>(state + 10);
        const short          thumbRX   = *reinterpret_cast<const short*>(state + 12);
        const short          thumbRY   = *reinterpret_cast<const short*>(state + 14);

        // Our own virtual pad reads back exactly what the rider last sent, so a field-for-field
        // match identifies it without depending on device order.
        if (buttons == g_lastReport.wButtons &&
            leftTrig == g_lastReport.bLeftTrigger && rightTrig == g_lastReport.bRightTrigger &&
            thumbLX == g_lastReport.sThumbLX && thumbLY == g_lastReport.sThumbLY &&
            thumbRX == g_lastReport.sThumbRX && thumbRY == g_lastReport.sThumbRY)
            continue;

        const Config& cfg = GetConfig();
        const double left = Trigger(leftTrig);
        const double right = Trigger(rightTrig);
        out.throttle = right;  // the same physical layout the virtual pad presents
        out.brakeFront = cfg.frontBrakeTrigger == 0 ? left : right;
        out.brakeRear = (buttons & cfg.rearBrakeButton) ? 1.0 : 0.0;
        out.clutch = (buttons & cfg.clutchButton) ? 1.0 : 0.0;
        out.steer = Axis(thumbLX);
        out.lean = Axis(thumbLY);
        return true;
    }
    return false;
}

const char* StatusText() { return g_status; }

}  // namespace pad
}  // namespace mxrider
