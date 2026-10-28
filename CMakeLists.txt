cmake_minimum_required(VERSION 3.20)

# Builds the HitmanCoop mod against a prebuilt ZHMModSDK developer package
# (DevPkg-ZHMModSDK.zip from the SDK's GitHub releases). See BUILDING.md.
project(HitmanCoop CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The SDK and its mods use the static runtime. Only release-type builds are supported, because
# the developer package's release libraries are what the shipped ZHMModSDK.dll was built from.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif ()

set(ZHMMODSDK_DEVPKG_DIR "" CACHE PATH "Folder where DevPkg-ZHMModSDK.zip was extracted")

if (NOT EXISTS "${ZHMMODSDK_DEVPKG_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Set ZHMMODSDK_DEVPKG_DIR to the extracted DevPkg-ZHMModSDK.zip (tools/fetch-sdk.ps1 does this).")
endif ()

set(DIRECTX_HEADERS_DIR "${CMAKE_SOURCE_DIR}/_deps/directx-headers/DirectX-Headers-1.614.0" CACHE PATH
    "Extracted DirectX-Headers (tools/fetch-sdk.ps1 does this); the SDK's headers include <directx/d3d12.h>")

if (NOT EXISTS "${DIRECTX_HEADERS_DIR}/include/directx/d3d12.h")
    message(FATAL_ERROR "DirectX-Headers not found at ${DIRECTX_HEADERS_DIR}. Run tools/fetch-sdk.ps1.")
endif ()

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

# fmt (bundled in the SDK headers) requires it.
add_compile_options(/utf-8)

add_subdirectory("${ZHMMODSDK_DEVPKG_DIR}" "${CMAKE_BINARY_DIR}/zhmmodsdk")
add_subdirectory(core)
add_subdirectory(plugin)

install(TARGETS HitmanCoop RUNTIME DESTINATION mods)
install(TARGETS coop_nettest RUNTIME DESTINATION .)
