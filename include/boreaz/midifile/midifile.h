#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "boreaz/engine/event.h"

namespace boreaz::engine {
class Engine;
} // namespace boreaz::engine

namespace boreaz::midifile {

// Reading a Standard MIDI File, in service of one thing: turning it into engine events at audio
// sample positions. `engine` stays ignorant of files and `midifile` stays ignorant of devices,
// which is what lets the offline renderer, the in-app sequencer and a unit test all ask the same
// question.
//
// This is a *declared subset*, the same doctrine as the SFZ reader (PLAN.md §4 phase 6): what we
// understand is listed below, what we do not is refused with a message that says which track and
// where, never quietly skipped. A file that plays "almost right" is the worst outcome, because
// nobody reports it.
//
// Understood: format 0 and 1, division in ticks per quarter note, delta time as variable length,
// running status, note on/off (including the "note on with velocity 0 is a note off" rule), poly
// aftertouch, control change, program change, channel pressure, pitch bend, system exclusive,
// the tempo meta event, the time signature meta event (parsed, reported, and not used: we have
// no bar-relative behaviour yet and pretending to would be a lie), marker/cue point/text meta
// events (parsed and ignored), and end-of-track.
//
// Refused: SMPTE frame division (the negative division form), formats 2 and 3, a track that ends
// without end-of-track, a delta that overflows 28 bits, and any event whose data bytes are
// missing because the file was truncated.

// Tempo map, ascending by tick. Kept public because the GUI playhead and the phase-10 transport
// both need tick<->time, and re-reading the file to get it would be silly.
struct TempoPoint {
    std::int64_t tick = 0;
    double microseconds_per_quarter = 500000.0; // MIDI's default tempo: 120 bpm
};

struct Song {
    int format = 0;
    int ticks_per_quarter = 0;
    double sample_rate = 0.0;

    std::vector<TempoPoint> tempos;
    // Ascending by sample position. Two events landing on the same sample keep the order the
    // tracks were read in, which is what makes a render reproducible.
    std::vector<boreaz::engine::Event> events;

    // One second past the last event: long enough for the last release to be heard, which is the
    // difference between a render that ends and one that is cut off.
    std::int64_t end_sample = 0;

    // Reported, not acted on. Empty for a file with no tempo meta event, which means 120 bpm.
    std::vector<std::string> time_signatures;

    // Position of `tick` in samples, using the tempo map. Cheap and monotonic, so a caller with
    // an ascending cursor can walk `tempos` instead of searching it.
    std::int64_t tick_to_sample(std::int64_t tick) const;
    // The inverse, for a playhead that knows where it is in the output and wants a bar:tempo.
    std::int64_t sample_to_tick(std::int64_t sample) const;
};

struct LoadResult {
    bool ok = false;
    // Empty unless ok. One line, sentence case, no trailing newline: it goes straight to stderr.
    std::string error;
    // Things we skipped but could play around - an unknown text event is not a reason to stop.
    std::vector<std::string> warnings;
    Song song;
};

// Parse bytes already in memory. Pure: no files, no engine, no global state, and it never throws
// past the buffer it was given. `sample_rate` is only used to turn ticks into samples.
LoadResult parse(const std::uint8_t* bytes, std::size_t size, double sample_rate);

// Read the whole file, then parse it. A file over kMaxFileBytes is refused before it is read:
// these are configuration-sized files, and an attacker-controlled 4 GB "song" should cost
// nothing. A directory, a missing file and an unreadable file all come back as `error`.
LoadResult load_file(const std::string& path, double sample_rate);

inline constexpr std::size_t kMaxFileBytes = 64u * 1024u * 1024u;

// The default tail added to `end_sample`.
inline constexpr double kReleaseTailSeconds = 1.0;

// What a feed reached: where to carry on from, and how many events the engine could not take.
// A caller that ignores `dropped` is a caller that loses notes in silence, which is why this is a
// number the caller has to look at rather than a line this library prints.
struct FeedResult {
    std::size_t cursor = 0;
    std::size_t dropped = 0;
};

// Render `frames` of interleaved stereo while feeding `song` into `engine`.
//
// The feed is incremental because a real file holds far more events than the engine's ring
// (4096) and an overflow drops the note in silence: pushing the whole song first is the classic
// way a long render quietly loses its middle. `cursor` is where to carry on from, so pausing and
// resuming is a matter of keeping the number.
//
// The engine must have been prepared at `song.sample_rate`.
FeedResult feed_and_render(boreaz::engine::Engine& engine, const Song& song, std::size_t cursor,
                           float* output, int frames);

// Total frames a render of `song` needs, which is `song.end_sample`. Kept as a function so the
// caller writes the intent rather than the field.
std::int64_t render_length(const Song& song);

} // namespace boreaz::midifile
