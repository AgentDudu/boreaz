#include "boreaz/engine/synth_voice.h"

#include <algorithm>
#include <cmath>

namespace boreaz::engine {
namespace {

constexpr float kMinSeconds = 1.0e-4f; // shorter than this is a click, and a click is a bug
constexpr float kMinFrequencyHz = 1.0f;   // the floor for a cutoff, which may not be zero
constexpr int kLastMidiNote = 127;        // the tuning table is 128 entries because MIDI is

// Cutoff for a note. keytrack 1 means the cutoff follows the note, which is what a piano does
// (the bass is dark, the treble is not); keytrack 0 means every note gets the cutoff middle C
// would get. Blending in the log domain costs one exp2f, once per note or per cutoff change.
float cutoff_for(int note, float note_frequency, const Params& params) {
    const double exponent = (60.0 - static_cast<double>(note)) *
                            (1.0 - static_cast<double>(params.filter_keytrack)) / 12.0;
    const float cutoff =
        note_frequency * params.filter_cutoff_ratio * static_cast<float>(std::exp2(exponent));
    return std::isfinite(cutoff) ? std::max(cutoff, kMinFrequencyHz) : kMinFrequencyHz;
}

void apply_envelope_times(boreaz::dsp::Adsr& amp, const Params& params) {
    amp.set_times(
        std::max(static_cast<double>(params.amp_attack_s), static_cast<double>(kMinSeconds)),
        std::max(static_cast<double>(params.amp_decay_s), static_cast<double>(kMinSeconds)),
        params.amp_sustain,
        std::max(static_cast<double>(params.amp_release_s), static_cast<double>(kMinSeconds)));
}

} // namespace

void SynthVoice::start(const NoteRequest& request, const Params& params, double sample_rate,
                  std::int64_t start_tick) {
    sample_rate_ = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 44100.0f;

    note_ = request.note;
    key_ = request.key;
    channel_ = request.channel;
    velocity_ = std::clamp(request.velocity, 0.0f, 1.0f);
    frequency_ = std::isfinite(request.frequency) ? clamp_pitch(request.frequency, sample_rate_)
                                                  : 261.625565f; // middle C, the least surprising default
    start_tick_ = start_tick;
    release_tick_ = start_tick;
    state_ = State::kDown;
    applied_ = params;

    // shortcut: exp2f/cosf/sinf once per note here, never per sample. The plan's ban on pow in
    // the audio path is about the per-sample pitch, which comes from the Tuning LUT.
    const float detune = std::exp2(params.osc_detune_cents / 1200.0f);
    osc1_.set_frequency(frequency_, sample_rate_);
    osc1_.reset();
    osc2_.set_frequency(frequency_ * 2.0f * detune, sample_rate_);
    osc2_.reset();
    osc3_.set_frequency(frequency_ / detune, sample_rate_);
    osc3_.reset();

    filter_.set_mode(boreaz::dsp::Svf::Mode::LowPass);
    filter_.reset(); // a reused voice must not carry the last note's ringing filter into this one
    filter_.prepare(sample_rate_, cutoff_for(note_, frequency_, params), params.filter_resonance);

    amp_.prepare(sample_rate_);
    apply_envelope_times(amp_, params);
    amp_.gate_on();

    peak_gain_ = velocity_gain(velocity_);
    noise_.seed(static_cast<std::uint32_t>(note_), start_tick);
    noise_step_ = 1.0f / (std::max(params.noise_decay_s, kMinSeconds) * sample_rate_);
    noise_phase_ = 1.0f;

    key_pan(key_, params.pan_spread, pan_left_, pan_right_);
}

void SynthVoice::release(const Params& params, std::int64_t tick) {
    if (!active() || state_ == State::kReleased)
        return;
    state_ = State::kReleased;
    release_tick_ = tick;
    apply_envelope_times(amp_,
                         params); // takes the release time that is current now, not at note start
    amp_.gate_off();
}

void SynthVoice::silence(std::int64_t tick) {
    if (state_ != State::kDown)
        return;
    state_ = State::kHeldByPedal;
    release_tick_ = tick;
}

void SynthVoice::refresh(const Params& params, bool force) {
    if (!active())
        return;
    if (!force && params == applied_)
        return;
    applied_ = params;

    filter_.prepare(sample_rate_, cutoff_for(note_, frequency_, params), params.filter_resonance);
    apply_envelope_times(amp_, params);
    noise_step_ = 1.0f / (std::max(params.noise_decay_s, kMinSeconds) * sample_rate_);

    key_pan(key_, params.pan_spread, pan_left_, pan_right_);
}

void SynthVoice::set_frequency(float hertz, const Params& params) {
    if (!std::isfinite(hertz))
        return;
    frequency_ = clamp_pitch(hertz, sample_rate_);
    const float detune = std::exp2(params.osc_detune_cents / 1200.0f);
    osc1_.set_frequency(frequency_, sample_rate_);
    osc2_.set_frequency(frequency_ * 2.0f * detune, sample_rate_);
    osc3_.set_frequency(frequency_ / detune, sample_rate_);
    filter_.prepare(sample_rate_, cutoff_for(note_, frequency_, params), params.filter_resonance);
}

void SynthVoice::retune(int semitones, const Params& params) {
    if (!active() || semitones == 0)
        return;
    // The note number moves with the pitch, so everything read from it agrees with what is audible:
    // the keytracked cutoff, the note the engine reports, and the frequency a pitch bend after this
    // one is measured from. Clamped to the MIDI range rather than wrapped or refused, because the
    // engine accepts the whole 0-127 range and the table is built for it.
    note_ = std::clamp(note_ + semitones, 0, kLastMidiNote);
    const float ratio = static_cast<float>(std::exp2(static_cast<double>(semitones) / 12.0));
    // `set_frequency` keeps the phase of all three oscillators and re-prepares the filter without
    // touching its memory, so the wave has no seam in it: the note changes, the sound does not stop.
    set_frequency(clamp_pitch(frequency_ * ratio, sample_rate_), params);
}

void SynthVoice::render(float* left, float* right, int frames) {
    if (!active())
        return;

    const float noise_level = applied_.noise_level;
    const float l2 = applied_.osc2_level;
    const float l3 = applied_.osc3_level;
    // The channel fader folded into the pan taps, so CC7 costs no extra multiply per sample.
    const float pan_left = pan_left_ * channel_gain_;
    const float pan_right = pan_right_ * channel_gain_;
    float* l = left;
    float* r = right;

    for (int i = 0; i < frames; ++i) {
        float mixed = osc1_.step();
        mixed += l2 * osc2_.step();
        mixed += l3 * osc3_.step();
        if (noise_level > 0.0f && noise_phase_ > 0.0f) {
            mixed += noise_level * noise_phase_ * noise_.next();
            noise_phase_ -= noise_step_;
        }
        // The voice keeps itself inside full scale so nothing downstream depends on the master
        // limiter to be legal; a resonant SVF pushed hard can leave the passband above 1.0.
        const float sample =
            std::clamp(filter_.process(mixed) * amp_.process() * peak_gain_, -1.0f, 1.0f);
        *l++ += sample * pan_left;
        *r++ += sample * pan_right;

        if (!amp_.active()) {
            // The envelope has run to zero: give the voice back. Without this the pool fills with
            // silent voices and the engine starts stealing from notes you are still holding.
            state_ = State::kIdle;
            return;
        }
    }
}

} // namespace boreaz::engine
