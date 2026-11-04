// config.h
//   Every tunable, loaded from mxrider.ini next to the DLL. Defaults match the shipped ini, so a
//   missing or half-written file still gives a working (and safe) rider.
//
//   Nothing here can make the mod write to the game, change physics, or press a control without
//   the virtual pad; these are detection thresholds, controller gains and file names.
#pragma once

namespace mxrider {

// How the rider decides what to press.
enum class Mode {
    Off,        // hands off: the pad (if attached) rests, nothing is driven
    Baseline,   // follow the recorded line with pure pursuit
    RecordLine, // the human drives; positions are appended to the line file, pad rests
    Policy,     // the learned network from the weights file drives
};

const char* ModeName(Mode mode);
bool        ParseMode(const char* name, Mode& out);

struct Config {
    // [General]
    bool enabled    = false;      // start with the rider driving?
    int  toggleKey  = 0x76;       // VK_F7: rider on / off
    int  modeKey    = 0x75;       // VK_F6: cycle off / baseline / record-line / policy
    int  lineKey    = 0x74;       // VK_F5: start / stop recording the line
    int  reloadKey  = 0x78;       // VK_F9: reload the ini, the line and the policy
    Mode startMode  = Mode::Off;

    // [Detection] shared with the reference hook; see mxcam/README for how it is found.
    char modelviewUniform[64] = "u_modelview";  // per-object View * Model
    char terrainUniform[64]   = "u_heights";    // a uniform only the terrain shader has
    char boneUniform[64]      = "u_bone_t";     // a uniform only skinned models (bikes) have
    char projectionUniform[64] = "u_projection"; // the camera projection, for screen positions
    double bikeSearchRadius   = 20.0;           // how far from the camera the rider's bike is drawn
    double teleportDistance   = 5.0;            // a jump this far is a respawn, not travel
    double worldUp[3]         = { 0.0, 1.0, 0.0 };

    // [Line] the baseline controller and the lap counter
    char   lineFile[260] = "mxrider_line.csv";
    double lookahead     = 20.0;  // units ahead on the line the steering aims at
    double targetSpeed   = 40.0;  // units/s the baseline tries to hold
    double steerGain     = 0.6;   // 1/rad of heading error
    double crossGain     = 0.04;  // 1/unit of sideways error
    double yawDamp       = 0.15;  // 1/(rad/s): counter-steer against the bike's own rotation
    double resumeUpright = 0.90;  // how upright the bike must be before the rider drives again
    double resumeSettle  = 0.30;  // ...and how long it must stay that way, seconds
    double steerSign     = 1.0;   // flip to -1 if steering turns the wrong way in game
    double leanSign      = 1.0;   // flip to -1 if leaning is inverted
    double leanGain      = 0.004; // lean per degree of nose-up pitch
    double lapRadius     = 8.0;   // how near the line's start counts as crossing it
    double speedGain     = 0.15;  // throttle / brake per unit of speed error

    // [Pad] which physical control of the virtual pad carries which game input. The defaults
    // match the layout documented in README.md; bind the game's controller settings to match.
    int    frontBrakeTrigger = 0;       // 0 = left trigger, 1 = right trigger
    int    clutchButton      = 0x1000;  // XUSB_GAMEPAD_A
    int    rearBrakeButton   = 0x2000;  // XUSB_GAMEPAD_B
    int    shiftUpButton     = 0x0200;  // XUSB_GAMEPAD_RB
    int    shiftDownButton   = 0x0100;  // XUSB_GAMEPAD_LB
    double stickDeadzone     = 0.02;

    // [Policy] the trained network the DLL evaluates
    char policyFile[260] = "mxrider_policy.txt";

    // [Log] the episode log the trainer reads
    bool   csv            = true;
    double csvFlushSeconds = 2.0;  // how often buffered rows hit the disk
    double refSpeed        = 30.0; // units/s that count as "1.0" in observations and reward
    int    maxEpisodeSeconds = 120;

    // Resolved once at load: where logs and data files live (the DLL's folder unless set).
    wchar_t dataDirectory[1024] = L"";
};

const Config& GetConfig();

// Reads mxrider.ini (the path is remembered from the first call) and clamps everything.
void LoadConfig(const wchar_t* iniPath);
void ReloadConfig();

// Where a data file named in the ini actually is: absolute paths are used as they are,
// relative ones are looked for in the data directory.
void ResolveDataPath(const char* name, wchar_t* out, unsigned count);

}  // namespace mxrider
