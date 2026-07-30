cmake_minimum_required(VERSION 3.15)
project(WireMerge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "WireMerge currently only targets Windows (see project scope).")
endif()

enable_language(RC)

find_package(portaudio CONFIG QUIET)
find_path(PORTAUDIO_INCLUDE_DIR NAMES portaudio.h)

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

include(FetchContent)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG cb16be3a3fc1f9cd146ae24d52b615f8a05fa93d #v1.90.9
    GIT_SHALLOW TRUE
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

add_executable(WireMerge WIN32
    src/main.cpp
    src/audio_handler.cpp
    src/usb_handler.cpp
    src/adb_handler.cpp
    src/mixer.cpp
    src/layout.cpp
    src/gui.cpp
    src/check_dependencies.cpp
    src/utils.cpp
    src/resources/WireMerge.rc
    ${IMGUI_SOURCES}
)

target_include_directories(WireMerge PRIVATE
    src
    src/resources
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

target_link_libraries(WireMerge PRIVATE d3d11 dxgi d3dcompiler dwmapi ws2_32 urlmon avrt ole32)

if(MINGW)
    target_link_options(WireMerge PRIVATE -static -static-libgcc -static-libstdc++)
    target_compile_options(WireMerge PRIVATE -O2)
endif()

if(VCPKG_TARGET_TRIPLET MATCHES "-static$")
    target_compile_definitions(WireMerge PRIVATE WM_STATIC_BUILD)
endif()

set_target_properties(WireMerge PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
