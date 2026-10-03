# vats_ui: every panel, menu, tool window, dialog and document operation of the editor, shared by the
# standalone app and an SL viewer. It talks to its host through ui::Host (host.h) and never to
# SDL or a renderer; see docs/spec/09 section 0b.
# A viewer build (tools/sync-to-viewer.sh) sets VATS_VIEWER and brings its own imgui target (the same
# release, vendored in the viewer tree, OpenGL 3 backend included).
if(NOT VATS_VIEWER)
    include(FetchContent)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9-docking
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(imgui)
    set(VATS_IMGUI_DIR ${imgui_SOURCE_DIR} CACHE INTERNAL "Dear ImGui sources, for the hosts' backends")

    # Dear ImGui itself, no backend: both hosts use it. The OpenGL 3 backend is its own target (the app
    # and the viewer both draw with it); each host brings its own platform glue (the app: imgui_impl_sdl3).
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp)
    target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)

    add_library(imgui_opengl3 STATIC ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_link_libraries(imgui_opengl3 PUBLIC imgui ${CMAKE_DL_LIBS})  # its own GL loader (dlopen)
endif()

add_library(vats_ui STATIC
    actors_ui.cpp
    sit_export_ui.cpp
    height_variant_ui.cpp
    clips_ui.cpp
    app.cpp
    audio_track.cpp
    bodies.cpp
    context_menu.cpp
    dynamics_ui.cpp
    soft_body_ui.cpp
    idle_ui.cpp
    overlap_ui.cpp
    balance_ui.cpp
    firewall.cpp
    gizmo.cpp
    curve_filter_ui.cpp
    motion_quality_ui.cpp
    simplify_ui.cpp
    dope_sheet.cpp
    graph_editor.cpp
    hand_poser.cpp
    file_library_ui.cpp
    help_ui.cpp
    icon_button.cpp
    widgets.cpp
    inventory.cpp
    light_ui.cpp
    listing_media_ui.cpp
    lint_ui.cpp
    rig_export_ui.cpp
    rig_map_ui.cpp
    rig_scratch_ui.cpp
    weight_paint_ui.cpp
    loop_ui.cpp
    loop_assist_ui.cpp
    footlock_ui.cpp
    pinned_ghosts_ui.cpp
    pose_match_ui.cpp
    pose_tools_ui.cpp
    mocap_ui.cpp
    modal.cpp
    motion_path_ui.cpp
    panels.cpp
    props.cpp
    reference_ui.cpp
    ragdoll_ui.cpp
    face_ui.cpp
    expression_pack_ui.cpp
    lip_sync_ui.cpp
    tween_ui.cpp
    key_tags_ui.cpp
    dock_layout.cpp
    workspaces.cpp
    workspace_ui.cpp
    pie.cpp
    pie_menu_ui.cpp
    keymap.cpp
    shortcuts_ui.cpp
    picker_ui.cpp
    retarget_ui.cpp
    retime_ui.cpp
    batch_retarget_ui.cpp
    priority_plan_ui.cpp
    viewer_tools_ui.cpp
    face_cam_ui.cpp
    sl_preview_ui.cpp
    settings.cpp
    theme.cpp
    udp.cpp
    view_cube.cpp
    viewport.cpp
    suggest_limits_ui.cpp
    tool_search.cpp
    undo_history_ui.cpp
    joint_limits_ui.cpp
    windows.cpp)
add_library(vats::ui ALIAS vats_ui)
if(VATS_LEGACY_IMPORT)
    target_sources(vats_ui PRIVATE migrate.cpp)  # the first-run legacy import (core/CMakeLists.txt)
endif()
target_include_directories(vats_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vats_ui PUBLIC vats::core imgui)
if(WIN32)
    target_link_libraries(vats_ui PRIVATE ws2_32)  # motion capture sockets
endif()
# The version, from project(), as a generated header (the About window; the app's metadata too).
# The viewer's copy is not built under VATs' project(), so its sync passes the version in VATS_VERSION.
if(NOT VATS_VERSION)
    set(VATS_VERSION ${PROJECT_VERSION})
endif()
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/vats_version.h
     CONTENT "#pragma once\n#define VATS_VERSION \"@VATS_VERSION@\"\n")
target_include_directories(vats_ui PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
# No SDL: no_sdl/SDL3/ comes before the system headers, and the headers every SDL header includes stop
# the build there. So vats_ui builds only while it stays SDL-free, even where SDL is installed.
target_include_directories(vats_ui BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/no_sdl)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(vats_ui PRIVATE -Wall -Wextra)
endif()
