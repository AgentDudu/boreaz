#pragma once

#include <cstdint>

#include "boreaz/engine/note.h"
#include "boreaz/engine/instrument.h"
#include "boreaz/engine/params.h"

namespace boreaz::engine {

// One sounding note, however it is made.
//
// This is the seam phase 6 was for: the oscillator set, the sample read-head, and anything after
// them are interchangeable behind these eleven calls. The pool, the stealing rules, the telemetry
// and the display all talk to this and none of them can tell a recording from three detuned
// oscillators - which is the whole point of holding the two sound sources to one interface
// (PLAN.md §2), that switching one for the other is a decision and not a rewrite.
//
// Real-time contract, binding on every implementation: no allocation, no I/O, nothing that can
// fail, nothing that branches on a file format. Every buffer is a member.
class Voice {
  public:
    virtual ~Voice() = default;
    Voice(const Voice&) = delete;
    Voice& operator=(const Voice&) = delete;

    // A note starts from clean state: voices are reused, and the residue of the previous note -
    // a filter memory, or the middle of a recording - would sound on top of the new one.
    // `request.frequency` is final; the engine has already applied transpose, octave and bend.
    virtual void start(const NoteRequest& request, const Params& params, double sample_rate,
                       std::int64_t start_tick) = 0;

    virtual void release(const Params& params, std::int64_t tick) = 0;

    // The key went up while the sustain pedal was down: stop being a held note, keep sounding, and
    // become the first candidate when the pool needs a voice.
    virtual void silence(std::int64_t tick) = 0;

    // Refresh the per-note constants after a parameter change. `force` is for a voice that has
    // just been reactivated and has to take the current block's parameters whatever happens.
    virtual void refresh(const Params& params, bool force) = 0;

    // Re-pitch a sounding voice (pitch bend). Phase is kept, so it glides instead of clicking.
    virtual void set_frequency(float hertz, const Params& params) = 0;

    // Adds into `left`/`right`; the caller owns the buffer and the mix.
    virtual void render(float* left, float* right, int frames) = 0;

    virtual bool active() const = 0;
    virtual bool released() const = 0;
    // Held by the sustain pedal after its key went up: still audible, but first in line to steal.
    virtual bool sustained() const = 0;

    virtual int note() const = 0;
    virtual int key() const = 0;
    virtual int channel() const = 0;
    virtual float velocity() const = 0;
    virtual float frequency() const = 0;
    virtual std::int64_t start_tick() const = 0;
    virtual std::int64_t release_tick() const = 0;

    // How long the voice has been sounding, in samples. Built on `start_tick`, so every kind of
    // voice is measured the same way and the stealing rules cannot drift between them.
    std::int64_t age(std::int64_t tick) const { return tick - start_tick(); }

    // Where the amplitude envelope is, for the display tap. Reading it does not advance anything.
    virtual float amplitude() const = 0;

    // How hard the note was struck, as a gain. The allocator's tie-break: two notes that began on
    // the same sample both have an envelope of zero until a block has rendered, so the strike is
    // the only honest measure of which one is the cheaper theft.
    virtual float strike() const = 0;

    // The recording this voice is reading, or nullptr for a voice that reads none. The probe wants
    // to say which recording a key is playing, and a test wants to know what kind of voice the pool
    // was built with; neither may have to ask the pool, and neither may reach into a subclass.
    virtual const VoiceSample* sample() const { return nullptr; }

  protected:
    Voice() = default;
};

} // namespace boreaz::engine
