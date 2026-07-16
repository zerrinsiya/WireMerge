#include "check_dependencies.h"
#include "utils.h"
#include <windows.h>
#include <sstream>

namespace wm {

#ifndef WM_STATIC_BUILD
// Tries to load a DLL by name using the normal Windows search order
// (application directory, system directories, then PATH). This mirrors
// what will actually happen when the real app tries to use PortAudio/
// libusb, so it's a faithful check rather than a guess. Only relevant for
// dynamically-linked builds -- see WM_STATIC_BUILD handling below.
static bool TryFindDll(const std::string& dllName, std::string& outPath) {
    HMODULE mod = LoadLibraryA(dllName.c_str());
    if (!mod) return false;

    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(mod, buf, MAX_PATH);
    outPath = (len > 0) ? std::string(buf, len) : dllName;

    FreeLibrary(mod);
    return true;
}
#endif

std::vector<DependencyStatus> CheckDependencies() {
    std::vector<DependencyStatus> results;

#ifdef WM_STATIC_BUILD
    // Statically linked build: PortAudio and libusb are compiled directly
    // into WireMerge.exe (see CMakeLists.txt / x64-mingw-static triplet),
    // so there is no separate DLL to hunt for on disk -- that's the whole
    // point of a static build. Report both as trivially present rather
    // than running a check that would always produce a false positive.
    results.push_back({"PortAudio", true, "Statically linked into WireMerge.exe"});
    results.push_back({"libusb", true, "Statically linked into WireMerge.exe"});
    return results;
#else
    // Common DLL names depending on how PortAudio was built.
    const char* portaudioCandidates[] = {"portaudio.dll", "portaudio_x64.dll", "libportaudio-2.dll"};
    // libusb-1.0.dll is the standard name across MinGW/MSVC builds.
    const char* libusbCandidates[] = {"libusb-1.0.dll"};

    {
        DependencyStatus status{"PortAudio", false, ""};
        for (const char* name : portaudioCandidates) {
            std::string path;
            if (TryFindDll(name, path)) {
                status.found = true;
                status.details = "Found: " + path;
                break;
            }
        }
        if (!status.found) {
            status.details =
                "Not found next to WireMerge.exe or on PATH.\n"
                "Get it via vcpkg (recommended):  vcpkg install portaudio\n"
                "...or download a prebuilt DLL from http://www.portaudio.com/download.html "
                "and place it next to WireMerge.exe.";
        }
        results.push_back(status);
    }

    {
        DependencyStatus status{"libusb", false, ""};
        for (const char* name : libusbCandidates) {
            std::string path;
            if (TryFindDll(name, path)) {
                status.found = true;
                status.details = "Found: " + path;
                break;
            }
        }
        if (!status.found) {
            status.details =
                "Not found next to WireMerge.exe or on PATH.\n"
                "Get it via vcpkg (recommended):  vcpkg install libusb\n"
                "...or download the latest release from "
                "https://github.com/libusb/libusb/releases and place "
                "libusb-1.0.dll next to WireMerge.exe.";
        }
        results.push_back(status);
    }

    return results;
#endif
}

bool ReportDependencyStatus(const std::vector<DependencyStatus>& statuses) {
    bool allFound = true;
    std::ostringstream missingMsg;
    missingMsg << "WireMerge is missing one or more required components:\n\n";

    for (const auto& s : statuses) {
        WM_LOG_INFO((s.found ? "[OK] " : "[MISSING] ") + s.name + " -- " + s.details);
        if (!s.found) {
            allFound = false;
            missingMsg << "- " << s.name << "\n  " << s.details << "\n\n";
        }
    }

    if (allFound) return true;

    missingMsg << "WireMerge will not start correctly without these. "
                   "Install them, restart WireMerge, and this check will pass automatically.\n\n"
                   "Continue launching anyway? (audio features will likely fail)";

    int result = MessageBoxA(nullptr, missingMsg.str().c_str(), "WireMerge - Missing Dependencies",
                              MB_ICONWARNING | MB_YESNO);
    return result == IDYES;
}

} // namespace wm
