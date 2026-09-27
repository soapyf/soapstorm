# vats_ui: every panel, menu, tool window, dialog and document operation of the editor, shared by the
# standalone app and the SoapStorm viewer. It talks to its host through ui::Host (host.h) and never to
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
    app.cpp
    audio_track.cpp
    bodies.cpp
    context_menu.cpp
    dynamics_ui.cpp
    firewall.cpp
    gizmo.cpp
    graph_editor.cpp
    hand_poser.cpp
    help_ui.cpp
    inventory.cpp
    loop_ui.cpp
    migrate.cpp
    mocap_ui.cpp
    modal.cpp
    panels.cpp
    props.cpp
    ragdoll_ui.cpp
    retarget_ui.cpp
    settings.cpp
    theme.cpp
    udp.cpp
    view_cube.cpp
    viewport.cpp
    windows.cpp)
add_library(vats::ui ALIAS vats_ui)
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
