cmake_minimum_required(VERSION 3.12)

# Normally built from the repo root against the SDK developer package. This folder can also be
# dropped into a full ZHMModSDK checkout as Mods/HitmanCoop, with the core copied next to it.
if (NOT TARGET coop_core)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt")
        set(COOP_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/core")
    else ()
        set(COOP_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core")
    endif ()

    set(COOP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${COOP_CORE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/coop_core")
endif ()

file(GLOB_RECURSE SRC_FILES
        CONFIGURE_DEPENDS
        Src/*.cpp
        Src/*.h
)

add_library(HitmanCoop SHARED
        ${SRC_FILES}
)

target_include_directories(HitmanCoop PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Src
)

# Which ZHMModSDK (and so which game version) this build is for. The normal build is 4.2.1 (game
# 3.290.1); separate builds for players who can't update use 4.1.1 (game 3.280.x) or 4.0.0-rc.1
# (game 3.240.x), each with that SDK's dev package (-DZHMMODSDK_DEVPKG_DIR) and its own package.
# The code sees COOP_SDK_VERSION (40201, 40101 or 40000) and leaves out what an older SDK lacks.
set(COOP_SDK "4.2.1" CACHE STRING "ZHMModSDK version built against: 4.2.1, 4.1.1 or 4.0.0-rc.1")
set_property(CACHE COOP_SDK PROPERTY STRINGS "4.2.1" "4.1.1" "4.0.0-rc.1")

if (COOP_SDK STREQUAL "4.2.1")
    target_compile_definitions(HitmanCoop PRIVATE COOP_SDK_VERSION=40201 COOP_GAME_TARGET="3.290")
elseif (COOP_SDK STREQUAL "4.1.1")
    target_compile_definitions(HitmanCoop PRIVATE COOP_SDK_VERSION=40101 COOP_GAME_TARGET="3.280")
elseif (COOP_SDK STREQUAL "4.0.0-rc.1")
    target_compile_definitions(HitmanCoop PRIVATE COOP_SDK_VERSION=40000 COOP_GAME_TARGET="3.240" COOP_LEGACY_SDK=1)
    target_compile_options(HitmanCoop PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/Src/LegacySdk.h")
else ()
    message(FATAL_ERROR "COOP_SDK must be 4.2.1, 4.1.1 or 4.0.0-rc.1, not ${COOP_SDK}.")
endif ()

# The co-op server players get by default (set with -DCOOP_DEFAULT_RELAY=host[:port]).
target_compile_definitions(HitmanCoop PRIVATE COOP_DEFAULT_RELAY="${COOP_DEFAULT_RELAY}")

if (DIRECTX_HEADERS_DIR)
    target_include_directories(HitmanCoop PRIVATE ${DIRECTX_HEADERS_DIR}/include ${DIRECTX_HEADERS_DIR}/include/directx)
endif ()

if (MSVC)
    target_link_options(HitmanCoop PRIVATE $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO /OPT:REF /OPT:ICF>)
    # Only the debug file's name goes into the DLL, not the folder it was built in.
    target_link_options(HitmanCoop PRIVATE "/PDBALTPATH:%_PDB%")
endif ()

target_link_libraries(HitmanCoop PRIVATE
        ZHMModSDK
        implot # IPluginInterface::SetupUI/CleanupUI use it; the dev package's target doesn't link it
        coop_core
        version
)
