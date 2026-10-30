#pragma once

#include <algorithm>
#include <cmath>

namespace boreaz::engine {

// Where a note sits across the stereo field, from its key: constant-power pan, hard left at key 12,
// hard right at key 108, `spread` scaling how far the ends reach.
//
// Both voices need the same answer. A sampled piano and a synth lead that spread differently move
// the image when the source changes, and a listener hears that as a bug in the sampler; two copies
// of this arithmetic is how that drift starts.
inline void key_pan(int key, float spread, float& left, float& right) {
    constexpr float kQuarterPi = 0.78539816339744831f;
    const float pan = std::clamp(static_cast<float>(key - 60) * (1.0f / 48.0f) * spread, -1.0f,
                                 1.0f);
    const float angle = (pan + 1.0f) * kQuarterPi;
    left = std::cos(angle);
    right = std::sin(angle);
}

// Velocity to peak gain. A straight map makes pp almost inaudible beside ff; loudness is felt
// roughly as velocity^1.5, which is what this is.
inline float velocity_gain(float velocity) {
    const float v = std::clamp(velocity, 0.0f, 1.0f);
    return v * (0.25f + 0.75f * v);
}

} // namespace boreaz::engine
