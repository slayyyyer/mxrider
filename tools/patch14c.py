# Remainder of the screen-centre patch set. uniforms.cpp's EndShaderFrame is already done; these
# are the ones the two aborted scripts never reached.
import pathlib
import sys


def patch(path, old, new):
    p = pathlib.Path(path)
    t = p.read_text(encoding="utf-8")
    if t.count(old) != 1:
        print("FAIL %s: anchor %dx" % (path, t.count(old)))
        sys.exit(1)
    p.write_text(t.replace(old, new), encoding="utf-8")
    print("ok %s" % path)


patch("mxrider/src/uniforms.cpp",
      "void BeginFrame()\n"
      "{\n"
      "    g_frameHasView = false;\n"
      "    g_observed = 0;\n"
      "    g_uploads.clear();\n"
      "    g_viewUpload = -1;\n"
      "}",
      "void BeginFrame()\n"
      "{\n"
      "    g_frameHasView = false;\n"
      "    g_observed = 0;\n"
      "    g_uploads.clear();\n"
      "    g_viewUpload = -1;\n"
      "    g_haveCameraProjection = false;\n"
      "}")

# ---- config: the projection uniform's name ------------------------------------------------
patch("mxrider/src/config.h",
      '    char boneUniform[64]      = "u_bone_t";     // a uniform only skinned models (bikes) have',
      '    char boneUniform[64]      = "u_bone_t";     // a uniform only skinned models (bikes) have\n'
      '    char projectionUniform[64] = "u_projection"; // the camera projection, for screen positions')

patch("mxrider/src/config.cpp",
      '    ReadString(L"Detection", L"BoneUniform", c.boneUniform);',
      '    ReadString(L"Detection", L"BoneUniform", c.boneUniform);\n'
      '    ReadString(L"Detection", L"ProjectionUniform", c.projectionUniform);')

patch("mxrider/mxrider.ini",
      "BoneUniform=u_bone_t     ; a uniform only the skinned bike / rider models have",
      "BoneUniform=u_bone_t     ; a uniform only the skinned bike / rider models have\n"
      "ProjectionUniform=u_projection   ; the camera projection, for where things land on screen")

# ---- bike: a candidate must be what the camera is looking at ------------------------------
patch("mxrider/src/bike.h",
      "// (View * Model, before our correction). program is only used for logging.\n"
      "void Observe(unsigned program, const float viewModel[16]);",
      "// (View * Model, before our correction). program is only used for logging.\n"
      "// centre: distance from the middle of the screen in NDC units, negative when unknown. A\n"
      "// chase camera keeps the rider's own bike centred.\n"
      "void Observe(unsigned program, const float viewModel[16], double centre);")

patch("mxrider/src/bike.cpp",
      "    unsigned    program;\n"
      "    int         order;        // draw order within the frame\n"
      "    bool        used;\n"
      "};",
      "    unsigned    program;\n"
      "    int         order;        // draw order within the frame\n"
      "    double      centre;       // NDC distance from the middle of the screen (negative: unknown)\n"
      "    bool        used;\n"
      "};")

patch("mxrider/src/bike.cpp",
      "    double   viewModel[16];\n"
      "    unsigned program;\n"
      "    int      order;\n"
      "};",
      "    double   viewModel[16];\n"
      "    unsigned program;\n"
      "    int      order;\n"
      "    double   centre;                   // NDC distance from the middle of the screen\n"
      "};")

patch("mxrider/src/bike.cpp",
      "constexpr int    kMaxLogLines   = 40;",
      "constexpr int    kMaxLogLines   = 40;\n"
      "// How far from the middle of the screen the rider's own bike may be, in NDC units (the screen\n"
      "// spans -1..1 each way). A chase camera puts it near 0; other riders are off to the side.\n"
      "constexpr double kMaxCentre     = 0.55;")

patch("mxrider/src/bike.cpp",
      "void Observe(unsigned program, const float viewModel[16])\n"
      "{\n"
      "    if (!g_frameOpen || g_candidates.size() >= kMaxCandidates)\n"
      "        return;\n"
      "    const Vec3 t{ viewModel[12], viewModel[13], viewModel[14] };  // object origin in camera space\n"
      "    if (Length(t) > GetConfig().bikeSearchRadius)\n"
      "        return;\n",
      "void Observe(unsigned program, const float viewModel[16], double centre)\n"
      "{\n"
      "    if (!g_frameOpen || g_candidates.size() >= kMaxCandidates)\n"
      "        return;\n"
      "    const Vec3 t{ viewModel[12], viewModel[13], viewModel[14] };  // object origin in camera space\n"
      "    if (Length(t) > GetConfig().bikeSearchRadius)\n"
      "        return;\n"
      "    // The chase camera follows our bike, so it sits on the middle of the screen while the\n"
      "    // other riders are off to the side. They slide against the view by units a second, and\n"
      "    // mixing their meshes up with ours is what made the lock useless before.\n"
      "    if (centre > kMaxCentre)\n"
      "        return;\n")

patch("mxrider/src/bike.cpp",
      "    c.program   = program;\n"
      "    c.order     = static_cast<int>(g_candidates.size());\n"
      "    c.used      = false;",
      "    c.program   = program;\n"
      "    c.order     = static_cast<int>(g_candidates.size());\n"
      "    c.centre    = centre;\n"
      "    c.used      = false;")

patch("mxrider/src/bike.cpp",
      "        t.program  = c.program;\n"
      "        t.order    = c.order;",
      "        t.program  = c.program;\n"
      "        t.order    = c.order;\n"
      "        t.centre   = c.centre;")

patch("mxrider/src/bike.cpp",
      "        t->program  = best->program;\n"
      "        t->order    = best->order;",
      "        t->program  = best->program;\n"
      "        t->order    = best->order;\n"
      "        t->centre   = best->centre;")

patch("mxrider/src/bike.cpp",
      '        Log("  track %d%s: program %u draw #%d, bounce %.0f mm, wobble %.1f deg, alive %.1f s, "\n'
      '            "travel %.1f, camera at (%.2f, %.2f, %.2f)",\n'
      '            t.id, t.id == g_lockedId ? " [BIKE]" : "", t.program, t.order,\n'
      "            QSpread(t) * 1000.0, AngleSpread(t), t.alive, t.travel,\n"
      "            t.meanQ.x, t.meanQ.y, t.meanQ.z);",
      '        Log("  track %d%s: program %u draw #%d, centre %.2f, bounce %.0f mm, wobble %.1f deg, "\n'
      '            "alive %.1f s, travel %.1f, camera at (%.2f, %.2f, %.2f)",\n'
      '            t.id, t.id == g_lockedId ? " [BIKE]" : "", t.program, t.order, t.centre,\n'
      "            QSpread(t) * 1000.0, AngleSpread(t), t.alive, t.travel,\n"
      "            t.meanQ.x, t.meanQ.y, t.meanQ.z);")

print("patched")
