#pragma once

#include <atomic>
#include <cstdint>

#include "boreaz/dsp/gain.h"
#include "boreaz/dsp/tuning.h"
#include "boreaz/engine/event.h"
#include "boreaz/engine/param_bus.h"
#include "boreaz/engine/params.h"
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
    static constexpr std::uint8_t kCcAllSoundOff = 120;
    static constexpr std::uint8_t kCcAllNotesOff = 123;

    explicit Engine(const Params& initial = {});
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Called when the device opens, which is not in the audio path.
    void prepare(double sample_rate);

    // Producer side.
    void set_params(const Params& params) { params_.publish(params); }
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
    std::size_t dropped_events() const { return queue_.dropped(); }
    bool sustain_pedal_down() const { return pedal_down_; }
    float bend_semitones(std::uint8_t channel) const { return bend_[channel % kMidiChannels]; }
    const VoiceAllocator& voices() const { return allocator_; }
    const boreaz::dsp::Tuning& tuning() const { return tuning_; }

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

    EventQueue queue_;
    ParamBus params_;
    Params live_{}; // the block the audio thread is playing
    Params applied_{};
    VoiceAllocator allocator_{VoiceAllocator::kDefaultPoolSize};
    boreaz::dsp::Tuning tuning_{};

    float scratch_l_[kMaxScratchFrames]{};
    float scratch_r_[kMaxScratchFrames]{};

    boreaz::dsp::Gain master_;
    boreaz::dsp::SoftClip clip_;
    float applied_master_gain_db_ = 0.0f;

    // Per-channel controller state, in semitones so a bend event does not redo the trig.
    float bend_[kMidiChannels]{};
    bool pedal_down_ = false;

    std::int64_t position_ = 0;
    std::atomic<int> active_voices_{0};
    std::atomic<int> steal_count_{0};
    float sample_rate_ = 44100.0f;
};

} // namespace boreaz::engine
