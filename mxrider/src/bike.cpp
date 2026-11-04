// bike.cpp
//   Per frame:
//     1. BeginFrame  - the camera's world position, for measuring how far the camera travels
//     2. Observe     - every rigid object drawn within BikeSearchRadius of the camera becomes a
//                      candidate, described relative to the camera: q = the camera's position in
//                      the object's own space, plus the camera's forward / up in that space
//     3. EndFrame    - each track takes the candidate closest to what it was last frame;
//                      unmatched candidates start new tracks, tracks unseen for a while are dropped
//
//   The game's first-person camera rides on the bike but is sprung: it moves ~10 cm against the
//   bike over bumps. Instead of demanding a perfectly fixed q, a track qualifies when, over about
//   a second of travel,
//     - q stays within a few tens of centimetres of its average (scenery slides away by metres),
//     - its orientation relative to the camera stays steady (wheels spin, the fork steers).
//   The steadiest qualifying track wins, and the lock is held through short gaps so the bike is
//   never dropped for a frame or two.

#include <windows.h>
#include <cmath>
#include <vector>

#include "bike.h"
#include "config.h"
#include "log.h"
#include "mathlib.h"
#include "motion.h"

namespace mxrider {
namespace bike {
namespace {

constexpr size_t kMaxCandidates = 64;
constexpr size_t kMaxTracks     = 64;
constexpr double kMaxMissTime   = 0.5;   // seconds a track (and the lock) survives without a match
constexpr double kMinAlive      = 1.0;   // seconds a track must persist before it can be locked
constexpr double kMinTravel     = 5.0;   // ...while the camera travels this far
constexpr double kMaxAngleSpreadDeg = 8.0;
constexpr int    kMaxLogLines   = 40;
// How far from the middle of the screen the rider's own bike may be, in NDC units (the screen
// spans -1..1 each way). A chase camera puts it near 0; the other riders are off to the side.
constexpr double kMaxCentre     = 0.35;
// Enabled at a standstill, the camera is still too, so the travel test below can never pass. The
// one thing that still holds is geometry: the chase camera keeps the bike about eleven units
// ahead and dead-centre, which is what makes it safe to lock a bike that is not moving yet.
constexpr double kStandstillCentre  = 0.20;
constexpr double kChaseDistanceMin  = 7.0;
constexpr double kChaseDistanceMax  = 16.0;
constexpr double kStandstillQSpread = 0.60;
// Nearer than this is not a bike but something the game draws at the eye itself.
constexpr double kMinBikeDistance = 3.0;
// Height of the camera above the locked object, in that object's own space. The bike's own
// parts sit in this band; the rider's upper body sits well below it.
constexpr double kChaseHeightMin = 4.0;
constexpr double kChaseHeightMax = 10.0;

struct Candidate {
    double   viewModel[16];
    Vec3     q;            // camera position in the object's space
    Vec3     cameraFwd;    // camera forward / up in the object's space
    Vec3     cameraUp;
    unsigned program;
    int      order;        // draw order within the frame
    double   centre;       // NDC distance from the middle of the screen (negative: unknown)
    bool     used;
};

struct Track {
    int      id;
    Vec3     lastQ, lastFwd, lastUp;   // last frame's match
    Vec3     meanQ, meanFwd, meanUp;   // ~1 s running averages
    double   qVar;                     // running mean of |q - meanQ|^2
    double   angleVar;                 // running mean of (angle to the mean orientation)^2, deg^2
    double   alive;                    // seconds tracked
    double   travel;                   // camera travel while tracked
    double   missTime;                 // seconds since the last match
    double   viewModel[16];
    unsigned program;
    int      order;
    double   centre;                   // NDC distance from the middle of the screen
};

std::vector<Candidate> g_candidates;
std::vector<Track>     g_tracks;
bool          g_frameOpen = false;
bool          g_havePos   = false;
Vec3          g_cameraPos;
Vec3          g_prevCameraPos;
LARGE_INTEGER g_lastTick{};
int           g_nextId    = 1;
int           g_lockedId  = 0;
Pose          g_pose;
int           g_logLines  = 0;
double        g_viewInverse[16] = {};  // this frame's camera -> world

// Diagnostics for a new track or view setting: what the tracker locked, and what else was near.
double g_lockedTime    = 0.0;   // seconds since the current lock began
int    g_dumps         = 0;
double g_maxCameraStep = 0.0;   // fastest single-frame camera movement since the last dump

// The locked bike's world motion.
bool     g_haveBikePos = false;
int      g_motionTrackId = 0;
Vec3     g_bikePos;
MotionFit g_bikeFit;

// Only to notice long gaps (menus, loading); the filters step by the frame time they are given.
double SecondsSinceLastFrame()
{
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const double seconds = g_lastTick.QuadPart
        ? double(now.QuadPart - g_lastTick.QuadPart) / double(frequency.QuadPart)
        : 0.0;
    g_lastTick = now;
    return seconds;
}

double AngleDeg(Vec3 a, Vec3 b)
{
    return std::acos(Clamp(Dot(Normalize(a), Normalize(b)), -1.0, 1.0)) * kRadToDeg;
}

double OrientationDiffDeg(Vec3 fwdA, Vec3 upA, Vec3 fwdB, Vec3 upB)
{
    return std::fmax(AngleDeg(fwdA, fwdB), AngleDeg(upA, upB));
}

double QSpread(const Track& t) { return std::sqrt(t.qVar); }
double AngleSpread(const Track& t) { return std::sqrt(t.angleVar); }

// The game draws the bike as several objects - frame, fork, rider - under the same shader program,
// and they sit metres apart. Whichever one the tracker picks, the pose it reports is offset by that
// distance, so silently switching between them mid-lap corrupts the position even though every lock
// looks healthy. Remembering where the locked object sat relative to the camera keeps the choice
// stable across dropouts.
Vec3     g_lastLockQ{};
unsigned g_lastLockProgram = 0;
constexpr double kIdentityDistance = 1.5;   // units, measured in the object's own space
constexpr double kIdentityPenalty  = 0.5;   // larger than the whole centre range, so it always wins
constexpr double kIdentityGrace    = 5.0;   // seconds without any lock before another object may be adopted
double           g_lastLockedTime  = 0.0;

bool SameObjectAsLocked(const Track& t)
{
    const bool havePrevious = g_lastLockQ.x != 0.0 || g_lastLockQ.y != 0.0 || g_lastLockQ.z != 0.0;
    return havePrevious && t.program == g_lastLockProgram &&
           Length(t.meanQ - g_lastLockQ) <= kIdentityDistance;
}

// Lower is better: how near the middle of the screen, with steadiness breaking ties, and a strong
// preference for the object that was locked last - a different draw of the same bike is another
// part, not another view of the same point.
double Score(const Track& t)
{
    return t.centre + 0.01 * AngleSpread(t) + (SameObjectAsLocked(t) ? 0.0 : kIdentityPenalty);
}

// A bike that has not moved yet, seen from a camera that has not moved either. Without this the
// rider can never start itself: no travel means no lock, no lock means no state, no state means no
// throttle, so the travel the lock waits for would have to come from the human.
bool StartsFromRest(const Track& t)
{
    if (t.travel >= kMinTravel)
        return false;  // it moved: the ordinary test applies
    const double distance = Length(t.meanQ);
    return t.alive >= kMinAlive && QSpread(t) <= kStandstillQSpread &&
           AngleSpread(t) <= kMaxAngleSpreadDeg && t.centre >= 0.0 &&
           t.centre <= kStandstillCentre && distance >= kChaseDistanceMin &&
           distance <= kChaseDistanceMax;
}

bool Qualifies(const Track& t)
{
    // A chase camera lags the bike, so the camera's position in the bike's space swings by metres
    // and the old steadiness test only ever passed by accident. What does hold is where the bike
    // lands on screen: the camera is pointed at it, so it stays in the middle of the frame.
    if (t.missTime != 0 || t.alive < kMinAlive || t.centre < 0.0 || t.centre > kMaxCentre)
        return false;
    // The chase camera sits a known height above the chassis, so the camera's height in the
    // object's own space says which part this is. The bike itself reads 6.7-8.3 units; a part up at
    // the rider's body reads about -1.9 (measured), and locking that corrupts the heading because
    // the rider leans and turns with the bars, which fed straight back into the steering.
    if (t.meanQ.y < kChaseHeightMin || t.meanQ.y > kChaseHeightMax)
        return false;
    return t.travel >= kMinTravel || StartsFromRest(t);
}

// The signed unit axis (+-X, +-Y, +-Z) closest to v, skipping axis `exclude`.
Vec3 SnapToAxis(Vec3 v, int exclude, int& axis)
{
    const double c[3] = { v.x, v.y, v.z };
    axis = -1;
    for (int i = 0; i < 3; ++i) {
        if (i != exclude && (axis < 0 || std::fabs(c[i]) > std::fabs(c[axis])))
            axis = i;
    }
    Vec3 out;
    (axis == 0 ? out.x : axis == 1 ? out.y : out.z) = c[axis] < 0 ? -1.0 : 1.0;
    return out;
}

const char* AxisName(Vec3 a)
{
    if (a.x > 0.5) return "+X";
    if (a.x < -0.5) return "-X";
    if (a.y > 0.5) return "+Y";
    if (a.y < -0.5) return "-Y";
    return a.z > 0 ? "+Z" : "-Z";
}

Track* FindTrack(int id)
{
    for (Track& t : g_tracks) {
        if (t.id == id)
            return &t;
    }
    return nullptr;
}

void Lock(const Track& t)
{
    g_lockedId = t.id;
    int forwardAxis, upAxis;
    const Vec3 forward = SnapToAxis(t.meanFwd, -1, forwardAxis);
    const Vec3 up      = SnapToAxis(t.meanUp, forwardAxis, upAxis);
    g_pose.forward[0] = forward.x; g_pose.forward[1] = forward.y; g_pose.forward[2] = forward.z;
    g_pose.up[0] = up.x;           g_pose.up[1] = up.y;           g_pose.up[2] = up.z;
    if (g_logLines++ < kMaxLogLines)
        Log("bike: locked on an object of program %u (draw #%d): camera sits at (%.2f, %.2f, %.2f) "
            "in its space, bounce %.0f mm, wobble %.1f deg; forward axis %s, up axis %s",
            t.program, t.order, t.meanQ.x, t.meanQ.y, t.meanQ.z, QSpread(t) * 1000.0,
            AngleSpread(t), AxisName(forward), AxisName(up));
}

void Unlock(const char* why)
{
    if (g_lockedId && g_logLines++ < kMaxLogLines)
        Log("bike: lost track (%s); the rider idles until the bike is seen again", why);
    g_lockedId = 0;
    g_pose.tracked = false;
    g_pose.fresh = false;
    g_pose.haveAccel = false;
}

}  // namespace

void Reset()
{
    g_tracks.clear();
    g_candidates.clear();
    g_frameOpen = false;
    g_havePos = false;
    g_lockedId = 0;
    g_pose = Pose{};
    g_haveBikePos = false;
    g_bikeFit.Reset();
}

void BeginFrame(const double view[16])
{
    if (!AffineInverse(view, g_viewInverse))
        return;
    g_cameraPos = { g_viewInverse[12], g_viewInverse[13], g_viewInverse[14] };
    g_candidates.clear();
    g_frameOpen = true;
}

void Observe(unsigned program, const float viewModel[16], double centre)
{
    if (!g_frameOpen || g_candidates.size() >= kMaxCandidates)
        return;
    const Vec3 t{ viewModel[12], viewModel[13], viewModel[14] };  // object origin in camera space
    if (Length(t) > GetConfig().bikeSearchRadius)
        return;
    if (Length(t) < kMinBikeDistance)
        return;  // drawn at the eye: an overlay, not a bike
    // The chase camera follows our bike, so it sits on the middle of the screen while the other
    // riders are off to the side. They slide against the view by units a second, and mixing
    // their meshes up with ours is what made the lock useless before.
    if (centre > kMaxCentre)
        return;

    Candidate c;
    for (int i = 0; i < 16; ++i)
        c.viewModel[i] = viewModel[i];
    // Rotation part R (camera <- object). Column i of R is m[i*4 .. i*4+2], so (R^T v)_i is a
    // dot product with that column.
    const auto ColumnDot = [&](int i, Vec3 v) {
        return c.viewModel[i * 4] * v.x + c.viewModel[i * 4 + 1] * v.y + c.viewModel[i * 4 + 2] * v.z;
    };
    c.q         = { -ColumnDot(0, t), -ColumnDot(1, t), -ColumnDot(2, t) };
    c.cameraFwd = { -c.viewModel[2], -c.viewModel[6], -c.viewModel[10] };
    c.cameraUp  = { c.viewModel[1], c.viewModel[5], c.viewModel[9] };
    c.program   = program;
    c.order     = static_cast<int>(g_candidates.size());
    c.centre    = centre;
    c.used      = false;

    // The same object is often drawn twice (e.g. once more for shadows): keep one.
    for (const Candidate& other : g_candidates) {
        if (Length(other.q - c.q) < 1e-4 && Length(other.cameraFwd - c.cameraFwd) < 1e-4)
            return;
    }
    g_candidates.push_back(c);
}

// Once a second for the first few seconds of a lock: the camera's own attitude, the fastest
// single-frame movement of the camera, and every track the tracker is following. This is how the
// lock target is identified on a new track, and how a camera that is not the camera shows itself.
void DescribeToLog()
{
    const double* w = g_viewInverse;
    const Vec3 camRight{ w[0], w[1], w[2] };
    const Vec3 camUp   { w[4], w[5], w[6] };
    const Vec3 camBack { w[8], w[9], w[10] };
    const Vec3 camFwd  { -camBack.x, -camBack.y, -camBack.z };
    const Vec3 worldUp{ GetConfig().worldUp[0], GetConfig().worldUp[1], GetConfig().worldUp[2] };

    Log("seen: camera pos (%.2f, %.2f, %.2f), pitch %.1f deg, roll %.1f deg, heading %.1f deg, "
        "fastest step %.2f units/frame; %d candidates, %d tracks",
        g_cameraPos.x, g_cameraPos.y, g_cameraPos.z,
        std::asin(Clamp(Dot(camFwd, worldUp), -1.0, 1.0)) * kRadToDeg,
        std::asin(Clamp(Dot(camRight, worldUp), -1.0, 1.0)) * kRadToDeg,
        std::atan2(camFwd.z, camFwd.x) * kRadToDeg,
        g_maxCameraStep, static_cast<int>(g_candidates.size()), static_cast<int>(g_tracks.size()));

    int shown = 0;
    for (const Track& t : g_tracks) {
        if (shown++ >= 8)
            break;
        Log("  track %d%s: program %u draw #%d, centre %.2f, bounce %.0f mm, wobble %.1f deg, alive %.1f s, "
            "travel %.1f, camera at (%.2f, %.2f, %.2f)",
            t.id, t.id == g_lockedId ? " [BIKE]" : "", t.program, t.order, t.centre,
            QSpread(t) * 1000.0, AngleSpread(t), t.alive, t.travel,
            t.meanQ.x, t.meanQ.y, t.meanQ.z);
    }
    Log("  bike: pos (%.2f, %.2f, %.2f), fwd (%.2f, %.2f, %.2f), up (%.2f, %.2f, %.2f), "
        "speed %.1f, vertical %.2f g",
        g_pose.worldPosition[0], g_pose.worldPosition[1], g_pose.worldPosition[2],
        g_pose.worldForward[0], g_pose.worldForward[1], g_pose.worldForward[2],
        g_pose.worldUp[0], g_pose.worldUp[1], g_pose.worldUp[2],
        Length(Vec3{ g_pose.velocity[0], g_pose.velocity[1], g_pose.velocity[2] }),
        g_pose.verticalAccelG);
}

void EndFrame(double gameTime, double frameDt)
{
    if (!g_frameOpen)
        return;
    g_frameOpen = false;

    const Config& cfg = GetConfig();
    if (SecondsSinceLastFrame() > 1.0) {
        Unlock("no camera for over a second");
        g_tracks.clear();
        g_havePos = false;
    }
    const double dt = Clamp(frameDt, 0.0, 0.1);

    double travelStep = g_havePos ? Length(g_cameraPos - g_prevCameraPos) : 0.0;
    if (travelStep > cfg.teleportDistance)
        travelStep = 0;  // a respawn is not travel
    g_prevCameraPos = g_cameraPos;
    g_havePos = true;
    if (travelStep > g_maxCameraStep)
        g_maxCameraStep = travelStep;

    // ---- Match: the oldest tracks pick first ----
    std::vector<Track*> order;
    order.reserve(g_tracks.size());
    for (Track& t : g_tracks)
        order.push_back(&t);
    for (size_t i = 1; i < order.size(); ++i) {
        for (size_t j = i; j > 0 && order[j]->alive > order[j - 1]->alive; --j) {
            Track* swap = order[j];
            order[j] = order[j - 1];
            order[j - 1] = swap;
        }
    }

    const double alpha = 1.0 - std::exp(-1.0 * dt);  // ~1 s averages
    for (Track* t : order) {
        Candidate* best = nullptr;
        double bestCost = 1e30;
        for (Candidate& c : g_candidates) {
            if (c.used)
                continue;
            const double dq    = Length(c.q - t->lastQ);
            const double angle = OrientationDiffDeg(c.cameraFwd, c.cameraUp, t->lastFwd, t->lastUp);
            // Generous: the chase camera may swing the bike several units between frames.
            if (dq > 3.0 || angle > 40.0)
                continue;
            const double cost = dq + 0.02 * angle;
            if (cost < bestCost) {
                best = &c;
                bestCost = cost;
            }
        }
        if (!best) {
            t->missTime += dt;
            continue;
        }
        best->used = true;
        t->lastQ   = best->q;
        t->lastFwd = best->cameraFwd;
        t->lastUp  = best->cameraUp;
        t->meanQ   = t->meanQ + (best->q - t->meanQ) * alpha;
        t->meanFwd = Normalize(t->meanFwd + (best->cameraFwd - t->meanFwd) * alpha);
        t->meanUp  = Normalize(t->meanUp + (best->cameraUp - t->meanUp) * alpha);
        const Vec3 dq = best->q - t->meanQ;
        t->qVar += (Dot(dq, dq) - t->qVar) * alpha;
        const double angle = OrientationDiffDeg(best->cameraFwd, best->cameraUp, t->meanFwd, t->meanUp);
        t->angleVar += (angle * angle - t->angleVar) * alpha;
        t->alive    += dt;
        t->travel   += travelStep;
        t->missTime  = 0;
        t->program   = best->program;
        t->order     = best->order;
        t->centre   = best->centre;
        for (int i = 0; i < 16; ++i)
            t->viewModel[i] = best->viewModel[i];
    }

    // ---- Drop stale tracks, start new ones ----
    for (size_t i = 0; i < g_tracks.size();) {
        if (g_tracks[i].missTime > kMaxMissTime) {
            if (g_tracks[i].id == g_lockedId)
                Unlock("the bike was not seen for half a second");
            g_tracks.erase(g_tracks.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (const Candidate& c : g_candidates) {
        if (c.used || g_tracks.size() >= kMaxTracks)
            continue;
        Track t{};
        t.id       = g_nextId++;
        t.lastQ    = t.meanQ = c.q;
        t.lastFwd  = t.meanFwd = c.cameraFwd;
        t.lastUp   = t.meanUp = c.cameraUp;
        t.qVar     = 0.3 * 0.3;   // unproven until it has a history
        t.angleVar = 10.0 * 10.0;
        t.program  = c.program;
        t.order    = c.order;
        t.centre   = c.centre;
        for (int i = 0; i < 16; ++i)
            t.viewModel[i] = c.viewModel[i];
        g_tracks.push_back(t);
    }

    // ---- Lock onto the steadiest qualified track, and hold on to it ----
    Track* locked = g_lockedId ? FindTrack(g_lockedId) : nullptr;
    Track* best = nullptr;
    for (Track& t : g_tracks) {
        if (Qualifies(t) && (!best || Score(t) < Score(*best)))
            best = &t;
    }
    // Adopting a different object while the known one is briefly missing is how the reported pose
    // came to jump metres mid-lap (draw #1 -> draw #0, up axis -X, and the bike on its side two
    // seconds later). Waiting for the bike we know is always better than a confident lie. Only
    // after kIdentityGrace with no lock at all is a fresh object accepted, so a new bike or track
    // can still be picked up.
    const bool haveIdentity = g_lastLockQ.x != 0.0 || g_lastLockQ.y != 0.0 || g_lastLockQ.z != 0.0;
    const bool mayLock = best && (!haveIdentity || SameObjectAsLocked(*best) ||
                                  gameTime - g_lastLockedTime > kIdentityGrace);
    if (!locked && mayLock) {
        Lock(*best);
        locked = best;
        g_lastLockQ = best->meanQ;
        g_lastLockProgram = best->program;
    } else if (locked && best && best != locked && best->alive > 2.0 &&
               Score(*best) < 0.5 * Score(*locked) - 0.5) {
        Lock(*best);  // a clearly steadier object (e.g. the frame rather than the steering fork)
        locked = best;
        g_lastLockQ = best->meanQ;
        g_lastLockProgram = best->program;
    }

    g_pose.fresh = false;
    if (locked && locked->missTime == 0) {
        g_lastLockedTime = gameTime;  // the grace period runs from when the lock was lost, not made
        g_pose.tracked = true;
        // World transform of the bike this frame: camera -> world times View * Model.
        double world[16];
        MatMul(g_viewInverse, locked->viewModel, world);
        const auto Axis = [&](const double a[3]) {
            return Normalize(Vec3{ world[0] * a[0] + world[4] * a[1] + world[8] * a[2],
                                   world[1] * a[0] + world[5] * a[1] + world[9] * a[2],
                                   world[2] * a[0] + world[6] * a[1] + world[10] * a[2] });
        };
        // The bike's own model axes, rotated into the world by View * Model. MX Simulator's
        // camera is a chase camera with a fixed pitch and roll (measured: -26.6 deg pitch and
        // 0 deg roll however the bike corners), so its basis says nothing about how the bike is
        // leaning; the model's does. An unrotated lock target reads as level, which the
        // diagnostic dump exposes.
        const Vec3 forward = Axis(g_pose.forward);
        const Vec3 up      = Axis(g_pose.up);
        g_pose.worldForward[0] = forward.x; g_pose.worldForward[1] = forward.y; g_pose.worldForward[2] = forward.z;
        g_pose.worldUp[0] = up.x;           g_pose.worldUp[1] = up.y;           g_pose.worldUp[2] = up.z;
        g_pose.fresh = true;

        // Motion of the bike itself (not of the sprung camera): a least-squares fit over the last
        // 0.15 s on the game's clock, which frame-timing jitter can't throw off.
        const Vec3 pos{ world[12], world[13], world[14] };
        g_pose.worldPosition[0] = pos.x;
        g_pose.worldPosition[1] = pos.y;
        g_pose.worldPosition[2] = pos.z;
        if (locked->id != g_motionTrackId || !g_haveBikePos ||
            Length(pos - g_bikePos) > cfg.teleportDistance) {
            g_motionTrackId = locked->id;
            g_bikeFit.Reset();
        }
        g_bikeFit.Add(gameTime, pos);
        Vec3 velocity, accel;
        const Vec3 worldUp{ cfg.worldUp[0], cfg.worldUp[1], cfg.worldUp[2] };
        g_pose.haveAccel = g_bikeFit.Estimate(0.15, velocity, accel);
        g_pose.verticalAccelG = g_pose.haveAccel ? Dot(accel, worldUp) / kGravity : 0.0;
        g_pose.velocity[0] = velocity.x;
        g_pose.velocity[1] = velocity.y;
        g_pose.velocity[2] = velocity.z;
        g_bikePos = pos;
        g_haveBikePos = true;
    } else if (!locked) {
        g_haveBikePos = false;
        g_pose.haveAccel = false;
    }

    // Once a second for the first eight seconds of a lock, say what is locked and what the camera
    // is doing. Off by itself afterwards, so a normal session's log stays short.
    if (g_pose.tracked) {
        g_lockedTime += dt;
    } else {
        g_lockedTime = 0.0;
        g_dumps = 0;
    }
    if (g_pose.tracked && g_dumps < 8 && g_lockedTime >= double(g_dumps) + 1.0) {
        ++g_dumps;
        DescribeToLog();
        g_maxCameraStep = 0.0;
    }
}

const Pose& Current() { return g_pose; }

int CandidateCount() { return static_cast<int>(g_candidates.size()); }
int TrackCount() { return static_cast<int>(g_tracks.size()); }

const char* StatusText()
{
    if (g_pose.tracked)
        return "BIKE FOUND";
    return g_tracks.empty() ? "NO BIKE IN VIEW" : "LOOKING FOR THE BIKE";
}

}  // namespace bike
}  // namespace mxrider
