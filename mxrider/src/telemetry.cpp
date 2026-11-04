// telemetry.cpp
//   Turns a pose into the numbers the rider reasons with, and writes them out.
//
//   Angles are measured against the config's WorldUp rather than a hard-coded Y axis, because the
//   game's world is not necessarily Y-up for every track; the reference mod ships Y-up and lets
//   the ini change it.

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <share.h>

#include "config.h"
#include "line.h"
#include "log.h"
#include "mathlib.h"
#include "telemetry.h"

namespace mxrider {
namespace {

double s_lastHeading = 0.0;
bool   s_haveHeading = false;

double WrapPi(double radians)
{
    while (radians > kPi)  radians -= 2.0 * kPi;
    while (radians < -kPi) radians += 2.0 * kPi;
    return radians;
}

// ---- The run log ---------------------------------------------------------
FILE* g_file = nullptr;
wchar_t g_path[1024] = L"";
ULONGLONG g_lastFlush = 0;
int g_rows = 0;

const char* kHeader =
    "episode,frame,t,dt,source,valid,airborne,"
    "px,py,pz,vx,vy,vz,speed,fspeed,accelG,pitch,roll,yawRate,upright,progress,lap,"
    "throttle,brakeFront,brakeRear,steer,lean,clutch,reward,done,reason\n";

}  // namespace

void BuildState(const bike::Pose& pose, double gameTime, double dt, State& out)
{
    out = State{};
    out.time = gameTime;
    out.dt = dt;
    out.valid = pose.tracked && pose.fresh;
    out.accelG = pose.verticalAccelG;

    if (!out.valid) {
        s_haveHeading = false;
        return;
    }

    const Config& cfg = GetConfig();
    for (int i = 0; i < 3; ++i) {
        out.pos[i] = pose.worldPosition[i];
        out.vel[i] = pose.velocity[i];
        out.fwd[i] = pose.worldForward[i];
        out.up[i]  = pose.worldUp[i];
    }

    const Vec3 forward{ out.fwd[0], out.fwd[1], out.fwd[2] };
    const Vec3 up{ out.up[0], out.up[1], out.up[2] };
    const Vec3 velocity{ out.vel[0], out.vel[1], out.vel[2] };
    const Vec3 worldUp{ cfg.worldUp[0], cfg.worldUp[1], cfg.worldUp[2] };

    out.speed = Length(velocity);
    out.forwardSpeed = Dot(velocity, forward);

    // Attitude. Right is world-right of the bike's heading; roll is how far the bike's up axis
    // leans that way. Both are clamped before asin, so a NaN pose can't poison the log.
    out.pitchDeg = std::asin(Clamp(Dot(forward, worldUp), -1.0, 1.0)) * kRadToDeg;
    Vec3 right = Cross(forward, worldUp);
    if (Length(right) < 1e-6)  // pointing straight up or down: any right axis will do
        right = Vec3{ 1.0, 0.0, 0.0 };
    right = Normalize(right);
    out.rollDeg = std::asin(Clamp(Dot(up, right), -1.0, 1.0)) * kRadToDeg;
    out.upright = Clamp(Dot(up, worldUp), -1.0, 1.0);

    // Heading rate, from the horizontal component of forward.
    const double heading = std::atan2(forward.z, forward.x);
    if (s_haveHeading && dt > 1e-4)
        out.yawRateDeg = WrapPi(heading - s_lastHeading) / dt * kRadToDeg;
    s_lastHeading = heading;
    s_haveHeading = true;

    // Airborne: the bike is in free fall (about -1 g) and not resting on anything. The terrain
    // shader is not read for height, so this is a motion estimate - good enough to tell a jump
    // from a rut, and the policy sees the raw acceleration anyway.
    out.airborne = pose.haveAccel && pose.verticalAccelG < -0.55;
}

namespace telemetry {

bool OpenLog()
{
    if (g_file)
        return true;
    const Config& cfg = GetConfig();
    if (!cfg.csv) {
        Log("telemetry: CSV logging is off");
        return false;
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    swprintf_s(g_path, L"%smxrider_run_%04u%02u%02u_%02u%02u%02u.csv", cfg.dataDirectory,
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    // _SH_DENYWR rather than plain fopen: other processes may read the CSV while the game runs
    // (to watch a session or copy it out), and only a second writer is refused.
    g_file = _wfsopen(g_path, L"w", _SH_DENYWR);
    if (!g_file) {
        Log("telemetry: could not open %ls; logging is off", g_path);
        return false;
    }
    fputs(kHeader, g_file);
    fflush(g_file);  // the header is on disk before any row, so an empty file means "no rows yet"
    g_lastFlush = GetTickCount64();
    Log("telemetry: writing %ls", g_path);
    return true;
}

void CloseLog()
{
    if (!g_file)
        return;
    fflush(g_file);
    fclose(g_file);
    g_file = nullptr;
    Log("telemetry: closed %ls after %d rows", g_path, g_rows);
}

bool LogOpen() { return g_file != nullptr; }

void WriteRow(int episode, Source source, const State& s, const Action& a, double reward,
              bool done, const char* reason)
{
    if (!g_file)
        return;

    fprintf(g_file,
            "%d,%d,%.4f,%.5f,%d,%d,%d,"
            "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.4f,%.4f,%d,"
            "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%s\n",
            episode, g_rows, s.time, s.dt, static_cast<int>(source), s.valid ? 1 : 0,
            s.airborne ? 1 : 0,
            s.pos[0], s.pos[1], s.pos[2], s.vel[0], s.vel[1], s.vel[2],
            s.speed, s.forwardSpeed, s.accelG, s.pitchDeg, s.rollDeg, s.yawRateDeg, s.upright,
            s.progress, line::Laps(),
            a.throttle, a.brakeFront, a.brakeRear, a.steer, a.lean, a.clutch,
            reward, done ? 1 : 0, reason && reason[0] ? reason : "-");
    ++g_rows;

    const Config& cfg = GetConfig();
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastFlush >= static_cast<ULONGLONG>(cfg.csvFlushSeconds * 1000.0)) {
        g_lastFlush = now;
        fflush(g_file);  // so a crash or an alt-tab doesn't cost the session
    }
}

}  // namespace telemetry
}  // namespace mxrider
