#pragma once
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <string>
#include <memory>

// ---------------------------------------------------------------------------
// mixer.h
//
// Combines N live audio sources (each fed by a PortAudio input callback,
// see audio_handler.cpp) into a single interleaved stereo output buffer
// consumed by the PortAudio output callback.
//
// Design notes:
//  - Every source writes into its own lock-free-ish ring buffer from its
//    own PortAudio callback thread. The output callback pulls from all
//    ring buffers and sums them. A mutex only guards the *set* of sources
//    (add/remove/enable/volume), not the per-sample hot path, to keep
//    latency low on weak CPUs.
//  - "External source controls volume" (the whole point of this app) means
//    we do NOT attempt to normalize or auto-gain the incoming stream -- we
//    pass amplitude through 1:1 by default. The per-source `volume` here
//    is an *optional* user-side trim (defaults to 1.0f / unity gain) for
//    cases where the phone/TV's own volume isn't enough to balance sources
//    against each other.
//  - Simple soft-clip on the summed output avoids harsh digital clipping
//    if multiple sources peak simultaneously.
// ---------------------------------------------------------------------------

namespace wm {

using SourceId = uint32_t;

class RingBuffer {
public:
    explicit RingBuffer(size_t capacityFrames, int channels);

    // Called from a PortAudio input callback thread.
    void Write(const float* interleaved, size_t frames);

    // Called from the PortAudio output callback thread. Reads up to
    // `frames` and adds them (scaled by `gain`) into `outAccum`.
    // Returns the number of frames actually available/mixed.
    size_t MixInto(float* outAccum, size_t frames, float gain);

    int Channels() const { return channels_; }

    // Total frames of silence contributed because the buffer didn't have
    // enough data when MixInto was called -- i.e. an audible gap. Cheap
    // atomic increment only (no I/O, no locking) so it's safe to touch
    // from the realtime output callback thread; the GUI polls this once
    // per frame to surface it, rather than logging directly from here.
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
    std::string name;      // e.g. "USB Audio Device (Phone)"
    bool enabled = true;
    float volume = 1.0f;   // unity gain by default -- see design notes above
    int channels = 2;
};

class Mixer {
public:
    Mixer();

    // Register a new source; returns its id. bufferMs controls latency vs
    // dropout resilience (default ~120ms is safe for low-end machines).
    SourceId AddSource(const std::string& name, int sampleRate, int channels,
                        int bufferMs = 120);
    void RemoveSource(SourceId id);

    void SetEnabled(SourceId id, bool enabled);
    void SetVolume(SourceId id, float volume); // 0.0 - 2.0 typical range

    // Master output volume -- applied once to the final mixed buffer,
    // after per-source trims are summed, before the soft-clip stage.
    // Distinct from per-source trim: this is the "overall output level"
    // control, closer to a normal volume slider than the per-source
    // balancing controls are.
    void SetMasterVolume(float volume); // 0.0 - 1.5 typical range
    float GetMasterVolume() const { return masterVolume_.load(std::memory_order_relaxed); }

    std::vector<SourceInfo> ListSources() const;

    // Underrun frames for a source since it was added -- rising quickly
    // means that source's ring buffer is running dry (audible gaps/stutter
    // from that source specifically). See RingBuffer::GetUnderrunFrames
    // for why this is a plain counter rather than a log line.
    uint64_t GetUnderrunFrames(SourceId id) const;

    // Feed raw captured samples for a source (called from that source's
    // PortAudio input callback).
    void PushSamples(SourceId id, const float* interleaved, size_t frames);

    // Pull the final mixed output (called from the output callback).
    // outChannels is normally 2 (stereo).
    void Mix(float* out, size_t frames, int outChannels);

private:
    struct Source {
        SourceInfo info;
        std::unique_ptr<RingBuffer> ring;
    };

    mutable std::mutex controlMutex_; // guards sources_ map structure/metadata only
    std::unordered_map<SourceId, Source> sources_;
    std::atomic<SourceId> nextId_{1};
    std::atomic<float> masterVolume_{1.0f}; // read from the realtime output callback, so atomic not mutex-guarded
};

} // namespace wm
