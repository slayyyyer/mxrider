// pad.h
//   The virtual gamepad: an emulated Xbox 360 controller the game sees through the same XInput
//   path it already uses (mx.exe imports XINPUT9_1_0.dll), so the rider drives the bike exactly
//   the way a person does. No input is hooked and no game memory is touched.
//
//   ViGEmClient.dll is loaded at runtime, never linked: if it is missing, or the ViGEmBus driver
//   won't create a pad, the rider logs why and leaves the game completely alone. That is the whole
//   failure policy in one place.
#pragma once

#include "action.h"

namespace mxrider {
namespace pad {

// Attaches a virtual pad. False if the client library or the driver is unavailable; the reason is
// logged once. Safe to call when already attached.
bool Attach();

// Creates the pad as the game starts, before MX Simulator enumerates controllers, and keeps
// it for the whole session: the game never re-scans, so a pad created later is invisible to
// it. The pad stays centred - sending nothing at all - until the rider drives.
void BeginEarlyAttach();

// Detaches the virtual pad, so the game sees only real controllers again.
void Detach();

bool Attached();

// Sends a full controller report. Everything not driven by the rider is left centred.
void Submit(const Action& action);

// Returns the pad to rest (all controls released) without detaching it.
void Release();

// Reads the first real controller through XInput into the same Action vocabulary, so human
// driving can be logged next to the bike state and used for imitation. False when there is no
// real controller. This only ever *reads*: the game still receives the real pad's input directly.
bool ReadHuman(Action& out);

// "VIRTUAL PAD" / "NO PAD (ViGEmClient.dll missing)" and similar, for the log.
const char* StatusText();

}  // namespace pad
}  // namespace mxrider
