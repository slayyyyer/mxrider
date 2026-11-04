// uniforms.h
//   Observation of the shader path. MX Simulator draws every object with a shader whose
//   'u_modelview' uniform receives View * Model, computed on the CPU and uploaded with
//   glUniformMatrix4fv. Reading those uploads is how this mod learns where the bike is - it never
//   changes them.
//
//   Per frame:
//     - the terrain shader (the program that owns the terrain uniform, 'u_heights') is drawn with
//       Model = identity, so its first u_modelview upload is the camera's view matrix;
//     - every other rigid u_modelview upload after that is a candidate for the rider's own bike.
//
//   If the terrain uniform never shows up (a different game version, or a shader edit), the first
//   rigid upload that carries a translation is taken as the view instead, which is what the
//   reference mod does as its fallback.
#pragma once

namespace mxrider {
namespace uniforms {

// Called by the frame boundary before new drawing starts.
void BeginFrame();

// Called at the swap. Returns true if a camera view matrix was captured this frame (so the bike
// tracker had something to work from).
bool EndShaderFrame();

// True once a frame has captured a camera view since the last BeginFrame().
bool FrameCaptured();

// The raw View * Model of the last rigid object observed (for logging).
int ObservedThisFrame();

}  // namespace uniforms
}  // namespace mxrider
