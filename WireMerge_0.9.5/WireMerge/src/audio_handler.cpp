#include "audio_handler.h"
#include "utils.h"
#include <portaudio.h>
#include <pa_win_wasapi.h>
#include <cstring>
#include <algorithm>
#include <windows.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

namespace wm {

static thread_local bool s_mmcssAttempted = false;

static void EnsureMmcssRegistered() {
    if (s_mmcssAttempted) return;
    s_mmcssAttempted = true;

    DWORD taskIndex = 0;
    HANDLE h = AvSetMmThreadCharacteristicsA("Pro Audio", &taskIndex);
    if (!h) {
        WM_LOG_WARN("AudioHandler: AvSetMmThreadCharacteristics(\"Pro Audio\") failed "
                     "(GetLastError=" + std::to_string(GetLastError()) + "). Audio will "
                     "still work but may be more prone to stutter under background CPU load.");
    }
}

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
    if (rescanThread_.joinable()) rescanThread_.join();
    if (!initialized_) return;
    CloseOutput();
    for (auto& h : inputStreams_) {
        if (h.stream) {
            Pa_StopStream(h.stream);
            Pa_CloseStream(h.stream);
        }
        delete static_cast<InputCallbackCtx*>(h.callbackCtx);
    }
    inputStreams_.clear();
    Pa_Terminate();
    initialized_ = false;
}

bool AudioHandler::RescanDevices() {
    if (!initialized_) return false;
    if (outputStream_ || !inputStreams_.empty()) {
        WM_LOG_WARN("Device rescan refused: stop active output/inputs first "
                     "(a rescan tears down and reinitializes PortAudio, which "
                     "would force-close any open streams).");
        return false;
    }
    Pa_Terminate();
    initialized_ = false;
    PaError err = Pa_Initialize();
    if (err != paNoError) {
        WM_LOG_ERROR(std::string("Device rescan failed to reinitialize PortAudio: ") + Pa_GetErrorText(err));
        return false;
    }
    initialized_ = true;
    return true;
}

bool AudioHandler::RescanDevicesAsync() {
    if (!initialized_) return false;
    if (outputStream_ || !inputStreams_.empty()) {
        WM_LOG_WARN("Device rescan refused: stop active output/inputs first "
                     "(a rescan tears down and reinitializes PortAudio, which "
                     "would force-close any open streams).");
        return false;
    }
    bool expected = false;
    if (!rescanInProgress_.compare_exchange_strong(expected, true)) {
        return false;
    }
    if (rescanThread_.joinable()) rescanThread_.join();
    rescanThread_ = std::thread(&AudioHandler::RescanDevicesBlocking, this);
    return true;
}

void AudioHandler::RescanDevicesBlocking() {
    Pa_Terminate();
    initialized_ = false;
    PaError err = Pa_Initialize();
    bool ok = (err == paNoError);
    if (!ok) {
        WM_LOG_ERROR(std::string("Device rescan failed to reinitialize PortAudio: ") + Pa_GetErrorText(err));
    }
    initialized_ = ok;
    rescanResult_ = ok;
    rescanResultReady_.store(true, std::memory_order_release);
    rescanInProgress_.store(false, std::memory_order_release);
}

bool AudioHandler::TryTakeRescanResult(bool& outSuccess) {
    if (!rescanResultReady_.load(std::memory_order_acquire)) return false;
    outSuccess = rescanResult_;
    rescanResultReady_.store(false, std::memory_order_release);
    return true;
}

static PaHostApiIndex FindWasapiHostApiIndex() {
    return Pa_HostApiTypeIdToHostApiIndex(paWASAPI);
}

std::vector<AudioDeviceInfo> AudioHandler::ListInputDevices() const {
    std::vector<AudioDeviceInfo> devices;
    int count = Pa_GetDeviceCount();
    int defaultIn = Pa_GetDefaultInputDevice();
    PaHostApiIndex wasapiIdx = FindWasapiHostApiIndex();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxInputChannels <= 0) continue;
        if (wasapiIdx >= 0 && info->hostApi != wasapiIdx) continue;
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
    PaHostApiIndex wasapiIdx = FindWasapiHostApiIndex();
    for (int i = 0; i < count; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (!info || info->maxOutputChannels <= 0) continue;
        if (wasapiIdx >= 0 && info->hostApi != wasapiIdx) continue;
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

static PaWasapiStreamInfo MakeWasapiAutoConvertInfo() {
    PaWasapiStreamInfo info{};
    info.size = sizeof(PaWasapiStreamInfo);
    info.hostApiType = paWASAPI;
    info.version = 1;
    info.flags = paWinWasapiAutoConvert;
    return info;
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

    auto* ctx = new InputCallbackCtx{&mixer, sourceId};

    PaWasapiStreamInfo wasapiInfo = MakeWasapiAutoConvertInfo();
    PaStreamParameters inParams{};
    inParams.device = deviceIndex;
    inParams.channelCount = channels;
    inParams.sampleFormat = paFloat32;
    inParams.suggestedLatency = devInfo->defaultLowInputLatency;
    inParams.hostApiSpecificStreamInfo = &wasapiInfo;

    PaStream* stream = nullptr;
    PaError err = Pa_OpenStream(&stream, &inParams, nullptr, sampleRate,
                                 paFramesPerBufferUnspecified, paNoFlag,
                                 InputCallback, ctx);
    if (err != paNoError) {
        WM_LOG_WARN(std::string("Input stream open failed at ") + std::to_string(sampleRate) +
                    "Hz (" + Pa_GetErrorText(err) + "), retrying at device's native " +
                    std::to_string(static_cast<int>(devInfo->defaultSampleRate)) + "Hz...");
        int nativeRate = static_cast<int>(devInfo->defaultSampleRate);
        err = Pa_OpenStream(&stream, &inParams, nullptr, nativeRate,
                             paFramesPerBufferUnspecified, paNoFlag,
                             InputCallback, ctx);
        if (err == paNoError) {
            WM_LOG_WARN("Opened at native rate. This source may sound pitch/speed-shifted "
                        "relative to other sources since it isn't resampled to match them.");
        }
    }
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

    inputStreams_.push_back({stream, sourceId, ctx});
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
            delete static_cast<InputCallbackCtx*>(it->callbackCtx);
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
    outputCallbackCtx_ = ctx;

    PaWasapiStreamInfo wasapiInfo = MakeWasapiAutoConvertInfo();
    PaStreamParameters outParams{};
    outParams.device = deviceIndex;
    outParams.channelCount = channels;
    outParams.sampleFormat = paFloat32;
    outParams.suggestedLatency = devInfo->defaultLowOutputLatency;
    outParams.hostApiSpecificStreamInfo = &wasapiInfo;

    PaError err = Pa_OpenStream(&outputStream_, nullptr, &outParams, sampleRate,
                                 paFramesPerBufferUnspecified, paNoFlag,
                                 OutputCallback, ctx);
    if (err != paNoError) {
        WM_LOG_WARN(std::string("Output stream open failed at ") + std::to_string(sampleRate) +
                    "Hz (" + Pa_GetErrorText(err) + "), retrying at device's native " +
                    std::to_string(static_cast<int>(devInfo->defaultSampleRate)) + "Hz...");
        int nativeRate = static_cast<int>(devInfo->defaultSampleRate);
        err = Pa_OpenStream(&outputStream_, nullptr, &outParams, nativeRate,
                             paFramesPerBufferUnspecified, paNoFlag,
                             OutputCallback, ctx);
        if (err == paNoError) {
            WM_LOG_WARN("Opened at native rate. Sources running at a different rate than "
                        "this may sound pitch/speed-shifted since Mixer doesn't resample.");
        }
    }
    if (err != paNoError) {
        WM_LOG_ERROR(std::string("Failed to open output stream: ") + Pa_GetErrorText(err));
        delete ctx;
        outputCallbackCtx_ = nullptr;
        return false;
    }

    err = Pa_StartStream(outputStream_);
    if (err != paNoError) {
        WM_LOG_ERROR(std::string("Failed to start output stream: ") + Pa_GetErrorText(err));
        Pa_CloseStream(outputStream_);
        outputStream_ = nullptr;
        delete ctx;
        outputCallbackCtx_ = nullptr;
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
    delete static_cast<OutputCallbackCtx*>(outputCallbackCtx_);
    outputCallbackCtx_ = nullptr;
}

}
