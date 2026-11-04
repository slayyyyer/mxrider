// rider.h
//   The part that decides what to press: mode state machine, episode bookkeeping, and the
//   baseline / policy controllers. Driven once per presented frame by hooks.cpp.
#pragma once

#include "config.h"

namespace mxrider {
namespace rider {

// One presented frame. Reads the tracked bike, chooses an action, drives the virtual pad, logs.
void Frame();

// Process teardown: release the pad, close the log.
void Shutdown();

// Hotkeys (hooks.cpp).
void ToggleEnabled();       // F7: rider on / off
void CycleMode();           // F6: off -> baseline -> record-line -> policy -> off
void ToggleLineRecording(); // F5: start / stop recording the racing line
void ReloadData();          // F9: reload ini, line and policy

bool Active();              // the rider is driving right now (pad attached, actions submitted)
Mode CurrentMode();
bool Enabled();
int  Episode();
int  Laps();

}  // namespace rider
}  // namespace mxrider
