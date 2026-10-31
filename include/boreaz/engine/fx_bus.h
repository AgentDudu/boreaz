#pragma once

#include "boreaz/dsp/delay.h"
#include "boreaz/engine/params.h"

namespace boreaz::engine {

// Everything that happens to the whole mix rather than to one voice: the FX bus.
//
// It sits **before** the master gain, so riding the volume rides the effects with it. The
// alternative — a send after the fader — makes the tail a second master volume that keeps playing
// when the piano is faded out, which is not what a fader means.
//
// Bypassed means not touched. A send reading zero takes its effect out of the chain instead of
// mixing it in at zero, which is the difference between "the committed goldens still match" and
// "they match to eleven decimal places and one denormal away from not matching".
//
// Phase 8 grows this chain: the delay is first because it is the effect whose every number can be
// asserted, and the reverb and the distortion both land in a bus that is already proven.
class FxBus {
  public:
    // The engine hands over chunks of at most `Engine::kMaxScratchFrames`; anything bigger is
    // processed in that many passes, for the same reason the engine chunks its own scratch — the
    // wet buffers here are fixed, like everything else on this path.
    static constexpr int kMaxChunkFrames = 512;

    // Setup path. The delay line is measured in samples, so it is sized when the rate is known and
    // rebuilt from silence if the rate moves.
    void prepare(double sample_rate);

    // In place on the planar dry sum. Reads the sends out of `params` every block, so a knob move
    // is one parameter write and no message.
    void process_block(float* left, float* right, int frames, const Params& params);

    // Whether this bus would change anything. The engine asks once per block so a bypassed bus
    // costs one compare, which is what keeps the master path identical to the pre-FX one.
    static bool active(const Params& params);

    void reset();

  private:
    boreaz::dsp::Delay delay_;
    float wet_l_[kMaxChunkFrames]{};
    float wet_r_[kMaxChunkFrames]{};
};

} // namespace boreaz::engine
