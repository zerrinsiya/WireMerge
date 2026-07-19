#pragma once
#include <string>
#include <vector>
#include <memory>
#include "mixer.h"

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

    bool IsInitialized() const { return initialized_; }

private:
    struct InputStreamHandle {
        PaStream* stream = nullptr;
        SourceId sourceId = 0;
    };

    bool initialized_ = false;
    PaStream* outputStream_ = nullptr;
    std::vector<InputStreamHandle> inputStreams_;
};

} // namespace wm
