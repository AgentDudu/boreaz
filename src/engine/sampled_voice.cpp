#include "boreaz/engine/sampled_voice.h"

#include <algorithm>
#include <cmath>

#include "boreaz/engine/voice_common.h"

namespace boreaz::engine {
namespace {

// A ramp shorter than this is a click, and a click is a bug.
constexpr float kMinRampSeconds = 0.005f;
// The attack a recording does not have: 2 ms, which takes the edge off a buffer boundary and leaves
// the file's own transient alone. Anything longer starts re-drawing dynamics the microphone got.
constexpr float kAttackSeconds = 0.002f;
// A release nobody can still hear should not be holding a voice in a 16-voice pool.
constexpr float kMaxReleaseSeconds = 5.0f;

// A recording that has run out of file has finished: what it recorded is over. Five milliseconds is
// long enough that the last frame is not a step (a step is a click) and short enough that the voice
// goes back to the pool now and not half a second from now - which matters most for the short
// percussive files a real library is mostly made of.

// A reading speed far outside that range is a broken mapping or a broken note, and either way the
// answer is not to play the file at 400x.
constexpr double kMinIncrement = 0.02;
constexpr double kMaxIncrement = 16.0;
constexpr float kEndFadeSeconds = 0.005f;

double clamp_increment(double value) {
    if (!std::isfinite(value))
        return 1.0;
    return std::clamp(value, kMinIncrement, kMaxIncrement);
}

// Frames for a ramp of this many seconds. Deliberately no minimum in here: the floor that keeps a
// release from being a click is not a reason to stretch an attack that was chosen at 2 ms, and the
// two ramps are not the same decision.
std::int64_t ramp_frames(double seconds, float sample_rate) {
    const double frames =
        std::clamp(seconds, 0.0, static_cast<double>(kMaxReleaseSeconds)) *
        static_cast<double>(sample_rate);
    return static_cast<std::int64_t>(frames + 0.5);
}

} // namespace

void SampledVoice::ramp_to(float target, std::int64_t frames) {
    gain_target_ = target;
    if (frames <= 0) {
        gain_ = target;
        gain_step_ = 0.0f;
        ramp_left_ = 0;
        return;
    }
    ramp_left_ = frames;
    gain_step_ = (target - gain_) / static_cast<float>(frames);
}

bool SampledVoice::advance_gain() {
    if (ramp_left_ > 0) {
        --ramp_left_;
        // Land on the target exactly on the last frame, so a release ends at true silence and the
        // voice can be reused without a residue.
        gain_ = ramp_left_ > 0 ? gain_ + gain_step_ : gain_target_;
    }
    if (ending_ && ramp_left_ <= 0 && gain_target_ <= 0.0f) {
        state_ = State::kIdle;
        gain_ = 0.0f;
        return false;
    }
    return true;
}

void SampledVoice::start(const NoteRequest& request, const Params& params, double sample_rate,
                         std::int64_t start_tick) {
    sample_rate_ = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 44100.0f;
    note_ = request.note;
    key_ = request.key;
    channel_ = request.channel;
    velocity_ = std::clamp(request.velocity, 0.0f, 1.0f);
    frequency_ = std::isfinite(request.frequency) ? request.frequency : 261.625565f;
    start_tick_ = start_tick;
    release_tick_ = start_tick;
    applied_ = params;
    ending_ = false;
    key_pan(key_, params.pan_spread, pan_left_, pan_right_);

    const int midi_velocity =
        std::clamp(static_cast<int>(velocity_ * 127.0f + 0.5f), 1, 127);
    // A release voice asks the other question of the same instrument, and gets nullptr when the
    // library has no release recordings - which lands in the silence path below, so a library of
    // attacks alone costs one extra call per note-off and nothing else.
    sample_ = instrument_ ? (release_samples_ ? instrument_->find_release(key_, midi_velocity)
                                              : instrument_->find(key_, midi_velocity))
                          : nullptr;
    if (sample_ == nullptr || sample_->data == nullptr || sample_->channels <= 0 ||
        sample_->frames <= 0) {
        // No recording for this key. Silence, and *not* active: a voice that is doing nothing must
        // not occupy the pool, or an instrument with 48 of its 88 keys missing would steal its own
        // silent voices away from the keys it does have.
        sample_ = nullptr;
        state_ = State::kIdle;
        gain_ = 0.0f;
        ramp_left_ = 0;
        return;
    }

    // Frames advanced per output sample. Three ratios multiplied once, here, at the top of the
    // note: the file's rate against the device's, the key's distance from the recording's own
    // centre, and the bend the note was struck with. `exp2` is allowed because this is note start
    // and not the sample loop - the audio path gets a multiply and an add.
    const double semitones = static_cast<double>(note_ - sample_->key_center) +
                             static_cast<double>(request.bend_semitones) +
                             static_cast<double>(sample_->tune_cents) * 0.01;
    const double key_ratio = std::exp2(semitones / 12.0);
    increment_ = clamp_increment((sample_->file_sample_rate / static_cast<double>(sample_rate_)) *
                                 key_ratio);
    position_ = static_cast<double>(
        std::clamp(sample_->start_frame, static_cast<std::int64_t>(0), sample_->frames - 1));

    // `amp_veltrack` says how much of the file's own dynamics to keep: at 0 the recording plays at
    // the same level however hard the key was hit, at 100 it follows the velocity curve exactly.
    peak_gain_ = std::clamp(sample_->gain * (1.0f - sample_->velocity_tracking *
                                                             (1.0f - velocity_gain(velocity_))),
                            0.0f, 4.0f);
    ramp_to(peak_gain_, ramp_frames(static_cast<double>(kAttackSeconds), sample_rate_));
    state_ = State::kDown;
}

void SampledVoice::release(const Params& params, std::int64_t tick) {
    if (!active() || state_ == State::kReleased)
        return;
    state_ = State::kReleased;
    release_tick_ = tick;
    // The key letting go is the end of the note. The loop only decides whether the recording *would*
    // have carried on, and holding a looping voice open forever is how a 16-voice pool turns into
    // one that never steals: `off` stops it, and the release time the user set says how politely.
    ending_ = true;
    ramp_to(0.0f, ramp_frames(std::max(static_cast<double>(params.amp_release_s),
                                            static_cast<double>(kMinRampSeconds)),
                            sample_rate_));
}

void SampledVoice::silence(std::int64_t tick) {
    if (state_ != State::kDown)
        return;
    // The pedal took over. Keep sounding, keep the read-head moving, and become the first voice the
    // allocator steals - which is what lets a held pad survive until the pool needs it and no
    // longer.
    state_ = State::kHeldByPedal;
    release_tick_ = tick;
}

void SampledVoice::refresh(const Params& params, bool force) {
    if (!active())
        return;
    if (!force && params == applied_)
        return;
    applied_ = params;
    // Pan and nothing else on purpose: a sampled voice has no filter, and its amp shape is not the
    // synth's ADSR, so the only parameter it can honour mid-note is where it sits in the field.
    key_pan(key_, params.pan_spread, pan_left_, pan_right_);
}

void SampledVoice::set_frequency(float hertz, const Params& params) {
    (void)params;
    if (!active() || !std::isfinite(hertz) || frequency_ <= 0.0f)
        return;
    // The engine sends the new target pitch for the same note, so the ratio between the two *is*
    // the bend. Applied to the read-head, which is what a sample's pitch actually is; the head
    // position survives, so it glides instead of starting the recording again.
    increment_ = clamp_increment(increment_ * (static_cast<double>(hertz) /
                                               static_cast<double>(frequency_)));
    frequency_ = hertz;
}

void SampledVoice::retune(int semitones, const Params& params) {
    if (!active() || semitones == 0)
        return;
    // The same recording, played at the new pitch: the head position survives, so the note keeps
    // ringing through the change instead of restarting. This is what makes a transposed piano sound
    // like a piano being transposed and not a tape stop.
    note_ = std::clamp(note_ + semitones, 0, 127);
    set_frequency(clamp_pitch(frequency_ *
                                  static_cast<float>(
                                      std::exp2(static_cast<double>(semitones) / 12.0)),
                                  sample_rate_),
                  params);
}

void SampledVoice::render(float* left, float* right, int frames) {
    if (state_ == State::kIdle || sample_ == nullptr)
        return;

    const int channels = sample_->channels;
    const std::int64_t count = sample_->frames;
    const float* data = sample_->data;
    const double loop_end = static_cast<double>(sample_->loop_end);
    const double loop_length = static_cast<double>(sample_->loop_end - sample_->loop_start);

    for (int i = 0; i < frames; ++i) {
        if (!advance_gain())
            return; // faded out mid-block: the rest of the block belongs to the other voices

        if (sample_->loops && loop_length > 0.0) {
            // One subtraction is the common case; the loop is only wrong when a mapping names a
            // loop shorter than a block, and a while loop costs nothing to be correct about.
            while (position_ >= loop_end)
                position_ -= loop_length;
        } else if (position_ >= static_cast<double>(count)) {
            if (!ending_) {
                // A recording that has run out has finished, whatever the envelope says. Ramp
                // instead of stopping, or the last frame of the file is a step and the step is a
                // click.
                ending_ = true;
                ramp_to(0.0f, ramp_frames(kEndFadeSeconds, sample_rate_));
            }
            position_ = static_cast<double>(count - 1); // hold the last frame while it fades
        }

        const std::int64_t first = static_cast<std::int64_t>(position_);
        const std::int64_t second = std::min(first + 1, count - 1);
        const float fraction = static_cast<float>(position_ - static_cast<double>(first));

        const float near_left = data[first * channels];
        const float far_left = data[second * channels];
        const float sample_left = near_left + (far_left - near_left) * fraction;
        // More than two channels is legal in a WAV and meaningless to a piano: the first two are
        // the image and the rest is somebody else's surround bed.
        const float near_right = channels > 1 ? data[first * channels + 1] : near_left;
        const float far_right = channels > 1 ? data[second * channels + 1] : far_left;
        const float sample_right = near_right + (far_right - near_right) * fraction;

        const float gain = gain_;
        left[i] += gain * pan_left_ * sample_left;
        right[i] += gain * pan_right_ * sample_right;
        position_ += increment_;
    }
}

} // namespace boreaz::engine
