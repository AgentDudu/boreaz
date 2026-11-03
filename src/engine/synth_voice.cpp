#include "boreaz/engine/synth_voice.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

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

void apply_env2_times(boreaz::dsp::Adsr& env, const Params& params) {
    env.set_times(
        std::max(static_cast<double>(params.env2_attack_s), static_cast<double>(kMinSeconds)),
        std::max(static_cast<double>(params.env2_decay_s), static_cast<double>(kMinSeconds)),
        params.env2_sustain,
        std::max(static_cast<double>(params.env2_release_s), static_cast<double>(kMinSeconds)));
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
    // The three frequencies and the cutoff are kept as the *unmodulated* values, because every
    // block with a patch on it re-derives the moving ones from them. Accumulating instead would
    // walk a vibrato progressively sharp, which is the classic failure of this design.
    osc1_hz_ = frequency_;
    osc2_hz_ = frequency_ * 2.0f * detune;
    osc3_hz_ = frequency_ / detune;
    cutoff_hz_ = cutoff_for(note_, frequency_, params);

    osc1_.set_frequency(osc1_hz_, sample_rate_);
    osc1_.reset();
    osc2_.set_frequency(osc2_hz_, sample_rate_);
    osc2_.reset();
    osc3_.set_frequency(osc3_hz_, sample_rate_);
    osc3_.reset();

    filter_.set_mode(boreaz::dsp::Svf::Mode::LowPass);
    filter_.reset(); // a reused voice must not carry the last note's ringing filter into this one
    filter_.prepare(sample_rate_, cutoff_hz_, params.filter_resonance);

    amp_.prepare(sample_rate_);
    apply_envelope_times(amp_, params);
    amp_.gate_on();

    // Env2 is struck with the note, like the amp envelope, and starts from nothing: a reused voice
    // that kept the previous note's envelope would open the filter before the key was hit.
    env2_ = boreaz::dsp::Adsr{};
    env2_.prepare(sample_rate_);
    apply_env2_times(env2_, params);
    env2_.gate_on();

    // The LFOs are not reset: several notes struck in sequence should not all start their vibrato
    // at the same phase, which is what makes a chord of vibrato voices sound like one instrument.
    mod_ = Modulation{};
    prepare_modulation(params);

    peak_gain_ = velocity_gain(velocity_);
    noise_.seed(static_cast<std::uint32_t>(note_), start_tick);
    noise_step_ = 1.0f / (std::max(params.noise_decay_s, kMinSeconds) * sample_rate_);
    noise_phase_ = 1.0f;

    pan_base_ = key_pan_value(key_, params.pan_spread);
    pan_gains(pan_base_, pan_left_, pan_right_);
}

void SynthVoice::release(const Params& params, std::int64_t tick) {
    if (!active() || state_ == State::kReleased)
        return;
    state_ = State::kReleased;
    release_tick_ = tick;
    apply_envelope_times(amp_,
                         params); // takes the release time that is current now, not at note start
    amp_.gate_off();
    apply_env2_times(env2_, params); // the mod envelope releases with the note, whatever it is
    env2_.gate_off();                // modulating after the key went up is what a real Env2 does
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

    cutoff_hz_ = cutoff_for(note_, frequency_, params);
    filter_.prepare(sample_rate_, cutoff_hz_ * std::exp2(mod_.cutoff_octaves),
                    params.filter_resonance);
    apply_envelope_times(amp_, params);
    apply_env2_times(env2_, params);
    noise_step_ = 1.0f / (std::max(params.noise_decay_s, kMinSeconds) * sample_rate_);

    pan_base_ = key_pan_value(key_, params.pan_spread);
    pan_gains(pan_base_ + mod_.pan, pan_left_, pan_right_);
    prepare_modulation(params);
}

void SynthVoice::set_frequency(float hertz, const Params& params) {
    if (!std::isfinite(hertz))
        return;
    frequency_ = clamp_pitch(hertz, sample_rate_);
    const float detune = std::exp2(params.osc_detune_cents / 1200.0f);
    osc1_hz_ = frequency_;
    osc2_hz_ = frequency_ * 2.0f * detune;
    osc3_hz_ = frequency_ / detune;
    // The bend itself pitches the oscillators; a vibrato on top is re-applied by the next block.
    osc1_.set_frequency(osc1_hz_, sample_rate_);
    osc2_.set_frequency(osc2_hz_, sample_rate_);
    osc3_.set_frequency(osc3_hz_, sample_rate_);
    cutoff_hz_ = cutoff_for(note_, frequency_, params);
    filter_.prepare(sample_rate_, cutoff_hz_ * std::exp2(mod_.cutoff_octaves),
                    params.filter_resonance);
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

void SynthVoice::prepare_modulation(const Params& params) {
    // Which sources exist and which destinations move is a property of the patch, not of the
    // block, so it is worked out here and read as eight bools by `apply_modulation`. A patch that
    // routes nothing leaves every one of them false and `render` never calls the router at all.
    mod_active_ = false;
    mod_used_env2_ = false;
    mod_used_lfo1_ = false;
    mod_used_lfo2_ = false;
    mod_moves_cutoff_ = false;
    mod_moves_pitch_ = false;
    mod_moves_pan_ = false;

    for (std::size_t index = 0; index < static_cast<std::size_t>(kModSlots); ++index) {
        const int raw_source = params.mod_source[index];
        const int raw_dest = params.mod_dest[index];
        const float depth = params.mod_depth[index];
        // A slot with an unwired end, or with nothing to say, is not routed. This is the whole
        // reason an unpatched instrument pays nothing for the eight slots it is carrying.
        if (raw_source <= static_cast<int>(ModSource::kOff) ||
            raw_dest <= static_cast<int>(ModDest::kOff) || depth == 0.0f)
            continue;

        switch (static_cast<ModSource>(raw_source)) {
            case ModSource::kEnv2:
                mod_used_env2_ = true;
                break;
            case ModSource::kLfo1:
                mod_used_lfo1_ = true;
                break;
            case ModSource::kLfo2:
                mod_used_lfo2_ = true;
                break;
            default:
                break; // velocity, keytrack and the wheel are free: they are already sitting here
        }
        switch (static_cast<ModDest>(raw_dest)) {
            case ModDest::kCutoff:
                mod_moves_cutoff_ = true;
                break;
            case ModDest::kPitch:
                mod_moves_pitch_ = true;
                break;
            case ModDest::kPan:
                mod_moves_pan_ = true;
                break;
            default:
                break; // a level is applied where the level is read, so it needs no flag
        }
        mod_active_ = true;
    }

    // Rates are re-read here rather than per block. Setting a frequency does not touch the phase,
    // so a rate knob moves the pitch of the LFO without restarting it.
    lfo1_.set_frequency(params.lfo1_rate_hz, sample_rate_);
    lfo2_.set_frequency(params.lfo2_rate_hz, sample_rate_);
}

void SynthVoice::apply_modulation(int frames) {
    // One value per source per block, and only for a source somebody routed. `advance_block`
    // returns the value where the block begins and moves the phase on by the block, which is the
    // rate the real-time rules set for LFOs and filter coefficients alike.
    //
    // shortcut: modulation is block-rate, so a 20 Hz LFO reaches 3 steps per cycle at 44.1 kHz's
    // 64-frame blocks. Fine for a cutoff or a pan; a per-sample ramp is the upgrade if a patch
    // ever wants smooth vibrato, and it costs one add per sample per routed destination.
    float env2_value = 0.0f;
    if (mod_used_env2_) {
        env2_value = env2_.value();
        for (int i = 0; i < frames; ++i)
            env2_.process(); // keeps the envelope's clock in step with the audio
    }
    const float lfo1_value = mod_used_lfo1_ ? lfo1_.advance_block(frames) : 0.0f;
    const float lfo2_value = mod_used_lfo2_ ? lfo2_.advance_block(frames) : 0.0f;
    const float keytrack = keytrack_value(key_);
    const float wheel = mod_wheel();

    Modulation totals{};
    for (std::size_t index = 0; index < static_cast<std::size_t>(kModSlots); ++index) {
        const int raw_source = applied_.mod_source[index];
        const int raw_dest = applied_.mod_dest[index];
        const float depth = applied_.mod_depth[index];
        if (raw_source <= static_cast<int>(ModSource::kOff) ||
            raw_dest <= static_cast<int>(ModDest::kOff) || depth == 0.0f)
            continue; // an unwired end or a depth of zero is not a modulation, and costs nothing
        const auto source = static_cast<ModSource>(raw_source);
        const auto dest = static_cast<ModDest>(raw_dest);

        float value = 0.0f;
        switch (source) {
            case ModSource::kEnv2: value = env2_value; break;
            case ModSource::kLfo1: value = lfo1_value; break;
            case ModSource::kLfo2: value = lfo2_value; break;
            case ModSource::kVelocity: value = velocity_; break;
            case ModSource::kKeyTrack: value = keytrack; break;
            case ModSource::kWheel: value = wheel; break;
            default: continue;
        }

        if (!std::isfinite(depth))
            continue; // a depth that is not a number would make the frequency one too

        const float amount = mod_amount(dest, depth,
                                        shape_source_value(source, value,
                                                           applied_.mod_unipolar[index] != 0));
        switch (dest) {
            case ModDest::kCutoff: totals.cutoff_octaves += amount; break;
            case ModDest::kPitch: totals.pitch_semitones += amount; break;
            case ModDest::kOsc2Level: totals.osc2_level += amount; break;
            case ModDest::kOsc3Level: totals.osc3_level += amount; break;
            case ModDest::kNoiseLevel: totals.noise_level += amount; break;
            case ModDest::kPan: totals.pan += amount; break;
            default: break;
        }
    }
    mod_ = totals;

    // Two slots on one destination add together, which is what a mixer of modulation means; the
    // filter and the oscillators are then re-prepared from the unmodulated values, not from last
    // block's, so nothing accumulates.
    if (mod_moves_cutoff_)
        filter_.prepare(sample_rate_, std::max(kMinFrequencyHz, cutoff_hz_ * std::exp2(mod_.cutoff_octaves)),
                        applied_.filter_resonance);
    if (mod_moves_pitch_) {
        const float ratio = std::exp2(mod_.pitch_semitones / 12.0f);
        osc1_.set_frequency(osc1_hz_ * ratio, sample_rate_);
        osc2_.set_frequency(osc2_hz_ * ratio, sample_rate_);
        osc3_.set_frequency(osc3_hz_ * ratio, sample_rate_);
    }
    if (mod_moves_pan_)
        pan_gains(pan_base_ + mod_.pan, pan_left_, pan_right_);
}

void SynthVoice::render(float* left, float* right, int frames) {
    if (!active())
        return;

    if (mod_active_)
        apply_modulation(frames);

    // A modulation of zero is added rather than branched on, because adding zero is exact and a
    // branch on a value that changes per block is a branch the compiler cannot hoist.
    const float noise_level = this->noise_level();
    const float l2 = osc2_level();
    const float l3 = osc3_level();
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
