# libvats: the animation core. No UI, no rendering, and no third-party dependencies except the
# vendored ufbx (third_party/ufbx, MIT) behind VATS_FBX and the public-domain audio decoders
# (third_party/dr_libs, third_party/stb), so it can be embedded in an SL viewer as well as the standalone app.
add_library(vats_core STATIC
    src/anim_convert.cpp
    src/audio.cpp
    src/audio_decoders.c
    src/anim_file.cpp
    src/avatar_mesh.cpp
    src/bvh.cpp
    src/clip.cpp
    src/curve_filter.cpp
    src/curve_ops.cpp
    src/dynamics.cpp
    src/idle.cpp
    src/overlap.cpp
    src/ragdoll.cpp
    src/time_edit.cpp
    src/dae.cpp
    src/fbx.cpp
    src/edit.cpp
    src/export_name.cpp
    src/sit_export.cpp
    src/file_library.cpp
    src/fcurve.cpp
    src/gltf.cpp
    src/json.cpp
    src/pose_ops.cpp
    src/pose_tools.cpp
    src/pose_presets.cpp
    src/project.cpp
    src/prop.cpp
    src/face_anim.cpp
    src/facecap.cpp
    src/footlock.cpp
    src/lint.cpp
    src/loop_tools.cpp
    src/loop_assist.cpp
    src/tween.cpp
    src/onion.cpp
    src/sl_preview.cpp
    src/mocap.cpp
    src/wiki.cpp
    src/retarget.cpp
    src/rig.cpp
    src/shape.cpp
    src/skeleton.cpp
    src/xml.cpp
)
add_library(vats::core ALIAS vats_core)
target_include_directories(vats_core PUBLIC include)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vats_core PRIVATE -Wall -Wextra -Wpedantic)
    # .anim codes must match the viewer bit for bit: no fused multiply-add in the quantiser.
    set_source_files_properties(src/anim_file.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
elseif(MSVC)
    target_compile_options(vats_core PRIVATE /W4)
    set_source_files_properties(src/anim_file.cpp PROPERTIES COMPILE_OPTIONS "/fp:precise")
endif()

# Audio decoding (spec 08 AU) through the vendored public-domain dr_libs and stb_vorbis, warnings off like ufbx.
enable_language(C)
target_include_directories(vats_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(src/audio_decoders.c PROPERTIES COMPILE_OPTIONS "-w")
elseif(MSVC)
    set_source_files_properties(src/audio_decoders.c PROPERTIES COMPILE_OPTIONS "/W0")
endif()
if(NOT WIN32)
    target_link_libraries(vats_core PRIVATE m)
endif()

# Legacy import: another app's project and library files (vats/legacy_import.h, ui/migrate.cpp and
# tests/test_legacy_import.cpp). On by default when those files are present, off where they are left out.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include/vats/legacy_import.h)
    option(VATS_LEGACY_IMPORT "Legacy project and library import" ON)
elseif(VATS_LEGACY_IMPORT)
    message(FATAL_ERROR "VATS_LEGACY_IMPORT is on, but this source tree has no legacy import files")
endif()
if(VATS_LEGACY_IMPORT)
    target_compile_definitions(vats_core PUBLIC VATS_LEGACY_IMPORT)
endif()

# FBX import (spec 07 RT-2.3, 08 BD-1) through the vendored ufbx.
option(VATS_FBX "Read FBX files with the vendored ufbx" ON)
if(VATS_FBX)
    enable_language(C)
    # third_party/ sits beside core/ both in the VATs repo and in a viewer's indra/libvats copy.
    set(UFBX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ufbx)
    target_sources(vats_core PRIVATE ${UFBX_DIR}/ufbx.c)
    target_include_directories(vats_core PRIVATE ${UFBX_DIR})
    target_compile_definitions(vats_core PRIVATE VATS_FBX)
    # Vendored code: its warnings are not ours (the viewer builds with -Werror and /WX).
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(${UFBX_DIR}/ufbx.c PROPERTIES COMPILE_OPTIONS "-w")
    elseif(MSVC)
        set_source_files_properties(${UFBX_DIR}/ufbx.c PROPERTIES COMPILE_OPTIONS "/W0")
    endif()
    if(NOT WIN32)
        target_link_libraries(vats_core PRIVATE m)
    endif()
endif()
