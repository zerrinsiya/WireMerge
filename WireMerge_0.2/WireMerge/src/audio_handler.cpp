#include "audio_handler.h"
#include "utils.h"
#include <portaudio.h>
#include <cstring>
#include <algorithm>
#include <windows.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

namespace wm {

// ---------------------------------------------------------------------------
// MMCSS registration (Multimedia Class Scheduler Service)
//
// Root cause of "audio stutters when WireMerge loses focus and something
// else opens a window": MMCSS's scheduling explicitly factors in whether a
// thread belongs to the foreground application (see Microsoft's own docs
// on MMCSS scheduling categories). PortAudio's callback threads here were
// running as plain, unregistered threads -- so when WireMerge lost focus,
// they had no protection against being starved by a burst of CPU activity
// from whatever became the new foreground app, on a low-end CPU with few
// cores to absorb that. This is a genuine, well-documented Windows
// mechanism, not a vague "priority issue" -- registering with MMCSS's
// "Pro Audio" task is the standard, correct fix used by real audio/DAW
// software for exactly this symptom.
//
// PortAudio doesn't expose a hook for "stream thread just started," so
// registration happens lazily on each callback thread's first invocation,
// gated by a thread_local flag (each PortAudio stream's callback runs
// consistently on one thread for the stream's lifetime, so this only
// actually calls into MMCSS once per thread, not once per buffer).
// ---------------------------------------------------------------------------
static thread_local bool s_mmcssAttempted = false;

static void EnsureMmcssRegistered() {
    if (s_mmcssAttempted) return;
    s_mmcssAttempted = true; // set first -- avoid retrying every single callback if this fails once

    DWORD taskIndex = 0;
    HANDLE h = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
    if (!h) {
        // Non-fatal: audio still works, just without the scheduling
        // protection -- log once per thread rather than spamming.
        WM_LOG_WARN("AudioHandler: AvSetMmThreadCharacteristics(\"Pro Audio\") failed "
                     "(GetLastError=" + std::to_string(GetLastError()) + "). Audio will "
                     "still work but may be more prone to stutter under background CPU load.");
    }
    // Intentionally not reverted: PortAudio owns this thread's lifecycle
    // and we don't get a callback for "stream is closing" on this same
    // thread to revert from; Windows releases the MMCSS registration
    // automatically when the thread itself terminates.
}

// Context passed into the input callback so it knows which mixer source
// to push samples into. Owned for the lifetime of the stream.
struct InputCallbackCtx {
    Mixer* mixer;
    SourceId sourceId;
};

static int InputCallback(const void* input, void* /*output*/,
                          unsigned long frameCount,
                          const PaStreamCallbackTimeInfo* /*timeInfo*/,
                          PaStreamCallbackFlags /*statusFlags*/,
                          void* userData) {
    EnsureMmcssRegistered();
    auto* ctx = static_cast<InputCallbackCtx*>(userData);
    if (input && ctx && ctx->mixer) {
        ctx->mixer->PushSamples(ctx->sourceId,
                                 static_cast<const float*>(input),
                                 frameCount);
    }
    return paContinue;
}

struct OutputCallbackCtx {
    Mixer* mixer;
    int channels;
};

static int OutputCallback(const void* /*input*/, void* output,
                           unsigned long frameCount,
                           const PaStreamCallbackTimeInfo* /*timeInfo*/,
                           PaStreamCallbackFlags /*statusFlags*/,
                           void* userData) {
    EnsureMmcssRegistered();
    auto* ctx = static_cast<OutputCallbackCtx*>(userData);
    float* out = static_cast<float*>(output);
    if (ctx && ctx->mixer) {
        ctx->mixer->Mix(out, frameCount, ctx->channels);
    } else {
        std::memset(out, 0, sizeof(float) * frameCount * (ctx ? ctx->channels : 2));
    }
    return paContinue;
}

AudioHandler::AudioHandler() = default;

AudioHandler::~AudioHandler() {
    Shutdown();
}

bool AudioHandler::Initialize() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        WM_LOG_ERROR(std::string("PortAudio init failed: ") + Pa_GetErrorText(err));
        return false;
    }
    initialized_ = true;
    WM_LOG_INFO("PortAudio initialized. Host APIs: " + std::to_string(Pa_GetHostApiCount()));
    return true;
}

void AudioHandler::Shutdown() {
    if (!initialized_) return;
    CloseOutput();
    for (auto& h : inputStreams_) {
        if (h.stream) {
            Pa_StopStream(h.stream);
            Pa_CloseStream(h.stream);
        }
    }
    inputStreams_.clear();
    Pa_Terminate();
    initialized_ = false;
}

std::vector<AudioDeviceInfo> AudioHandler::ListInputDevices() const {
    std::vector<AudioDeviceInfo> devices;
    int count = Pa_GetDeviceCount();
    int defaultIn = Pa_GetDefaultInputDevice();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels <= 0) continue;
        AudioDeviceInfo d;
        d.index = i;
        d.name = info->name ? info->name : "Unknown Device";
        d.maxInputChannels = info->maxInputChannels;
        d.maxOutputChannels = info->maxOutputChannels;
        d.defaultSampleRate = info->defaultSampleRate;
        d.isDefaultInput = (i == defaultIn);
        d.isDefaultOutput = false;
        devices.push_back(d);
    }
    return devices;
}

std::vector<AudioDeviceInfo> AudioHandler::ListOutputDevices() const {
    std::vector<AudioDeviceInfo> devices;
    int count = Pa_GetDeviceCount();
    int defaultOut = Pa_GetDefaultOutputDevice();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;
        AudioDeviceInfo d;
        d.index = i;
        d.name = info->name ? info->name : "Unknown Device";
        d.maxInputChannels = info->maxInputChannels;
        d.maxOutputChannels = info->maxOutputChannels;
        d.defaultSampleRate = info->defaultSampleRate;
        d.isDefaultInput = false;
        d.isDefaultOutput = (i == defaultOut);
        devices.push_back(d);
    }
    return devices;
}

SourceId AudioHandler::OpenInputSource(Mixer& mixer, int deviceIndex,
                                        int channels, int sampleRate) {
    const PaDeviceInfo* devInfo = Pa_GetDeviceInfo(deviceIndex);
    if (!devInfo) {
        WM_LOG_ERROR("OpenInputSource: invalid device index " + std::to_string(deviceIndex));
        return 0;
    }
    channels = std::min(channels, devInfo->maxInputChannels);
    if (channels <= 0) channels = 1;

    SourceId sourceId = mixer.AddSource(devInfo->name ? devInfo->name : "USB Input",
                                         sampleRate, channels);

    // ctx must outlive the stream; leaked deliberately for stream lifetime
    // and freed in CloseInputSource via the stream's owning struct below.
    auto* ctx = new InputCallbackCtx{&mixer, sourceId};

    PaStreamParameters inParams{};
    inParams.device = deviceIndex;
    inParams.channelCount = channels;
    inParams.sampleFormat = paFloat32;
    inParams.suggestedLatency = devInfo->defaultLowInputLatency;
    inParams.hostApiSpecificStreamInfo = nullptr;

    PaStream* stream = nullptr;
    PaError err = Pa_OpenStream(&stream, &inParams, nullptr, sampleRate,
                                 paFramesPerBufferUnspecified, paNoFlag,
                                 InputCallback, ctx);
    if (err != paNoError) {
        WM_LOG_ERROR(std::string("Failed to open input stream: ") + Pa_GetErrorText(err));
        delete ctx;
        mixer.RemoveSource(sourceId);
        return 0;
    }

    err = Pa_StartStream(stream);
    if (err != paNoError) {
        WM_LOG_ERROR(std::string("Failed to start input stream: ") + Pa_GetErrorText(err));
        Pa_CloseStream(stream);
        delete ctx;
        mixer.RemoveSource(sourceId);
        return 0;
    }

    inputStreams_.push_back({stream, sourceId});
    WM_LOG_INFO("Opened input source '" + std::string(devInfo->name) +
                "' as source id " + std::to_string(sourceId));
    return sourceId;
}

void AudioHandler::CloseInputSource(SourceId id) {
    for (auto it = inputStreams_.begin(); it != inputStreams_.end(); ++it) {
        if (it->sourceId == id) {
            if (it->stream) {
                Pa_StopStream(it->stream);
                Pa_CloseStream(it->stream);
            }
            inputStreams_.erase(it);
            break;
        }
    }
}

bool AudioHandler::OpenOutput(Mixer& mixer, int deviceIndex, int channels, int sampleRate) {
    const PaDeviceInfo* devInfo = Pa_GetDeviceInfo(deviceIndex);
    if (!devInfo) {
        WM_LOG_ERROR("OpenOutput: invalid device index " + std::to_string(deviceIndex));
        return false;
    }
    channels = std::min(channels, devInfo->maxOutputChannels);
    if (channels <= 0) channels = 2;

    auto* ctx = new OutputCallbackCtx{&mixer, channels};

    PaStreamParameters outParams{};
    outParams.device = deviceIndex;
    outParams.channelCount = channels;
    outParams.sampleFormat = paFloat32;
    outParams.suggestedLatency = devInfo->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = nullptr;

    PaError err = Pa_OpenStream(&outputStream_, nullptr, &outParams, sampleRate,
                                 paFramesPerBufferUnspecified, paNoFlag,
                                 OutputCallback, ctx);
    if (err != paNoError) {
        WM_LOG_ERROR(std::string("Failed to open output stream: ") + Pa_GetErrorText(err));
        delete ctx;
        return false;
    }

    err = Pa_StartStream(outputStream_);
    if (err != paNoError) {
        WM_LOG_ERROR(std::string("Failed to start output stream: ") + Pa_GetErrorText(err));
        Pa_CloseStream(outputStream_);
        outputStream_ = nullptr;
        delete ctx;
        return false;
    }

    WM_LOG_INFO("Opened output device '" + std::string(devInfo->name) + "'");
    return true;
}

void AudioHandler::CloseOutput() {
    if (outputStream_) {
        Pa_StopStream(outputStream_);
        Pa_CloseStream(outputStream_);
        outputStream_ = nullptr;
    }
}

std::string AudioHandler::GetStableDeviceId(const AudioDeviceInfo& device, bool isInput) const {
    // Same caching lesson as the ADB device list bug (see gui.cpp's
    // RenderAndroidPanel fix): WASAPI enumeration involves real COM work
    // (CoCreateInstance, property store queries per device), and this can
    // be called from GUI code every frame -- so cache with a refresh
    // interval instead of re-enumerating on every call.
    constexpr long long kCacheIntervalMs = 2000;
    long long now = static_cast<long long>(GetTickCount64());
    if (now - wasapiCacheTimeMs_ >= kCacheIntervalMs) {
        wasapiCache_ = WasapiIdentity::EnumerateEndpoints();
        wasapiCacheTimeMs_ = now;
    }

    auto match = WasapiIdentity::FindByName(wasapiCache_, device.name, isInput);
    if (!match) return "";

    // Store as UTF-8 for the public API's plain std::string return type --
    // wasapi_identity.h keeps the raw ID as std::wstring internally since
    // that's IMMDevice::GetId()'s native form, but callers of this method
    // (GUI display code, future config persistence) don't need to deal
    // with wide strings for what's ultimately just an opaque identity key.
    int len = WideCharToMultiByte(CP_UTF8, 0, match->endpointId.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string utf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, match->endpointId.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    return utf8;
}

std::string AudioHandler::GetDeviceFingerprint(const AudioDeviceInfo& device, bool isInput) const {
    constexpr long long kCacheIntervalMs = 2000;
    long long now = static_cast<long long>(GetTickCount64());
    if (now - wasapiCacheTimeMs_ >= kCacheIntervalMs) {
        wasapiCache_ = WasapiIdentity::EnumerateEndpoints();
        wasapiCacheTimeMs_ = now;
    }

    auto match = WasapiIdentity::FindByName(wasapiCache_, device.name, isInput);
    if (!match) return "";
    return WasapiIdentity::ShortFingerprint(match->endpointId);
}

} // namespace wm
