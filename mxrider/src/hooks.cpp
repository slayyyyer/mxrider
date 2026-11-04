// hooks.cpp
//   The intercepted OpenGL entry points - three of them, all read-only.
//
//   The frame boundary is wglSwapBuffers: when the game presents a frame, everything it drew has
//   been observed, so the rider ticks there. Some drivers swap through wglSwapLayerBuffers, and a
//   few games clear instead, so both are watched; if neither has been seen for a second, a colour
//   clear after a captured frame also ends one.
//
//   Nothing here changes a single argument: every hook calls its real function with the values
//   the game passed in. The observation happens on the side (uniforms.cpp, bike.cpp).

#include <windows.h>
#include <GL/gl.h>
#include <cstdio>
#include <cstdlib>

#include "config.h"
#include "hooks.h"
#include "log.h"
#include "rider.h"
#include "uniforms.h"

namespace mxrider {
namespace {

using PFN_wglSwapBuffers      = BOOL(WINAPI*)(HDC);  // exported, but not declared in the SDK
using PFN_wglSwapLayerBuffers = BOOL(WINAPI*)(HDC, UINT);

decltype(&::glClear)             real_glClear             = nullptr;
PFN_wglSwapBuffers               real_wglSwapBuffers      = nullptr;
PFN_wglSwapLayerBuffers          real_wglSwapLayerBuffers = nullptr;
decltype(&::wglGetProcAddress)   real_wglGetProcAddress   = nullptr;

ULONGLONG g_lastSwapTick = 0;

bool Pressed(int vk, bool& wasDown)
{
    const bool down = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !wasDown;
    wasDown = down;
    return pressed;
}

// Hotkeys, sampled once per frame. Every key is sampled even when the game is in the background,
// so the edge detection can't fire a stale press when focus comes back.
void PollHotkeys()
{
    static bool toggleDown = false, modeDown = false, lineDown = false, reloadDown = false;
    const Config& cfg = GetConfig();

    const bool toggle = Pressed(cfg.toggleKey, toggleDown);
    const bool mode   = Pressed(cfg.modeKey, modeDown);
    const bool line   = Pressed(cfg.lineKey, lineDown);
    const bool reload = Pressed(cfg.reloadKey, reloadDown);
    if (!GameHasFocus())
        return;

    if (toggle)
        rider::ToggleEnabled();
    if (mode)
        rider::CycleMode();
    if (line)
        rider::ToggleLineRecording();
    if (reload) {
        ReloadConfig();
        rider::ReloadData();
    }
}

void APIENTRY Hook_glClear(GLbitfield mask)
{
    // Fallback frame boundary for drivers whose swap bypasses opengl32!wglSwapBuffers.
    if ((mask & GL_COLOR_BUFFER_BIT) && uniforms::FrameCaptured() &&
        GetTickCount64() - g_lastSwapTick > 1000)
        EndFrame();
    real_glClear(mask);
}

BOOL WINAPI Hook_wglSwapBuffers(HDC dc)
{
    g_lastSwapTick = GetTickCount64();
    EndFrame();
    return real_wglSwapBuffers(dc);
}

BOOL WINAPI Hook_wglSwapLayerBuffers(HDC dc, UINT planes)
{
    if (planes & WGL_SWAP_MAIN_PLANE) {
        g_lastSwapTick = GetTickCount64();
        EndFrame();
    }
    return real_wglSwapLayerBuffers(dc, planes);
}

// Everything past OpenGL 1.1 (shaders, uniforms, buffers) is fetched through here.
PROC WINAPI Hook_wglGetProcAddress(LPCSTR name)
{
    const PROC real = real_wglGetProcAddress(name);
    return reinterpret_cast<PROC>(WrapProc(name, reinterpret_cast<void*>(real)));
}

}  // namespace

bool GameHasFocus()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void* GetRealGLFunction(const char* name)
{
    if (void* exported = reinterpret_cast<void*>(GetProcAddress(RealOpenGLModule(), name)))
        return exported;
    if (!real_wglGetProcAddress)
        return nullptr;
    // Some drivers return small sentinel values instead of NULL for unsupported functions.
    const auto address = reinterpret_cast<INT_PTR>(real_wglGetProcAddress(name));
    return (address >= -1 && address <= 3) ? nullptr : reinterpret_cast<void*>(address);
}

void EndFrame()
{
    uniforms::EndShaderFrame();
    rider::Frame();
    uniforms::BeginFrame();
    PollHotkeys();
}

void OnProcessDetach()
{
    rider::Shutdown();
    LogClose();
}

#define MXRIDER_HOOK(fn) { #fn, reinterpret_cast<void*>(&Hook_##fn), reinterpret_cast<void**>(&real_##fn) }

const HookEntry* GetHookTable(size_t& count)
{
    static const HookEntry kHooks[] = {
        MXRIDER_HOOK(glClear),
        MXRIDER_HOOK(wglSwapBuffers),
        MXRIDER_HOOK(wglSwapLayerBuffers),
        MXRIDER_HOOK(wglGetProcAddress),
    };
    count = _countof(kHooks);
    return kHooks;
}

}  // namespace mxrider
