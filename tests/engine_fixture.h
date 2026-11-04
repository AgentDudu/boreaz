#pragma once

// Shared render helpers for the engine tests. Kept to the four things the phase-2 exit
// criteria actually measure: peak, finiteness, when sound starts, and how to feed events.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "boreaz/engine/engine.h"
#include "boreaz/engine/instrument.h"

namespace fixture {

inline constexpr float kRate = 44100.0f;
inline constexpr float kSilence = 1.0e-4F; // below this, nothing is sounding

using Engine = boreaz::engine::Engine;
using Event = boreaz::engine::Event;
using Instrument = boreaz::engine::Instrument;
using VoiceSample = boreaz::engine::VoiceSample;

struct Audio {
    std::vector<float> data; // interleaved stereo
    float peak = 0.0f;
    bool finite = true;

    std::size_t frames() const { return data.size() / 2u; }
    float at(std::size_t frame, int channel = 0) const {
        return data[frame * 2u + static_cast<std::size_t>(channel)];
    }

    // First frame whose magnitude is above the threshold, or frames() for pure silence.
    std::size_t first_sound(float threshold = static_cast<float>(kSilence)) const {
        for (std::size_t frame = 0; frame < frames(); ++frame) {
            if (std::fabs(at(frame, 0)) > threshold || std::fabs(at(frame, 1)) > threshold)
                return frame;
        }
        return frames();
    }
};

// Renders in 64-frame blocks, which is what the audio callback will ask for.
inline Audio render(Engine& engine, int frames, int block = 64) {
    Audio audio;
    audio.data.assign(static_cast<std::size_t>(frames) * 2u, 0.0f);
    std::vector<float> chunk(static_cast<std::size_t>(block) * 2u, 0.0f);

    for (int done = 0; done < frames; done += block) {
        const int n = std::min(block, frames - done);
        std::fill(chunk.begin(), chunk.end(), 0.0f);
        engine.process(chunk.data(), n);
        for (int i = 0; i < n * 2; ++i) {
            const std::size_t index =
                static_cast<std::size_t>(done) * 2u + static_cast<std::size_t>(i);
            audio.data[index] = chunk[static_cast<std::size_t>(i)];
            const float value = chunk[static_cast<std::size_t>(i)];
            audio.finite = audio.finite && std::isfinite(value);
            audio.peak = std::max(audio.peak, std::fabs(value));
        }
    }
    return audio;
}

// Peak of one channel over a frame window, for "is this side of the stereo field sounding".
inline float peak(const Audio& audio, std::size_t from, std::size_t to, int channel) {
    float value = 0.0f;
    to = std::min(to, audio.frames());
    for (std::size_t frame = from; frame < to; ++frame) {
        value = std::max(value, std::fabs(audio.at(frame, channel)));
    }
    return value;
}

// Signed mean over a frame window: the second of the two questions a mix can only answer with both
// hands. Peak says *something* is sounding; the mean says whether it was a constant or a waveform.
inline float mean(const Audio& audio, std::size_t from, std::size_t to, int channel) {
    to = std::min(to, audio.frames());
    if (to <= from)
        return 0.0f;
    double sum = 0.0;
    for (std::size_t frame = from; frame < to; ++frame)
        sum += static_cast<double>(audio.at(frame, channel));
    return static_cast<float>(sum / static_cast<double>(to - from));
}

// An instrument made of two recordings, for an engine test that has to tell a note from its
// release. The note is 100 ms of +1 and the release is 100 ms of alternating +/-1, both full scale
// and neither looped, so `peak` and `mean` together say which of the two is sounding in any window
// and a test never has to ask the engine to expose its voice pools. The velocity curve is switched
// off on both, so a level the test reads is the level the pan made.
class FakeLibrary final : public Instrument {
  public:
    FakeLibrary() {
        point_at(note, note_pcm);
        point_at(release, release_pcm);
        note.velocity_tracking = 0.0f;
        release.velocity_tracking = 0.0f;
    }

    const VoiceSample* find(int, int) const override { return &note; }
    const VoiceSample* find_release(int, int velocity) const override {
        ++release_lookups;
        last_release_velocity = velocity;
        return has_releases ? &release : nullptr;
    }
    bool empty() const override { return false; }
    std::size_t resident_bytes() const override { return 0; }
    std::size_t refused_regions() const override { return 0; }

    static std::vector<float> alternating() {
        std::vector<float> out(4410, 1.0f);
        for (std::size_t i = 1; i < out.size(); i += 2)
            out[i] = -1.0f;
        return out;
    }
    static void point_at(VoiceSample& sample, const std::vector<float>& pcm) {
        sample.data = pcm.data();
        sample.frames = static_cast<std::int64_t>(pcm.size());
        sample.channels = 1;
        sample.file_sample_rate = kRate;
        sample.key_center = 60; // every other key plays these two shifted, as a real library does
    }

    std::vector<float> note_pcm = std::vector<float>(4410, 1.0f);
    std::vector<float> release_pcm = alternating();
    VoiceSample note{};
    VoiceSample release{};
    bool has_releases = true;
    // What the instrument was asked for, which is the only way to see the strike the engine
    // remembered: a note-off arrives as velocity 0 and the release must not be chosen by it.
    mutable int release_lookups = 0;
    mutable int last_release_velocity = -1;
};

} // namespace fixture
