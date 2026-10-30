#pragma once

#include <cstddef>
#include <cstdint>

namespace boreaz::engine {

// One recording of part of an instrument, already decoded and resident.
//
// This is the whole of what the engine knows about samples: a pointer into a buffer somebody else
// decoded, where its loop is, which key it is already in tune with, and how much to turn it up.
// It is an engine type and it includes nothing, which is what lets the sampled voice live in the
// audio core while the thing that reads `.sfz` files and decodes WAVs stays out of it - the hygiene
// test refuses an `engine/` file that includes `samples/` or `sfz/`, on purpose.
struct VoiceSample {
    const float* data = nullptr; // interleaved by `channels`, in the file's own sample rate
    int channels = 1;
    std::int64_t frames = 0;

    // Frame indices, both or neither, and only meaningful when `loops` is true. Outside the loop
    // the voice runs to `frames` and then behaves according to `loops`.
    bool loops = false;
    std::int64_t loop_start = 0;
    std::int64_t loop_end = 0;

    double file_sample_rate = 44100.0; // so a 44.1 kHz instrument on a 48 kHz device is in tune
    int key_center = 60;              // the MIDI key this recording already matches
    std::int64_t start_frame = 0;     // the first frame to play, from `offset`

    float gain = 1.0f;   // linear, from `volume`
    float tune_cents = 0.0f;
    float velocity_tracking = 1.0f; // 1 = the full velocity curve, 0 = no dynamics at all
};

// What a sampled voice asks an instrument: "what do I play for this key at this velocity?".
//
// `boreaz::samples::SampleStore` implements it by reading files. A test implements it by holding a
// handful of sine buffers, which is how the sampled voice is tested with no audio in git and no
// download in CI. The engine never opens a file, so nothing here can delay an audio callback.
class Instrument {
  public:
    virtual ~Instrument() = default;

    // The recording to play, or nullptr when the instrument is empty. `key` and `velocity` are
    // MIDI numbers 0-127. A key outside the instrument's own span is clamped to the nearest edge
    // recording rather than answered with nullptr: an acoustic piano has no such key, and the plan
    // wants that stated by sounding wrong, not by going silent.
    virtual const VoiceSample* find(int key, int velocity) const = 0;

    virtual bool empty() const = 0;
    // What it cost to load, for the startup line that prints it. Called from the setup path only.
    virtual std::size_t resident_bytes() const = 0;
    // How many regions were refused because they cannot be played (a release sample, say). Zero is
    // the normal answer; anything else belongs on the console at load.
    virtual std::size_t refused_regions() const = 0;
};

} // namespace boreaz::engine
