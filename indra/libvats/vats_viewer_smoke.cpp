// libvats smoke test for viewer builds: build a clip, write it as .anim, read it back.
// Copyright (C) 2026 Viewport Avatar Toolset contributors. LGPL-2.1, see LICENSE.
#include <cstdio>

#include "vats/anim_convert.h"
#include "vats/anim_file.h"
#include "vats/edit.h"

int main() {
    // A minimal two-joint skeleton, so the test needs no data files.
    const char* skeleton =
        R"(<linden_skeleton num_bones="2" num_collision_volumes="0" version="2.0">
<bone name="mPelvis" pos="0 0 1.067" rot="0 0 0" scale="1 1 1" pivot="0 0 1.067" connected="false" support="base" group="Torso">
<bone name="mTorso" pos="0 0 0.084" rot="0 0 0" scale="1 1 1" pivot="0 0 0.084" connected="true" support="base" group="Torso"/>
</bone></linden_skeleton>)";
    const char* lad = R"(<linden_avatar version="1.0"><skeleton file_name="avatar_skeleton.xml"></skeleton></linden_avatar>)";
    vats::Skeleton skel;
    std::string err;
    if (!skel.load(skeleton, lad, err)) return std::fprintf(stderr, "libvats: skeleton: %s\n", err.c_str()), 1;
    vats::Clip clip;
    clip.fps = 30;
    clip.end_frame = 30;
    vats::key_euler(clip, "mTorso", 0, {0, 0, 0});
    vats::key_euler(clip, "mTorso", 30, {0, 0, 45});
    auto out = vats::export_anim(skel, clip, {});
    if (!out.errors.empty()) return std::fprintf(stderr, "libvats: export: %s\n", out.errors[0].c_str()), 1;
    const std::vector<std::uint8_t> bytes = vats::write_anim(out.file);
    vats::AnimFile back;
    if (!vats::parse_anim(bytes, back, err)) return std::fprintf(stderr, "libvats: parse: %s\n", err.c_str()), 1;
    std::printf("libvats OK: %zu bytes, %zu joint(s)\n", bytes.size(), back.joints.size());
    return back.joints.size() == 1 ? 0 : 1;
}
