// uniforms.cpp
//   Wrappers for the OpenGL 2.0+ functions games fetch through wglGetProcAddress.
//
//   This file is the mod's eyes. It records which (program, location) pairs are the per-object
//   modelview uniform and which program draws the terrain, then watches the matrices go by.
//   The matrices are never modified: each wrapper passes the game's own pointer straight to the
//   real driver, after telling bike.cpp what it was. If anything looks wrong (a matrix that isn't
//   rigid, a program that never appeared) the observation is simply skipped - the game keeps
//   drawing exactly what it would have drawn without us.

#include <windows.h>
#include <GL/gl.h>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "bike.h"
#include "config.h"
#include "log.h"
#include "uniforms.h"

namespace mxrider {
namespace {

using GLchar = char;
using PFN_UseProgram         = void (APIENTRY*)(GLuint);
using PFN_GetUniformLocation = GLint (APIENTRY*)(GLuint, const GLchar*);
using PFN_UniformMatrix4fv   = void (APIENTRY*)(GLint, GLsizei, GLboolean, const GLfloat*);

PFN_UseProgram         real_glUseProgram         = nullptr;
PFN_UseProgram         real_glUseProgramObjectARB = nullptr;
PFN_GetUniformLocation real_glGetUniformLocation = nullptr;
PFN_GetUniformLocation real_glGetUniformLocationARB = nullptr;
PFN_UniformMatrix4fv   real_glUniformMatrix4fv   = nullptr;
PFN_UniformMatrix4fv   real_glUniformMatrix4fvARB = nullptr;

constexpr int kMaxLookupsLogged = 400;

// (program << 32 | location) -> uniform name, so a lookup can be recognised later.
std::unordered_map<unsigned long long, std::string> g_uniforms;
std::unordered_set<unsigned long long> g_modelviewUniforms;
std::unordered_set<unsigned long long> g_projectionUniforms;  // the camera's projection
std::array<float, 16> g_cameraProjection{};
bool g_haveCameraProjection = false;
std::unordered_set<GLuint>             g_terrainPrograms;  // programs that own the terrain uniform
std::unordered_set<GLuint>             g_skinnedPrograms;  // programs that draw skinned models (bikes)
bool                                   g_seenSkinned = false;
GLuint g_program = 0;
int    g_lookupsLogged = 0;

bool   g_frameHasView  = false;   // this frame already found the camera
bool   g_haveTerrain   = false;   // the terrain program has been seen at least once
int    g_observed      = 0;       // rigid objects observed this frame

// One rigid object upload, kept until the frame ends. The bike is drawn before the terrain, so
// the camera is usually only known after the bike has already gone past: buffering makes the draw
// order irrelevant instead of silently dropping whatever was drawn first.
struct Upload {
    unsigned program;
    float    model[16];
};
constexpr size_t kMaxUploads = 4096;
std::vector<Upload> g_uploads;
int g_viewUpload = -1;            // index in g_uploads of the matrix taken as the camera

unsigned long long Key(GLuint program, GLint location)
{
    return (static_cast<unsigned long long>(program) << 32) | static_cast<unsigned int>(location);
}

bool IsIdentity(const GLfloat* m)
{
    for (int i = 0; i < 16; ++i) {
        if (m[i] != (i % 5 == 0 ? 1.0f : 0.0f))
            return false;
    }
    return true;
}

// A rotation + translation with unit scale: world objects and the bike. HUD elements are scaled
// to fractions of a unit or flattened, so they fail this test and are ignored.
bool IsRigid(const GLfloat* m)
{
    // Every comparison against NaN is false, so a matrix the game never filled in would sail
    // through the tests below and put NaN positions into the tracker. Reject it first.
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i]))
            return false;
    }
    if (std::fabs(m[3]) > 1e-4f || std::fabs(m[7]) > 1e-4f ||
        std::fabs(m[11]) > 1e-4f || std::fabs(m[15] - 1.0f) > 1e-4f)
        return false;
    for (int a = 0; a < 3; ++a) {
        for (int b = a; b < 3; ++b) {
            const float dot = m[a * 4] * m[b * 4] + m[a * 4 + 1] * m[b * 4 + 1] +
                              m[a * 4 + 2] * m[b * 4 + 2];
            if (std::fabs(dot - (a == b ? 1.0f : 0.0f)) > 0.02f)
                return false;
        }
    }
    return true;
}

bool HasTranslation(const GLfloat* m)
{
    return std::fabs(m[12]) + std::fabs(m[13]) + std::fabs(m[14]) > 1e-3f;
}

void NoteUniformLocation(GLuint program, const GLchar* name, GLint location)
{
    if (!name || location < 0)
        return;
    const Config& cfg = GetConfig();
    const unsigned long long key = Key(program, location);

    const bool added = g_uniforms.emplace(key, name).second;
    if (strcmp(name, cfg.modelviewUniform) == 0)
        g_modelviewUniforms.insert(key);
    if (strcmp(name, cfg.projectionUniform) == 0)
        g_projectionUniforms.insert(key);
    if (strcmp(name, cfg.boneUniform) == 0) {
        g_skinnedPrograms.insert(program);
        g_seenSkinned = true;
    }
    if (strcmp(name, cfg.terrainUniform) == 0) {
        g_terrainPrograms.insert(program);
        g_haveTerrain = true;
    }
    if (added && g_lookupsLogged < kMaxLookupsLogged) {
        ++g_lookupsLogged;
        Log("uniform: program %u '%s' = location %d", program, name, location);
    }
}

// A few lines per (program, location) showing exactly what the game uploads. This is how the
// camera is confirmed on a game build, and how a rejected matrix explains itself instead of
// leaving the tracker permanently blind.
void LogMatrixVerdict(GLuint program, GLint location, GLsizei count, GLboolean transpose,
                      const GLfloat* v)
{
    static std::unordered_map<unsigned long long, int> seen;
    const unsigned long long key = Key(program, location);
    int& lines = seen[key];
    if (lines >= 4)
        return;
    ++lines;

    const auto name = g_uniforms.find(key);
    const double sx = std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1] + double(v[2]) * v[2]);
    const double sy = std::sqrt(double(v[4]) * v[4] + double(v[5]) * v[5] + double(v[6]) * v[6]);
    const double sz = std::sqrt(double(v[8]) * v[8] + double(v[9]) * v[9] + double(v[10]) * v[10]);
    Log("matrix: program %u loc %d '%s' count=%d transpose=%d rigid=%d identity=%d "
        "scale=%.4f,%.4f,%.4f origin=%.2f,%.2f,%.2f",
        program, location, name != g_uniforms.end() ? name->second.c_str() : "?",
        static_cast<int>(count), static_cast<int>(transpose), IsRigid(v) ? 1 : 0,
        IsIdentity(v) ? 1 : 0, sx, sy, sz, v[12], v[13], v[14]);
}

// The game is uploading one matrix for a per-object uniform. Decide what it is, tell the tracker,
// and then let the real driver have it unchanged.
void OnModelview(GLuint program, GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    if (!value)
        return;

    const unsigned long long key = Key(program, location);
    if (count == 1 && g_projectionUniforms.count(key) != 0 &&
        g_terrainPrograms.count(program) != 0 &&
        value[15] == 0.0f && std::fabs(value[11] + 1.0f) < 1e-3f) {
        std::memcpy(g_cameraProjection.data(), value, sizeof(float) * 16);
        g_haveCameraProjection = true;
    }
    if (g_modelviewUniforms.count(key) == 0)
        return;

    LogMatrixVerdict(program, location, count, transpose, value);

    if (count != 1 || transpose || !IsRigid(value) || IsIdentity(value))
        return;

    const bool isTerrain = g_terrainPrograms.count(program) != 0;

    // The camera: the terrain program's first rigid upload, or - when nothing is known about the
    // terrain - the first rigid upload that carries a translation.
    const bool couldBeView = g_haveTerrain ? isTerrain : true;
    if (!g_frameHasView && couldBeView && HasTranslation(value)) {
        g_frameHasView = true;
        g_viewUpload = static_cast<int>(g_uploads.size());
        double view[16];
        for (int i = 0; i < 16; ++i)
            view[i] = value[i];
        bike::BeginFrame(view);
    }

    // Keep every rigid object of the frame, camera or not; EndShaderFrame replays them once the
    // camera for this frame is known.
    if (g_uploads.size() < kMaxUploads) {
        Upload& upload = g_uploads.emplace_back();
        upload.program = program;
        std::memcpy(upload.model, value, sizeof(upload.model));
    }
    ++g_observed;
}

void APIENTRY Hook_glUseProgram(GLuint program)
{
    g_program = program;
    real_glUseProgram(program);
}

void APIENTRY Hook_glUseProgramObjectARB(GLuint program)
{
    g_program = program;
    real_glUseProgramObjectARB(program);
}

GLint APIENTRY Hook_glGetUniformLocation(GLuint program, const GLchar* name)
{
    const GLint location = real_glGetUniformLocation(program, name);
    NoteUniformLocation(program, name, location);
    return location;
}

GLint APIENTRY Hook_glGetUniformLocationARB(GLuint program, const GLchar* name)
{
    const GLint location = real_glGetUniformLocationARB(program, name);
    NoteUniformLocation(program, name, location);
    return location;
}

void APIENTRY Hook_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    OnModelview(g_program, location, count, transpose, value);
    real_glUniformMatrix4fv(location, count, transpose, value);
}

void APIENTRY Hook_glUniformMatrix4fvARB(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
    OnModelview(g_program, location, count, transpose, value);
    real_glUniformMatrix4fvARB(location, count, transpose, value);
}

}  // namespace

// Called by hooks.cpp's WrapProc for every wglGetProcAddress result.
void* WrapProc(const char* name, void* real)
{
    if (!name || !real)
        return real;

    if (strcmp(name, "glUseProgram") == 0) {
        real_glUseProgram = reinterpret_cast<PFN_UseProgram>(real);
        return reinterpret_cast<void*>(&Hook_glUseProgram);
    }
    if (strcmp(name, "glUseProgramObjectARB") == 0) {
        real_glUseProgramObjectARB = reinterpret_cast<PFN_UseProgram>(real);
        return reinterpret_cast<void*>(&Hook_glUseProgramObjectARB);
    }
    if (strcmp(name, "glGetUniformLocation") == 0) {
        real_glGetUniformLocation = reinterpret_cast<PFN_GetUniformLocation>(real);
        return reinterpret_cast<void*>(&Hook_glGetUniformLocation);
    }
    if (strcmp(name, "glGetUniformLocationARB") == 0) {
        real_glGetUniformLocationARB = reinterpret_cast<PFN_GetUniformLocation>(real);
        return reinterpret_cast<void*>(&Hook_glGetUniformLocationARB);
    }
    if (strcmp(name, "glUniformMatrix4fv") == 0) {
        real_glUniformMatrix4fv = reinterpret_cast<PFN_UniformMatrix4fv>(real);
        return reinterpret_cast<void*>(&Hook_glUniformMatrix4fv);
    }
    if (strcmp(name, "glUniformMatrix4fvARB") == 0) {
        real_glUniformMatrix4fvARB = reinterpret_cast<PFN_UniformMatrix4fv>(real);
        return reinterpret_cast<void*>(&Hook_glUniformMatrix4fvARB);
    }
    return real;
}

namespace uniforms {

void BeginFrame()
{
    g_frameHasView = false;
    g_observed = 0;
    g_uploads.clear();
    g_viewUpload = -1;
    g_haveCameraProjection = false;
}

bool EndShaderFrame()
{
    // The camera is known now, so hand this frame's objects to the tracker whatever order the game
    // drew them in. The camera's own upload is not an object.
    if (g_frameHasView) {
        for (size_t i = 0; i < g_uploads.size(); ++i) {
            if (static_cast<int>(i) == g_viewUpload)
                continue;
            // The rider's own bike and rider are skinned models; scenery, the sky and overlays
            // welded to the camera are not. Once a skinned model has been seen, follow only those:
            // a chase camera draws the bike a dozen units ahead, and the steadiest-object rule
            // would otherwise always pick whatever the game draws at the eye itself.
            if (g_seenSkinned && g_skinnedPrograms.count(g_uploads[i].program) == 0)
                continue;
            // Where this object lands on screen, as NDC distance from the middle. A chase
            // camera keeps the rider's own bike centred, which is a steadier fact than how
            // fast it slides against the camera. Negative when the projection is unknown.
            double centre = -1.0;
            if (g_haveCameraProjection) {
                const float* p = g_cameraProjection.data();
                const float* m = g_uploads[i].model;
                const double x = m[12], y = m[13], z = m[14];
                const double w = double(p[3]) * x + double(p[7]) * y +
                                 double(p[11]) * z + double(p[15]);
                if (w > 1e-4) {
                    const double cx = (double(p[0]) * x + double(p[4]) * y +
                                       double(p[8]) * z + double(p[12])) / w;
                    const double cy = (double(p[1]) * x + double(p[5]) * y +
                                       double(p[9]) * z + double(p[13])) / w;
                    centre = std::sqrt(cx * cx + cy * cy);
                }
            }
            bike::Observe(g_uploads[i].program, g_uploads[i].model, centre);
        }
    }
    g_uploads.clear();
    g_viewUpload = -1;
    return g_frameHasView;
}

bool FrameCaptured() { return g_frameHasView; }

int ObservedThisFrame() { return g_observed; }

}  // namespace uniforms

}  // namespace mxrider
