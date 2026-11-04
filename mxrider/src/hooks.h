// hooks.h
//   Interface between the generic proxy (proxy.cpp) and the input we intercept.
//
//   MXRider only ever *reads*. There is no matrix correction, no shader swap and no state
//   substitution: the hooks observe the arguments of a handful of calls, hand them to the
//   tracker, and then forward the original arguments to the real driver untouched.
#pragma once

#include <windows.h>
#include <cstddef>

namespace mxrider {

// The genuine system opengl32.dll (loaded by proxy.cpp).
HMODULE RealOpenGLModule();

// A driver entry point that bypasses our hooks, for anything we need to call ourselves.
void* GetRealGLFunction(const char* name);

// True when the game window is the foreground window (hotkeys are ignored otherwise).
bool GameHasFocus();

struct HookEntry {
    const char* name;      // export name, e.g. "glClear"
    void*       hook;      // our replacement, which the export stub will jump to
    void**      realSlot;  // receives the genuine function from the system opengl32.dll
};

// The table of exports hooks.cpp wants to intercept. The proxy fills every realSlot before
// pointing the export at the hook, so a hook can always call its real function.
const HookEntry* GetHookTable(size_t& count);

// Called for every function the game fetches through wglGetProcAddress. Returns our wrapper for
// the ones we intercept (uniform uploads), otherwise the real pointer.
void* WrapProc(const char* name, void* real);

// Called once per presented frame, at the buffer swap: closes the frame, ticks the rider, and
// starts a new one.
void EndFrame();

// Called from DllMain on process detach: stops the rider, detaches the pad and closes the log.
void OnProcessDetach();

}  // namespace mxrider
