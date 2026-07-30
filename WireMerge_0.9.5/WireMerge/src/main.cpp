#include <windows.h>
#include <chrono>
#include <cstdio>
#include "utils.h"
#include "check_dependencies.h"
#include "audio_handler.h"
#include "usb_handler.h"
#include "adb_handler.h"
#include "mixer.h"
#include "gui.h"

int WINAPI WinMain(HINSTANCE /*hInstance*/, HINSTANCE /*hPrevInstance*/,
                   LPSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    wm::Logger::Instance().SetLogFile("WireMerge.log");
    WM_LOG_INFO(std::string("WireMerge starting up. Version: ") + wm::kWireMergeVersion);

    using Clock = std::chrono::steady_clock;
    const auto bootStart = Clock::now();
    auto LogBootStage = [&](const char* label) {
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - bootStart).count();
        char buf[96];
        snprintf(buf, sizeof(buf), "[boot] %s: %.1fms since process start", label, ms);
        WM_LOG_INFO(buf);
    };

    if (!SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS)) {
        WM_LOG_WARN("Could not raise process priority (GetLastError=" +
                     std::to_string(GetLastError()) + "); continuing at normal priority.");
    }

    auto depStatuses = wm::CheckDependencies();
    if (!wm::ReportDependencyStatus(depStatuses)) {
        WM_LOG_INFO("User chose to exit due to missing dependencies.");
        return 1;
    }
    LogBootStage("CheckDependencies done");

    wm::AudioHandler audio;
    if (!audio.Initialize()) {
        MessageBoxA(nullptr,
                    "Failed to initialize audio (PortAudio). See WireMerge.log for details.",
                    "WireMerge - Fatal Error", MB_ICONERROR);
        return 1;
    }
    WM_LOG_INFO("PortAudio ready.");
    wm::UiLog::Instance().Push("Audio engine ready.");
    LogBootStage("AudioHandler::Initialize (Pa_Initialize) done");

    wm::UsbHandler usb;
    bool usbReady = usb.Initialize();
    if (!usbReady) {
        WM_LOG_WARN("libusb failed to initialize; USB connect/disconnect "
                     "notifications will be unavailable, but audio routing "
                     "will still work via manual device selection.");
    } else {
        WM_LOG_INFO("libusb ready.");
    }
    LogBootStage("UsbHandler::Initialize (libusb_init) done");

    wm::Mixer mixer;

    wm::AdbHandler adb;
    adb.InitializeAsync(/*autoDownloadIfMissing=*/false);
    wm::UiLog::Instance().Push("Android capture: checking for adb/sndcpy in tools/ "
                                "(see Log for detail once ready)...");

    std::string bootSummary = std::string("Startup complete. PortAudio: OK, USB hotplug: ") +
                (usbReady ? "OK" : "unavailable") + ", Android capture: checking...";
    WM_LOG_INFO(bootSummary);
    wm::UiLog::Instance().Push(bootSummary);

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
    LogBootStage("Gui::Initialize (window created) done");

    gui.Run();

    gui.Shutdown();
    adb.Shutdown();
    audio.Shutdown();
    usb.Shutdown();

    WM_LOG_INFO("WireMerge shut down cleanly.");
    return 0;
}
