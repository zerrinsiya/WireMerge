#pragma once
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include "mixer.h"

struct PaStreamCallbackTimeInfo; //from portaudio.h
typedef void PaStream; //opaque PortAudio stream handle

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

    bool Initialize();
    void Shutdown();

    std::vector<AudioDeviceInfo> ListInputDevices() const;
    std::vector<AudioDeviceInfo> ListOutputDevices() const;

    bool RescanDevices();

    bool RescanDevicesAsync();
    bool IsRescanInProgress() const { return rescanInProgress_.load(std::memory_order_acquire); }

    bool TryTakeRescanResult(bool& outSuccess);

    SourceId OpenInputSource(Mixer& mixer, int deviceIndex,
                              int channels = 2, int sampleRate = 48000);
    void CloseInputSource(SourceId id);

    bool OpenOutput(Mixer& mixer, int deviceIndex,
                     int channels = 2, int sampleRate = 48000);
    void CloseOutput();

    bool IsInitialized() const { return initialized_; }

private:
    struct InputStreamHandle {
        PaStream* stream = nullptr;
        SourceId sourceId = 0;
        void* callbackCtx = nullptr; //InputCallbackCtx*
    };

    void RescanDevicesBlocking();

    bool initialized_ = false;
    PaStream* outputStream_ = nullptr;
    void* outputCallbackCtx_ = nullptr; //OutputCallbackCtx*
    std::vector<InputStreamHandle> inputStreams_;

    std::thread rescanThread_;
    std::atomic<bool> rescanInProgress_{false};
    std::atomic<bool> rescanResultReady_{false};
    bool rescanResult_ = false;
};

}
