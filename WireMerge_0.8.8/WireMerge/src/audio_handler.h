#pragma once
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
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

    // Forces PortAudio to actually re-enumerate hardware (WASAPI) instead
    // of returning the same snapshot it took at Initialize() time -- see
    // .cpp for why "Rescan Devices" previously did nothing until a full
    // app restart.
    bool RescanDevices();

    // Non-blocking version of RescanDevices(): runs the same
    // Pa_Terminate()/Pa_Initialize() sequence on a background thread
    // instead of blocking the caller. Profiling confirmed this call was
    // sitting directly in the render-thread call chain (RenderInputsContent
    // -> RescanDevices), same class of stutter as ADB's device polling
    // fix below. Same refusal rule as RescanDevices() -- if output/inputs
    // are still open, or a rescan is already running, this returns false
    // immediately without spawning a thread, so the refusal-and-log path
    // stays synchronous exactly as it is today. Callers must gate any
    // stream-opening UI (Add Source / Start Output) on
    // IsRescanInProgress() while a rescan is in flight -- unlike the old
    // synchronous call, which incidentally blocked the whole render
    // thread (and therefore every other button) for its duration, this
    // one does not, so that protection has to be explicit now.
    bool RescanDevicesAsync();
    bool IsRescanInProgress() const { return rescanInProgress_.load(std::memory_order_acquire); }

    // Non-blocking poll: true exactly once when a rescan started via
    // RescanDevicesAsync() has finished, with outSuccess mirroring
    // RescanDevices()'s own bool return.
    bool TryTakeRescanResult(bool& outSuccess);

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
        // Perf round: the InputCallbackCtx heap-allocated in
        // OpenInputSource() was never freed anywhere -- a real leak on
        // every source removal. Tracked here (opaque, like PaStream*
        // above, to avoid pulling the callback-context struct definition
        // into this header) so CloseInputSource()/Shutdown() can free it.
        void* callbackCtx = nullptr;
    };

    // Background-thread body for RescanDevicesAsync() -- identical logic
    // to RescanDevices(), minus the stream-open refusal check (already
    // done on the calling thread before this gets spawned at all).
    void RescanDevicesBlocking();

    bool initialized_ = false;
    PaStream* outputStream_ = nullptr;
    // Perf round: same leak class as InputStreamHandle::callbackCtx above,
    // for the single output stream's OutputCallbackCtx.
    void* outputCallbackCtx_ = nullptr;
    std::vector<InputStreamHandle> inputStreams_;

    // Single-writer (background thread) / single-reader (whoever calls
    // TryTakeRescanResult) handoff: rescanResult_ is written strictly
    // before the release store to rescanResultReady_, and only read
    // after the matching acquire load observes it, so no mutex is
    // needed here -- same technique AdbHandler already uses for
    // asyncInitDone_/PendingStart::done.
    std::thread rescanThread_;
    std::atomic<bool> rescanInProgress_{false};
    std::atomic<bool> rescanResultReady_{false};
    bool rescanResult_ = false;
};

} // namespace wm
