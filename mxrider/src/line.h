// line.h
//   The recorded racing line: the track centreline as a polyline in world space, plus the two
//   things it buys us - a progress / lap measure, and a steering target for the baseline
//   controller.
//
//   Recording it once by hand is what makes the rest of the project measurable: with a line, the
//   baseline can follow it round the track unaided, and every logged frame has a progress
//   fraction, so laps and lap times can be counted without the game's timing (which the GL hook
//   cannot see).
#pragma once

#include "mathlib.h"

namespace mxrider {
namespace line {

// Reads the CSV the recorder writes ("x,y,z" per point, '#' comments ignored). Replaces whatever
// line was loaded. Returns false - and leaves no line - if the file is missing or unusable.
bool Load(const wchar_t* path);
void Clear();
bool Loaded();
int  Count();
const wchar_t* Path();

// Recording. StartRecording() begins a new line (the old one is forgotten); RecordPosition()
// appends while the bike is moving; StopRecording() writes the file.
bool Recording();
bool StartRecording();
bool StopRecording();

// Appends the position if it is at least kMinSpacing from the last point. Cheap enough per frame.
void RecordPosition(const double position[3]);

struct Projection {
    bool   valid     = false;
    int    segment   = -1;    // index of the polyline segment the bike is beside
    double t         = 0.0;   // how far along that segment, 0..1
    double progress  = 0.0;   // 0..1 along the line (0 at the start of the recording)
    double crossTrack = 0.0;  // signed sideways offset (units); sign matches the steering sign
    double lookaheadX = 0.0;  // a point on the line ~lookahead units ahead of the bike
    double lookaheadZ = 0.0;
};

// Finds where the bike is on the line and the point it should be aiming at. Only the horizontal
// plane (x, z) is used: jumps and hills must not confuse the steering.
Projection Project(const double position[3], double lookahead);

// One point of the line ahead of the bike, and the radius of the bend there. Straight line reports
// a huge radius rather than infinity, so the caller can take cornerGrip * radius without a divide.
struct BendSample {
    double distance = 0.0;  // units ahead of the bike, measured along the line
    double radius   = 1e9;  // units
};

// Walks up to `reach` units ahead of the projection, sampling the bend radius every few units, and
// returns how many samples were written. Turning that into a speed is plain physics for the caller
// (v = sqrt(a * r) into the bend, braking out of it), which keeps geometry here and tuning in config.
int ScanAhead(const Projection& projection, double reach, BendSample* out, int maxCount);

// True once per completed lap: progress wrapped from near 1 back to near 0 while the bike was
// moving. Detected inside Project(), so call this right after it.
bool LapCompleted();
int  Laps();

}  // namespace line
}  // namespace mxrider
