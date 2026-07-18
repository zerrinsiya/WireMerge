#pragma once
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <optional>
#include <memory>
#include "mixer.h"

// ---------------------------------------------------------------------------
// adb_handler.h
//
// Adds Android phones as a WireMerge audio source, using a completely
// different mechanism from usb_handler.h/audio_handler.h:
//
//   USB Audio Class (what usb_handler.h detects, what audio_handler.h
//   captures via PortAudio) is for devices that present themselves as a
//   sound card over USB -- mics, USB DACs, audio interfaces. Plugging in
//   a phone and pressing play on Spotify does NOT do this: Android phones
//   don't route app audio over the USB cable's audio class endpoints, so
//   they never show up in AudioHandler::ListInputDevices().
//
// Instead, this uses the same technique scrcpy/sndcpy use: install a tiny
// helper app on the phone (via ADB, over the same USB cable) that uses
// Android 10+'s Playback Capture API to grab system/app audio, and stream
// it back to the PC over an "adb forward" TCP tunnel. WireMerge shells out
// to adb.exe rather than reimplementing the ADB wire protocol -- it's a
// well-known, free, redistributable Google binary and doing so keeps this
// file small.
//
// Caveats worth surfacing to the user (see gui.cpp tooltip / README):
//   - Requires USB debugging enabled on the phone + one-time RSA key
//     authorization tap.
//   - Requires Android 10+ on the phone.
//   - Playback capture is opt-in per app; apps targeting Android 9 or
//     below don't allow it by default, and some DRM-guarded apps block it
//     regardless. Most mainstream apps (browsers, Spotify, YouTube, etc.)
//     work, but this is not universal and isn't something WireMerge can
//     fix from the PC side.
//   - iOS has no equivalent capability exposed to third parties; this
//     path is Android-only.
// ---------------------------------------------------------------------------

namespace wm {

struct AdbDeviceInfo {
    std::string serial;
    std::string state; // "device", "unauthorized", "offline"
};

class AdbHandler {
public:
    AdbHandler();
    ~AdbHandler();

    // Locates adb.exe (checks tools/adb.exe next to the executable first,
    // then PATH) and sndcpy.apk (tools/sndcpy.apk). Returns false and logs
    // details if either is missing -- this is a real external dependency,
    // unlike PortAudio/libusb, and can't be statically linked in.
    //
    // If autoDownloadIfMissing is true (default), a missing tools/ folder
    // triggers a one-time attempt to fetch adb.exe (from Google's official
    // platform-tools archive) and sndcpy.apk (from its GitHub releases)
    // directly into tools/. This is meant as a rare-case safety net --
    // normal distributions of WireMerge ship tools/ already populated
    // (see README "Packaging a release"), so this path should rarely
    // trigger in practice. Requires internet access; failures are logged
    // and fall back to the existing manual-instructions messaging.
    bool Initialize(bool autoDownloadIfMissing = true);

    bool IsAvailable() const { return adbPath_.has_value() && apkPath_.has_value(); }

    std::vector<AdbDeviceInfo> ListDevices() const;

    // Installs the helper app (if not already installed), grants the
    // capture permission, sets up the forward tunnel, and launches the
    // app -- then starts a background reader thread once connected. This
    // whole setup sequence involves several `adb.exe` round-trips (an APK
    // install can genuinely take several seconds) and previously ran
    // synchronously on whichever thread called it, which -- when called
    // directly from a GUI button handler -- froze the render thread and
    // made Windows mark the window "not responding" for up to ~10s. Now
    // runs on its own background thread; poll IsStarting()/TryTakeStartResult()
    // from the GUI's render loop instead of blocking on this call.
    void StartCaptureAsync(Mixer& mixer, const std::string& deviceSerial, int localPort = 28200);

    // True while a StartCaptureAsync() call for this device is still in
    // progress (install/forward/launch sequence not yet finished).
    bool IsStarting(const std::string& deviceSerial) const;

    // Non-blocking poll for a finished StartCaptureAsync() call. Returns
    // true exactly once per completed start (and consumes/removes the
    // pending-start record at that point) -- outSourceId is set to the
    // new source's ID, or 0 if that start failed. Returns false while
    // still in progress or if there's no pending/completed start for this
    // device at all.
    bool TryTakeStartResult(const std::string& deviceSerial, SourceId& outSourceId);

    void StopCapture(const std::string& deviceSerial);

    // Stops all active capture sessions, tells the phone-side app to stop,
    // tears down adb forward tunnels, and kills the local adb server
    // process. Call this explicitly during application shutdown rather
    // than relying on the destructor -- see main.cpp, where shutdown order
    // and logging need this to have actually completed, not just be
    // "eventually handled" during stack unwind.
    void Shutdown();
    void StopAll();

private:
    struct Session {
        std::string deviceSerial;
        int localPort = 0;
        SourceId sourceId = 0;
        std::thread readerThread;
        std::atomic<bool> running{false};
        // SOCKET is a Windows type (winsock2.h) not included in this header
        // to avoid forcing winsock on every includer; stored as uintptr_t
        // and cast back in the .cpp (SOCKET is itself just a UINT_PTR).
        uintptr_t socket = static_cast<uintptr_t>(~0); // INVALID_SOCKET
    };

    // Runs `adb.exe <args>` synchronously, captures stdout, returns exit code.
    int RunAdb(const std::vector<std::string>& args, std::string& output) const;

    // Spawns `adb.exe <args>` WITHOUT waiting for it to finish -- used
    // during shutdown for cleanup commands (force-stop, forward --remove,
    // kill-server) that don't need to block WireMerge's own exit. Windows
    // child processes are fully independent of their parent by default:
    // not waiting on/closing our handles to it does not kill it, it keeps
    // running to completion on its own. This is what makes app close
    // instant instead of waiting out however long those adb round-trips
    // take (previously ~1-2s, all subprocess-spawn overhead, matching the
    // same root cause as the earlier "Start Capture freezes the window"
    // bug -- blocking subprocess work on a thread that shouldn't wait).
    void FireAndForgetAdb(const std::vector<std::string>& args) const;

    // Rare-case fallback: fetches adb.exe (+ its two companion DLLs) and
    // sndcpy.apk into a tools/ folder next to the executable. See
    // Initialize()'s doc comment for when this actually runs. Returns true
    // only if BOTH ended up successfully in place.
    bool TryAutoDownload(const std::string& toolsDir);

    // The actual (blocking) install/forward/launch sequence -- this is
    // what StartCaptureAsync runs on a background thread. Returns the new
    // SourceId, or 0 on failure.
    SourceId StartCaptureBlocking(Mixer& mixer, const std::string& deviceSerial, int localPort);

    void ReaderLoop(Session* session, Mixer* mixer);

    struct PendingStart {
        std::string deviceSerial;
        std::thread thread;
        std::atomic<bool> done{false};
        SourceId result = 0;
    };

    std::optional<std::string> adbPath_;
    std::optional<std::string> apkPath_;
    // Tracks whether adb.exe has actually been invoked this session (any
    // RunAdb/FireAndForgetAdb call) -- used at shutdown to skip the
    // kill-server cleanup call entirely when adb was never touched (e.g.
    // the user never opened the Android panel), rather than unconditionally
    // spawning a process on every single app close regardless of whether
    // it was ever needed. mutable: RunAdb is const, but still needs to
    // record this.
    mutable bool everInvokedAdb_ = false;
    std::vector<std::unique_ptr<Session>> sessions_;
    std::vector<std::unique_ptr<PendingStart>> pendingStarts_;
};

} // namespace wm
