#pragma once
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>

namespace wm {

using SourceId = uint32_t;

//Per-source audio ring buffer.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacityFrames, int channels);

    void Write(const float* interleaved, size_t frames);

    size_t MixInto(float* outAccum, size_t frames, float gain);

    int Channels() const { return channels_; }

    uint64_t GetUnderrunFrames() const { return underrunFrames_.load(std::memory_order_relaxed); }

private:
    std::vector<float> buffer_;
    size_t capacityFrames_;
    int channels_;
    std::atomic<size_t> writePos_{0};
    std::atomic<size_t> availableFrames_{0};
    std::atomic<uint64_t> underrunFrames_{0};
};

struct SourceInfo {
    SourceId id;
    std::string name;
    bool enabled = true;
    float volume = 1.0f;
    int channels = 2;
};

class Mixer {
public:
    Mixer();

    SourceId AddSource(const std::string& name, int sampleRate, int channels,
                        int bufferMs = 120);
    void RemoveSource(SourceId id);

    void SetEnabled(SourceId id, bool enabled);
    void SetVolume(SourceId id, float volume);

    void SetMasterVolume(float volume);
    float GetMasterVolume() const { return masterVolume_.load(std::memory_order_relaxed); }

    std::vector<SourceInfo> ListSources() const;

    uint64_t GetUnderrunFrames(SourceId id) const;

    void PushSamples(SourceId id, const float* interleaved, size_t frames);

    void Mix(float* out, size_t frames, int outChannels);

private:
    struct Source {
        SourceInfo info;
        std::unique_ptr<RingBuffer> ring;
    };

    mutable std::mutex controlMutex_;
    std::unordered_map<SourceId, Source> sources_;
    std::atomic<SourceId> nextId_{1};
    std::atomic<float> masterVolume_{1.0f};

    std::vector<float> monoScratch_;
};

}
