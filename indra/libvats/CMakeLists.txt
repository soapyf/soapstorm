# -*- cmake -*-
# libvats: the animation core of Viewport Avatar Toolset (LGPL-2.1-only, see LICENSE), vendored from the
# VATs repository by its tools/sync-to-viewer.sh. Edit it there, not here.

project(libvats)

include(00-Common)

add_subdirectory(core)  # target vats_core, alias vats::core

# Links the core with the viewer's flags and runs a tiny round trip, so a broken sync fails the build.
add_executable(vats_viewer_smoke vats_viewer_smoke.cpp)
target_link_libraries(vats_viewer_smoke PRIVATE vats::core)
add_custom_command(TARGET vats_viewer_smoke POST_BUILD COMMAND vats_viewer_smoke
                   COMMENT "Running the libvats smoke test")
