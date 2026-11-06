// config.cpp
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

#include "config.h"
#include "log.h"
#include "mathlib.h"

namespace mxrider {
namespace {

Config  g_config;
wchar_t g_path[1024] = L"";

bool ReadRaw(const wchar_t* section, const wchar_t* key, wchar_t* buffer, DWORD size)
{
    GetPrivateProfileStringW(section, key, L"", buffer, size, g_path);
    return buffer[0] != L'\0';
}

double ReadDouble(const wchar_t* section, const wchar_t* key, double fallback)
{
    wchar_t buffer[128];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return fallback;
    wchar_t* end = nullptr;
    const double value = wcstod(buffer, &end);  // stops at a trailing "; comment"
    return end != buffer ? value : fallback;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int fallback)
{
    wchar_t buffer[128];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return fallback;
    wchar_t* end = nullptr;
    const long value = wcstol(buffer, &end, 0);  // base 0: 0x74 and 116 both work
    return end != buffer ? static_cast<int>(value) : fallback;
}

// A string value, cut at the first space, tab or ';' so a trailing comment can't leak in.
void ReadString(const wchar_t* section, const wchar_t* key, char (&value)[64])
{
    wchar_t buffer[256];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return;
    wchar_t* end = buffer;
    while (*end && *end != L' ' && *end != L'\t' && *end != L';')
        ++end;
    *end = L'\0';
    if (!WideCharToMultiByte(CP_UTF8, 0, buffer, -1, value, sizeof value, nullptr, nullptr))
        value[0] = '\0';
}

void ReadLongString(const wchar_t* section, const wchar_t* key, char (&value)[260])
{
    wchar_t buffer[260];
    if (!ReadRaw(section, key, buffer, _countof(buffer)))
        return;
    wchar_t* end = buffer;
    while (*end && *end != L'\t' && *end != L';')  // paths may contain spaces
        ++end;
    while (end > buffer && (end[-1] == L' ' || end[-1] == L'\t'))
        --end;
    *end = L'\0';
    if (!WideCharToMultiByte(CP_UTF8, 0, buffer, -1, value, sizeof value, nullptr, nullptr))
        value[0] = '\0';
}

void ReadVector(const wchar_t* section, const wchar_t* key, double (&value)[3])
{
    wchar_t buffer[128];
    double x, y, z;
    if (!ReadRaw(section, key, buffer, _countof(buffer)) ||
        swscanf_s(buffer, L"%lf , %lf , %lf", &x, &y, &z) != 3)
        return;
    const double length = std::sqrt(x * x + y * y + z * z);
    if (length < 1e-9)
        return;
    value[0] = x / length;
    value[1] = y / length;
    value[2] = z / length;
}

}  // namespace

const char* ModeName(Mode mode)
{
    switch (mode) {
    case Mode::Baseline:   return "baseline";
    case Mode::RecordLine: return "record-line";
    case Mode::Policy:     return "policy";
    default:               return "off";
    }
}

bool ParseMode(const char* name, Mode& out)
{
    if (!name || !name[0])
        return false;
    if (_stricmp(name, "off") == 0)         { out = Mode::Off;        return true; }
    if (_stricmp(name, "baseline") == 0)    { out = Mode::Baseline;   return true; }
    if (_stricmp(name, "record") == 0 ||
        _stricmp(name, "record-line") == 0) { out = Mode::RecordLine; return true; }
    if (_stricmp(name, "policy") == 0)      { out = Mode::Policy;     return true; }
    return false;
}

const Config& GetConfig() { return g_config; }

void ResolveDataPath(const char* name, wchar_t* out, unsigned count)
{
    if (!name || !name[0]) {
        out[0] = L'\0';
        return;
    }
    wchar_t wide[260];
    if (!MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, _countof(wide))) {
        out[0] = L'\0';
        return;
    }
    const bool absolute = (wide[0] == L'\\' && wide[1] == L'\\') || (wide[1] == L':' && wide[2] == L'\\');
    if (absolute)
        swprintf_s(out, count, L"%s", wide);
    else
        swprintf_s(out, count, L"%s%s", g_config.dataDirectory, wide);
}

void LoadConfig(const wchar_t* iniPath)
{
    wcscpy_s(g_path, iniPath);
    ReloadConfig();
}

void ReloadConfig()
{
    Config c;  // start from the defaults so keys missing from the file keep sane values

    // The data directory defaults to the DLL's own folder; the log already knows where that is.
    if (wcscpy_s(c.dataDirectory, LogDirectory()) != 0)
        c.dataDirectory[0] = L'\0';

    if (GetFileAttributesW(g_path) == INVALID_FILE_ATTRIBUTES) {
        Log("config: %ls not found, using built-in defaults", g_path);
        g_config = c;
        return;
    }

    c.enabled   = ReadInt(L"General", L"Enabled", c.enabled ? 1 : 0) != 0;
    c.toggleKey = ReadInt(L"General", L"ToggleKey", c.toggleKey);
    c.modeKey   = ReadInt(L"General", L"ModeKey", c.modeKey);
    c.lineKey   = ReadInt(L"General", L"LineKey", c.lineKey);
    c.reloadKey = ReadInt(L"General", L"ReloadKey", c.reloadKey);
    {
        char mode[64] = "";
        ReadString(L"General", L"Mode", mode);
        Mode parsed = c.startMode;
        if (ParseMode(mode, parsed))
            c.startMode = parsed;
        else if (mode[0])
            Log("config: Mode='%s' is not off / baseline / record-line / policy; using %s",
                mode, ModeName(c.startMode));
    }
    {
        wchar_t directory[1024];
        if (ReadRaw(L"General", L"LogDirectory", directory, _countof(directory))) {
            wchar_t* end = directory;
            while (*end && *end != L'\t' && *end != L';')
                ++end;
            while (end > directory && (end[-1] == L' ' || end[-1] == L'\t'))
                --end;
            *end = L'\0';
            if (directory[0]) {
                const size_t length = wcslen(directory);
                if (length + 1 < _countof(c.dataDirectory)) {
                    wcscpy_s(c.dataDirectory, directory);
                    if (c.dataDirectory[length - 1] != L'\\' && length + 2 < _countof(c.dataDirectory))
                        wcscat_s(c.dataDirectory, L"\\");
                }
            }
        }
    }

    ReadString(L"Detection", L"ModelviewUniform", c.modelviewUniform);
    ReadString(L"Detection", L"TerrainUniform", c.terrainUniform);
    ReadString(L"Detection", L"BoneUniform", c.boneUniform);
    ReadString(L"Detection", L"ProjectionUniform", c.projectionUniform);
    ReadVector(L"Detection", L"WorldUp", c.worldUp);
    c.bikeSearchRadius = ReadDouble(L"Detection", L"BikeSearchRadius", c.bikeSearchRadius);
    c.teleportDistance = ReadDouble(L"Detection", L"TeleportDistance", c.teleportDistance);

    ReadLongString(L"Line", L"File", c.lineFile);
    c.lookahead   = ReadDouble(L"Line", L"Lookahead", c.lookahead);
    c.targetSpeed = ReadDouble(L"Line", L"TargetSpeed", c.targetSpeed);
    c.steerGain   = ReadDouble(L"Line", L"SteerGain", c.steerGain);
    c.crossGain   = ReadDouble(L"Line", L"CrossGain", c.crossGain);
    c.yawDamp     = ReadDouble(L"Line", L"YawDamp", c.yawDamp);
    c.resumeUpright = ReadDouble(L"Line", L"ResumeUpright", c.resumeUpright);
    c.resumeSettle  = ReadDouble(L"Line", L"ResumeSettle", c.resumeSettle);
    c.steerSign   = ReadDouble(L"Line", L"SteerSign", c.steerSign);
    c.leanSign    = ReadDouble(L"Line", L"LeanSign", c.leanSign);
    c.leanGain    = ReadDouble(L"Line", L"LeanGain", c.leanGain);
    c.lapRadius   = ReadDouble(L"Line", L"LapRadius", c.lapRadius);
    c.speedGain   = ReadDouble(L"Line", L"SpeedGain", c.speedGain);
    c.cornerGrip  = ReadDouble(L"Line", L"CornerGrip", c.cornerGrip);
    c.brakeDecel  = ReadDouble(L"Line", L"BrakeDecel", c.brakeDecel);
    c.brakeReach  = ReadDouble(L"Line", L"BrakeReach", c.brakeReach);
    c.rearBrakeShare = ReadDouble(L"Line", L"RearBrakeShare", c.rearBrakeShare);
    c.maxSteerRate   = ReadDouble(L"Line", L"MaxSteerRate", c.maxSteerRate);
    c.maxThrottleRate = ReadDouble(L"Line", L"MaxThrottleRate", c.maxThrottleRate);
    c.minSpeed    = ReadDouble(L"Line", L"MinSpeed", c.minSpeed);
    c.lineTolerance = ReadDouble(L"Line", L"LineTolerance", c.lineTolerance);
    c.launchSpeed = ReadDouble(L"Line", L"LaunchSpeed", c.launchSpeed);
    c.launchThrottle = ReadDouble(L"Line", L"LaunchThrottle", c.launchThrottle);
    c.wheeliePitch = ReadDouble(L"Line", L"WheeliePitch", c.wheeliePitch);
    c.uprightThrottle = ReadDouble(L"Line", L"UprightThrottle", c.uprightThrottle);

    c.frontBrakeTrigger = ReadInt(L"Pad", L"FrontBrakeTrigger", c.frontBrakeTrigger);
    c.clutchButton      = ReadInt(L"Pad", L"ClutchButton", c.clutchButton);
    c.rearBrakeButton   = ReadInt(L"Pad", L"RearBrakeButton", c.rearBrakeButton);
    c.shiftUpButton     = ReadInt(L"Pad", L"ShiftUpButton", c.shiftUpButton);
    c.shiftDownButton   = ReadInt(L"Pad", L"ShiftDownButton", c.shiftDownButton);
    c.stickDeadzone     = ReadDouble(L"Pad", L"StickDeadzone", c.stickDeadzone);

    ReadLongString(L"Policy", L"File", c.policyFile);

    c.csv              = ReadInt(L"Log", L"Csv", c.csv ? 1 : 0) != 0;
    c.csvFlushSeconds  = ReadDouble(L"Log", L"FlushSeconds", c.csvFlushSeconds);
    c.refSpeed         = ReadDouble(L"Log", L"RefSpeed", c.refSpeed);
    c.maxEpisodeSeconds = ReadInt(L"Log", L"MaxEpisodeSeconds", c.maxEpisodeSeconds);

    // Keep everything inside a range that can't produce absurd controls.
    c.bikeSearchRadius  = Clamp(c.bikeSearchRadius, 0.5, 100.0);
    c.teleportDistance  = Clamp(c.teleportDistance, 0.5, 1e6);
    c.lookahead         = Clamp(c.lookahead, 1.0, 200.0);
    c.targetSpeed       = Clamp(c.targetSpeed, 1.0, 1000.0);
    c.steerGain         = Clamp(c.steerGain, 0.0, 20.0);
    c.crossGain         = Clamp(c.crossGain, 0.0, 5.0);
    c.yawDamp           = Clamp(c.yawDamp, 0.0, 2.0);
    c.resumeUpright     = Clamp(c.resumeUpright, 0.0, 1.0);
    c.resumeSettle      = Clamp(c.resumeSettle, 0.0, 5.0);
    c.steerSign         = c.steerSign < 0.0 ? -1.0 : 1.0;
    c.leanSign          = c.leanSign < 0.0 ? -1.0 : 1.0;
    c.leanGain          = Clamp(c.leanGain, 0.0, 1.0);
    c.lapRadius         = Clamp(c.lapRadius, 1.0, 200.0);
    c.speedGain         = Clamp(c.speedGain, 0.0, 50.0);
    c.cornerGrip        = Clamp(c.cornerGrip, 1.0, 400.0);
    c.brakeDecel        = Clamp(c.brakeDecel, 1.0, 400.0);
    c.brakeReach        = Clamp(c.brakeReach, 10.0, 1000.0);
    c.rearBrakeShare    = Clamp(c.rearBrakeShare, 0.0, 1.0);
    c.maxSteerRate      = Clamp(c.maxSteerRate, 0.5, 200.0);
    c.maxThrottleRate   = Clamp(c.maxThrottleRate, 0.2, 200.0);
    c.minSpeed          = Clamp(c.minSpeed, 0.0, 200.0);
    c.lineTolerance     = Clamp(c.lineTolerance, 0.5, 100.0);
    c.launchSpeed       = Clamp(c.launchSpeed, 0.0, 100.0);
    c.launchThrottle    = Clamp(c.launchThrottle, 0.05, 1.0);
    c.wheeliePitch      = Clamp(c.wheeliePitch, 1.0, 80.0);
    c.uprightThrottle   = Clamp(c.uprightThrottle, 0.05, 1.0);
    c.frontBrakeTrigger = c.frontBrakeTrigger != 0 ? 1 : 0;
    c.stickDeadzone     = Clamp(c.stickDeadzone, 0.0, 0.5);
    c.csvFlushSeconds   = Clamp(c.csvFlushSeconds, 0.25, 60.0);
    c.refSpeed          = Clamp(c.refSpeed, 1.0, 1000.0);
    c.maxEpisodeSeconds = static_cast<int>(Clamp(c.maxEpisodeSeconds, 5, 3600));

    g_config = c;
    Log("config: loaded %ls (mode=%s, modelview='%s', terrain='%s', up=%.2f,%.2f,%.2f)",
        g_path, ModeName(c.startMode), c.modelviewUniform, c.terrainUniform,
        c.worldUp[0], c.worldUp[1], c.worldUp[2]);
}

}  // namespace mxrider
