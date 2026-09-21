# -*- cmake -*-

include(Variables)
include(GLEXT)
include(Prebuilt)

include_guard()
add_library( ll::SDL INTERFACE IMPORTED )


if (LINUX)
  #Must come first as use_system_binary can exit this file early
  #target_compile_definitions( ll::SDL INTERFACE LL_SDL=1)

  #use_system_binary(SDL)
  #use_prebuilt_binary(SDL)
  
  target_include_directories( ll::SDL SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include)

  if( USE_SDL1 )
    target_compile_definitions( ll::SDL INTERFACE LL_SDL=1 )

    use_system_binary(SDL)
    use_prebuilt_binary(SDL)
    set (SDL_FOUND TRUE)

    target_link_libraries (ll::SDL INTERFACE SDL directfb fusion direct X11)

  else()
    # The SDL window backend now uses SDL3. LL_SDL2 is kept as the
    # "SDL2+ window" feature switch used across the UI code (IME etc.).
    target_compile_definitions( ll::SDL INTERFACE LL_SDL3=1 LL_SDL2=1 LL_SDL=1 )

    use_system_binary(SDL3)
    use_prebuilt_binary(SDL3)
    set (SDL3_FOUND TRUE)

    target_link_libraries( ll::SDL INTERFACE SDL3 X11 )
  endif()
endif (LINUX)


