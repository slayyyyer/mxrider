// bike.h
//   Finds the rider's own bike among the objects the game draws, and reports where it is and how
//   it is moving. This is the mod's only source of truth about the bike: the hook cannot see
//   throttle, suspension, grip or crash flags, so everything the rider needs is read off motion.
//
//   How the bike is found (unchanged from the reference camera mod, which is good at this):
//   the game's camera rides on the bike, so the camera's position *in the bike's local space*
//   stays put while riding, however the bike moves. Scenery slides past, wheels spin, the fork
//   steers. The tracker follows every rigid object drawn near the camera from frame to frame and
//   locks onto the one that stays steadiest relative to the camera while the camera travels. No
//   draw order, program number or model name is assumed.
#pragma once

namespace mxrider {
namespace bike {

// Called when this frame's camera view matrix is captured (uniforms.cpp).
void BeginFrame(const double view[16]);

// Every rigid modelview upload after the camera in the same frame, exactly as the game sent it
// (View * Model, untouched). program is only used for logging.
// centre: distance from the middle of the screen in NDC units, negative when the projection was
// not available. A chase camera keeps the rider's own bike centred, which is what identifies it.
void Observe(unsigned program, const float viewModel[16], double centre);

// Called once per frame after drawing. Matches this frame's objects against the tracks.
// gameTime: the game's clock for this frame (seconds); frameDt: the smoothed frame interval.
void EndFrame(double gameTime, double frameDt);

// Forget every track (mode changes, menus, respawns).
void Reset();

struct Pose {
    bool   tracked = false;   // a bike track is locked
    bool   fresh   = false;   // ...and it was matched in the frame just ended
    bool   haveAccel = false; // the motion fit has enough samples for an acceleration estimate

    // The bike model's axes for "forward" and "up" in model space (unit, axis-aligned).
    double forward[3] = { 0.0, 0.0, -1.0 };
    double up[3]      = { 0.0, 1.0, 0.0 };

    // The same axes in world space, and the model origin, in the latest matched frame.
    double worldForward[3] = { 0.0, 0.0, -1.0 };
    double worldUp[3]      = { 0.0, 1.0, 0.0 };
    double worldPosition[3] = { 0.0, 0.0, 0.0 };

    // World velocity (units/s) and the bike's own vertical acceleration along WorldUp, in g
    // (free fall is about -1). A least-squares fit over recent frames, so frame jitter doesn't
    // produce phantom accelerations.
    double velocity[3] = { 0.0, 0.0, 0.0 };
    double verticalAccelG = 0.0;
};

const Pose& Current();

// Sensing health for the log: objects seen this frame, and tracks still alive.
int CandidateCount();
int TrackCount();

// Short status for the log: "BIKE FOUND" / "LOOKING FOR THE BIKE" / "NO BIKE IN VIEW".
const char* StatusText();

}  // namespace bike
}  // namespace mxrider
