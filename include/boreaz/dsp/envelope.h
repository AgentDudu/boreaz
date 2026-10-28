#pragma once

#include <cmath>

namespace boreaz::dsp {

// Attack / decay / sustain / release whose segments last exactly the number of samples you
// asked for, while still looking exponential.
//
// Curvature and duration are independent here. A segment is a fixed sample count and the
// value walks from wherever it was to the segment target along
//
//     s(x) = (1 - e^(-c*x)) / (1 - e^(-c))          c = curvature, x = progress in [0, 1]
//
// which is 0 at x = 0 and 1 at x = 1 for any c, so an exponential shape never eats the last
// few milliseconds the way an asymptotic one-pole does. c = 0 is the straight line. Per
// sample it costs one multiply: e^(-c*x) is carried forward by a factor computed once per
// segment, so there is no exp() and no pow() in the audio path.
//
// Retrigger reads the current value, so re-striking a decaying note starts from where the
// envelope is instead of jumping to zero.
class Adsr {
  public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };

    void prepare(double sample_rate_hz) noexcept {
        sample_rate_hz_ = sample_rate_hz > 0.0 ? sample_rate_hz : 0.0;
    }

    // Times in seconds, sustain level 0..1. Negative or non-finite times become 0, meaning
    // "jump", and durations are rounded to whole samples.
    void set_times(double attack_s, double decay_s, double sustain_level,
                   double release_s) noexcept {
        attack_samples_ = samples_for(attack_s);
        decay_samples_ = samples_for(decay_s);
        release_samples_ = samples_for(release_s);
        sustain_level_ = clamp01(sustain_level);
    }

    // 0 is linear; 5-10 reads as an analog envelope.
    void set_curvature(float curvature) noexcept {
        curvature_ = curvature > 0.0f ? curvature : 0.0f;
    }

    void gate_on() noexcept { begin(Stage::Attack, value_, 1.0f, attack_samples_); }

    void gate_off() noexcept {
        if (stage_ == Stage::Attack || stage_ == Stage::Decay || stage_ == Stage::Sustain) {
            begin(Stage::Release, value_, 0.0f, release_samples_);
        }
    }

    float process() noexcept {
        switch (stage_) {
            case Stage::Idle:
                return value_;
            case Stage::Sustain:
                value_ = sustain_level_;
                return value_;
            case Stage::Attack:
            case Stage::Decay:
            case Stage::Release: {
                advance();
                if (remaining_ > 0) {
                    return value_;
                }
                // The segment ends on its own last sample: land exactly on the target, then move on
                // within the same sample so a zero-length segment adds no phantom latency.
                value_ = to_;
                if (stage_ == Stage::Attack) {
                    begin(Stage::Decay, 1.0f, sustain_level_, decay_samples_);
                    if (decay_samples_ == 0) {
                        // An instantaneous attack and decay lands on sustain in the same sample;
                        // making the player wait a sample per empty segment would be latency
                        // invented by us.
                        stage_ = Stage::Sustain;
                        value_ = sustain_level_;
                    }
                } else if (stage_ == Stage::Decay) {
                    stage_ = Stage::Sustain;
                    value_ = sustain_level_;
                } else {
                    stage_ = Stage::Idle;
                    value_ = 0.0f;
                }
                return value_;
            }
        }
        return value_;
    }

    void process(float* out, int frames) noexcept {
        for (int i = 0; i < frames; ++i) {
            out[i] = process();
        }
    }

    float value() const noexcept { return value_; }
    Stage stage() const noexcept { return stage_; }
    bool active() const noexcept { return stage_ != Stage::Idle; }
    float sustain_level() const noexcept { return sustain_level_; }

    // For tests and for a GUI that draws the envelope shape.
    int segment_samples(Stage stage) const noexcept {
        switch (stage) {
            case Stage::Attack:
                return attack_samples_;
            case Stage::Decay:
                return decay_samples_;
            case Stage::Release:
                return release_samples_;
            case Stage::Sustain:
            case Stage::Idle:
                break;
        }
        return -1; // not timed
    }

  private:
    static float clamp01(double value) noexcept {
        if (!(value > 0.0)) {
            return 0.0f;
        }
        return value > 1.0 ? 1.0f : static_cast<float>(value);
    }

    int samples_for(double seconds) const noexcept {
        if (!(seconds > 0.0) || !(sample_rate_hz_ > 0.0)) {
            return 0;
        }
        const double count = seconds * sample_rate_hz_;
        if (count > 1.0e9) {
            return 1000000000;
        }
        return static_cast<int>(count + 0.5);
    }

    void begin(Stage stage, float from, float to, int samples) noexcept {
        stage_ = stage;
        from_ = from;
        to_ = to;
        remaining_ = samples;
        total_ = samples;

        // Carried-forward e^(-c*x): ratio^samples equals e^(-c) at the end of the segment, which
        // is what makes s(1) == 1 for every curvature.
        if (samples > 0 && curvature_ > 1.0e-4f) {
            const double c = static_cast<double>(curvature_);
            decay_per_sample_ = static_cast<float>(std::exp(-c / samples));
            curve_denominator_ = static_cast<float>(1.0 - std::exp(-c));
        } else {
            decay_per_sample_ = 0.0f; // selects the straight-line branch below
            curve_denominator_ = 1.0f;
        }
        curve_ = 1.0f;
    }

    void advance() noexcept {
        if (total_ <= 0) {
            value_ = to_; // zero-length segment: jump, and process() moves on in this sample
            return;
        }

        --remaining_;

        float shape;
        if (decay_per_sample_ == 0.0f) {
            shape = static_cast<float>(total_ - remaining_) / static_cast<float>(total_);
        } else {
            curve_ *= decay_per_sample_;
            shape = (1.0f - curve_) / curve_denominator_;
        }
        value_ = from_ + (to_ - from_) * shape;
    }

    double sample_rate_hz_ = 0.0;
    int attack_samples_ = 0;
    int decay_samples_ = 0;
    int release_samples_ = 0;
    float sustain_level_ = 0.0f;
    float curvature_ = 0.0f;

    Stage stage_ = Stage::Idle;
    float value_ = 0.0f;
    float from_ = 0.0f;
    float to_ = 0.0f;
    int remaining_ = 0;
    int total_ = 0;
    float curve_ = 1.0f;
    float decay_per_sample_ = 0.0f;
    float curve_denominator_ = 1.0f;
};

} // namespace boreaz::dsp
