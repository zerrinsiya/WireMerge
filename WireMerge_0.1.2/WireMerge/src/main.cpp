#include <windows.h>
#include "utils.h"
#include "check_dependencies.h"
#include "audio_handler.h"
#include "usb_handler.h"
#include "adb_handler.h"
#include "mixer.h"
#include "gui.h"

// ---------------------------------------------------------------------------
// main.cpp
//
// Startup sequence:
//   1. Set up logging.
//   2. Check runtime dependencies (PortAudio/libusb DLLs) are reachable;
//      warn and offer to abort if not (see check_dependencies.cpp for why
//      this doesn't silently auto-install anything).
//   3. Init PortAudio (AudioHandler) and libusb (UsbHandler).
//   4. Create the Mixer that ties input sources to the output stream.
//   5. Launch the ImGui/Win32/DX11 GUI loop; it owns the rest of the
//      session (adding/removing sources, starting output, etc).
//   6. Clean shutdown on window close.
// ---------------------------------------------------------------------------

int WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                    LPSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    wm::Logger::Instance().SetLogFile("WireMerge.log");
    WM_LOG_INFO(std::string("WireMerge starting up. Version: ") + wm::kWireMergeVersion);

    auto depStatuses = wm::CheckDependencies();
    if (!wm::ReportDependencyStatus(depStatuses)) {
        WM_LOG_INFO("User chose to exit due to missing dependencies.");
        return 1;
    }

    wm::AudioHandler audio;
    if (!audio.Initialize()) {
        MessageBoxA(nullptr,
                    "Failed to initialize audio (PortAudio). See WireMerge.log for details.",
                    "WireMerge - Fatal Error", MB_ICONERROR);
        return 1;
    }

    wm::UsbHandler usb;
    if (!usb.Initialize()) {
        // Non-fatal: USB hotplug notifications are a convenience feature.
        // Audio still works via PortAudio device enumeration without it.
        WM_LOG_WARN("libusb failed to initialize; USB connect/disconnect "
                     "notifications will be unavailable, but audio routing "
                     "will still work via manual device selection.");
    }

    wm::Mixer mixer;

    wm::AdbHandler adb;
    if (!adb.Initialize()) {
        // Non-fatal, and expected on a fresh install: adb.exe/sndcpy.apk
        // are optional extras the user places in tools/ themselves (see
        // README) rather than something WireMerge bundles or auto-fetches.
        // Android app-audio capture just won't be offered in the GUI until
        // they're present; USB mic/DAC input and everything else works
        // regardless.
        WM_LOG_WARN("Android (ADB) audio capture unavailable -- see tools/ "
                     "folder requirements in README.md if you want this feature.");
    }

    wm::Gui gui(audio, usb, adb, mixer);
    if (!gui.Initialize()) {
        MessageBoxA(nullptr,
                    "Failed to initialize the application window (DirectX 11). "
                    "See WireMerge.log for details.",
                    "WireMerge - Fatal Error", MB_ICONERROR);
        adb.Shutdown();
        audio.Shutdown();
        usb.Shutdown();
        return 1;
    }

    gui.Run();

    gui.Shutdown();
    // adb.Shutdown() runs before the "shut down cleanly" log line on
    // purpose -- it can take a few seconds (force-stopping the phone-side
    // app, tearing down the forward tunnel, killing the local adb server),
    // and previously this only happened implicitly via AdbHandler's
    // destructor during stack unwind, which meant the log claimed a clean
    // shutdown before that work had actually finished.
    adb.Shutdown();
    audio.Shutdown();
    usb.Shutdown();

    WM_LOG_INFO("WireMerge shut down cleanly.");
    return 0;
}
