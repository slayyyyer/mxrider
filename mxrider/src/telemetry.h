// telemetry.h
//   The per-frame picture of the bike, and the episode log the trainer reads.
//
//   The GL hook cannot see throttle, suspension, grip, engine state or crash flags, so everything
//   here is derived from motion: pose and its derivatives, attitude, airborne estimate, and the
//   progress fraction along the recorded line. That is the observation the policy gets, and the
//   same numbers are written to CSV so training happens on exactly what the DLL will see.
#pragma once

#include "action.h"
#include "bike.h"

namespace mxrider {

// Where a logged row's controls came from.
enum class Source {
    Idle = 0,     // nobody was driving (menus, no bike, rider off)
    Baseline = 1, // the line follower
    Policy = 2,   // the network
    Human = 3,    // the real controller, logged for imitation
};

struct State {
    bool   valid     = false;  // the bike was tracked this frame
    bool   airborne  = false;
    double time      = 0.0;    // game clock, seconds
    double dt        = 0.0;

    double pos[3] = {};   // model origin, world units
    double vel[3] = {};   // world velocity, units/s
    double fwd[3] = {};   // bike forward, unit
    double up[3]  = {};   // bike up, unit

    double speed        = 0.0;  // |velocity|
    double forwardSpeed = 0.0;  // velocity along the bike's forward axis
    double accelG       = 0.0;  // vertical acceleration along WorldUp, in g
    double pitchDeg     = 0.0;  // nose up positive
    double rollDeg      = 0.0;  // leaning right positive
    double yawRateDeg   = 0.0;  // heading change, deg/s
    double upright      = 1.0;  // dot(bike up, world up): 1 upright, 0 on its side, -1 upside down
    double progress     = 0.0;  // 0..1 along the recorded line (0 when there is none)
};

// Fills `out` from the tracker's pose. Uses the config's WorldUp and refSpeed.
void BuildState(const bike::Pose& pose, double gameTime, double dt, State& out);

namespace telemetry {

// Opens a new run log (<data>/mxrider_run_YYYYMMDD_HHMMSS.csv). Called once, from the rider.
bool OpenLog();
void CloseLog();
bool LogOpen();

// Appends one row. `done` marks the last frame of an episode; `reason` says why.
void WriteRow(int episode, Source source, const State& state, const Action& action,
              double reward, bool done, const char* reason);

}  // namespace telemetry
}  // namespace mxrider
