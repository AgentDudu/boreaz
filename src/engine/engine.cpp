#include "boreaz/engine/engine.h"

#include <algorithm>
#include <cmath>

#include "boreaz/engine/param_table.h"

namespace boreaz::engine {
namespace {

// How long a parameter takes to get where it is going. Two to three time constants is ~30 ms, which
// is below the ear's ability to call it a glide and above what a block boundary can turn into a
// click.
constexpr float kParamSlewSeconds = 0.010f;

// dB to linear. One pow per master-gain *change*, never per sample; moving it to the publish
// path is the fix if a UI ever publishes a new gain every block.
float db_to_linear(float db) {
    return std::isfinite(db) ? static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0))
                             : 1.0f;
}

float bend_factor(float semitones) {
    // exp2 at event rate (a bend message), not per sample: the per-sample pitch stays a LUT
    // lookup multiplied by this one cached number.
    return std::isfinite(semitones)
               ? static_cast<float>(std::exp2(static_cast<double>(semitones) / 12.0))
               : 1.0f;
}

// A key that was never struck can still be let go by a stray note-off, and a note that was stolen
// by voice-stealing was struck. Middle-forward is the honest guess at how hard; the alternative,
// not firing a release at all, loses the sound the opcode exists for.
constexpr std::uint8_t kUnstruckVelocity = 100;

} // namespace

Engine::Engine(const Params& initial) {
    Params params = initial;
    params.sanitize();
    // Publish the starting parameters to the bus the audio thread reads. A bus nobody published to
    // hands back defaults, so an engine built from a parameter file would play its first block as
    // configured and every block after that at defaults - which is how boreaz-render produced byte
    // identical audio for master_gain_db = -40 and -12 while reporting both as success.
    set_params(params);
    applied_ = params;
    live_ = params; // until a block is processed, this is what the engine would play
    applied_shift_ = effective_transpose(params);
    applied_master_gain_db_ = params.master_gain_db;
    // Immediately, not with a ramp: this is the level the engine starts at, and ramping to it from
    // unity is a click at the top of every render and every patch load.
    master_.set_linear_immediate(db_to_linear(params.master_gain_db));
    // From here to the first process() call is setup, not the audio path.
    clip_.prepare(sample_rate_);
    strike_.fill(kUnstruckVelocity);
    channel_gain_.fill(1.0f);
    for (SampledVoice& voice : releases_)
        voice.plays_release_samples(true);
}

void Engine::prepare(double sample_rate) {
    sample_rate_ = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 44100.0f;
    clip_.prepare(sample_rate_);
    // Nothing else to size: the pool, the ring and the scratch buffers are already fixed, which
    // is the point (plan 3: all buffers preallocated at device open).
}

void Engine::set_pool_size_for_test(int pool_size) {
    allocator_.reallocate(pool_size);
    active_voices_.store(0, std::memory_order_relaxed);
}

int Engine::sounding_note(int key) const {
    return note_for_key(key, current_params());
}

int Engine::note_for_key(int key, const Params& params) {
    return std::clamp(key + effective_transpose(params), 0, boreaz::dsp::Tuning::kNoteCount - 1);
}

void Engine::process(float* output, int frames) {
    if (output == nullptr || frames <= 0)
        return;

    // Once per block: take the parameters the UI published last and move toward them. The move is
    // a ramp, not a jump - zipper smoothing, after the potentiometer that clicks when you turn it
    // fast. A resonant filter whose cutoff goes 0 to 1 in one sample is a discontinuity, and a
    // discontinuity is a click you can hear through a whole phrase. 10 ms is the compromise: fast
    // enough that a key press is still a key press, slow enough that nothing in a 512-frame block
    // moves far enough to ring.
    const Params target = params_.latest();
    const float block_seconds =
        static_cast<float>(frames) / std::max(sample_rate_, 1.0f); // set once at device open
    param_slew(live_, target, 1.0f - std::exp(-block_seconds / kParamSlewSeconds));

    const Params params = live_;
    // A transpose or an octave shift moves the keys under notes that are already sounding, and the
    // player expects those notes to move with them. Do it before the refresh below so the cutoff a
    // voice re-prepares is measured from the note it is now playing, not the one it started on.
    const int shift = effective_transpose(params);
    if (shift != applied_shift_) {
        retune_voices(shift - applied_shift_, params);
        applied_shift_ = shift;
    }
    if (params != applied_) {
        allocator_.refresh(params, false);
        applied_ = params;
    }
    if (params.master_gain_db != applied_master_gain_db_) {
        master_.set_linear(db_to_linear(params.master_gain_db));
        applied_master_gain_db_ = params.master_gain_db;
    }

    int done = 0;
    while (done < frames) {
        const int chunk = std::min(frames - done, kMaxScratchFrames);
        render_chunk(output + static_cast<std::size_t>(done) * 2u, chunk, params);
        position_ += chunk;
        done += chunk;
    }

    active_voices_.store(allocator_.active_count(), std::memory_order_relaxed);
    steal_count_.store(allocator_.steal_count(), std::memory_order_relaxed);
}

void Engine::retune_voices(int semitones, const Params& params) {
    for (int index = 0; index < allocator_.pool_size(); ++index) {
        Voice& voice = allocator_.voice(index);
        if (voice.active())
            voice.retune(semitones, params);
    }
}

void Engine::push_channel_gains() {
    for (int index = 0; index < allocator_.pool_size(); ++index) {
        Voice& voice = allocator_.voice(index);
        if (!voice.active())
            continue;
        const int channel = voice.channel();
        if (channel >= 0 && channel < kMidiChannels)
            voice.set_channel_gain(channel_gain_[static_cast<std::size_t>(channel)]);
    }
    // The release pool takes its level from the key that went down, and CC7 is not a key: a channel
    // fader pulled to zero while a hammer noise is sounding is a player asking for silence.
    for (SampledVoice& voice : releases_) {
        if (!voice.active())
            continue;
        const int channel = voice.channel();
        if (channel >= 0 && channel < kMidiChannels)
            voice.set_channel_gain(channel_gain_[static_cast<std::size_t>(channel)]);
    }
}

void Engine::render_chunk(float* output, int frames, const Params& params) {
    std::fill_n(scratch_l_, frames, 0.0f);
    std::fill_n(scratch_r_, frames, 0.0f);

    // Events land on the sample they were scheduled for rather than on the block boundary,
    // because a note on and off inside one 64-frame block would otherwise cancel out and vanish.
    // With no events queued this is two atomics and one render call.
    int offset = 0;
    Event next;
    while (offset < frames) {
        bool moved = false;
        while (queue_.peek(next) && next.tick <= position_ + offset) {
            queue_.pop(next);
            apply_event(next, params);
            moved = true;
        }
        // The fader is pushed after the events of this stretch are applied, not once per block at
        // the top: a note and a CC7 that arrive in the same block must not leak one block of sound
        // at the level the fader used to be at, which is exactly the case when a keyboard sends its
        // volume along with the first note. Idle blocks cost nothing, which is why this is not
        // unconditional.
        if (moved)
            push_channel_gains();
        int run = frames - offset;
        if (queue_.peek(next)) {
            const std::int64_t due = next.tick - (position_ + offset);
            if (due >= 0)
                run = static_cast<int>(std::min<std::int64_t>(due, frames - offset));
        }
        // `run` cannot be zero: every event due at this sample was just applied, so whatever is
        // next is at least one sample ahead.
        allocator_.render(scratch_l_ + offset, scratch_r_ + offset, run);
        // The release recordings render at the same offset in the same pass, so a release starts on
        // the sample its key-up landed on and still goes through the master gain and the clipper
        // below. Idle voices cost one enum compare, which is why there is no "any sounding?" flag.
        for (SampledVoice& voice : releases_)
            voice.render(scratch_l_ + static_cast<std::size_t>(offset),
                         scratch_r_ + static_cast<std::size_t>(offset), run);
        offset += run;
    }

    // Master gain ramps inside the chunk, so a gain move is a slide and not a click.
    master_.process_block(scratch_l_, frames);
    master_.process_block(scratch_r_, frames);
    // tanh first: sixteen voices summed past full scale should saturate, not turn into a square
    // wave. The clamp after it is the promise that nothing leaves this block above full scale,
    // which the real limiter (plan 3, FX bus) takes over in the FX phase.
    const float drive_l = clip_.process_block(scratch_l_, frames);
    const float drive_r = clip_.process_block(scratch_r_, frames);
    const float drive = drive_l > drive_r ? drive_l : drive_r;
    // One relaxed store per block. A meter that reads a value from two blocks ago is still a
    // working meter, and this keeps the display off the audio path's critical instructions.
    drive_peak_.store(drive, std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) {
        output[static_cast<std::size_t>(i) * 2u] = std::clamp(scratch_l_[i], -1.0f, 1.0f);
        output[static_cast<std::size_t>(i) * 2u + 1u] = std::clamp(scratch_r_[i], -1.0f, 1.0f);
    }
}

void Engine::apply_event(const Event& event, const Params& params) {
    switch (event.type) {
        case EventType::kNoteOn:
            // Real devices send note-on with velocity 0 for a release, so honour it instead of
            // starting a note that never ends.
            if (event.velocity == 0) {
                allocator_.note_off(event.channel, note_for_key(event.note, params), params,
                                    position_, pedal_down_);
                start_release(event.note, event.channel, params);
            } else {
                note_on(event, params);
            }
            break;
        case EventType::kNoteOff:
            allocator_.note_off(event.channel, note_for_key(event.note, params), params, position_,
                                pedal_down_);
            start_release(event.note, event.channel, params);
            break;
        case EventType::kControlChange:
            if (event.note == kCcChannelVolume) {
                // Per channel and not global, because a general MIDI file sets its own drum channel
                // quiet and a shared fader would take the piano down with it.
                channel_gain_[event.channel % kMidiChannels] =
                    static_cast<float>(event.velocity) * (1.0f / 127.0f);
            } else if (event.note == kCcSustainPedal) {
                const bool down = event.velocity >= 64;
                if (down != pedal_down_) {
                    pedal_down_ = down;
                    allocator_.pedal(down, params, position_);
                }
            } else if (event.note == kCcAllSoundOff || event.note == kCcAllNotesOff) {
                allocator_.panic(event.channel, position_, params);
                // Same ramp, same promise: a release recording is not exempt from all-sound-off,
                // because a hammer noise that survives the panic button is a stuck note with a
                // nicer name.
                fade_releases(params);
            }
            // Every other controller is accepted and ignored until the phase that gives it a
            // meaning.
            break;
        case EventType::kPitchBend:
            pitch_bend(event, params);
            break;
        case EventType::kAllNotesOff:
            // Global on purpose: this is the panic button. Per-channel all-notes-off arrives as the
            // CC above, which is what a MIDI cable actually carries.
            allocator_.panic(-1, position_, params);
            fade_releases(params);
            break;
    }
}

void Engine::note_on(const Event& event, const Params& params) {
    // Remember how hard the key was hit, because the note-off that follows it arrives as velocity 0
    // and the release recording has to be chosen by the strike and not by the let-go.
    strike_[static_cast<std::size_t>(std::clamp<int>(event.note, 0, 127))] = event.velocity;
    NoteRequest request;
    request.channel = event.channel;
    request.key = event.note;
    request.note = note_for_key(event.note, params);
    request.velocity = static_cast<float>(event.velocity) * (1.0f / 127.0f);
    request.bend_semitones = bend_[event.channel % kMidiChannels];
    request.frequency =
        tuning_.frequency(request.note) * bend_factor(bend_[event.channel % kMidiChannels]);
    allocator_.allocate(request, params, sample_rate_, position_);
}

void Engine::start_release(int key, std::uint8_t channel, const Params& params) {
    if (instrument_ == nullptr)
        return; // the synthesiser has nothing to let go with, so there is no pool to look in
    const int index = std::clamp<int>(key, 0, 127);

    NoteRequest request;
    request.channel = channel;
    request.key = key;
    request.note = note_for_key(key, params);
    // The level and the recording come from the strike, so a pianissimo lets go quietly and a
    // fortissimo lets go with the hammer noise in it - which is the whole reason the opcode exists.
    request.velocity = static_cast<float>(strike_[static_cast<std::size_t>(index)]) * (1.0f / 127.0f);
    request.bend_semitones = bend_[channel % kMidiChannels];
    request.frequency = tuning_.frequency(request.note) * bend_factor(request.bend_semitones);

    SampledVoice* slot = nullptr;
    for (SampledVoice& voice : releases_) {
        if (!voice.active()) {
            slot = &voice;
            break;
        }
    }
    if (slot == nullptr) {
        // All eight are still sounding, which is a glissando. The oldest lets go: the newest key-up
        // is the one the ear is still waiting for, and this is the same rule the 16 use, which is
        // why a release pool overflowing is a quieter event than a note pool overflowing.
        slot = &releases_.front();
        for (SampledVoice& voice : releases_) {
            if (voice.start_tick() < slot->start_tick())
                slot = &voice;
        }
    }
    // A library with no release recordings answers nullptr, the voice goes straight back to idle,
    // and this costs what an idle slot costs: one lookup per note-off.
    slot->start(request, params, sample_rate_, position_);
}

void Engine::fade_releases(const Params& params) {
    for (SampledVoice& voice : releases_)
        voice.release(params, position_); // a no-op unless it is sounding
}

void Engine::pitch_bend(const Event& event, const Params& params) {
    const std::uint8_t channel = event.channel % kMidiChannels;
    const float semitones = event.bend_normalized() * params.bend_range_semitones;
    bend_[channel] = semitones;
    const float factor = bend_factor(semitones);
    // Notes already sounding follow the wheel; the frequency is still the LUT value times this.
    for (int i = 0; i < allocator_.pool_size(); ++i) {
        Voice& voice = allocator_.voice(i);
        if (!voice.active() || voice.channel() != channel)
            continue;
        voice.set_frequency(tuning_.frequency(voice.note()) * factor, params);
    }
}

} // namespace boreaz::engine
