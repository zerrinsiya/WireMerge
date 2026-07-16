#include "mixer.h"
#include "utils.h"
#include <algorithm>
#include <cmath>

namespace wm {

// ---------------------------------------------------------------------------
// RingBuffer
// ---------------------------------------------------------------------------

RingBuffer::RingBuffer(size_t capacityFrames, int channels)
    : buffer_(capacityFrames * channels, 0.0f),
      capacityFrames_(capacityFrames),
      channels_(channels) {}

void RingBuffer::Write(const float* interleaved, size_t frames) {
    if (frames > capacityFrames_) {
        // Caller is writing more than the whole buffer at once; only keep
        // the most recent chunk that fits (prevents pointer math past end).
        interleaved += (frames - capacityFrames_) * channels_;
        frames = capacityFrames_;
    }

    size_t pos = writePos_.load(std::memory_order_relaxed);
    for (size_t f = 0; f < frames; ++f) {
        size_t idx = ((pos + f) % capacityFrames_) * channels_;
        for (int c = 0; c < channels_; ++c) {
            buffer_[idx + c] = interleaved[f * channels_ + c];
        }
    }
    writePos_.store((pos + frames) % capacityFrames_, std::memory_order_relaxed);

    size_t avail = availableFrames_.load(std::memory_order_relaxed) + frames;
    if (avail > capacityFrames_) avail = capacityFrames_; // drop oldest on overrun
    availableFrames_.store(avail, std::memory_order_relaxed);
}

size_t RingBuffer::MixInto(float* outAccum, size_t frames, float gain) {
    size_t avail = availableFrames_.load(std::memory_order_relaxed);
    size_t toRead = std::min(frames, avail);

    if (toRead < frames) {
        // Buffer didn't have enough (or any) data for this callback --
        // the missing frames become silence, which is an audible gap.
        underrunFrames_.fetch_add(frames - toRead, std::memory_order_relaxed);
    }

    if (toRead == 0) return 0;

    size_t writePos = writePos_.load(std::memory_order_relaxed);
    // Oldest unread frame is `avail` frames behind writePos.
    size_t startPos = (writePos + capacityFrames_ - avail) % capacityFrames_;

    for (size_t f = 0; f < toRead; ++f) {
        size_t idx = ((startPos + f) % capacityFrames_) * channels_;
        for (int c = 0; c < channels_; ++c) {
            // outAccum is assumed to be laid out for `channels_` too;
            // Mixer::Mix handles any channel-count mismatch before calling in.
            outAccum[f * channels_ + c] += buffer_[idx + c] * gain;
        }
    }

    availableFrames_.store(avail - toRead, std::memory_order_relaxed);
    return toRead;
}

// ---------------------------------------------------------------------------
// Mixer
// ---------------------------------------------------------------------------

Mixer::Mixer() = default;

SourceId Mixer::AddSource(const std::string& name, int sampleRate, int channels,
                           int bufferMs) {
    std::lock_guard<std::mutex> lock(controlMutex_);

    SourceId id = nextId_.fetch_add(1);
    size_t capacityFrames = static_cast<size_t>(sampleRate) * bufferMs / 1000;

    Source s;
    s.info.id = id;
    s.info.name = name;
    s.info.channels = channels;
    s.ring = std::make_unique<RingBuffer>(capacityFrames, channels);

    sources_.emplace(id, std::move(s));
    WM_LOG_INFO("Mixer: added source '" + name + "' (id=" + std::to_string(id) + ")");
    return id;
}

void Mixer::RemoveSource(SourceId id) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    sources_.erase(id);
}

void Mixer::SetEnabled(SourceId id, bool enabled) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    auto it = sources_.find(id);
    if (it != sources_.end()) it->second.info.enabled = enabled;
}

void Mixer::SetVolume(SourceId id, float volume) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    auto it = sources_.find(id);
    if (it != sources_.end()) it->second.info.volume = std::clamp(volume, 0.0f, 2.0f);
}

void Mixer::SetMasterVolume(float volume) {
    masterVolume_.store(std::clamp(volume, 0.0f, 1.5f), std::memory_order_relaxed);
}

std::vector<SourceInfo> Mixer::ListSources() const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    std::vector<SourceInfo> out;
    out.reserve(sources_.size());
    for (const auto& kv : sources_) out.push_back(kv.second.info);
    return out;
}

uint64_t Mixer::GetUnderrunFrames(SourceId id) const {
    std::lock_guard<std::mutex> lock(controlMutex_);
    auto it = sources_.find(id);
    if (it == sources_.end() || !it->second.ring) return 0;
    return it->second.ring->GetUnderrunFrames();
}

void Mixer::PushSamples(SourceId id, const float* interleaved, size_t frames) {
    // NOTE: intentionally does NOT take controlMutex_ -- this runs on a
    // realtime PortAudio callback thread and must never block on the UI
    // thread. We only need the ring buffer pointer, which is stable once
    // the source exists (sources are added/removed from the UI thread
    // while streams are stopped, not mid-stream).
    auto it = sources_.find(id);
    if (it == sources_.end()) return;
    it->second.ring->Write(interleaved, frames);
}

void Mixer::Mix(float* out, size_t frames, int outChannels) {
    std::fill(out, out + frames * outChannels, 0.0f);

    std::lock_guard<std::mutex> lock(controlMutex_);
    for (auto& kv : sources_) {
        Source& s = kv.second;
        if (!s.info.enabled) continue;

        if (s.info.channels == outChannels) {
            s.ring->MixInto(out, frames, s.info.volume);
        } else if (s.info.channels == 1 && outChannels == 2) {
            // Mono source into stereo output: mix into a temp mono buffer,
            // then duplicate to both channels.
            std::vector<float> mono(frames, 0.0f);
            s.ring->MixInto(mono.data(), frames, s.info.volume);
            for (size_t f = 0; f < frames; ++f) {
                out[f * 2 + 0] += mono[f];
                out[f * 2 + 1] += mono[f];
            }
        }
        // Other channel-count combinations are uncommon for USB audio
        // devices in this use case and are skipped rather than guessed at.
    }

    // Soft clip to avoid harsh digital clipping if multiple loud sources
    // peak at once. tanh saturates gently above +-1.0 instead of hard-cutting.
    float master = masterVolume_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames * static_cast<size_t>(outChannels); ++i) {
        float v = out[i] * master;
        if (v > 1.0f || v < -1.0f) {
            v = std::tanh(v);
        }
        out[i] = v;
    }
}

} // namespace wm
