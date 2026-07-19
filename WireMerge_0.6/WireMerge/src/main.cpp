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

    // Modest process priority boost. Deliberately ABOVE_NORMAL, not
    // HIGH_PRIORITY_CLASS: HIGH would make WireMerge preempt essentially
    // everything else on a low-end, few-core machine -- exactly the
    // hardware this whole project targets -- which directly fights the
    // "lightweight, don't burden the low-end PC" goal from the original
    // design. ABOVE_NORMAL gives a real scheduling edge over default-
    // priority background work without WireMerge starving the very
    // foreground task (game, work, etc) the user is trying to protect.
    // The actual hard-realtime protection for the audio callback threads
    // themselves is MMCSS registration (see audio_handler.cpp/
    // adb_handler.cpp) -- this process-level bump is a secondary,
    // complementary measure, not the primary fix.
    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
        WM_LOG_WARN("Could not raise process priority (GetLastError=" +
                     std::to_string(GetLastError()) + "); continuing at normal priority.");
    }

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
    WM_LOG_INFO("PortAudio ready.");

    wm::UsbHandler usb;
    bool usbReady = usb.Initialize();
    if (!usbReady) {
        // Non-fatal: USB hotplug notifications are a convenience feature.
        // Audio still works via PortAudio device enumeration without it.
        WM_LOG_WARN("libusb failed to initialize; USB connect/disconnect "
                     "notifications will be unavailable, but audio routing "
                     "will still work via manual device selection.");
    } else {
        WM_LOG_INFO("libusb ready.");
    }

    wm::Mixer mixer;

    wm::AdbHandler adb;
    bool adbReady = adb.Initialize();
    if (!adbReady) {
        // Non-fatal, and expected on a fresh install: adb.exe/sndcpy.apk
        // are optional extras the user places in tools/ themselves (see
        // README) rather than something WireMerge bundles or auto-fetches.
        // Android app-audio capture just won't be offered in the GUI until
        // they're present; USB mic/DAC input and everything else works
        // regardless.
        WM_LOG_WARN("Android (ADB) audio capture unavailable -- see tools/ "
                     "folder requirements in README.md if you want this feature.");
    } else {
        WM_LOG_INFO("ADB + sndcpy ready -- Android app-audio capture available.");
    }

    // Consolidated boot summary (3.1) -- one line covering every
    // subsystem's readiness, so a tester can see at a glance what's
    // actually available this session without piecing it together from
    // the individual messages above.
    WM_LOG_INFO(std::string("Initialization complete. PortAudio: OK, libusb: ") +
                (usbReady ? "OK" : "unavailable") + ", ADB/sndcpy: " +
                (adbReady ? "OK" : "unavailable") + ".");

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
    // adb.Shutdown() is fire-and-forget for its cleanup adb.exe calls (see
    // adb_handler.cpp's FireAndForgetAdb) -- it no longer blocks waiting
    // on those, so this ordering is about correctness (stop before audio/
    // usb teardown) rather than absorbing a multi-second wait.
    adb.Shutdown();
    audio.Shutdown();
    usb.Shutdown();

    WM_LOG_INFO("WireMerge shut down cleanly.");
    return 0;
}
