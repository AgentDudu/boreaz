#pragma once

#include <cstdint>

#include "boreaz/dsp/envelope.h"
#include "boreaz/dsp/oscillator.h"
#include "boreaz/dsp/svf.h"
#include "boreaz/engine/note.h"
#include "boreaz/engine/voice_common.h"
#include "boreaz/engine/voice.h"
#include "boreaz/engine/params.h"

namespace boreaz::engine {

// The subtractive-synth voice: osc x3 + noise -> mix -> SVF -> ADSR amp -> pan.
//
// This is phase 2's sound, unchanged, now behind `engine::Voice`. It is the default: no instrument
// arrives, and this is what plays. The name says what kind of voice it is, because there are two.
//
// Real-time contract: no allocation, no I/O, nothing that can fail. Every buffer is a member.
class SynthVoice final : public Voice {
  public:
    SynthVoice() = default;

    // A note starts from clean filter and envelope state: voices are reused, and the residue of
    // the previous note would sound on top of the new one. `request.frequency` is final - the
    // engine has already applied transpose, octave and bend.
    void start(const NoteRequest& request, const Params& params, double sample_rate,
               std::int64_t start_tick) override;

    void release(const Params& params, std::int64_t tick) override;

    // The key went up while the sustain pedal was down: stop being a held note, keep sounding,
    // and become the first candidate when the pool needs a voice.
    void silence(std::int64_t tick) override;

    // Refresh the per-note constants after a parameter change. `force` is for a voice that has
    // just been reactivated and has to take the current block's parameters whatever happens.
    // Svf::prepare only moves the cutoff and never touches the filter memory, so a slider cannot
    // kick a decaying note.
    void refresh(const Params& params, bool force) override;

    // Re-pitch a sounding voice (pitch bend). Phases are kept, so it glides instead of clicking.
    void set_frequency(float hertz, const Params& params) override;

    // Transpose or octave shift moved the keyboard under this note: move the note number, the pitch,
    // and the cutoff that follows the note. See `Voice::retune`.
    void retune(int semitones, const Params& params) override;

    // Adds into `left`/`right`; the caller owns the buffer and the mix.
    void render(float* left, float* right, int frames) override;

    bool active() const override {
        return state_ == State::kDown || state_ == State::kHeldByPedal ||
               state_ == State::kReleased;
    }
    bool released() const override { return state_ == State::kReleased; }
    // Held by the sustain pedal after its key went up: still audible, but first in line to steal.
    bool sustained() const override { return state_ == State::kHeldByPedal; }

    int note() const override { return note_; }
    int key() const override { return key_; }
    int channel() const override { return channel_; }
    float velocity() const override { return velocity_; }
    float frequency() const override { return frequency_; }
    std::int64_t start_tick() const override { return start_tick_; }

    std::int64_t release_tick() const override { return release_tick_; }


    // Where the amplitude envelope is, for the display tap later. Reading it does not advance
    // anything.
    float amplitude() const override { return amp_.value() * peak_gain_; }

    // How hard the note was struck, as a gain. The allocator's tie-break: two notes that began on
    // the same sample both have an envelope of zero until a block has rendered, so the strike is
    // the only honest measure of which one is the cheaper theft.
    float strike() const override { return peak_gain_; }

  private:
    enum class State : std::uint8_t { kIdle, kDown, kHeldByPedal, kReleased };

    // Noise for the hammer transient. Deterministic on purpose: seeded from the note and its
    // start tick, so rendering the same input twice gives the same bytes.
    struct Noise {
        std::uint32_t state = 1u;
        void seed(std::uint32_t note, std::int64_t tick) {
            state = (note + 1u) * 2654435761u ^ static_cast<std::uint32_t>(tick) * 1013904223u ^ 1u;
        }
        // Any fixed arithmetic sequence will do for a hammer click; this one is a multiply and a
        // shift and never returns zero.
        float next() {
            state = state * 1664525u + 1013904223u;
            return static_cast<float>(state >> 9) * (1.0f / 8388608.0f) - 1.0f; // -1 .. 1
        }
    };

    State state_ = State::kIdle;

    boreaz::dsp::Oscillator osc1_{boreaz::dsp::Waveform::Sine};
    boreaz::dsp::Oscillator osc2_{boreaz::dsp::Waveform::Sine};
    boreaz::dsp::Oscillator osc3_{boreaz::dsp::Waveform::Saw};
    boreaz::dsp::Svf filter_;
    boreaz::dsp::Adsr amp_;
    Noise noise_{};
    float noise_phase_ = 0.0f;
    float noise_step_ = 1.0f;

    int note_ = 0;
    int key_ = 0;
    std::uint8_t channel_ = 0;
    float velocity_ = 0.0f;
    float frequency_ = 0.0f;
    float peak_gain_ = 0.0f;
    float pan_left_ = 0.70710678f;
    float pan_right_ = 0.70710678f;
    std::int64_t start_tick_ = 0;
    std::int64_t release_tick_ = 0;
    float sample_rate_ = 44100.0f;

    // Cached so a parameter change is noticed once per block instead of once per sample.
    Params applied_{};
};

} // namespace boreaz::engine
