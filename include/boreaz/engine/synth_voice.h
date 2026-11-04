#pragma once

#include <algorithm>
#include <cstdint>

#include "boreaz/dsp/envelope.h"
#include "boreaz/dsp/oscillator.h"
#include "boreaz/dsp/svf.h"
#include "boreaz/engine/note.h"
#include "boreaz/engine/params.h"
#include "boreaz/engine/voice.h"
#include "boreaz/engine/voice_common.h"

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

    // What this voice is asking its oscillators and its mixer for in the block that was rendered
    // last, after the modulation router. `render` reads these same functions, which is the only
    // reason they are here rather than private: a voice whose modulation can only be inferred from
    // its samples is a voice whose arithmetic can only be tested by guessing at waveforms.
    float pitch_hz() const noexcept { return osc1_.frequency(); }
    float osc2_level() const noexcept { return mixed_level(applied_.osc2_level, mod_.osc2_level); }
    float osc3_level() const noexcept { return mixed_level(applied_.osc3_level, mod_.osc3_level); }
    float noise_level() const noexcept {
        return mixed_level(applied_.noise_level, mod_.noise_level);
    }

    // A modulated level is added and then floored at zero: a negative level is a legal gain
    // inversion on an oscillator, but "move this level down" that ends up inverting the wave
    // changes the timbre rather than the balance, which is not what the destination means.
    static float mixed_level(float base, float modulation) noexcept {
        return std::max(0.0f, base + modulation);
    }

    // Transpose or octave shift moved the keyboard under this note: move the note number, the
    // pitch, and the cutoff that follows the note. See `Voice::retune`.
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

    // What the eight modulation slots add up to for one block, in each destination's own units.
    // Zero means the destination is untouched, which is why an unpatched instrument reads the same
    // numbers out of `Params` that it always did.
    struct Modulation {
        float cutoff_octaves = 0.0f;
        float pitch_semitones = 0.0f;
        float osc2_level = 0.0f;
        float osc3_level = 0.0f;
        float noise_level = 0.0f;
        float pan = 0.0f;
    };

    // Work out which slots are wired at all, and so which sources have to be evaluated and which
    // destinations have to be moved. Called when a note starts and whenever a parameter moves; the
    // render loop only ever reads the result, which is what keeps an unpatched voice free of the
    // router's cost entirely.
    void prepare_modulation(const Params& params);

    // Evaluate the sources for one block and fold the slots into `mod_`, then move the destinations
    // that need moving before any of this block's samples are generated.
    void apply_modulation(int frames);

    State state_ = State::kIdle;

    boreaz::dsp::Oscillator osc1_{boreaz::dsp::Waveform::Sine};
    boreaz::dsp::Oscillator osc2_{boreaz::dsp::Waveform::Sine};
    boreaz::dsp::Oscillator osc3_{boreaz::dsp::Waveform::Saw};
    boreaz::dsp::Svf filter_;
    boreaz::dsp::Adsr amp_;

    // The router's own state. Mod env is a second ADSR struck with the note, and the two LFOs are
    // free oscillators whose phase runs from the voice's start; all three are evaluated once per
    // block, which is the rate the real-time rules set for LFOs and filter coefficients.
    boreaz::dsp::Adsr mod_env_;
    boreaz::dsp::Oscillator lfo1_{boreaz::dsp::Waveform::Sine};
    boreaz::dsp::Oscillator lfo2_{boreaz::dsp::Waveform::Sine};

    // One bit per source and one per destination, so the block does not test eight slots.
    bool mod_used_env_ = false;
    bool mod_used_lfo1_ = false;
    bool mod_used_lfo2_ = false;
    bool mod_moves_cutoff_ = false;
    bool mod_moves_pitch_ = false;
    bool mod_moves_pan_ = false;
    bool mod_active_ = false; // nothing wired at all: `render` then behaves exactly as it did

    // The values the note would use with no modulation, kept so a block can be re-derived from
    // them instead of drifting a little further every block.
    Modulation mod_{};
    float cutoff_hz_ = 0.0f;
    float osc1_hz_ = 0.0f;
    float osc2_hz_ = 0.0f;
    float osc3_hz_ = 0.0f;
    float pan_base_ = 0.0f;
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
