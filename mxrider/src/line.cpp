// line.cpp
//   A polyline in world space, recorded by driving, plus projection onto it.
//
//   Everything here is deliberately 2D (x and z): a jump face or a rut must not change how far
//   round the track the bike is, and the steering target is a direction on the ground.

#include <windows.h>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "config.h"
#include "line.h"
#include "log.h"
#include "mathlib.h"

namespace mxrider {
namespace line {
namespace {

// Points are only appended this far apart, so a slow lap doesn't produce a huge file and a fast
// one doesn't produce gaps the projection can't follow.
constexpr double kMinSpacing = 1.5;
constexpr int    kMaxPoints  = 200000;

// A bend radius is measured over this many points either side - a chord of roughly 12 units. Long
// enough that point jitter does not read as a corner, short enough to see a rut.
constexpr int    kCurveSpan  = 4;
// How far apart the bend samples handed to the speed planner are, in units along the line.
constexpr double kSampleSpacing = 4.0;

// The projection searches a window around the last segment it found first (the bike moves
// forwards along the line), then falls back to a full scan when the line is new or teleported.
constexpr int kSearchWindow = 60;

std::vector<Vec3> g_points;
wchar_t g_path[1024] = L"";
bool    g_recording = false;
int     g_lastSegment = -1;
double  g_lastProgress = -1.0;
bool    g_lapCompleted = false;
int     g_laps = 0;
int     g_wrapGuard = 0;

constexpr double kPi = 3.14159265358979323846;

double WrapPi(double radians)
{
    while (radians > kPi)  radians -= 2.0 * kPi;
    while (radians < -kPi) radians += 2.0 * kPi;
    return radians;
}

// Closest point on segment a->b to p, in the x/z plane. Returns the parameter t along a->b.
double ClosestOnSegment(const Vec3& a, const Vec3& b, const Vec3& p, double& distance)
{
    const double dx = b.x - a.x, dz = b.z - a.z;
    const double lengthSq = dx * dx + dz * dz;
    double t = 0.0;
    if (lengthSq > 1e-12)
        t = Clamp(((p.x - a.x) * dx + (p.z - a.z) * dz) / lengthSq, 0.0, 1.0);
    const double cx = a.x + dx * t, cz = a.z + dz * t;
    distance = std::sqrt((p.x - cx) * (p.x - cx) + (p.z - cz) * (p.z - cz));
    return t;
}

double HorizontalDistance(const Vec3& a, const Vec3& b)
{
    const double dx = b.x - a.x, dz = b.z - a.z;
    return std::sqrt(dx * dx + dz * dz);
}

// Radius of the bend at point i, from the angle between the two chords through it. A straight
// stretch reports the "no bend" radius instead of infinity, so nothing downstream divides by zero.
double RadiusAt(int i, int segments)
{
    const auto wrap = [segments](int k) { k %= segments; return k < 0 ? k + segments : k; };
    const Vec3& a = g_points[wrap(i - kCurveSpan)];
    const Vec3& b = g_points[wrap(i)];
    const Vec3& c = g_points[wrap(i + kCurveSpan)];
    const double ux = b.x - a.x, uz = b.z - a.z;
    const double vx = c.x - b.x, vz = c.z - b.z;
    const double lu = std::sqrt(ux * ux + uz * uz);
    const double lv = std::sqrt(vx * vx + vz * vz);
    if (lu < 1e-6 || lv < 1e-6)
        return 1e9;
    const double turn = std::acos(Clamp((ux * vx + uz * vz) / (lu * lv), -1.0, 1.0));
    if (turn < 1e-4)
        return 1e9;
    return 0.5 * (lu + lv) / turn;
}

}  // namespace

void Clear()
{
    g_points.clear();
    g_lastSegment = -1;
    g_lastProgress = -1.0;
    g_laps = 0;
    g_wrapGuard = 0;
    g_lapCompleted = false;
}

bool Load(const wchar_t* path)
{
    Clear();
    g_recording = false;
    if (path)
        wcscpy_s(g_path, path);
    if (!path || !path[0]) {
        Log("line: no line file configured");
        return false;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, path, L"r") != 0 || !file) {
        Log("line: %ls not found; the baseline and the lap counter need a recorded line", path);
        return false;
    }

    char text[256];
    int lineNumber = 0;
    while (fgets(text, sizeof text, file)) {
        ++lineNumber;
        char* p = text;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '#' || *p == '\0' || *p == '\r' || *p == '\n')
            continue;
        double x, y, z;
        if (sscanf_s(p, "%lf,%lf,%lf", &x, &y, &z) != 3) {
            Log("line: %ls line %d is not 'x,y,z'; giving up on the file", path, lineNumber);
            Clear();
            fclose(file);
            return false;
        }
        if (g_points.size() >= static_cast<size_t>(kMaxPoints))
            break;
        g_points.push_back(Vec3{ x, y, z });
    }
    fclose(file);

    if (g_points.size() < 8) {
        Log("line: %ls has %zu points; that is not enough to follow", path, g_points.size());
        Clear();
        return false;
    }
    Log("line: loaded %zu points from %ls", g_points.size(), path);
    return true;
}

bool Loaded() { return g_points.size() >= 8; }

int Count() { return static_cast<int>(g_points.size()); }

const wchar_t* Path() { return g_path; }

bool Recording() { return g_recording; }

bool StartRecording()
{
    Clear();
    g_recording = true;
    Log("line: recording a new line; drive a clean lap, then press the line key again to save");
    return true;
}

bool StopRecording()
{
    if (!g_recording)
        return false;
    g_recording = false;

    if (g_points.size() < 8) {
        Log("line: only %zu points recorded - not saved", g_points.size());
        Clear();
        return false;
    }
    if (!g_path[0]) {
        Log("line: no path configured, points discarded");
        Clear();
        return false;
    }

    FILE* file = nullptr;
    if (_wfopen_s(&file, g_path, L"w") != 0 || !file) {
        Log("line: could not write %ls (error %d)", g_path, errno);
        return false;
    }
    fprintf(file, "# MXRider racing line: x,y,z per point, world units. Recorded by driving.\n");
    for (const Vec3& p : g_points)
        fprintf(file, "%.3f,%.3f,%.3f\n", p.x, p.y, p.z);
    fclose(file);
    Log("line: saved %zu points to %ls", g_points.size(), g_path);
    return true;
}

void RecordPosition(const double position[3])
{
    if (!g_recording || g_points.size() >= static_cast<size_t>(kMaxPoints))
        return;
    const Vec3 p{ position[0], position[1], position[2] };
    if (!g_points.empty()) {
        const Vec3& last = g_points.back();
        const double dx = p.x - last.x, dz = p.z - last.z;
        if (std::sqrt(dx * dx + dz * dz) < kMinSpacing)
            return;
    }
    g_points.push_back(p);
}

Projection Project(const double position[3], double lookahead)
{
    Projection out;
    g_lapCompleted = false;
    if (!Loaded())
        return out;

    const Vec3 p{ position[0], position[1], position[2] };
    const int segments = static_cast<int>(g_points.size()) - 1;

    int bestIndex = -1;
    double bestDistance = 1e30;
    double bestT = 0.0;

    // Near the last segment first, then everywhere if that found nothing plausible. The window has
    // to wrap: the line is a closed loop, so near the start/finish the nearest segment is as likely
    // to sit at the end of the file as at the beginning. Without the wrap that looks like a jump
    // across the whole track, and progress oscillated +/-0.96 five times in a single run (measured),
    // each time aiming the steering at the other end of the circuit.
    const auto wrap = [segments](int i) { i %= segments; return i < 0 ? i + segments : i; };
    const bool haveHint = g_lastSegment >= 0 && g_lastSegment < segments;
    const int windowStart = haveHint ? g_lastSegment - 8 : 0;
    const int windowCount = haveHint ? (8 + kSearchWindow) : segments;
    for (int k = 0; k < windowCount; ++k) {
        const int i = haveHint ? wrap(windowStart + k) : k;
        double distance = 0.0;
        const double t = ClosestOnSegment(g_points[i], g_points[i + 1], p, distance);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestIndex = i;
            bestT = t;
        }
    }
    if (haveHint && bestDistance > 25.0) {
        // Look again, but only where the bike could plausibly have got to. Between two frames it
        // moves well under a unit, while a rescan over the whole line will happily latch onto a
        // different pass of the same track a few units away: progress then teleports (measured
        // 0.045 -> 1.000 in a single corner) and the steering target jumps to another part of the
        // circuit, which is why the bike could not go straight. A bike that is far from every
        // nearby segment has genuinely been respawned, and only then is a global rescan allowed.
        constexpr double kMaxJumpFraction  = 0.08;   // of the lap
        constexpr double kRespawnDistance  = 60.0;   // units; further than this means a respawn
        const int span = static_cast<int>(segments * kMaxJumpFraction);

        int    nearIndex = -1;
        double nearDistance = 1e30;
        double nearT = 0.0;
        for (int k = 0; k < 2 * span; ++k) {
            const int i = wrap(g_lastSegment - span + k);
            double distance = 0.0;
            const double t = ClosestOnSegment(g_points[i], g_points[i + 1], p, distance);
            if (distance < nearDistance) {
                nearDistance = distance;
                nearIndex = i;
                nearT = t;
            }
        }
        if (nearIndex >= 0 && nearDistance <= kRespawnDistance) {
            bestIndex = nearIndex;
            bestDistance = nearDistance;
            bestT = nearT;
        } else {
            bestIndex = -1;
            bestDistance = 1e30;
            for (int i = 0; i < segments; ++i) {
                double distance = 0.0;
                const double t = ClosestOnSegment(g_points[i], g_points[i + 1], p, distance);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestIndex = i;
                    bestT = t;
                }
            }
        }
    }
    if (bestIndex < 0)
        return out;

    g_lastSegment = bestIndex;
    out.valid = true;
    out.segment = bestIndex;
    out.progress = (bestIndex + bestT) / static_cast<double>(segments);
    out.t = bestT;

    // Signed sideways offset: positive when the bike is to the right of the direction of travel
    // (in x/z, with y up). Whether that is the sign the game calls "right" is what SteerSign in
    // the ini is for.
    const Vec3& a = g_points[bestIndex];
    const Vec3& b = g_points[bestIndex + 1];
    const double dx = b.x - a.x, dz = b.z - a.z;
    const double length = std::sqrt(dx * dx + dz * dz);
    if (length > 1e-9) {
        const double ux = dx / length, uz = dz / length;
        const double vx = p.x - a.x, vz = p.z - a.z;
        // Positive means the bike sits to the RIGHT of the line's direction of travel, which is
        // the way positive steer turns it, so steering with this sign pulls the bike back to the
        // line. The raw cross product points the other way in this x/z, y-up frame: with it a
        // bike 35 units left of the line was steered further left until it circled (measured:
        // crossTrack mean +35.6 units, steer pinned at +0.85 with no way to recover).
        out.crossTrack = uz * vx - ux * vz;
    }

    // Walk forward along the line until the lookahead distance is used up; that point is the
    // steering target.
    double travelled = (1.0 - bestT) * length;
    int index = bestIndex + 1;
    double targetX = b.x, targetZ = b.z;
    while (travelled < lookahead && index < segments) {
        const Vec3& s = g_points[index];
        const Vec3& e = g_points[index + 1];
        const double sx = e.x - s.x, sz = e.z - s.z;
        const double segmentLength = std::sqrt(sx * sx + sz * sz);
        if (travelled + segmentLength >= lookahead && segmentLength > 1e-9) {
            const double t = (lookahead - travelled) / segmentLength;
            targetX = s.x + sx * t;
            targetZ = s.z + sz * t;
            break;
        }
        travelled += segmentLength;
        targetX = e.x;
        targetZ = e.z;
        ++index;
    }
    out.lookaheadX = targetX;
    out.lookaheadZ = targetZ;

    // Lap: progress wrapped from the end of the line back to the start while the bike is moving.
    // The guard that follows a counted lap absorbs the projection jittering across the seam, which
    // would otherwise count the same crossing two or three times.
    if (g_wrapGuard > 0)
        --g_wrapGuard;
    const bool wrapped = g_lastProgress > 0.9 && out.progress < 0.1;
    if (wrapped && g_wrapGuard == 0) {
        g_lapCompleted = true;
        ++g_laps;
        g_wrapGuard = 30;
        Log("line: lap %d completed", g_laps);
    }
    g_lastProgress = out.progress;
    return out;
}

int ScanAhead(const Projection& projection, double reach, BendSample* out, int maxCount)
{
    if (!projection.valid || !out || maxCount <= 0 || !Loaded())
        return 0;

    const int segments = static_cast<int>(g_points.size()) - 1;
    const int start = projection.segment;
    if (start < 0 || start >= segments)
        return 0;

    double travelled = (1.0 - Clamp(projection.t, 0.0, 1.0)) *
                       HorizontalDistance(g_points[start], g_points[start + 1]);
    double nextSample = 0.0;  // the bend the bike is in right now counts too
    int written = 0;
    int index = start + 1;
    for (int guard = 0; guard <= segments && written < maxCount && travelled <= reach; ++guard) {
        if (index >= segments)
            index = 0;  // the line is a closed loop
        const double length = HorizontalDistance(g_points[index], g_points[index + 1]);
        if (travelled >= nextSample) {
            out[written].distance = travelled;
            out[written].radius = RadiusAt(index, segments);
            ++written;
            nextSample += kSampleSpacing;
        }
        travelled += length;
        ++index;
    }
    return written;
}

bool LapCompleted() { return g_lapCompleted; }

int Laps() { return g_laps; }

}  // namespace line
}  // namespace mxrider
