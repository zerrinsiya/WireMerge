cmake_minimum_required(VERSION 3.15)
project(WireMerge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "WireMerge currently only targets Windows (see project scope).")
endif()

# -----------------------------------------------------------------------------
# Dependencies
#
# Recommended: use vcpkg (see README.md).
#   vcpkg install portaudio libusb
#   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=[vcpkg root]/scripts/buildsystems/vcpkg.cmake
#
# Fallback: if you'd rather drop prebuilt headers/libs into include/portaudio
# and include/libusb manually (matching the original project layout), this
# script also checks those paths so nothing breaks if vcpkg isn't used.
# -----------------------------------------------------------------------------

find_package(portaudio CONFIG QUIET)

# Belt-and-suspenders: even when the portaudio CMake target is found, its
# INTERFACE_INCLUDE_DIRECTORIES doesn't always propagate reliably across
# vcpkg/toolchain/generator combinations. Locate the header explicitly so
# the build doesn't silently lose the include path.
find_path(PORTAUDIO_INCLUDE_DIR NAMES portaudio.h)

# libusb ships NO CMake config file at all (unlike portaudio) -- only a
# pkg-config .pc file. vcpkg's own libusb usage instructions confirm this:
# it must be consumed via find_package(PkgConfig) + pkg_check_modules,
# not a plain find_package(libusb) or find_package(unofficial-libusb).
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUSB QUIET IMPORTED_TARGET libusb-1.0)
endif()

if(NOT portaudio_FOUND)
    message(STATUS "portaudio not found via vcpkg config; falling back to include/portaudio + manual lib path.")
endif()
if(NOT LIBUSB_FOUND)
    message(STATUS "libusb not found via pkg-config; falling back to include/libusb + manual lib path.")
endif()

# -----------------------------------------------------------------------------
# Dear ImGui (fetched, header+source only, no separate library needed)
# -----------------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.9
    GIT_SHALLOW    TRUE
    # Without this, FetchContent re-checks the git remote against GIT_TAG on
    # every single configure, which under Ninja can trigger an infinite
    # "Re-running CMake..." loop (each configure regenerates build files,
    # which Ninja then treats as changed, triggering another configure).
    # We've already pinned an exact tag, so there's nothing to re-check.
    UPDATE_DISCONNECTED TRUE
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp
)

# -----------------------------------------------------------------------------
# WireMerge executable
# -----------------------------------------------------------------------------
add_executable(WireMerge WIN32
    src/main.cpp
    src/audio_handler.cpp
    src/usb_handler.cpp
    src/adb_handler.cpp
    src/mixer.cpp
    src/gui.cpp
    src/check_dependencies.cpp
    src/utils.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(WireMerge PRIVATE
    src
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    include/portaudio
    include/libusb
)

if(PORTAUDIO_INCLUDE_DIR)
    target_include_directories(WireMerge PRIVATE ${PORTAUDIO_INCLUDE_DIR})
    message(STATUS "portaudio.h located at: ${PORTAUDIO_INCLUDE_DIR}")
else()
    message(STATUS "portaudio.h not found via find_path; relying on include/portaudio fallback or the imported target's own include dirs.")
endif()

# Link libs: prefer vcpkg targets when present, else fall back to manually
# placed libs under include/<lib>/lib (adjust to taste if you go this route).
if(portaudio_FOUND)
    if(TARGET portaudio_static)
        target_link_libraries(WireMerge PRIVATE portaudio_static)
    else()
        target_link_libraries(WireMerge PRIVATE portaudio)
    endif()
else()
    target_link_directories(WireMerge PRIVATE include/portaudio/lib)
    target_link_libraries(WireMerge PRIVATE portaudio_x64)
endif()

if(LIBUSB_FOUND)
    target_link_libraries(WireMerge PRIVATE PkgConfig::LIBUSB)
else()
    target_link_directories(WireMerge PRIVATE include/libusb/lib)
    target_link_libraries(WireMerge PRIVATE libusb-1.0)
endif()

target_link_libraries(WireMerge PRIVATE d3d11 dxgi d3dcompiler dwmapi ws2_32 urlmon avrt)

# Keep the binary small/lightweight in Release builds -- matches the
# project's core "low-end PC" goal.
if(MINGW)
    target_link_options(WireMerge PRIVATE -static -static-libgcc -static-libstdc++)
    target_compile_options(WireMerge PRIVATE -O2)
endif()

# Statically-linked builds (the "plug and play standalone .exe" goal) bake
# PortAudio/libusb directly into the binary -- there's no separate DLL to
# hunt for at runtime, so check_dependencies.cpp's DLL-presence check would
# always produce a false "missing" positive. Detect this from the vcpkg
# triplet and let that file skip the check accordingly.
if(VCPKG_TARGET_TRIPLET MATCHES "-static$")
    target_compile_definitions(WireMerge PRIVATE WM_STATIC_BUILD)
endif()

set_target_properties(WireMerge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
