cmake_minimum_required(VERSION 3.15)

# Engine-independent co-op core: wire protocol, reliable UDP and session/sync logic.
# Builds on its own (for the tests) or as a subdirectory of the ZHMModSDK build (for the mod).
project(HitmanCoopCore CXX)

if (NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif ()

add_library(coop_core STATIC
        src/Connection.cpp
        src/GameEvents.cpp
        src/Interpolation.cpp
        src/Protocol.cpp
        src/RelaySocket.cpp
        src/Session.cpp
        src/Socket.cpp
)

target_include_directories(coop_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(coop_core PUBLIC ws2_32)
target_compile_definitions(coop_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)

if (MSVC)
    target_compile_options(coop_core PRIVATE /W4 /permissive-)
endif ()

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(COOP_TOP_LEVEL ON)
else ()
    set(COOP_TOP_LEVEL OFF)
endif ()

option(COOP_BUILD_TESTS "Build the co-op core tests" ${COOP_TOP_LEVEL})
option(COOP_BUILD_NETTEST "Build the player-facing connection test" ON)
set(COOP_DEFAULT_RELAY "" CACHE STRING "Co-op relay server (host[:port]) built into the mod and tools; empty for none")
option(COOP_ASAN "Build with AddressSanitizer" OFF)

if (COOP_ASAN AND MSVC)
    target_compile_options(coop_core PRIVATE /fsanitize=address /Zi)
endif ()

# A Windows version resource for a target (core/res/VersionInfo.rc.in): file type (VFT_APP or
# VFT_DLL), description, internal name and file name.
function(coop_add_version_resource p_Target p_Type p_Description p_Name p_File)
    if (NOT WIN32)
        return()
    endif ()
    set(COOP_RC_FILETYPE ${p_Type})
    set(COOP_RC_DESCRIPTION ${p_Description})
    set(COOP_RC_NAME ${p_Name})
    set(COOP_RC_FILE ${p_File})
    set(s_Out "${CMAKE_CURRENT_BINARY_DIR}/${p_Target}-version.rc")
    configure_file("${COOP_RES_DIR}/VersionInfo.rc.in" "${s_Out}" @ONLY)
    target_sources(${p_Target} PRIVATE "${s_Out}")
    set_source_files_properties("${s_Out}" PROPERTIES INCLUDE_DIRECTORIES "${COOP_RES_DIR}")
endfunction()
set(COOP_RES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/res" CACHE INTERNAL "")

if (COOP_BUILD_NETTEST)
    add_executable(coop_nettest nettest/NetTest.cpp)
    set_target_properties(coop_nettest PROPERTIES OUTPUT_NAME "HitmanCoop-NetTest")
    target_link_libraries(coop_nettest PRIVATE coop_core winmm)
    target_compile_definitions(coop_nettest PRIVATE COOP_DEFAULT_RELAY="${COOP_DEFAULT_RELAY}")
    coop_add_version_resource(coop_nettest VFT_APP "HITMAN Co-op connection test" "Test-Connection" "Test-Connection.exe")

    if (MSVC)
        target_compile_options(coop_nettest PRIVATE /W4 /permissive-)
        target_link_options(coop_nettest PRIVATE "/PDBALTPATH:%_PDB%")
    endif ()
endif ()

if (COOP_BUILD_TESTS)
    add_executable(coop_tests
            tests/Main.cpp
            tests/TestCore.cpp
            tests/TestGameEvents.cpp
            tests/TestSession.cpp
    )

    target_link_libraries(coop_tests PRIVATE coop_core)

    if (MSVC)
        target_compile_options(coop_tests PRIVATE /W4 /permissive-)
    endif ()

    if (COOP_ASAN AND MSVC)
        target_compile_options(coop_tests PRIVATE /fsanitize=address /Zi)
        target_link_options(coop_tests PRIVATE /INCREMENTAL:NO /DEBUG)
    endif ()
endif ()
