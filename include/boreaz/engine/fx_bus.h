#pragma once

#include "boreaz/dsp/delay.h"
#include "boreaz/dsp/distortion.h"
#include "boreaz/dsp/reverb.h"
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
//
// Order in the chain is drive, delay, room: the distortion is the source's own voice, so it has to
// be in front of everything that repeats or spaces it - an echo of a clean note into a driven
// channel would put the effect's harmonics back under the delay's control - and the room is last
// because it is the furthest thing from the speaker. Each stage is fed what the one before it left
// on the dry sum, so a send feeds the next one without an extra wire.
class FxBus {
  public:
    // The engine hands over chunks of at most `Engine::kMaxScratchFrames`; anything bigger is
    // processed in that many passes, for the same reason the engine chunks its own scratch — the
    // wet buffers here are fixed, like everything else on this path.
    static constexpr int kMaxChunkFrames = 512;

    // Setup path. Both effects are measured in samples, so they are sized when the rate is known
    // and rebuilt from silence if the rate moves.
    void prepare(double sample_rate);

    // In place on the planar dry sum. Reads the sends out of `params` every block, so a knob move
    // is one parameter write and no message.
    void process_block(float* left, float* right, int frames, const Params& params);

    // Whether this bus would change anything. The engine asks once per block so a bypassed bus
    // costs one compare, which is what keeps the master path identical to the pre-FX one.
    static bool active(const Params& params);

    void reset();

  private:
    boreaz::dsp::Distortion distortion_;
    boreaz::dsp::Delay delay_;
    boreaz::dsp::Reverb reverb_;
    // One wet buffer per stage, not one buffer reused: each stage is fed the same block, and a wet
    // buffer that the stage before already wrote is not a wet buffer.
    float wet_l_[kMaxChunkFrames]{};
    float wet_r_[kMaxChunkFrames]{};
    float drive_l_[kMaxChunkFrames]{};
    float drive_r_[kMaxChunkFrames]{};
    // The room gets buffers of its own rather than reusing the delay's: the two are fed from the
    // same block, and a wet buffer that one effect has already written is not a wet buffer.
    float room_l_[kMaxChunkFrames]{};
    float room_r_[kMaxChunkFrames]{};
};

} // namespace boreaz::engine
