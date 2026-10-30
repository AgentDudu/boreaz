#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "boreaz/engine/instrument.h"
#include "boreaz/sfz/sfz.h"

namespace boreaz::samples {

// Decoded PCM in memory: the sample library's whole job, and the only way anything gets audio into
// the engine. It reads files on the setup path and never on the audio path, so the callback's
// "no I/O, no page faults, nothing that can fail" rule survives contact with a 32 MB instrument.
//
// Everything is held as 32-bit float in the file's own channel count and rate. There is no decoder
// on the audio path (PLAN.md §2), no resampler at load (the voice's read-head rate already carries
// the ratio between the file's rate and the device's, which is both cheaper and more honest than
// throwing resamples away and back again), and no normalisation of a file's own level (a
// recording's dynamics are its velocity layers; evening them out is how a sampled piano ends up
// sounding like one held at one dynamic).

// A canonical or extensible PCM/float WAV file, decoded. Accepting anything else, including
// compressed `wFormatTag` values and anything but RIFF, is refused with the four bytes it found,
// because "unsupported format" and "corrupt file" need different answers from whoever shipped it.
struct Pcm {
    std::vector<float> data; // interleaved
    int channels = 0;
    double sample_rate = 0.0;
    std::int64_t frames = 0;

    // The `smpl` chunk, when present, in frames. A mapping's own loop points win over these; an
    // instrument with only `smpl` loops still loops, which is why they are kept and not skipped.
    std::int64_t smpl_loop_start = -1;
    std::int64_t smpl_loop_end = -1;
};

// Decode bytes already in memory. Never throws, never reads past `size`.
bool decode_wav(const std::uint8_t* bytes, std::size_t size, Pcm& out, std::string& error);
// Read the file, then decode it. A missing file and a directory both come back as `error`.
bool load_wav(const std::string& path, Pcm& out, std::string& error);

struct LoadResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;

    std::size_t regions = 0;   // regions this store can play
    std::size_t refused = 0;   // regions it cannot, and said so about
    std::size_t files = 0;     // distinct sample files read
    std::size_t bytes = 0;     // resident float PCM
    double load_ms = 0.0;
    std::size_t samples_in_error = 0; // files that were unreadable but not fatal - normally zero
};

// A resident instrument: the mapping plus every recording it names, ready for the audio thread.
//
// `load_*` belongs to startup (or a test). After that the object is read-only, which is what lets a
// voice hold a `const engine::Instrument*` and nothing else.
class SampleStore : public boreaz::engine::Instrument {
  public:
    // Read `path` (a `.sfz`), then every file it points at, resolved against `root_dir` - which is
    // the mapping's own directory when the caller passes nothing. Fails if a named file is missing
    // or undecodable: an instrument with silent gaps is the outcome this library exists to prevent.
    LoadResult load_mapping(const std::string& path, std::string root_dir = {});

    // The same, from a mapping the caller already parsed. `root_dir` is where its relative paths
    // start, with a trailing separator or without.
    LoadResult load(const boreaz::sfz::Instrument& mapping, const std::string& root_dir);

    const boreaz::sfz::Instrument& mapping() const { return mapping_; }

    // boreaz::engine::Instrument
    const boreaz::engine::VoiceSample* find(int key, int velocity) const override;
    bool empty() const override { return entries_.empty(); }
    std::size_t resident_bytes() const override { return bytes_; }
    std::size_t refused_regions() const override { return refused_; }

    // The whole resident budget. An SFZ that names 8 GB of WAVs is a mistake, and a mistake that
    // pages the disk during a recital is the one failure mode a synthesizer is not allowed.
    static constexpr std::size_t kMaxResidentBytes = 1024u * 1024u * 1024u;

  private:
    // `deque` and not `vector`: a VoiceSample points into `pcm`, and a deque does not move what it
    // holds when the next entry arrives.
    struct Entry {
        boreaz::engine::VoiceSample sample; // `data` points into `files_`, which never moves
        int key = 60;
        int lo_velocity = 0;
        int hi_velocity = 127;
        int lo_key = 0;
        int hi_key = 127;
    };

    // One decoded file, shared by every region that names it (a mapping with 88 regions over 28
    // samples is normal, and holding them 88 times is not).
    struct File {
        std::string path;
        Pcm pcm;
    };

    boreaz::sfz::Instrument mapping_;
    std::deque<Entry> entries_;
    std::deque<File> files_;
    std::size_t bytes_ = 0;
    std::size_t refused_ = 0;
};

} // namespace boreaz::samples
