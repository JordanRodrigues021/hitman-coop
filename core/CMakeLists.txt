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

if (COOP_BUILD_NETTEST)
    add_executable(coop_nettest nettest/NetTest.cpp)
    set_target_properties(coop_nettest PROPERTIES OUTPUT_NAME "HitmanCoop-NetTest")
    target_link_libraries(coop_nettest PRIVATE coop_core winmm)
    target_compile_definitions(coop_nettest PRIVATE COOP_DEFAULT_RELAY="${COOP_DEFAULT_RELAY}")

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
