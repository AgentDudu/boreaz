#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "boreaz/engine/instrument.h"
#include "boreaz/engine/note.h"
#include "boreaz/engine/params.h"
#include "boreaz/engine/voice.h"

namespace boreaz::engine {

// Fixed pool of voices plus the rules for who gets one.
//
// The pool never grows: a voice is a fixed amount of work, and a synthesizer that silently
// doubles its cost when you play more keys is a synthesizer that crackles on other people's
// machines (plan 6). When it is full, something gets stolen.
class VoiceAllocator {
  public:
    // The 16-voice pool from the plan.
    static constexpr int kDefaultPoolSize = 16;

    // An instrument makes the pool out of sampled voices; no instrument makes it out of the synth.
    // One kind per pool, chosen before the audio thread starts - see `reallocate`.
    explicit VoiceAllocator(int pool_size = kDefaultPoolSize, const Instrument* instrument = nullptr);

    // Start a note, taking a free voice or stealing one. Returns the voice index.
    int allocate(const NoteRequest& request, const Params& params, double sample_rate,
                 std::int64_t tick);

    // The key went up. With the pedal down the note keeps sounding but stops counting as held
    // (Voice::silence); without it, it releases.
    void note_off(std::uint8_t channel, int note, const Params& params, std::int64_t tick,
                  bool pedal_down);

    // Pedal up: everything the pedal was holding on to releases now.
    void pedal(bool down, const Params& params, std::int64_t tick);

    // Panic. `channel` is -1 for every channel. Releases immediately, whatever the pedal is doing.
    void panic(int channel, std::int64_t tick, const Params& params);

    // Per-block parameter refresh (filter, envelope times, pan).
    void refresh(const Params& params, bool force);

    void render(float* left, float* right, int frames);

    // Only for resizing the pool in a test, and only before anything sounds: it drops the voices
    // that no longer fit. The instrument is passed again because it decides what kind of voice a
    // pool is made of, and a pool does not change species halfway through a performance.
    // Passing no instrument keeps the pool's existing kind rather than defaulting it back to the
    // synth, which is what makes `reallocate(4)` in a test resize a sampled pool.
    void reallocate(int pool_size, const Instrument* instrument = nullptr);

    int pool_size() const { return static_cast<int>(voices_.size()); }
    int active_count() const;
    int steal_count() const { return steal_count_; }
    // Only the engine writes here; the const view is what telemetry and tests read.
    const Voice& voice(int index) const { return *voices_[static_cast<std::size_t>(index)]; }
    Voice& voice(int index) { return *voices_[static_cast<std::size_t>(index)]; }

  private:
    // Stealing order: an already-released voice, then one the pedal is holding, then a held key,
    // and within those the oldest, ties broken by the quietest.
    static int rank(const Voice& voice) {
        if (voice.released())
            return 0;
        if (voice.sustained())
            return 1;
        return 2;
    }

    // Owned by the pool and fixed at construction: a voice holds no heap of its own, but the pool
    // has to hold two different kinds behind one interface, and it may not slice either of them.
    std::vector<std::unique_ptr<Voice>> voices_;
    const Instrument* instrument_ = nullptr;
    int steal_count_ = 0;
};

} // namespace boreaz::engine
