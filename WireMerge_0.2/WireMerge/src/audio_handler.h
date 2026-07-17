#pragma once
#include <string>
#include <vector>
#include <memory>
#include "mixer.h"
#include "wasapi_identity.h"

struct PaStreamCallbackTimeInfo; // fwd decl, real def comes from portaudio.h in .cpp
typedef void PaStream; // opaque, avoids forcing portaudio.h on every includer

// ---------------------------------------------------------------------------
// audio_handler.h
//
// Thin wrapper around PortAudio. Handles:
//   - Enumerating input devices (this is how USB audio devices -- phones,
//     TVs, USB sound cards -- show up once Windows has already installed
//     a driver for them; PortAudio talks to them via WASAPI/MME/DirectSound
//     without us needing to touch raw USB endpoints).
//   - Opening one input stream per selected source, each writing straight
//     into the Mixer's ring buffer for that source.
//   - Opening a single output stream that pulls the mixed result.
// ---------------------------------------------------------------------------

namespace wm {

struct AudioDeviceInfo {
    int index;
    std::string name;
    int maxInputChannels;
    int maxOutputChannels;
    double defaultSampleRate;
    bool isDefaultInput;
    bool isDefaultOutput;
};

class AudioHandler {
public:
    AudioHandler();
    ~AudioHandler();

    // Must be called once before anything else. Returns false + logs on failure.
    bool Initialize();
    void Shutdown();

    std::vector<AudioDeviceInfo> ListInputDevices() const;
    std::vector<AudioDeviceInfo> ListOutputDevices() const;

    // Opens an input stream on `deviceIndex` and registers a new source in
    // `mixer` for it. Returns the new SourceId, or 0 on failure.
    SourceId OpenInputSource(Mixer& mixer, int deviceIndex,
                              int channels = 2, int sampleRate = 48000);
    void CloseInputSource(SourceId id);

    // Opens the single output stream that plays the mixed signal.
    bool OpenOutput(Mixer& mixer, int deviceIndex,
                     int channels = 2, int sampleRate = 48000);
    void CloseOutput();

    // Real, stable per-device identity (Windows IMMDevice endpoint ID),
    // distinct from the display name in AudioDeviceInfo::name -- two
    // identical-model USB devices can share the exact same name string,
    // but never share this ID. See wasapi_identity.h for why PortAudio's
    // public API alone can't provide this. isInput must match which list
    // (ListInputDevices/ListOutputDevices) the device came from -- some
    // devices are capture+render capable and could otherwise be ambiguous.
    // Returns empty if no match was found (e.g. a non-WASAPI host API
    // device, or correlation failed).
    std::string GetStableDeviceId(const AudioDeviceInfo& device, bool isInput) const;

    // Short (8 hex char) fingerprint derived from GetStableDeviceId,
    // suitable for appending to a display label so testers can visually
    // tell apart two identically-named devices. Empty if no ID was found.
    std::string GetDeviceFingerprint(const AudioDeviceInfo& device, bool isInput) const;

    bool IsInitialized() const { return initialized_; }

private:
    struct InputStreamHandle {
        PaStream* stream = nullptr;
        SourceId sourceId = 0;
    };

    bool initialized_ = false;
    PaStream* outputStream_ = nullptr;
    std::vector<InputStreamHandle> inputStreams_;

    // Cached WASAPI endpoint enumeration for GetStableDeviceId/
    // GetDeviceFingerprint -- COM enumeration isn't free, and these can be
    // called from GUI code on every frame (device dropdowns), so cache
    // with a time-based refresh rather than repeat the same mistake as
    // the ADB device list originally calling out to adb.exe every frame
    // (see gui.cpp's RenderAndroidPanel fix for that one). mutable because
    // these are logically const queries that need to update an internal
    // cache.
    mutable std::vector<WasapiEndpointIdentity> wasapiCache_;
    mutable long long wasapiCacheTimeMs_ = -1000000;
};

} // namespace wm
