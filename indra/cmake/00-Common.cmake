# -*- cmake -*-
#
# Compilation options shared by all Second Life components.

#*****************************************************************************
#   It's important to realize that CMake implicitly concatenates
#   CMAKE_CXX_FLAGS with (e.g.) CMAKE_CXX_FLAGS_RELEASE for Release builds. So
#   set switches in CMAKE_CXX_FLAGS that should affect all builds, but in
#   CMAKE_CXX_FLAGS_RELEASE or CMAKE_CXX_FLAGS_RELWITHDEBINFO for switches
#   that should affect only that build variant.
#
#   Also realize that CMAKE_CXX_FLAGS may already be partially populated on
#   entry to this file.
#*****************************************************************************
include_guard()

include(Variables)

# We go to some trouble to set LL_BUILD to the set of relevant compiler flags.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} $ENV{LL_BUILD}")
# Given that, all the flags you see added below are flags NOT present in
# https://bitbucket.org/lindenlab/viewer-build-variables/src/tip/variables.
# Before adding new ones here, it's important to ask: can this flag really be
# applied to the viewer only, or should/must it be applied to all 3p libraries
# as well?

# Portable compilation flags.
add_compile_definitions( ADDRESS_SIZE=${ADDRESS_SIZE})
# Because older versions of Boost.Bind dumped placeholders _1, _2 et al. into
# the global namespace, Boost now requires either BOOST_BIND_NO_PLACEHOLDERS
# to avoid that or BOOST_BIND_GLOBAL_PLACEHOLDERS to state that we require it
# -- which we do. Without one or the other, we get a ton of Boost warnings.
add_compile_definitions(BOOST_BIND_GLOBAL_PLACEHOLDERS)

# Force enable SSE2 instructions in GLM per the manual
# https://github.com/g-truc/glm/blob/master/manual.md#section2_10
add_compile_definitions(GLM_FORCE_DEFAULT_ALIGNED_GENTYPES=1 GLM_ENABLE_EXPERIMENTAL=1)

# SSE2NEON throws a pointless warning when compiler optimizations are enabled
add_compile_definitions(SSE2NEON_SUPPRESS_WARNINGS=1)

# Configure crash reporting
set(RELEASE_CRASH_REPORTING OFF CACHE BOOL "Enable use of crash reporting in release builds")
set(NON_RELEASE_CRASH_REPORTING OFF CACHE BOOL "Enable use of crash reporting in developer builds")

if(RELEASE_CRASH_REPORTING)
  add_compile_definitions( LL_SEND_CRASH_REPORTS=1)
endif()

if(NON_RELEASE_CRASH_REPORTING)
  add_compile_definitions( LL_SEND_CRASH_REPORTS=1)
endif()

set(USE_LTO OFF CACHE BOOL "Enable Link Time Optimization")
if(USE_LTO)
  # <FS:PP> (revised by Beq to allow flto=auto on Linux/GCC to re-enable parallel builds with LTO enabled.)
  if(NOT MSVC)
    if(LINUX AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
      set(CMAKE_C_COMPILE_OPTIONS_IPO "-flto=auto")
      set(CMAKE_C_LINK_OPTIONS_IPO "-flto=auto")
    else()
      set(CMAKE_C_COMPILE_OPTIONS_IPO "-flto")
      set(CMAKE_C_LINK_OPTIONS_IPO "-flto")
    endif()
    set(CMAKE_XCODE_ATTRIBUTE_LLVM_LTO "YES")
  endif()
  # </FS:PP>
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  add_compile_definitions(USE_LTO) # <FS:PP> LTO indicator
endif()

# Don't bother with a MinSizeRel or Debug builds.
set(CMAKE_CONFIGURATION_TYPES "RelWithDebInfo;Release" CACHE STRING "Supported build types." FORCE)

# <SS:Nexii> ss_hot_loop_codegen(<target>)
#
# Opts a target out of the MSVC buffer security check (/GS). /GS makes the compiler plant a stack canary and a validation epilogue in any function holding an array or taking the address of a local, which describes essentially every loop in a math or pixel kernel - so the cost lands hardest exactly where it buys least.
#
# It is scoped per target rather than set globally because /GS is worth keeping on everything that parses input the viewer did not produce: network messages, LLSD, asset and mesh decoders, the UI. Only call this for libraries whose work is arithmetic over buffers the caller already owns.
#
# No effect on GCC or Clang builds, where the equivalent (-fno-stack-protector) is already applied to everything in the LINUX block below.
function(ss_hot_loop_codegen target)
  if (WINDOWS AND TARGET ${target})
    target_compile_options(${target} PRIVATE /GS-)
  endif ()
endfunction()

# Platform-specific compilation flags.

if (WINDOWS)
  # Don't build DLLs.
  set(BUILD_SHARED_LIBS OFF)

  if( USE_COMPILERCACHE )
    string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_REELASE}")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  endif()

  # for "backwards compatibility", cmake sneaks in the Zm1000 option which royally
  # screws incredibuild. this hack disables it.
  # for details see: http://connect.microsoft.com/VisualStudio/feedback/details/368107/clxx-fatal-error-c1027-inconsistent-values-for-ym-between-creation-and-use-of-precompiled-headers
  # http://www.ogre3d.org/forums/viewtopic.php?f=2&t=60015
  # http://www.cmake.org/pipermail/cmake/2009-September/032143.html
  string(REPLACE "/Zm1000" " " CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})

  # <SS:Nexii> Raise inline expansion from /Ob2 to /Ob3. This is a substitution rather than an appended flag on purpose: /Ob2 arrives twice, once from LL_BUILD (fs-build-variables/variables, LL_BUILD_WINDOWS_RELEASE_SWITCHES) and once from CMake's own CMAKE_CXX_FLAGS_RELEASE default, and the Visual Studio generator lifts inline expansion out of the flag string into an MSBuild property - so appending /Ob3 and trusting last-wins is not reliable. Replacing leaves exactly one.
  #
  # /Ob3 trades binary size for inlining depth, and it compounds with USE_LTO: if link times or the working set become a problem, this is the first switch to put back to /Ob2.
  # The inline level goes in the common flags, and every per-config copy is then stripped so exactly one reaches cl. Both halves are needed: the Visual Studio generator recognises /Ob0 through /Ob2 and folds them into the InlineFunctionExpansion MSBuild property, but does not recognise /Ob3, which therefore lands in AdditionalOptions instead. Leaving a per-config /Ob1 in place would put both on the command line - MSBuild emits AdditionalOptions last so /Ob3 would still win, but cl warns D9025 "overriding /Ob1 with /Ob3" once per translation unit, which buries everything else in the build log.
  set(SS_INLINE_EXPANSION "/Ob3" CACHE STRING "MSVC inline expansion level; set to /Ob2 to revert")
  string(REPLACE "/Ob2" "${SS_INLINE_EXPANSION}" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  string(REPLACE "/Ob2" "${SS_INLINE_EXPANSION}" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  foreach(_ss_cfg RELEASE RELWITHDEBINFO MINSIZEREL DEBUG)
    foreach(_ss_lang CXX C)
      string(REGEX REPLACE "/Ob[0-3]" "" CMAKE_${_ss_lang}_FLAGS_${_ss_cfg} "${CMAKE_${_ss_lang}_FLAGS_${_ss_cfg}}")
    endforeach()
  endforeach()

  add_link_options(/LARGEADDRESSAWARE
          /NODEFAULTLIB:LIBCMT
          /IGNORE:4099)

  add_compile_definitions(
      LL_WINDOWS=1
      WIN32_LEAN_AND_MEAN
      NOMINMAX
      UNICODE
      _UNICODE
#     DOM_DYNAMIC                     # For shared library colladadom
      _CRT_SECURE_NO_WARNINGS         # Allow use of sprintf etc
      _CRT_NONSTDC_NO_DEPRECATE       # Allow use of sprintf etc
      _CRT_OBSOLETE_NO_WARNINGS
      _WINSOCK_DEPRECATED_NO_WARNINGS # Disable deprecated WinSock API warnings
      )
  add_compile_options(
          /Zo
          /GS
          /TP
          /W3
          /c
          /Zc:forScope
          /nologo
          /Oi
          /Ot
          /fp:precise
          # <SS:Nexii> Put each global in its own COMDAT so the linker can fold duplicates and drop what nothing references. Wants /OPT:REF,ICF at link time, which is already the MSVC release default.
          /Gw
          /MP
          /permissive-
      )

  # <SS:Nexii> /fp:contract was here and is deliberately NOT set. Do not add it back without reading this.
  #
  # It let the compiler fuse a*b+c into a single FMA. That is more accurate in isolation - one rounding where the unfused form rounds twice - but accuracy is not the property culling needs, CONSISTENCY is. MSVC contracts scalar float code and does NOT fuse SSE intrinsics, and this viewer's culling is a mixture of both, so the same geometric predicate could be computed two ways that disagree in the last bit. A frustum or occlusion test that straddles zero then answers differently depending on which path asked, and the error is worst where a small difference is taken between large magnitudes - exactly a plane-distance test far from the origin.
  #
  # This is CONFIRMED, not suspected. Symptom: object and water geometry dropping out for single frames, triggered by camera movement, at 1-2.5 km altitude, with everything else on the branch held constant. Removing this flag and changing nothing else stopped it. The mechanism above was written down before the test rather than fitted to it afterwards, and it predicted all four properties of the symptom - camera-motion triggered, single frame, water affected as well as prims, and worse with altitude.
  #
  # Note what the symptom was NOT: it was not a slowdown, not a crash, and not wrong pixels. It was geometry vanishing for one frame. Anyone re-reading this while looking at a performance flag should understand that contraction here bought a small, unmeasured speedup and cost visible correctness.
  #
  # It was the only change in this file that alters a computed result: /Ob3, /Gw and /GS- change inlining, COMDAT folding and stack canaries, none of which move a float. Those all remain.
  #
  # If contraction is ever wanted back, the way in is NOT to re-add it globally and hope. Find the specific predicate that two code paths compute inconsistently - the suspicion is a frustum or occlusion plane-distance test evaluated once in contracted scalar code and once in SIMD intrinsics, which MSVC does not fuse - make that one computation consistent, and only then consider re-enabling contraction for everything else.
  #
  # /fp:precise above is what MSVC applies by default anyway, and under it contraction is off - so this line's absence is the safe state, not a missing optimisation.

  # <SS:Nexii> /Oy- was here. It disables frame pointer omission, which is an x86-only optimisation - on x64 the switch is parsed and ignored, because the ABI requires unwind data for every frame regardless. It has never done anything in a 64 bit build and reading it here suggests otherwise.

  # <FS:Ansariel> AVX/AVX2 support
  if (USE_AVX_OPTIMIZATION)
    add_compile_options(/arch:AVX)
  elseif (USE_AVX2_OPTIMIZATION)
    add_compile_options(/arch:AVX2)
  else (USE_AVX_OPTIMIZATION)
    # Nicky: x64 implies SSE2
    if (ADDRESS_SIZE EQUAL 32)
      add_compile_options(/arch:SSE2)
    endif()
  endif (USE_AVX_OPTIMIZATION)
  # </FS:Ansariel> AVX/AVX2 support

  # Are we using the crummy Visual Studio KDU build workaround?
  if (NOT VS_DISABLE_FATAL_WARNINGS)
    add_compile_options(/WX)
  endif (NOT VS_DISABLE_FATAL_WARNINGS)

  #ND: When using something like buildcache (https://github.com/mbitsnbites/buildcache)
  # to make those wrappers work /Zi must be changed to /Z7, as /Zi due to it's nature is not compatible with caching
  if(${CMAKE_CXX_COMPILER_LAUNCHER} MATCHES ".*cache.*")
    add_compile_options( /Z7 )
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/Zi" "/Z7" CMAKE_C_FLAGS_RELWITHDEBINFO "${CMAKE_C_FLAGS_RELWITHDEBINFO}")
    string(REPLACE "/Zi" "/Z7" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  endif()
endif (WINDOWS)


if (LINUX)
  set(CMAKE_SKIP_RPATH TRUE)

  # <FS:ND/>
  # And another hack for FORTIFY_SOURCE. Some distributions (for example Gentoo) define FORTIFY_SOURCE by default.
  # Check if this is the case, if yes, do not define it again.
  execute_process(
      COMMAND echo "int main( char **a, int c ){ \n#ifdef _FORTIFY_SOURCE\n#error FORTITY_SOURCE_SET\n#else\nreturn 0;\n#endif\n}" 
      COMMAND sh -c "${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_ARG1} -xc++ -w - -o /dev/null"
      OUTPUT_VARIABLE FORTIFY_SOURCE_OUT
         ERROR_VARIABLE FORTIFY_SOURCE_ERR
         RESULT_VARIABLE FORTIFY_SOURCE_RES
     )


  if ( ${FORTIFY_SOURCE_RES} EQUAL 0 )
   add_definitions(-D_FORTIFY_SOURCE=2)
  endif()

  # gcc 4.3 and above don't like the LL boost and also
  # cause warnings due to our use of deprecated headers

  add_definitions(
      -DLL_LINUX=1
      -D_REENTRANT
      )
  add_compile_options(
      -fexceptions
      -fno-math-errno
      -fno-strict-aliasing
      -fsigned-char
      -msse2
      -mfpmath=sse
      -pthread
      -Wno-error=array-bounds
      )

  # force this platform to accept TOS via external browser <FS:ND> No, do not.
  # add_definitions(-DEXTERNAL_TOS)

  add_definitions(-DAPPID=secondlife)
  add_compile_options(-fvisibility=hidden)
  # don't catch SIGCHLD in our base application class for the viewer - some of
  # our 3rd party libs may need their *own* SIGCHLD handler to work. Sigh! The
  # viewer doesn't need to catch SIGCHLD anyway.
  add_definitions(-DLL_IGNORE_SIGCHLD)
  if (ADDRESS_SIZE EQUAL 32)
    add_compile_options(-march=pentium4)
  endif (ADDRESS_SIZE EQUAL 32)

  # <SS:Nexii> AVX/AVX2 on Linux, mirroring the /arch block in the WINDOWS section above.
  #
  # Until now USE_AVX2_OPTIMIZATION was honoured on Windows only: indra/CMakeLists.txt add_compile_definitions() it on every platform, but the flag that actually changes code generation sat inside if (WINDOWS). A Linux build therefore compiled -msse2 while reporting "AVX2" in the about box and to the crash handler, because both of those read the define.
  #
  # -mfma is listed explicitly rather than left to -mavx2. The two are separate feature flags to GCC and Clang even though every shipping AVX2 CPU has FMA3.
  if (USE_AVX_OPTIMIZATION)
    add_compile_options(-mavx)
  elseif (USE_AVX2_OPTIMIZATION)
    add_compile_options(-mavx2 -mfma)
  endif ()

  # <SS:Nexii> The counterpart of NOT setting /fp:contract on Windows - see the note in the WINDOWS block above for why contraction is being kept off.
  #
  # This has to be stated rather than left alone, because the platforms disagree on the default: MSVC under /fp:precise does not contract, while GCC and Clang contract by default for C++. Saying nothing here would mean a Linux build quietly did the exact thing the Windows build was changed to stop doing - and -mfma above has just guaranteed it the hardware to do it with.
  add_compile_options(-ffp-contract=off)

  # this stops us requiring a really recent glibc at runtime
  add_compile_options(-fno-stack-protector)
  # linking can be very memory-hungry, especially the final viewer link
  #set(CMAKE_CXX_LINK_FLAGS "-Wl,--no-keep-memory")
  set(CMAKE_CXX_LINK_FLAGS "-Wl,--no-keep-memory -Wl,--build-id -Wl,-rpath,'$ORIGIN:$ORIGIN/../lib' -Wl,--exclude-libs,ALL")
  set(CMAKE_EXE_LINKER_FLAGS "-Wl,--no-keep-memory -Wl,--build-id -Wl,-rpath,'$ORIGIN:$ORIGIN/../lib' -Wl,--exclude-libs,ALL")

  set(CMAKE_CXX_FLAGS_DEBUG "-fno-inline ${CMAKE_CXX_FLAGS_DEBUG}")
  # Prefer static libraries on Linux
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".so;.a")
endif (LINUX)

if (DARWIN)
  # Use rpath loading on macos
  set(CMAKE_MACOSX_RPATH TRUE)

  # Warnings should be fatal -- thanks, Nicky Perian, for spotting reversed default
  set(CLANG_DISABLE_FATAL_WARNINGS OFF)
  set(CMAKE_CXX_LINK_FLAGS "-Wl,-headerpad_max_install_names,-search_paths_first")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_CXX_LINK_FLAGS}")

  # Ensure debug symbols are always generated
  add_compile_options(-g --debug) # --debug is a clang synonym for -g that bypasses cmake behaviors

  # Silence GL deprecation warnings
  add_compile_definitions(LL_DARWIN=1 GL_SILENCE_DEPRECATION=1)

  set(ENABLE_SIGNING TRUE)
  set(SIGNING_IDENTITY "Developer ID Application: The Phoenix Firestorm Project, Inc." )
endif(DARWIN)

if (LINUX OR DARWIN)
  add_compile_options(-Wall -Wno-sign-compare -Wno-trigraphs -Wno-reorder -Wno-unused-but-set-variable -Wno-unused-variable)

  if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    # libstdc++ headers contain deprecated declarations that fail on clang
    # macOS currently has many deprecated calls
    add_compile_options(-Wno-unused-local-typedef)
  endif()

  if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-stringop-truncation -Wno-parentheses -Wno-maybe-uninitialized -Wno-error=aggressive-loop-optimizations)
  endif()

  if (NOT GCC_DISABLE_FATAL_WARNINGS AND NOT CLANG_DISABLE_FATAL_WARNINGS)
    add_compile_options(-Werror)
  endif ()

  add_compile_options(${GCC_WARNINGS})
  add_compile_options(-m${ADDRESS_SIZE})
endif (LINUX OR DARWIN)

