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
    // capture permission, sets up the forward tunnel, launches the app,
    // and starts a background thread reading PCM off the socket into
    // `mixer` as a new source. Returns the new SourceId, or 0 on failure.
    // sampleRate/channels must match what the helper actually emits
    // (sndcpy emits 48000 Hz stereo 16-bit PCM; converted to float32 here
    // before handing to the mixer, since Mixer/RingBuffer work in float32
    // throughout to stay consistent with the PortAudio path).
    SourceId StartCapture(Mixer& mixer, const std::string& deviceSerial,
                           int localPort = 28200);

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

    // Rare-case fallback: fetches adb.exe (+ its two companion DLLs) and
    // sndcpy.apk into a tools/ folder next to the executable. See
    // Initialize()'s doc comment for when this actually runs. Returns true
    // only if BOTH ended up successfully in place.
    bool TryAutoDownload(const std::string& toolsDir);

    void ReaderLoop(Session* session, Mixer* mixer);

    std::optional<std::string> adbPath_;
    std::optional<std::string> apkPath_;
    std::vector<std::unique_ptr<Session>> sessions_;
};

} // namespace wm
