#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "boreaz/dsp/gain.h"
#include "boreaz/dsp/tuning.h"
#include "boreaz/engine/event.h"
#include "boreaz/engine/fx_bus.h"
#include "boreaz/engine/instrument.h"
#include "boreaz/engine/param_bus.h"
#include "boreaz/engine/params.h"
#include "boreaz/engine/sampled_voice.h"
#include "boreaz/engine/voice_allocator.h"

namespace boreaz::engine {

// The synthesizer: events in, audio out, no devices and no files anywhere in sight. That is
// what makes the whole thing testable headless (plan 3).
//
// Threading: one thread produces (MIDI callback, sequencer, GUI button, offline renderer) with
// push_event() and set_params(); one thread consumes with process(). Nothing else is safe from
// both, and nothing in here allocates, opens a file, or locks.
class Engine {
  public:
    // Scratch is fixed, so a bigger `frames` is processed in that many chunks instead of growing
    // anything at run time.
    static constexpr int kMaxScratchFrames = 512;
    static constexpr int kMaxPolyphony = VoiceAllocator::kDefaultPoolSize;
    static constexpr std::uint8_t kMidiChannels = 16;
    // MIDI's own "note on with velocity 0 means note off", honoured because real devices send it.
    static constexpr std::uint8_t kCcSustainPedal = Event::kSustainPedal;
    // GM's channel volume. A keyboard's volume slider is this controller, so it is a mixer input
    // and not an engine parameter: it belongs to a channel, it arrives on the audio path, and it is
    // meant to move while a note is sounding.
    static constexpr std::uint8_t kCcChannelVolume = 7;
    static constexpr std::uint8_t kCcAllSoundOff = 120;
    static constexpr std::uint8_t kCcAllNotesOff = 123;

    explicit Engine(const Params& initial = {});
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Called when the device opens, which is not in the audio path.
    void prepare(double sample_rate);

    // Producer side.
    void set_params(const Params& params) {
        params_.publish(params);
        // The reference pitch is the one parameter that cannot ride the per-block ramp, because it
        // is not a number the audio path multiplies - it is a 128-entry lookup table built with
        // `pow`, and `pow` does not belong in a callback. Rebuilding it here happens on the thread
        // that owns the change, at most once per knob move. Held notes keep the pitch they started
        // with, which is what a piano being tuned between movements does too; `transpose` is the
        // control that retunes what is already sounding.
        if (params.tuning_reference_hz != tuning_.reference_hz())
            tuning_ = boreaz::dsp::Tuning(params.tuning_reference_hz);
    }
    bool push_event(const Event& event) { return queue_.push(event); }

    // Consumer side: one block of interleaved stereo, guaranteed inside +/-1.0.
    void process(float* output, int frames);

    // Keys 0-20 and 109-127 are MIDI keys an acoustic piano does not have. They play - the pitch
    // comes from the LUT, so they are not silence - and stay flagged for the display, which draws
    // 88 keys and must not pretend they are not there.
    static bool is_extended_key(int key) {
        return key < boreaz::dsp::Tuning::kFirstKey || key > boreaz::dsp::Tuning::kLastKey;
    }
    // Where a key lands once transposed, so the GUI lights the key that is actually sounding.
    int sounding_note(int key) const;

    // Telemetry: scalars. The UI may look any time, and a display that can delay the audio path
    // is a bug (plan 3).
    int active_voices() const { return active_voices_.load(std::memory_order_relaxed); }
    int pool_size() const { return allocator_.pool_size(); }
    int steal_count() const { return steal_count_.load(std::memory_order_relaxed); }
    std::int64_t sample_position() const { return position_; }
    // The loudest sample of the last block, before the limiter bent it. 1.0 is where the tanh
    // starts working, so this is the clip meter: 0.7 is a mix with room, 1.05 is a mix that is
    // being squashed and nobody said so. Per block, not per sample, which is what a light needs.
    float drive_peak() const { return drive_peak_.load(std::memory_order_relaxed); }
    std::size_t dropped_events() const { return queue_.dropped(); }
    bool sustain_pedal_down() const { return pedal_down_; }
    float bend_semitones(std::uint8_t channel) const { return bend_[channel % kMidiChannels]; }
    const VoiceAllocator& voices() const { return allocator_; }
    const boreaz::dsp::Tuning& tuning() const { return tuning_; }

    // What plays: a sample library, or nothing, which is the synth. Setup path only - it rebuilds
    // the voice pool, and a pool rebuilt while notes are sounding drops them.
    //
    // The instrument must outlive the engine: after the first block, a voice holds a pointer into
    // it, and there is no way to take that pointer back.
    void set_instrument(const boreaz::engine::Instrument* instrument) {
        instrument_ = instrument;
        allocator_.reallocate(allocator_.pool_size(), instrument);
        // The release pool too, and emptied first: a release recording is a buffer inside the store
        // being replaced, and that store may be freed the moment this returns. Setup path, with the
        // device stopped, so the hard stop is not a click anybody hears.
        for (SampledVoice& voice : releases_) {
            voice.stop();
            voice.set_instrument(instrument);
        }
    }

    // What the engine has been told to play: the published set, not the block currently being
    // heard. A mixer asks this question, because the block being heard is on its way to the
    // published one, and a slider that displayed the number it was ramping from would fight the
    // mouse. Takes the bus's lock, so it belongs on the UI thread and not in a callback.
    Params params() { return params_.latest(); }

    // Test seam: a smaller pool makes the stealing rules testable without 40 held notes. Never
    // from the audio path, and never while notes are sounding.
    void set_pool_size_for_test(int pool_size);

  private:
    // What the audio thread applied in the last block. Published parameters take effect when a
    // block is processed, so telemetry and sounding_note() report the engine as it is playing,
    // not as it was told to be.
    Params current_params() const { return live_; }
    static int note_for_key(int key, const Params& params);
    static int effective_transpose(const Params& params) {
        return params.transpose_semitones + params.octave_shift * 12;
    }

    void apply_event(const Event& event, const Params& params);
    void note_on(const Event& event, const Params& params);
    void pitch_bend(const Event& event, const Params& params);
    void render_chunk(float* output, int frames, const Params& params);

    // The key went up. Start its release recording, if the instrument has one, in the pool outside
    // the 16: a release must outlive the voice it was born from, so it cannot be a slot in it, and
    // polyphony, stealing and the display all read exactly what they read before this existed.
    void start_release(int key, std::uint8_t channel, const Params& params);
    void fade_releases(const Params& params);

    // Move every sounding note by the change in transpose/octave. One call per keystroke, not per
    // block: the voice keeps the phase, so there is nothing to smooth afterwards.
    void retune_voices(int semitones, const Params& params);

    // Hand every sounding voice the fader of the channel it is on. Called whenever a block contains
    // an event, which is what makes a CC7 move land on a note that is already held instead of only
    // on the next note, and makes a fader and a note that arrive in the same block agree. Cheap: a
    // compare and a store per voice, and idle blocks do not call it at all.
    void push_channel_gains();

    EventQueue queue_;
    ParamBus params_;
    Params live_{}; // the block the audio thread is playing
    Params applied_{};
    int applied_shift_ = 0; // the transpose+octave the sounding notes were pitched with
    VoiceAllocator allocator_{VoiceAllocator::kDefaultPoolSize};
    boreaz::dsp::Tuning tuning_{};

    // `trigger=release`, in its own eight slots (PLAN.md 6's answer to "release samples fight
    // note-off voice reuse"). They are the same voice reading the other half of the library; they
    // are never stolen from, never counted in `active_voices`, and never the reason a held note is
    // still sounding. Eight is the size of a fast glissando's worth of overlapping hammer noise, and
    // fixed because a pool that grows is a pool that mallocs.
    static constexpr int kMaxReleaseVoices = 8;
    std::array<SampledVoice, kMaxReleaseVoices> releases_;
    const Instrument* instrument_ = nullptr;
    // A note-off arrives as velocity 0, which is not how the note was struck. The release recording
    // and its level come from the strike, so the last velocity of every key is remembered here.
    std::array<std::uint8_t, 128> strike_{};

    float scratch_l_[kMaxScratchFrames]{};
    float scratch_r_[kMaxScratchFrames]{};

    boreaz::dsp::Gain master_;
    boreaz::dsp::SoftClip clip_;
    // Between the voices and the master gain: an effect return that sat after the fader would be a
    // second master volume that keeps playing when the piano is faded out.
    FxBus fx_;
    float applied_master_gain_db_ = 0.0f;

    // Per-channel controller state, in semitones so a bend event does not redo the trig.
    float bend_[kMidiChannels]{};
    // CC7 per channel, linear. Unity until a cable says otherwise: GM's own default is 100, and
    // starting there would be a 2.1 dB surprise for anything that never sends the controller.
    std::array<float, kMidiChannels> channel_gain_{};
    bool pedal_down_ = false;

    std::atomic<float> drive_peak_{0.0f};
    std::int64_t position_ = 0;
    std::atomic<int> active_voices_{0};
    std::atomic<int> steal_count_{0};
    float sample_rate_ = 44100.0f;
};

} // namespace boreaz::engine
