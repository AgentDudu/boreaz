#pragma once

// Shared render helpers for the engine tests. Kept to the four things the phase-2 exit
// criteria actually measure: peak, finiteness, when sound starts, and how to feed events.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "boreaz/engine/engine.h"

namespace fixture {

inline constexpr float kRate = 44100.0f;
inline constexpr float kSilence = 1.0e-4F; // below this, nothing is sounding

using Engine = boreaz::engine::Engine;
using Event = boreaz::engine::Event;

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

} // namespace fixture
