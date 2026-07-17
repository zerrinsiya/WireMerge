#pragma once
#include <vector>
#include <string>

// ---------------------------------------------------------------------------
// check_dependencies.h
//
// Runs before the main app starts. Checks that the DLLs WireMerge needs at
// runtime (PortAudio, libusb) are actually reachable -- either next to the
// .exe or somewhere on PATH.
//
// A note on "and then installs the dependencies if they are missing" from
// the original project plan: this build deliberately does NOT silently
// download and execute installers for missing runtime dependencies. That
// pattern (an app fetching and running arbitrary binaries on first launch)
// is exactly what antivirus/SmartScreen heuristics flag, and it takes the
// decision to install something out of the user's hands. Instead, this
// checker detects what's missing and tells the user exactly what to do
// (including a direct link), which is safer and just as fast for a
// single-purpose native tool with only two dependencies.
// ---------------------------------------------------------------------------

namespace wm {

struct DependencyStatus {
    std::string name;
    bool found;
    std::string details; // path found at, or guidance if missing
};

// Checks for portaudio dll and libusb dll reachability.
std::vector<DependencyStatus> CheckDependencies();

// Shows a native message box summarizing missing dependencies and how to
// get them. Returns true if the app should continue launching anyway
// (e.g. all deps found, or user acknowledged and wants to proceed), false
// if the app should exit.
bool ReportDependencyStatus(const std::vector<DependencyStatus>& statuses);

} // namespace wm
