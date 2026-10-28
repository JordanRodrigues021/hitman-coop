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
