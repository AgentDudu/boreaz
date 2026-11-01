#pragma once

// The third thing on the FX bus, and the only one that is not a send into a space: a driven grand is
// a real patch and a driven synth is what the phase-2 engine needs, since two sine oscillators
// through a lowpass have no harmonics to speak of until something makes them.
//
// The curve is `tanh(drive·x) / drive`, which is chosen for two properties rather than for the
// sound of tanh: the small-signal slope is exactly 1.0, so driving a quiet note changes its colour
// and not its level, and the output of a full-scale peak is `tanh(drive)/drive`, which is below 1.0
// and gets lower as drive goes up. **Distortion can therefore never raise a peak** - the assertion
// lives in tests/test_fx.cpp, and it is the reason the master limiter downstream has one fewer job.
//
// On the flagship's hardest fixture (glissando-88, sampled, master at -12 dB) the two properties
// together are what a compressor meter would call the whole point: at a full send and drive 8 the
// peak falls 13.2 dB while the body of the note falls 1.8 dB, so the crest factor goes 20.5 dB ->
// 9.1 dB. The curve predicts that render to a twentieth of a decibel - the pre-fader peak is 0.572,
// `tanh(8 * 0.572)/8` is 0.102, and the render measures -30.07 dBFS against a dry -16.86. Measured
// here, on this laptop, in the same hour as everything else in PLAN 8's cost row: two `tanh` per
// frame costs +0.09 points of one core on the engine call and +0.20 on the callback that pays for
// it, which is the bus's per-sample behaviour again rather than anything per voice.
//
// Like `Delay` and `Reverb` this writes the wet line and nothing else, and the caller adds it. What
// it writes is `shaped - in` rather than `shaped`, so the bus's `dry + mix·wet` is a blend between
// the clean note and the driven one: at mix 1 the output is exactly the driven note, at mix 0
// exactly the dry one, and nothing in between has to be quieter to make room. A wet line holding
// the shaped signal itself would make mix 1 a sum of two signals that are nearly the same thing at
// low drive, which is a level jump dressed up as a knob.

#ifndef BOREAZ_DSP_DISTORTION_H
#define BOREAZ_DSP_DISTORTION_H

#include <cmath>

namespace boreaz::dsp {

class Distortion {
  public:
    // One is "no more shaping than the master bus is already doing", and ten is a fuzz: past that
    // the curve is a square wave with a few samples of transition, and the harmonics it invents are
    // above Nyquist at the top of the keyboard anyway.
    static constexpr float kMinDrive = 1.0f;
    static constexpr float kMaxDrive = 10.0f;
    static constexpr float kDefaultDrive = 3.0f;

    // Read every block, like the delay time: it is a compare and a divide, and a drive knob that
    // only took effect on a change event would be out of step with a patch.
    void set_drive(float drive) {
        drive_ = drive >= kMinDrive && std::isfinite(drive)
                     ? (drive <= kMaxDrive ? drive : kMaxDrive)
                     : kMinDrive;
        reciprocal_ = 1.0f / drive_;
    }

    float drive() const noexcept { return drive_; }

    // Both ears through the same curve, independently: no cross-coupling, so a mono source stays
    // mono and a wide one stays wide. Nothing here depends on the sample rate, so no prepare().
    void process(const float* in_l, const float* in_r, float* wet_l, float* wet_r, int frames) {
        if (frames <= 0)
            return;
        for (int i = 0; i < frames; ++i) {
            const float left = in_l[i];
            const float right = in_r[i];
            wet_l[i] = saturate(left) - left;
            wet_r[i] = saturate(right) - right;
        }
    }

    // The curve itself, which the tests assert against directly: `std::tanh`, not an approximation,
    // for the same reason `SoftClip` gives - a rational approximation is off by ~2 % near x = 1,
    // which is where a saturation curve is all about.
    float saturate(float in) const noexcept { return std::tanh(in * drive_) * reciprocal_; }

    void reset() noexcept {}

  private:
    float drive_ = kDefaultDrive;
    float reciprocal_ = 1.0f / kDefaultDrive;
};

} // namespace boreaz::dsp

#endif // BOREAZ_DSP_DISTORTION_H
