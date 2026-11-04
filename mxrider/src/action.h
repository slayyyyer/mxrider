// action.h
//   What the rider asks the bike to do this frame, in the units the game's controls have:
//   throttle / brakes / clutch are 0..1, steering and lean are -1..1.
//
//   This is the only vocabulary the rest of the rider speaks. The baseline line follower and the
//   learned policy both produce one of these, and pad.cpp turns it into a controller report. A
//   neutral Action (the default) is "hands off the bike", which is what every failure path falls
//   back to.
#pragma once

namespace mxrider {

struct Action {
    double throttle   = 0.0;  // 0..1
    double brakeFront = 0.0;  // 0..1
    double brakeRear  = 0.0;  // 0..1
    double steer      = 0.0;  // -1 left .. +1 right (after the configured sign)
    double lean       = 0.0;  // -1 back .. +1 forward (after the configured sign)
    double clutch     = 0.0;  // 0..1

    // Keep every channel inside the range the pad can express, and drop NaNs (a policy with a
    // broken weight file must never be able to slam a trigger).
    Action Sanitized() const
    {
        Action out;
        out.throttle   = Clamp01(throttle);
        out.brakeFront = Clamp01(brakeFront);
        out.brakeRear  = Clamp01(brakeRear);
        out.steer      = ClampSigned(steer);
        out.lean       = ClampSigned(lean);
        out.clutch     = Clamp01(clutch);
        return out;
    }

private:
    // NaN-safe: a NaN compares false against everything, so it lands in the 0 branch.
    static double Clamp01(double v) { return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0; }
    static double ClampSigned(double v) { return v > -1.0 ? (v < 1.0 ? v : 1.0) : -1.0; }
};

}  // namespace mxrider
