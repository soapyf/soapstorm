# -*- cmake -*-
# Dear ImGui v1.92.9-docking (MIT, see LICENSE.txt), vendored unmodified for the in-viewer VATs
# editor UI: the core files plus the OpenGL3 renderer backend. The viewer feeds input itself
# (newview/fsvatsimgui.cpp); no platform backend is built.

project(imgui)

include(00-Common)

add_library(imgui STATIC
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/backends)
# Vendored code: its warnings are not ours (the viewer builds with -Werror and /WX).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(imgui PRIVATE -w)
elseif(MSVC)
    target_compile_options(imgui PRIVATE /W0)
endif()
# imgl3w, the backend's GL loader, dlopens libGL/libOpenGL at run time.
if(UNIX AND NOT APPLE)
    target_link_libraries(imgui PRIVATE ${CMAKE_DL_LIBS})
endif()
