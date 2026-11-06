// rider.cpp
//   One presented frame of riding.
//
//   The loop is deliberately dumb and hard to break:
//     1. the tracker settles this frame's bike pose (bike::EndFrame, called by the frame boundary)
//     2. the pose is turned into a State, and projected onto the racing line for progress / laps
//     3. the active controller - baseline line follower or the network - produces an Action
//     4. the action is sanitised and sent to the virtual pad
//     5. the frame is logged, with a reward, and episodes are started and ended around crashes
//
//   Every failure path converges on the same behaviour: release the controls, log once, try again
//   next frame. There is no path here that can throw, allocate per frame without bound, or press a
//   control when the bike is not understood.

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "action.h"
#include "bike.h"
#include "config.h"
#include "line.h"
#include "log.h"
#include "mathlib.h"
#include "pad.h"
#include "policy.h"
#include "rider.h"
#include "telemetry.h"
#include "uniforms.h"

namespace mxrider {
namespace rider {
namespace {

// The observation layout. It is duplicated in train/mxrider_learn.py (OBS_NAMES) and in the
// README; changing it means retraining, and the policy loader rejects a mismatched obs count.
constexpr int kObsSize = 16;

constexpr double kCrashSeconds = 0.5;   // on its side this long ends the episode
constexpr double kLostSeconds  = 1.0;   // bike unseen this long ends the episode
constexpr double kCrashPenalty = 5.0;

bool g_initialised = false;
bool g_enabled = false;
Mode g_mode = Mode::Off;
bool g_padWarned = false;

Policy g_policy;
bool   g_policyTried = false;

Action g_previous;            // what we submitted last frame (part of the observation)
Action g_lastSent;            // what the pad was actually told, so inputs can be rate limited
double g_lastTarget = 0.0;    // the speed the line allowed this frame, for the status log
double g_lastStatusLog = 0.0; // game time of the last "how is it going" line
bool   g_havePreviousPos = false;
double g_previousPos[3] = {};
bool   g_haveProjection = false;
line::Projection g_projection;

LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_lastSwap{};
double g_gameTime = 0.0;
int    g_frameNumber = 0;

int    g_episode = 0;
bool   g_episodeOpen = false;
double g_episodeTime = 0.0;
double g_episodeReturn = 0.0;
int    g_episodeFrames = 0;
double g_upsideDownTime = 0.0;
double g_lostTime = 0.0;
bool   g_waitingForUpright = false;  // crashed: hold nothing until the bike is reset
double g_uprightTime = 0.0;          // seconds the bike has been fully upright while waiting
int    g_lapAtStart = 0;
int    g_lapsSeen = 0;
const char* g_pendingReason = "";

double WrapPi(double radians)
{
    while (radians > kPi)  radians -= 2.0 * kPi;
    while (radians < -kPi) radians += 2.0 * kPi;
    return radians;
}

double SecondsSinceLastSwap()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double seconds = g_lastSwap.QuadPart
        ? double(now.QuadPart - g_lastSwap.QuadPart) / double(g_frequency.QuadPart)
        : 0.0;
    g_lastSwap = now;
    return seconds;
}

void StartEpisode(const char* why)
{
    ++g_episode;
    g_episodeOpen = true;
    g_episodeTime = 0.0;
    g_episodeReturn = 0.0;
    g_episodeFrames = 0;
    g_upsideDownTime = 0.0;
    g_lostTime = 0.0;
    g_pendingReason = "";
    g_havePreviousPos = false;
    g_previous = Action{};
    g_lastSent = Action{};
    g_lapAtStart = line::Laps();
    Log("episode %d begins (%s, mode=%s, lap %d)", g_episode, why, ModeName(g_mode), g_lapAtStart + 1);
}

void EndEpisode(const char* reason)
{
    if (!g_episodeOpen)
        return;
    g_episodeOpen = false;
    Log("episode %d ended (%s): %d frames, %.1f s, return %.2f, laps %d", g_episode, reason,
        g_episodeFrames, g_episodeTime, g_episodeReturn, g_lapsSeen - g_lapAtStart);
    g_pendingReason = reason;
}

// The fastest the recorded line says is safe right now. Every bend within braking range sets the
// speed the bike must already be down to (v^2 = corner^2 + 2 * a * distance) and the tightest
// answer wins. Without this the rider held one speed for the whole lap: too slow on the straights
// and still too quick for the tight stuff, which is exactly how it felt to ride behind.
double SpeedTarget(const Config& cfg)
{
    double limit = cfg.targetSpeed;
    line::BendSample bends[120];
    const int count = line::ScanAhead(g_projection, cfg.brakeReach, bends, 120);
    for (int i = 0; i < count; ++i) {
        const double corner = std::sqrt(cfg.cornerGrip * bends[i].radius);
        const double distance = bends[i].distance > 0.0 ? bends[i].distance : 0.0;
        const double allowed = std::sqrt(corner * corner + 2.0 * cfg.brakeDecel * distance);
        if (allowed < limit)
            limit = allowed;
    }
    return limit > cfg.minSpeed ? limit : cfg.minSpeed;
}

// Rate limits the stick and the throttle, so one noisy frame cannot snap either of them: turning a
// bike is a continuous thing, and the first version let the stick jump from lock to lock.
Action RateLimited(const Action& want, const Action& previous, double dt, const Config& cfg)
{
    if (!(dt > 0.0))
        return want;
    Action out = want;
    const double steerStep = cfg.maxSteerRate * dt;
    out.steer = previous.steer + Clamp(want.steer - previous.steer, -steerStep, steerStep);
    const double throttleStep = cfg.maxThrottleRate * dt;
    out.throttle = previous.throttle + Clamp(want.throttle - previous.throttle, -throttleStep, throttleStep);
    return out;
}

// How much throttle the bike can actually use right now. Two things ruined whole runs without any
// input from the steering: pinning the throttle from a standstill loops the bike out, and asking
// for power with the nose in the air or the bike on its side cannot steer, so it goes down.
double ThrottleCeiling(const Config& cfg, const State& s)
{
    double ceiling = 1.0;
    if (s.forwardSpeed < cfg.launchSpeed && ceiling > cfg.launchThrottle)
        ceiling = cfg.launchThrottle;
    if (s.pitchDeg > cfg.wheeliePitch) {
        const double fade = 1.0 - (s.pitchDeg - cfg.wheeliePitch) / 20.0;
        ceiling *= Clamp(fade, 0.0, 1.0);
    }
    if (s.upright < cfg.uprightThrottle)
        ceiling *= Clamp(s.upright / cfg.uprightThrottle, 0.0, 1.0);
    return Clamp(ceiling, 0.0, 1.0);
}

// Turns the projection onto the line into a steering target. Pure pursuit: aim at a point a
// little way ahead, plus a nudge away from the line's centre.
Action BaselineAction(const State& s)
{
    Action a;
    if (!s.valid || !g_haveProjection || !g_projection.valid)
        return a;

    const Config& cfg = GetConfig();
    const double dx = g_projection.lookaheadX - s.pos[0];
    const double dz = g_projection.lookaheadZ - s.pos[2];
    if (dx * dx + dz * dz < 1e-6)
        return a;

    const double desired = std::atan2(dz, dx);
    const double current = std::atan2(s.fwd[2], s.fwd[0]);
    const double error = WrapPi(desired - current);

    // A lateral term that grows without limit simply saturates the stick when the bike is far from
    // the line - 35 units off gave 1.4 of steer on its own - so it is capped at a gentle nudge.
    const double lateral = Clamp(g_projection.crossTrack, -8.0, 8.0);
    // Counter-steer against the rate the bike is already rotating at. Without it the loop is purely
    // proportional: when the rear steps out nothing arrests the yaw, so it keeps rotating and goes
    // down (measured at 32 units/s: upright 0.01, progress running backwards).
    const double yawRate = s.yawRateDeg * (kPi / 180.0);
    const double steer = cfg.steerSign * (cfg.steerGain * error + cfg.crossGain * lateral) -
                         cfg.yawDamp * yawRate;
    a.steer = Clamp(steer, -1.0, 1.0);

    // Hold the speed the line allows. The front wheel does the stopping and a little rear brake
    // trails it, so the bike keeps rotating into the bend instead of standing up and running wide.
    double target = SpeedTarget(cfg);

    // Being off the line is a speed problem before it is a steering problem. Every resume after a
    // crash started 30 to 55 units off the line and asked for full throttle, and the bike went
    // down again within two seconds: once the rear is spinning there is no grip left to turn back.
    // Grip scales with (1 + (offset / tolerance)^2), so 6 units off halves the target and 20 units
    // off leaves little more than walking pace.
    const double offLine = std::fabs(g_projection.crossTrack);
    const double ratio = offLine / (cfg.lineTolerance > 0.1 ? cfg.lineTolerance : 6.0);
    const double recovery = cfg.targetSpeed / (1.0 + ratio * ratio);
    if (recovery < target)
        target = recovery;

    g_lastTarget = target;
    const double speedError = target - s.forwardSpeed;
    if (speedError > 0.0) {
        a.throttle = Clamp(speedError * cfg.speedGain, 0.0, ThrottleCeiling(cfg, s));
    } else {
        const double brake = Clamp(-speedError * cfg.speedGain, 0.0, 1.0);
        a.brakeFront = brake;
        a.brakeRear = brake * cfg.rearBrakeShare;
    }
    a.lean = Clamp(cfg.leanSign * cfg.leanGain * (s.pitchDeg > 0.0 ? s.pitchDeg : 0.0), 0.0, 1.0);
    return a;
}

Action PolicyAction(const State& s, const Action& previous)
{
    const Config& cfg = GetConfig();
    double obs[kObsSize];
    obs[0]  = s.forwardSpeed / cfg.refSpeed;
    obs[1]  = s.speed / cfg.refSpeed;
    obs[2]  = s.vel[0] / cfg.refSpeed;
    obs[3]  = s.vel[1] / cfg.refSpeed;
    obs[4]  = s.vel[2] / cfg.refSpeed;
    obs[5]  = s.accelG;
    obs[6]  = s.pitchDeg / 45.0;
    obs[7]  = s.rollDeg / 45.0;
    obs[8]  = s.yawRateDeg / 180.0;
    obs[9]  = s.airborne ? 1.0 : 0.0;
    obs[10] = previous.throttle;
    obs[11] = previous.brakeFront;
    obs[12] = previous.brakeRear;
    obs[13] = previous.steer;
    obs[14] = previous.lean;
    obs[15] = s.progress;

    double out[6] = { 0, 0, 0, 0, 0, 0 };
    if (!g_policy.Eval(obs, out))
        return Action{};

    Action a;
    a.throttle   = out[0] > 0.0 ? out[0] : 0.0;
    a.brakeFront = out[1] > 0.0 ? out[1] : 0.0;
    a.brakeRear  = out[2] > 0.0 ? out[2] : 0.0;
    a.steer      = out[3];
    a.lean       = out[4];
    a.clutch     = out[5] > 0.0 ? out[5] : 0.0;
    return a;
}

double RewardFor(const State& s, const Action& a)
{
    const Config& cfg = GetConfig();
    // Forward progress is the point; being slow, or lying on the ground, is worth less; using the
    // bars a lot costs a little, which keeps the first learned policies smooth.
    const double speed = Clamp(s.forwardSpeed / cfg.refSpeed, -1.0, 2.0);
    const double uprightPenalty = s.upright < 0.7 ? (0.7 - s.upright) * 2.0 : 0.0;
    return 0.01 + speed - uprightPenalty - 0.02 * std::fabs(a.steer);
}

void ReloadPolicy()
{
    const Config& cfg = GetConfig();
    wchar_t path[1024];
    ResolveDataPath(cfg.policyFile, path, _countof(path));
    g_policyTried = true;
    if (!g_policy.Load(path) && g_mode == Mode::Policy)
        Log("rider: mode is policy but no usable weights; falling back to the baseline if it can");
}

void AttachPadIfDriving()
{
    if (pad::Attached())
        return;
    if (!pad::Attach()) {
        if (!g_padWarned) {
            g_padWarned = true;
            Log("rider: %s - the rider cannot drive, the game is untouched", pad::StatusText());
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void Frame()
{
    if (!g_initialised) {
        const Config& cfg = GetConfig();
        QueryPerformanceFrequency(&g_frequency);
        QueryPerformanceCounter(&g_lastSwap);
        g_enabled = cfg.enabled;
        g_mode = cfg.startMode;
        g_initialised = true;
        telemetry::OpenLog();
        ReloadData();
        Log("rider: ready (mode=%s, enabled=%d, line=%s, pad=%s)", ModeName(g_mode),
            g_enabled ? 1 : 0, line::Loaded() ? "loaded" : "none", pad::StatusText());
    }

    const Config& cfg = GetConfig();
    const double dt = Clamp(SecondsSinceLastSwap(), 0.0, 0.25);
    g_gameTime += dt;
    bike::EndFrame(g_gameTime, dt);
    ++g_frameNumber;

    // While nothing is locked, say why every ~5 s: no camera, no objects, or objects that never
    // settle. Without this the only symptom of a broken capture is silence in the log.
    if (!bike::Current().tracked && g_frameNumber % 300 == 0)
        Log("sense: camera=%d observed=%d candidates=%d tracks=%d (%s), mode=%s",
            uniforms::FrameCaptured() ? 1 : 0, uniforms::ObservedThisFrame(),
            bike::CandidateCount(), bike::TrackCount(), bike::StatusText(), ModeName(g_mode));

    State state;
    BuildState(bike::Current(), g_gameTime, dt, state);

    // Progress and laps come from the recorded line, when there is one.
    g_haveProjection = false;
    if (line::Loaded() && state.valid) {
        // Pure pursuit needs a target that moves with the bike. A fixed 20-unit lookahead is a
        // reasonable aim for a bike at speed, but from a standstill it points metres off to the
        // side, which saturates the steering (measured: 79% of frames at full lock, mean +0.82).
        const double lookahead = Clamp(6.0 + 0.70 * state.forwardSpeed, 6.0, cfg.lookahead);
        g_projection = line::Project(state.pos, lookahead);
        g_haveProjection = g_projection.valid;
        if (g_projection.valid)
            state.progress = g_projection.progress;
        if (line::LapCompleted()) {
            g_lapsSeen = line::Laps();
            Log("rider: lap %d done (episode %d, %.1f s)", line::Laps() - g_lapAtStart + 1,
                g_episode, g_episodeTime);
        }
    }

    // ---- Waiting for a reset ----------------------------------------------------------
    // The pad cannot reset a fallen bike (that is a keyboard binding), so after a crash the only
    // honest thing to do is hold nothing - not sit at full throttle on a bike lying on its side -
    // until the bike is upright again. The keyboard is the way out: F7 or F6 clears this.
    if (g_waitingForUpright) {
        // Hold everything until the bike is properly upright and has stayed there. Resuming at 0.6
        // was early: that is still a leaning bike, so the rider was handed throttle while the bike
        // was going over, which is exactly when it dabs a foot, sits down and falls again.
        pad::Release();
        g_lastSent = Action{};
        const bool upright = state.valid && state.upright > cfg.resumeUpright;
        g_uprightTime = upright ? g_uprightTime + dt : 0.0;
        if (upright && g_uprightTime >= cfg.resumeSettle) {
            Log("rider: bike upright again (%.2f, held %.2fs), driving resumes",
                state.upright, g_uprightTime);
            g_waitingForUpright = false;
            g_uprightTime = 0.0;
        } else if (state.valid && (g_frameNumber % 30) == 0) {
            telemetry::WriteRow(0, Source::Idle, state, Action{}, 0.0, false, "waiting");
        }
        return;
    }

    const bool wantsDriving = g_enabled && (g_mode == Mode::Baseline || g_mode == Mode::Policy);

    // ---- Not driving: hands off, but still useful data -------------------------------
    if (!wantsDriving) {
        // Never detach while the game runs: it enumerated controllers at startup, so a pad
        // created later is invisible to it. Attached and centred is already "doing nothing".
        pad::Release();
        g_lastSent = Action{};
        EndEpisode("rider off");

        if (g_mode == Mode::RecordLine && state.valid) {
            // The human is driving: log their own controller alongside the bike state, which is
            // the imitation data the trainer starts from.
            line::RecordPosition(state.pos);
            Action human;
            const bool haveHuman = pad::ReadHuman(human);
            telemetry::WriteRow(0, Source::Human, state, haveHuman ? human : Action{}, 0.0, false, "-");
        } else if (state.valid &&
                   (g_frameNumber % 30) == 0) {  // a slow pulse of state while idle
            telemetry::WriteRow(0, Source::Idle, state, Action{}, 0.0, false, "-");
        }
        return;
    }

    // ---- Driving ---------------------------------------------------------------------
    if (!state.valid) {
        // No bike this frame: release the controls, and end the episode if it stays that way.
        pad::Release();
        g_lastSent = Action{};
        g_lostTime += dt;
        if (g_episodeOpen && g_lostTime > kLostSeconds)
            EndEpisode("bike lost");
        return;
    }

    AttachPadIfDriving();
    if (!pad::Attached()) {
        pad::Release();
        g_lastSent = Action{};
        return;
    }

    if (!g_episodeOpen)
        StartEpisode("driving");
    g_episodeTime += dt;
    ++g_episodeFrames;

    // A respawn is a jump the bike cannot make.
    if (g_havePreviousPos) {
        const double dx = state.pos[0] - g_previousPos[0];
        const double dy = state.pos[1] - g_previousPos[1];
        const double dz = state.pos[2] - g_previousPos[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > cfg.teleportDistance)
            EndEpisode("respawn");
    }
    g_previousPos[0] = state.pos[0];
    g_previousPos[1] = state.pos[1];
    g_previousPos[2] = state.pos[2];
    g_havePreviousPos = true;

    // Choose the controller. Policy falls back to the baseline when the weights are unusable,
    // and to nothing when there is no line either.
    Action action;
    Source source = Source::Idle;
    if (g_mode == Mode::Policy && g_policy.Loaded() && g_policy.ObsSize() == kObsSize) {
        action = PolicyAction(state, g_previous);
        source = Source::Policy;
    } else if (line::Loaded()) {
        // Also the fallback when the mode is policy but the weights were unusable: the log then
        // shows baseline rows, so the trainer cannot mistake a fallback for learned driving.
        action = BaselineAction(state);
        source = Source::Baseline;
    } else {
        action = Action{};
    }

    action = action.Sanitized();
    action = RateLimited(action, g_lastSent, dt, cfg);
    action = action.Sanitized();
    pad::Submit(action);
    g_lastSent = action;

    // A slow pulse of what the rider is actually doing, so a session can be judged from the log.
    if (state.time - g_lastStatusLog >= 5.0) {
        g_lastStatusLog = state.time;
        Log("rider: %.1f units/s (line allows %.1f), upright %.2f, %+.1f off the line",
            state.forwardSpeed, g_lastTarget, state.upright,
            g_projection.valid ? g_projection.crossTrack : 0.0);
    }

    // Reward and episode end conditions.
    const double reward = RewardFor(state, action);
    g_episodeReturn += reward;

    const bool upsideDown = state.upright < 0.25;
    g_upsideDownTime = upsideDown ? g_upsideDownTime + dt : 0.0;

    const char* ended = nullptr;
    if (g_upsideDownTime > kCrashSeconds) {
        g_episodeReturn -= kCrashPenalty;
        ended = "crash";
        g_waitingForUpright = true;
        g_uprightTime = 0.0;
    } else if (g_episodeTime > cfg.maxEpisodeSeconds) {
        ended = "time";
    } else if (!g_episodeOpen) {
        ended = g_pendingReason && g_pendingReason[0] ? g_pendingReason : "ended";
    }

    telemetry::WriteRow(g_episode, source, state, action, reward, ended != nullptr,
                        ended ? ended : "-");
    g_previous = action;

    if (ended)
        EndEpisode(ended);
}

void Shutdown()
{
    if (pad::Attached()) {
        pad::Release();
        pad::Detach();
    }
    EndEpisode("shutdown");
    telemetry::CloseLog();
}

void ToggleEnabled()
{
    g_enabled = !g_enabled;
    if (g_enabled) {
        g_waitingForUpright = false;  // an explicit toggle is a request to try again
        // Turning the rider on while the mode is off is a request to drive. Without this the key
        // looks broken - it has stranded two sessions - because mode=off has nothing to drive.
        // Recording is never started this way; only the baseline is.
        if (g_mode == Mode::Off && line::Loaded())
            g_mode = Mode::Baseline;
    }
    Log("rider: %s (mode=%s)", g_enabled ? "ON" : "OFF", ModeName(g_mode));
    if (!g_enabled) {
        pad::Release();
        g_lastSent = Action{};
        g_havePreviousPos = false;
    }
}

void CycleMode()
{
    g_waitingForUpright = false;
    // off -> baseline -> record-line -> policy -> off. Recording is only ever started on purpose,
    // so cycling never silently wipes the line file.
    switch (g_mode) {
    case Mode::Off:        g_mode = Mode::Baseline;   break;
    case Mode::Baseline:   g_mode = Mode::RecordLine; break;
    case Mode::RecordLine: g_mode = Mode::Policy;     break;
    default:               g_mode = Mode::Off;        break;
    }

    if (g_mode == Mode::Policy && !g_policy.Loaded() && g_policyTried)
        ReloadPolicy();

    Log("rider: mode = %s", ModeName(g_mode));
    bike::Reset();
    g_havePreviousPos = false;
    if (g_mode == Mode::Baseline || g_mode == Mode::Policy) {
        g_enabled = true;
        AttachPadIfDriving();
    } else {
        pad::Release();
        g_lastSent = Action{};
    }
    EndEpisode("mode change");
}

void ToggleLineRecording()
{
    if (line::Recording()) {
        if (line::StopRecording()) {
            g_mode = Mode::Baseline;
            Log("rider: line saved, mode = baseline");
        } else {
            Log("rider: line recording stopped without a usable file");
        }
        return;
    }
    g_mode = Mode::RecordLine;
    pad::Release();
    line::StartRecording();
    Log("rider: mode = record-line; drive a lap, then press the line key again to save");
}

void ReloadData()
{
    const Config& cfg = GetConfig();

    wchar_t path[1024];
    ResolveDataPath(cfg.lineFile, path, _countof(path));
    if (!line::Recording())
        line::Load(path);

    ReloadPolicy();

    Log("rider: data reloaded (line=%d points, policy=%s)", line::Count(),
        g_policy.Loaded() ? "loaded" : "not loaded");
}

bool Active() { return pad::Attached(); }
Mode CurrentMode() { return g_mode; }
bool Enabled() { return g_enabled; }
int  Episode() { return g_episode; }
int  Laps() { return line::Laps(); }

}  // namespace rider
}  // namespace mxrider
