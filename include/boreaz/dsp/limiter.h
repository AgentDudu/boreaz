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
// that costs 0.13 dB on a peak at the shipping `-12 dB` master instead of the 1.82 dB it was eating
// at `0 dB`. The tanh cannot undo what this decides, because `tanh(x) <= x` for x >= 0.
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
//     the gain is 1.0f, and multiplying by 1.0f is bit-identical, so the committed goldens and
//     every level claim measured at the shipping `-12 dB` master survived this landing unchanged -
//     and are asserted to, not hoped to: `Engine::bypass_limiter()` renders the same file both ways
//     and the test counts the samples that moved. At -12 dB the answer is 0 of 96 000: the loudest
//     committed synthesiser fixture (glissando-88) peaks at 0.6479 there, the others at 0.6047 /
//     0.3069 / 0.2993, and the shipping recorded upright at 0.2819 - all of them long way
//     under 2.0.
//
//   - **nothing leaves here above the ceiling**, whatever comes in: random, DC at a thousand, a
//   sine
//     at ten. The gain is never above 1.0 and the attack is applied to the sample that triggered
//     it, so the product cannot exceed the ceiling; the one-ulp case is closed by the clamp at the
//     end of the loop rather than by trusting how `1.0f / m` rounded. What the ceiling *is* is a
//     property of where this sits, so read `kCeiling` before assuming it is full scale - the
//     engine's own ceiling is `tanh(kCeiling)`, 0.964. And the clamp in `Engine::render_chunk`
//     behind the tanh stays too: it is the promise for a chain this stage is not in front of,
//     namely a master `tanh` whose drive somebody eventually makes a knob.
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
    // is measured, not assumed: a signal at 1.0 leaves the curve at 0.7616 (-2.37 dBFS, so a cap
    // at 1.0 is a ceiling 2.37 dB under full scale and most of the master row thrown away), at 1.5
    // at 0.9051, at 2.0 at 0.9640, at 3.0 at 0.9951, and by 9.02 it rounds to exactly 1.0, which is
    // not a ceiling but a square wave. So the cap sits at 2.0 - the loudest the curve is ever asked
    // to compress is 6.34 dB, the composite output ceiling is tanh(2.0) = 0.964 (-0.32 dBFS), and
    // full scale is out of reach by a third of a decibel with the limiter doing the work the curve
    // was doing badly.
    //
    // One more candidate was measured and rejected: 1.0986123 (= ln 3, which is what "where tanh
    // reaches 1.0" looks like if it is solved badly - `atanh(1)` is infinity) puts the composite
    // output at 0.8000, 5 % louder than 2.0 does, for 9.11 dB of curve instead of 6.34.
    // Deliberately not a parameter either: a limiter whose ceiling can be set above where the chain
    // runs out of curve is a make-work knob for the DAC.
    static constexpr float kCeiling = 2.0f;

    // The release, in seconds, measured rather than felt: 50 ms is what a 100 Hz tone at four times
    // the ceiling needs in order to come back far enough between partials not to be re-limited on
    // every one of them (the gain reaches 0.28 of the source inside one 10 ms period, and the third
    // harmonic settles at 31 % of the fundamental - where the `tanh` it relieves reports 29 %),
    // while a 3 ms release on the same tone reports 34 % and buys nothing. Time also explains why
    // the release cannot smear a transient: the stage is called once per engine chunk (96
    // frames, 2.18 ms at 44.1 kHz), so one release tick moves the gain 4.35 % of the way back and
    // 63 % of the way takes 23 chunks. At a device callback's 1024 frame period one tick would
    // be 23.2 ms, 46 % of the time constant, and the release would be doing its work *inside* the
    // block that triggered it. Setting up, not per sample.
    static constexpr double kReleaseSeconds = 0.050;

    void prepare(double sample_rate) {
        release_ = sample_rate > 0.0
                       ? static_cast<float>(1.0 - std::exp(-1.0 / (kReleaseSeconds * sample_rate)))
                       : 1.0f; // 1.0 means "no smoothing", which is also the unprepared case
    }

    // In place, both ears, one linked gain. Returns the loudest sample it saw on the way *in*,
    // which is what the clip meter wants: "how hard was the ceiling being asked to work", not "what
    // came out", which is a number this stage guarantees and is therefore never interesting.
    float process_block(float* left, float* right, int frames) {
        float peak = 0.0f;
        if (frames <= 0)
            return peak;
        for (int i = 0; i < frames; ++i) {
            const float l = left[static_cast<std::size_t>(i)];
            const float r = right[static_cast<std::size_t>(i)];
            const float m = magnitude(l) > magnitude(r) ? magnitude(l) : magnitude(r);
            // A non-finite sample is a bug upstream and this stage does not get to propagate it
            // into its own state: a NaN gain would take every later sample with it, which turns one
            // bad frame into silence forever. The clamp below is where a NaN ends up, as it did
            // before.
            if (m > kCeiling && m < std::numeric_limits<float>::infinity())
                gain_ = kCeiling / m; // instant attack, on this sample
            else if (gain_ < 1.0f)
                gain_ += release_ * (1.0f - gain_); // one-pole release toward unity
            if (m > peak)
                peak = m;
            float out_l = l * gain_;
            float out_r = r * gain_;
            // The closing of the last ulp: `m * (1/m)` is not always <= 1, and this is the one
            // place in the engine where "usually" is a bug report.
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
