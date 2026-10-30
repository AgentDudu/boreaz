#pragma once

#include <cstdint>

#include "boreaz/engine/instrument.h"
#include "boreaz/engine/note.h"
#include "boreaz/engine/voice_common.h"
#include "boreaz/engine/params.h"
#include "boreaz/engine/voice.h"

namespace boreaz::engine {

// One playing recording: a read-head over PCM somebody else decoded.
//
// What it deliberately is not:
//   - not a decoder. `VoiceSample::data` is float memory the instrument placed there before the
//     audio thread started. There is no decoder on this path (PLAN.md §2), because a decoder's
//     worst case would become the audio thread's worst case, and a pop in the middle of a chord is
//     not something a pianist forgives.
//   - not a resampler. The increment carries the ratio between the file's rate and the device's
//     together with the pitch of the key, so a 44.1 kHz instrument on a 48 kHz device costs one
//     multiply per note and no double-buffer of resampled audio.
//   - not a filter, an LFO, or an effects box. The amp shape is a short attack and a release: that
//     is what a recording needs, and the synth's ADSR would only re-draw dynamics the microphone
//     already captured.
//
// Real-time contract: no allocation, no I/O, nothing that can fail. The buffers are not even here -
// they belong to the instrument and outlive every voice.
class SampledVoice final : public Voice {
  public:
    // The instrument outlives the voice: it is loaded before the audio thread starts and is only
    // ever read from here. A null instrument is allowed and silent, which is how a pool can be
    // built before the sample library has finished loading.
    explicit SampledVoice(const Instrument* instrument) : instrument_(instrument) {}

    void start(const NoteRequest& request, const Params& params, double sample_rate,
               std::int64_t start_tick) override;
    void release(const Params& params, std::int64_t tick) override;
    void silence(std::int64_t tick) override;
    void refresh(const Params& params, bool force) override;
    void set_frequency(float hertz, const Params& params) override;
    void retune(int semitones, const Params& params) override;
    void render(float* left, float* right, int frames) override;

    bool active() const override { return state_ != State::kIdle; }
    bool released() const override { return state_ == State::kReleased; }
    bool sustained() const override { return state_ == State::kHeldByPedal; }

    int note() const override { return note_; }
    int key() const override { return key_; }
    int channel() const override { return channel_; }
    float velocity() const override { return velocity_; }
    float frequency() const override { return frequency_; }
    std::int64_t start_tick() const override { return start_tick_; }
    std::int64_t release_tick() const override { return release_tick_; }
    float amplitude() const override { return gain_; }
    float strike() const override { return peak_gain_; }

    // What the voice is doing, for a test that wants the loop proven and not merely implied.
    double position() const { return position_; }
    double increment() const { return increment_; }
    const VoiceSample* sample() const override { return sample_; }

  private:
    enum class State : std::uint8_t { kIdle, kDown, kHeldByPedal, kReleased };

    // Ramp the output gain to `target` over `frames` samples. One straight line, no exponential:
    // an attack long enough to hear would colour a recording that already has its own transient.
    void ramp_to(float target, std::int64_t frames);
    // One sample of the gain envelope. Goes idle, and says so, when a voice that was ending has
    // finished ramping: that is what returns a stolen voice to the pool.
    bool advance_gain();

    const Instrument* instrument_ = nullptr;
    const VoiceSample* sample_ = nullptr;

    State state_ = State::kIdle;
    bool ending_ = false; // running out, or let go: ramping down, then idle

    double position_ = 0.0;  // fractional frame index into `sample_`
    double increment_ = 1.0; // frames advanced per output sample: rate ratio x key ratio x bend

    float gain_ = 0.0f;
    float gain_target_ = 0.0f;
    float gain_step_ = 0.0f;
    std::int64_t ramp_left_ = 0;

    int note_ = 0;
    int key_ = 60;
    int channel_ = 0;
    float velocity_ = 0.0f;
    float frequency_ = 0.0f;
    float peak_gain_ = 0.0f;
    float pan_left_ = 0.70710678f;
    float pan_right_ = 0.70710678f;
    std::int64_t start_tick_ = 0;
    std::int64_t release_tick_ = 0;
    float sample_rate_ = 44100.0f;

    Params applied_{};
};

} // namespace boreaz::engine
