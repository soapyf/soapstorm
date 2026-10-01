# -*- cmake -*-
# <SS:Nexii> recastnavigation (Recast + Detour + DetourTileCache), vendored from
# https://github.com/recastnavigation/recastnavigation at 9f4ce64458dfae86e1239c525ddc219c4e9e06f1 (2026-02-27),
# zlib licence (License.txt). Feeds the census navmesh in newview/ssnavmesh.cpp; doc/atmo_magic_navmesh.md section 10
# records the benchmark that chose it over the homebrew tile raster.
# ALTERED: DetourTileCache/Source/DetourTileCacheBuilder.cpp tessellates portal edges on a world-aligned lattice
# (tessellatePortalEdges) so neighbouring tiles share border vertices, keeps the lattice vertices where the border
# bends through the poly mesh's border-vertex removal, and fixes two undersized stack buffers upstream (canRemoveVertex's
# edge scratch, removeVertex's tverts) that overran once a border vertex was shared by many polygons.
# ALTERED: DetourTileCache/Source/DetourTileCache.cpp raises the per-column tile list from 32 to 256 layers, and with
# DetourTileCache/Include/DetourTileCache.h adds dtTileCacheMeshProcess::detail, a hook that offers the decompressed layer
# so the viewer can attach a height detail mesh before dtCreateNavMeshData.
# Everything else is upstream verbatim.

project(recastnavigation)

include(00-Common)

set(recastnavigation_SOURCE_FILES
        Recast/Source/Recast.cpp
        Recast/Source/RecastAlloc.cpp
        Recast/Source/RecastArea.cpp
        Recast/Source/RecastAssert.cpp
        Recast/Source/RecastContour.cpp
        Recast/Source/RecastFilter.cpp
        Recast/Source/RecastLayers.cpp
        Recast/Source/RecastMesh.cpp
        Recast/Source/RecastMeshDetail.cpp
        Recast/Source/RecastRasterization.cpp
        Recast/Source/RecastRegion.cpp
        Detour/Source/DetourAlloc.cpp
        Detour/Source/DetourAssert.cpp
        Detour/Source/DetourCommon.cpp
        Detour/Source/DetourNavMesh.cpp
        Detour/Source/DetourNavMeshBuilder.cpp
        Detour/Source/DetourNavMeshQuery.cpp
        Detour/Source/DetourNode.cpp
        DetourTileCache/Source/DetourTileCache.cpp
        DetourTileCache/Source/DetourTileCacheBuilder.cpp
        )

set(recastnavigation_HEADER_FILES
        CMakeLists.txt
        Recast/Include/Recast.h
        Recast/Include/RecastAlloc.h
        Recast/Include/RecastAssert.h
        Detour/Include/DetourAlloc.h
        Detour/Include/DetourAssert.h
        Detour/Include/DetourCommon.h
        Detour/Include/DetourMath.h
        Detour/Include/DetourNavMesh.h
        Detour/Include/DetourNavMeshBuilder.h
        Detour/Include/DetourNavMeshQuery.h
        Detour/Include/DetourNode.h
        Detour/Include/DetourStatus.h
        DetourTileCache/Include/DetourTileCache.h
        DetourTileCache/Include/DetourTileCacheBuilder.h
        )

set_source_files_properties(${recastnavigation_HEADER_FILES}
        PROPERTIES HEADER_FILE_ONLY TRUE)

list(APPEND recastnavigation_SOURCE_FILES ${recastnavigation_HEADER_FILES})

add_library (recastnavigation ${recastnavigation_SOURCE_FILES})
# 64-bit polygon references: 28 tile bits and 20 poly bits, so whole-region coverage (thousands of band layers)
# never squeezes polygons per tile. PUBLIC so every consumer sees the same dtPolyRef.
target_compile_definitions(recastnavigation PUBLIC DT_POLYREF64=1)
target_include_directories( recastnavigation INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Recast/Include
        ${CMAKE_CURRENT_SOURCE_DIR}/Detour/Include
        ${CMAKE_CURRENT_SOURCE_DIR}/DetourTileCache/Include)
target_include_directories( recastnavigation PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Recast/Include
        ${CMAKE_CURRENT_SOURCE_DIR}/Detour/Include
        ${CMAKE_CURRENT_SOURCE_DIR}/DetourTileCache/Include)

# Upstream builds with its own warning set; the viewer's /W3 plus /WX would fail on their casts. 00-Common puts both
# on this directory's COMPILE_OPTIONS, so they are removed there rather than overridden: a second /W level on the
# same command line is cl's D9025, which /WX turns fatal (the /Ob3 substitution in 00-Common exists for the same
# reason).
if (WINDOWS)
    get_property(_ss_rc_opts DIRECTORY PROPERTY COMPILE_OPTIONS)
    list(REMOVE_ITEM _ss_rc_opts "/W3" "/WX")
    set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "${_ss_rc_opts}")
    target_compile_options(recastnavigation PRIVATE /W1 /wd4244 /wd4267)
else ()
    target_compile_options(recastnavigation PRIVATE -w)
endif ()
