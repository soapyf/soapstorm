# -*- cmake -*-

project(llimage)

include(00-Common)
include(LLCommon)
include(LLImage)
include(JPEG)
include(LLKDU)
include(ZLIBNG)
include(LLAddBuildTest)
include(bugsplat)
include(Tut)

set(llimage_SOURCE_FILES
    llimagebmp.cpp
    llimage.cpp
    llimagedimensionsinfo.cpp
    llimagedxt.cpp
    llimagefilter.cpp
    llimagej2c.cpp
    llimagejpeg.cpp
    llimagepng.cpp
    llimagetga.cpp
    llimageworker.cpp
    llpngwrapper.cpp
    ssbc7block_mode6.cpp
    ssbc7encoder.cpp
    )

set(llimage_HEADER_FILES
    CMakeLists.txt

    llimage.h
    llimagebmp.h
    llimagedimensionsinfo.h
    llimagedxt.h
    llimagefilter.h
    llimagej2c.h
    llimagejpeg.h
    llimagepng.h
    llimagetga.h
    llimageworker.h
    llmapimagetype.h
    llpngwrapper.h
    ssbc7encoder.h
    )

list(APPEND llimage_SOURCE_FILES ${llimage_HEADER_FILES})

# <SS:Nexii> Squeeze's BC7 block backend. bc7f, Binomial's analytical BC7 encoder, ships inside the
# Basis Universal transcoder vendored UNMODIFIED under basis_universal/ (Apache 2.0, see the README
# there and doc/super_compressed_textures.md). It is plain C++ with no SIMD and no toolchain beyond
# the compiler, so it is on by default on every platform and needs nothing from CI or autobuild.
# SS_BC7F=OFF builds the portable mode 6 backend in ssbc7block_mode6.cpp instead, which is entirely
# our own code - the structural hedge the licensing section of the doc describes. Exactly one backend
# is ever compiled, because both define the same functions behind the seam in ssbc7encoder.h.
option(SS_BC7F "Squeeze: use Binomial's bc7f (Basis Universal, Apache 2.0) as the BC7 block backend" ON)
set(SS_BASISU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/basis_universal/transcoder")
set(SS_BASISU_TRANSCODER "${SS_BASISU_DIR}/basisu_transcoder.cpp")
if(SS_BC7F AND EXISTS "${SS_BASISU_TRANSCODER}")
  list(REMOVE_ITEM llimage_SOURCE_FILES ssbc7block_mode6.cpp)
  list(APPEND llimage_SOURCE_FILES ssbc7block_bc7f.cpp "${SS_BASISU_TRANSCODER}")
  # Nothing here reads a .basis or .ktx2 file, so the container and zstd paths are compiled out. These
  # two are the only trims upstream builds with; every other BASISD_SUPPORT_* switch is left at its
  # default because bc7f's section does not compile without them. The defines go on both translation
  # units that see the header so the two agree on its layout.
  set(SS_BASISU_DEFINES "BASISD_SUPPORT_KTX2=0;BASISD_SUPPORT_KTX2_ZSTD=0")
  set_source_files_properties("${SS_BASISU_TRANSCODER}" ssbc7block_bc7f.cpp PROPERTIES COMPILE_DEFINITIONS "${SS_BASISU_DEFINES}")
  # Third party code is compiled at its own warning level, not ours: the viewer builds with warnings as
  # errors and upstream is not written to that standard. The 46k line translation unit also overflows
  # the default COFF section limit, hence /bigobj.
  if(MSVC)
    set_source_files_properties("${SS_BASISU_TRANSCODER}" PROPERTIES COMPILE_OPTIONS "/W0;/bigobj")
  else()
    set_source_files_properties("${SS_BASISU_TRANSCODER}" PROPERTIES COMPILE_OPTIONS "-w")
  endif()
  message(STATUS "Squeeze: using the bc7f BC7 backend (Basis Universal)")
else()
  message(STATUS "Squeeze: using the portable C++ mode 6 BC7 backend")
endif()
# </SS:Nexii>

add_library (llimage ${llimage_SOURCE_FILES})

# <SS:Nexii> only the backend shim includes the transcoder header, so the directory is private
if(SS_BC7F AND EXISTS "${SS_BASISU_TRANSCODER}")
  target_include_directories(llimage PRIVATE "${SS_BASISU_DIR}")
endif()
# </SS:Nexii>
target_include_directories( llimage  INTERFACE   ${CMAKE_CURRENT_SOURCE_DIR})
# Libraries on which this library depends, needed for Linux builds
# Sort by high-level to low-level
if (USE_KDU)
    target_link_libraries(llimage llkdu)
else (USE_KDU)
    target_link_libraries(llimage llimagej2coj)
endif (USE_KDU)

target_link_libraries(llimage
        llfilesystem
        llmath
        llcommon
        ll::libpng
        ll::libjpeg
    )

# Add tests
if (LL_TESTS)
  SET(llimage_TEST_SOURCE_FILES
    llimageworker.cpp
    )
  LL_ADD_PROJECT_UNIT_TESTS(llimage "${llimage_TEST_SOURCE_FILES}")
endif (LL_TESTS)



