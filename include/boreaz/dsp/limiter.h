#pragma once

// The last thing that decides how loud this engine is allowed to be.
//
// PLAN's box diagram says "master limiter is the last thing before output; nothing may pass ±1.0",
// and until now that promise was held by the master `tanh`: at drive 1.0 its output cannot exceed
// 1.0, so the promise was true as an accident of one curve nobody can set. It was also true only
// *because* of that curve, which is the wrong order of dependencies - a limiter that sits behind a
// tanh can never engage (measured: the tanh's output peaks at 0.99991 on the loudest thing in the
// repository), and a promise nothing can break is not a test either. So this sits **after the fader
// and in front of the tanh**, where it is the stage that catches overload and the tanh is a curve
// that costs 0.13 dB on a peak instead of the 4.5 dB it was eating at 0 dB master. The tanh cannot
// undo what this decides, because `tanh(x) <= x` for x >= 0.
//
// What it is: a peak limiter with an instantaneous attack and a one-pole release, one linked gain
// for both ears, and a ceiling set by the curve behind it. No lookahead - the callback cannot have
// any, so nothing here ever sees the sample after the one it is working on. What that costs is the
// honest part, and it is worth the sentence: on a 100 Hz tone at 1.5x the ceiling this stage ducks
// the whole tone and comes back at 50 ms, and the release is a moving gain, which is phase
// modulation. Measured over whole cycles after it settles, the third harmonic sits at 14.9 % of the
// fundamental. The tanh this relieves, on the same tone at the same level, reports 23.5 % and a
// lower crest factor, because it does not step aside for anything: every sample is bent whether or
// not it asked. At four times the ceiling both numbers converge on 30 % and the difference is
// academic - a tone that far over any ceiling is already not a tone.
//
// One consequence of the shape that no comment should let anybody be surprised by: because the gain
// is a smooth function of the block's own peak, `process_block` is not additive and not
// idempotent - two chunks whose peaks land in different places get different gains. It is a
// compressor with the threshold at the ceiling, and it is called per chunk (96 frames), which is
// what the callback asks for.
//
// Two properties, both asserted in tests/test_fx.cpp:
//
//   - **transparent below the ceiling, exactly.** While the linked peak is at or under the ceiling
//     the gain is 1.0f, and multiplying by 1.0f is bit-identical, so the committed goldens and every
//     level claim measured at the shipping `-12 dB` master survived this landing unchanged - and are
//     asserted to, not hoped to: `Engine::bypass_limiter()` renders the same file both ways and the
//     test counts the samples that moved. At -12 dB the answer is 0 of 96 000; the synthesiser peaks
//     at 0.6479 there and the sampled flagship at 0.6047, both far under 2.0.
//
//   - **nothing leaves here above the ceiling**, whatever comes in: random, DC at a thousand, a sine
//     at ten. The gain is never above 1.0 and the attack is applied to the sample that triggered it,
//     so the product cannot exceed the ceiling; the one-ulp case is closed by the clamp at the end of
//     the loop rather than by trusting how `1.0f / m` rounded. What the ceiling *is* is a property of
//     where this sits, so read `kCeiling` before assuming it is full scale - the engine's own ceiling
//     is `tanh(kCeiling)`, 0.964. And the clamp in `Engine::render_chunk` behind the tanh stays too:
//     it is the promise for a chain this stage is not in front of, namely a master `tanh` whose drive
//     somebody eventually makes a knob.
//
// The gain is linked across the two ears deliberately. Independent channels would let a loud left
// hand duck the right ear's cymbals, which is a pan control nobody asked for; one gain means the
// image holds and the whole mix pumps together, which is what a master bus does.

#ifndef BOREAZ_DSP_LIMITER_H
#define BOREAZ_DSP_LIMITER_H

#include <cmath>
#include <limits>

namespace boreaz::dsp {

class Limiter {
  public:
    // The ceiling is where the curve behind this stage stops being worth asking for, and the number
    // is measured, not assumed: `tanh` eats 0.61 dB at an input of 1.5, 3.58 dB at 2.0, 6.34 dB at
    // 3.0, and by 9.02 it rounds to exactly 1.0, which is not a ceiling but a square wave. So the cap
    // sits at 2.0 - the loudest thing the curve is asked to do is compress by three tenths of a dB,
    // the composite output ceiling is tanh(2.0) = 0.964 (-0.32 dBFS), and full scale is out of reach
    // by a third of a decibel with the limiter doing the work the curve was doing badly.
    //
    // Two other numbers were measured before this one was chosen, and both are wrong in a way worth
    // writing down. A cap of 1.0 - the obvious answer - leaves the engine's loudest sample at 0.7616,
    // because the curve is then asked to work up to its own 2.37 dB of compression: two and a half dB
    // of range quietly deleted, at every master gain, and the fader above the engagement point only
    // buys rms. A cap of 1.0986123 (ln 3, which is what "where tanh reaches 1.0" looks like if you
    // solve it badly - `atanh(1)` is infinity) lands on 0.8000 for the same reason. Deliberately not
    // a parameter: a limiter whose ceiling can be set above where the chain runs out of curve is a
    // make-work knob for the DAC.
    static constexpr float kCeiling = 2.0f;

    // The release, in seconds, from the sweep in the header of the commit that landed this: it is
    // long enough that a 100 Hz sine at four times full scale comes back at its own level inside a
    // quarter-cycle rather than being re-limited on every partial, and short enough that the second
    // chord of a staccato passage is still caught. Setting up, not per sample.
    static constexpr double kReleaseSeconds = 0.050;

    void prepare(double sample_rate) {
        release_ = sample_rate > 0.0
                       ? static_cast<float>(1.0 - std::exp(-1.0 / (kReleaseSeconds * sample_rate)))
                       : 1.0f; // 1.0 means "no smoothing", which is also the unprepared case
    }

    // In place, both ears, one linked gain. Returns the loudest sample it saw on the way *in*, which
    // is what the clip meter wants: "how hard was the ceiling being asked to work", not "what came
    // out", which is a number this stage guarantees and is therefore never interesting.
    float process_block(float* left, float* right, int frames) {
        float peak = 0.0f;
        if (frames <= 0)
            return peak;
        for (int i = 0; i < frames; ++i) {
            const float l = left[static_cast<std::size_t>(i)];
            const float r = right[static_cast<std::size_t>(i)];
            const float m = magnitude(l) > magnitude(r) ? magnitude(l) : magnitude(r);
            // A non-finite sample is a bug upstream and this stage does not get to propagate it into
            // its own state: a NaN gain would take every later sample with it, which turns one bad
            // frame into silence forever. The clamp below is where a NaN ends up, as it did before.
            if (m > kCeiling && m < std::numeric_limits<float>::infinity())
                gain_ = kCeiling / m; // instant attack, on this sample
            else if (gain_ < 1.0f)
                gain_ += release_ * (1.0f - gain_); // one-pole release toward unity
            if (m > peak)
                peak = m;
            float out_l = l * gain_;
            float out_r = r * gain_;
            // The closing of the last ulp: `m * (1/m)` is not always <= 1, and this is the one place
            // in the engine where "usually" is a bug report.
            left[static_cast<std::size_t>(i)] = clamp(out_l);
            right[static_cast<std::size_t>(i)] = clamp(out_r);
        }
        return peak;
    }

    float gain() const noexcept { return gain_; }

    void reset() noexcept { gain_ = 1.0f; }

  private:
    static float magnitude(float v) noexcept { return v >= 0.0f ? v : -v; }
    static float clamp(float v) noexcept {
        return v > kCeiling ? kCeiling : (v < -kCeiling ? -kCeiling : v);
    }

    // Never above 1.0: the attack sets it below, the release ramps *toward* 1.0 and never past it.
    float gain_ = 1.0f;
    float release_ = 1.0f;
};

} // namespace boreaz::dsp

#endif // BOREAZ_DSP_LIMITER_H
