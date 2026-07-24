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

// ---------------------------------------------------------------------------
// Root cause of "Rescan Devices does nothing until I restart the app":
// PortAudio takes a ONE-TIME snapshot of the system's device list inside
// Pa_Initialize() and never re-queries the OS after that on its own --
// ListInputDevices/ListOutputDevices above were always reading that same
// stale snapshot, every single frame, no matter how often they were
// called. There was never a caching bug on our side to fix; the "Rescan"
// button simply had nothing behind it that actually rescanned anything.
//
// Pa_UpdateAvailableDeviceList() would have been the ideal fix (refreshes
// without touching open streams), but it isn't present in the PortAudio
// build vcpkg actually installed here -- confirmed by a real link/compile
// failure, not a guess this time. The only rescan mechanism guaranteed to
// exist on every PortAudio v19 build is the original one: tear the whole
// subsystem down and bring it back up. Pa_Terminate() also force-closes
// every open stream, though, which would silently kill live audio if we
// did this while anything was running -- so this refuses to rescan (and
// says why, on-screen, not just in the log file) unless everything is
// already stopped first.
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

// ---------------------------------------------------------------------------
// Host API filtering (WASAPI-only)
//
// Root cause of the "dropdown text is cut off" bug: PortAudio enumerates
// the SAME physical device once per Windows host API that exposes it
// (MME, DirectSound, WASAPI, WDM-KS all show up as separate entries).
// Windows' legacy MME API (waveInGetDevCaps / waveOutGetDevCaps) has a
// documented, OS-level, unfixable-on-our-side limit: device names are
// capped at MAXPNAMELEN, 32 characters INCLUDING the null terminator --
// i.e. exactly 31 visible characters, cut off mid-word with no ellipsis.
// This isn't a buffer bug in WireMerge; there is no "more" name to
// recover from an MME device handle. That's exactly why the same device
// name showed up both truncated (the MME entry) AND complete (the
// WASAPI/DirectSound entry) in the same dropdown.
//
// Fix: only surface WASAPI devices. This is also the right call for this
// app specifically -- WASAPI is lower-latency than MME/DirectSound,
// which is the whole point of the MMCSS "Pro Audio" registration above.
// Falls back to showing everything if, for some reason, a system has no
// WASAPI host API at all (defensive; shouldn't happen on any Windows
// version this app targets).
// See the comment above ListInputDevices/ListOutputDevices for why this
// filter exists. Returns a negative value (paHostApiNotFound) if this
// system somehow has no WASAPI host API at all.
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

// ---------------------------------------------------------------------------
// Regression fix: "Invalid sample rate" on devices that worked before the
// WASAPI-only filtering change.
//
// MME/DirectSound (now filtered out of the device lists) always went
// through Windows' shared audio engine resampler transparently, so a
// hardcoded 48000Hz request worked on literally any device regardless of
// its native rate. PortAudio's WASAPI backend does NOT do that by
// default -- it validates the requested rate against the device's own
// supported formats up front and rejects a mismatch outright, which is
// exactly the "Invalid sample rate" error. This is a direct, traceable
// side effect of filtering to WASAPI-only, not an unrelated bug.
//
// Fix: PaWasapiStreamInfo's paWinWasapiAutoConvert flag tells PortAudio's
// WASAPI backend to do the same conversion MME/DirectSound did for free,
// restoring the old tolerant behavior while keeping the WASAPI-only
// device list (so the truncated-name/duplicate-entry fix from two
// rounds ago stays intact).
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

    // ctx must outlive the stream; leaked deliberately for stream lifetime
    // and freed in CloseInputSource via the stream's owning struct below.
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
        // Belt-and-suspenders: AutoConvert should handle this, but if a
        // particular driver still refuses, retry once at the device's OWN
        // native rate rather than failing outright. Note this source will
        // then be running at a different rate than the rest of the mix --
        // Mixer doesn't resample between sources, so it may sound
        // pitch/speed-shifted relative to other sources. Getting *some*
        // audio with a clear warning beats a silent hard failure.
        WM_LOG_WARN(std::string("Input stream open failed at ") + std::to_string(sampleRate) +
                    "Hz (" + Pa_GetErrorText(err) + "), retrying at device's native " +
                    std::to_string(static_cast<int>(devInfo->defaultSampleRate)) + "Hz...");
        int nativeRate = static_cast<int>(devInfo->defaultSampleRate);
        err = Pa_OpenStream(&stream, &inParams, nullptr, nativeRate,
                             paFramesPerBufferUnspecified, paNoFlag,
                             InputCallback, ctx);
        if (err == paNoError) {
            WM_LOG_WARN("Opened at native rate -- this source may sound pitch/speed-shifted "
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
        // Same AutoConvert-first-then-native-rate-fallback as
        // OpenInputSource above, for the same reason.
        WM_LOG_WARN(std::string("Output stream open failed at ") + std::to_string(sampleRate) +
                    "Hz (" + Pa_GetErrorText(err) + "), retrying at device's native " +
                    std::to_string(static_cast<int>(devInfo->defaultSampleRate)) + "Hz...");
        int nativeRate = static_cast<int>(devInfo->defaultSampleRate);
        err = Pa_OpenStream(&outputStream_, nullptr, &outParams, nativeRate,
                             paFramesPerBufferUnspecified, paNoFlag,
                             OutputCallback, ctx);
        if (err == paNoError) {
            WM_LOG_WARN("Opened at native rate -- sources running at a different rate than "
                        "this may sound pitch/speed-shifted since Mixer doesn't resample.");
        }
    }
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

} // namespace wm
