#include "boreaz/samples/sample_store.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

namespace boreaz::samples {
namespace {

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string fourcc(const std::uint8_t* p) {
    return std::string(reinterpret_cast<const char*>(p), 4);
}

// A name=value helper for the error strings, so "unsupported format" says which four bytes.
std::string describe_format(std::uint16_t tag, std::uint16_t bits) {
    std::string text = "format " + std::to_string(tag) + ", " + std::to_string(bits) + "-bit";
    return text;
}

bool read_whole_file(const std::string& path, std::vector<std::uint8_t>& out, std::string& error) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        error = "cannot open " + path;
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        error = path + " is empty or not a file";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(out.data()), size);
    if (!file) {
        error = "cannot read " + path;
        return false;
    }
    return true;
}

// One sample of one channel, widened to float. 8-bit PCM is unsigned, everything else is two's
// complement, and 24-bit arrives as three bytes that have to be sign-extended by hand.
float to_float(const std::uint8_t* at, std::uint16_t bits, std::uint16_t format) {
    if (format == 3 && bits == 32) {
        float value = 0.0f;
        std::memcpy(&value, at, 4);
        return value;
    }
    if (format == 3 && bits == 64) {
        double value = 0.0;
        std::memcpy(&value, at, 8);
        return static_cast<float>(value);
    }
    switch (bits) {
        case 8:
            return (static_cast<float>(at[0]) - 128.0f) / 128.0f;
        case 16: {
            const auto value = static_cast<std::int16_t>(le16(at));
            return static_cast<float>(value) / 32768.0f;
        }
        case 24: {
            std::int32_t value = static_cast<std::int32_t>(at[0]) |
                                (static_cast<std::int32_t>(at[1]) << 8) |
                                (static_cast<std::int32_t>(at[2]) << 16);
            if (value & 0x800000)
                value |= ~0x7FFFFF; // sign extend the missing byte
            return static_cast<float>(value) / 8388608.0f;
        }
        case 32: {
            const auto value = static_cast<std::int32_t>(le32(at));
            return static_cast<float>(value) / 2147483648.0f;
        }
        default:
            return 0.0f;
    }
}

} // namespace

bool decode_wav(const std::uint8_t* bytes, std::size_t size, Pcm& out, std::string& error) {
    out = Pcm{};
    error.clear();
    if (size < 12) {
        error = "too short to be a RIFF file";
        return false;
    }
    if (fourcc(bytes) != "RIFF" || fourcc(bytes + 8) != "WAVE") {
        error = "not a RIFF/WAVE file: starts with `" + fourcc(bytes) + "`, `" + fourcc(bytes + 8) +
                "` (a WAV is what this library reads, so run it through mkinst first)";
        return false;
    }

    std::uint16_t format = 0;
    std::uint16_t bits = 0;
    std::uint16_t channels = 0;
    double rate = 0.0;
    const std::uint8_t* data = nullptr;
    std::size_t data_size = 0;
    bool have_fmt = false;

    std::size_t at = 12;
    while (at + 8 <= size) {
        const std::string id = fourcc(bytes + at);
        const std::uint32_t chunk = le32(bytes + at + 4);
        const std::size_t body = at + 8;
        // A chunk may declare more than the file holds; reading to `size` instead of trusting it
        // is what makes a truncated file an error rather than a segfault.
        const std::size_t available = std::min<std::size_t>(chunk, size >= body ? size - body : 0);

        if (id == "fmt " && available >= 16) {
            format = le16(bytes + body);
            channels = le16(bytes + body + 2);
            rate = static_cast<double>(le32(bytes + body + 4));
            bits = le16(bytes + body + 14);
            if (format == 0xFFFE && available >= 24)
                format = le16(bytes + body + 24); // extensible: the real tag is in the subformat
            have_fmt = true;
        } else if (id == "data") {
            data = bytes + body;
            // A WAV written by a streaming encoder says 0 or 0xFFFFFFFF for the data size, and the
            // rest of the file *is* the data. `available` is already capped by the file's length,
            // so a declared size nobody honoured cannot read past the buffer.
            data_size =
                (chunk == 0u || chunk == 0xFFFFFFFFu) ? (size - body) : available;
        } else if (id == "smpl" && available >= 44) {
            // The first sampler loop, which is where a file says where its own loop is. The loops
            // live after the header and after however many cue points the file has, so the offset
            // is arithmetic and not a constant - assuming 44 is what breaks a file with a cue in it.
            const std::uint32_t loops = le32(bytes + body + 28);
            const std::uint32_t cues = le32(bytes + body + 40);
            const std::size_t record = body + 44 + static_cast<std::size_t>(cues) * 24u;
            if (loops > 0 && record + 24 <= body + available) {
                out.smpl_loop_start = static_cast<std::int64_t>(le32(bytes + record + 8));
                out.smpl_loop_end = static_cast<std::int64_t>(le32(bytes + record + 12));
            }
        }
        // Chunks are word aligned; a one-byte pad after an odd-sized chunk is not a new chunk.
        at = body + available + (available & 1u);
    }

    if (!have_fmt) {
        error = "no fmt chunk";
        return false;
    }
    // The format is answered before the data chunk: "I do not decode ADPCM" is the truth about a
    // compressed file, and "no data chunk" is an accident of how this one happened to be written.
    if (channels == 0 || channels > 8) {
        error = std::to_string(channels) + " channels";
        return false;
    }
    if (!(format == 1 || format == 3)) {
        error = describe_format(format, bits) + ": only PCM and IEEE float are decoded";
        return false;
    }
    const bool supported_bits =
        (format == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) ||
        (format == 3 && (bits == 32 || bits == 64));
    if (!supported_bits) {
        error = describe_format(format, bits) + ": not a bit depth this decoder knows";
        return false;
    }
    if (rate <= 0.0) {
        error = "sample rate is not a number of hertz";
        return false;
    }
    if (data == nullptr || data_size == 0) {
        error = "no data chunk";
        return false;
    }

    const std::size_t bytes_per_sample = bits / 8u;
    const std::size_t align = static_cast<std::size_t>(channels) * bytes_per_sample;
    const std::size_t frames = data_size / align;
    if (frames == 0) {
        error = "the data chunk holds no whole frame";
        return false;
    }

    out.channels = channels;
    out.sample_rate = rate;
    out.frames = static_cast<std::int64_t>(frames);
    out.data.resize(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::uint8_t* at_sample = data + frame * align;
        for (int channel = 0; channel < channels; ++channel) {
            out.data[frame * channels + static_cast<std::size_t>(channel)] =
                to_float(at_sample + static_cast<std::size_t>(channel) * bytes_per_sample, bits,
                         format);
        }
    }
    return true;
}

bool load_wav(const std::string& path, Pcm& out, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!read_whole_file(path, bytes, error))
        return false;
    if (!decode_wav(bytes.data(), bytes.size(), out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

namespace {

// A Windows separator, in hex, because every quoting layer between here and a compiler handles a
// backslash character literal slightly differently and this one has to work in all of them.
constexpr char kBackslash = 0x5C;

std::string join(const std::string& directory, const std::string& file) {
    if (directory.empty())
        return file;
    const char last = directory.back();
    if (last == '/' || last == kBackslash)
        return directory + file;
    return directory + "/" + file;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string directory_of(const std::string& path) {
    // The last separator of either kind. `std::max` on the two `find_last_of` results is wrong
    // here and looks right: "not found" is the largest size_t there is, so a path with only
    // backslashes would come back with no directory at all and every sample would be looked for in
    // the working directory.
    std::size_t cut = std::string::npos;
    for (const char separator : {'/', kBackslash}) {
        const std::size_t at = path.find_last_of(separator);
        if (at != std::string::npos && (cut == std::string::npos || at > cut))
            cut = at;
    }
    if (cut == std::string::npos)
        return {}; // the mapping is in the working directory, like a bare name on the command line
    return path.substr(0, cut + 1);
}

// dB and cents to multipliers. Once per region at load, so `pow` is free here and stays out of the
// callback, which is the only reason the plan allows it.
double db_to_gain(double decibels) {
    if (!std::isfinite(decibels))
        return 1.0;
    return std::pow(10.0, decibels / 20.0);
}

} // namespace

LoadResult SampleStore::load_mapping(const std::string& path, std::string root_dir) {
    LoadResult result;
    boreaz::sfz::LoadResult mapping = boreaz::sfz::load_file(path);
    result.warnings = mapping.warnings;
    if (!mapping.ok) {
        result.error = path + ": " + mapping.error;
        return result;
    }
    if (root_dir.empty())
        root_dir = directory_of(path);
    if (!mapping.instrument.default_path.empty()) {
        // The `<control>` directory is relative to the mapping, and joining it here means every
        // later path resolution has one rule instead of two.
        root_dir = join(root_dir, mapping.instrument.default_path);
        if (!root_dir.empty() && root_dir.back() != '/' && root_dir.back() != kBackslash)
            root_dir += "/";
    }
    LoadResult filled = load(mapping.instrument, root_dir);
    // The mapping's warnings came first and the store's follow, so the console says "the file is
    // fine, and here is what could not be played" in that order.
    filled.warnings.insert(filled.warnings.begin(), result.warnings.begin(), result.warnings.end());
    return filled;
}

LoadResult SampleStore::load(const boreaz::sfz::Instrument& mapping, const std::string& root_dir) {
    LoadResult result;
    mapping_ = mapping;
    entries_.clear();
    releases_.clear();
    files_.clear();
    bytes_ = 0;
    refused_ = 0;

    const auto started = std::chrono::steady_clock::now();

    // Half an instrument is worse than no instrument: the keys that loaded would play at the right
    // pitch while the ones that did not are simply gone, and nothing would say so. A failed load
    // leaves the store exactly as it was before it.
    auto fail = [this, &result](const std::string& message) {
        entries_.clear();
        releases_.clear();
        files_.clear();
        mapping_ = boreaz::sfz::Instrument{};
        bytes_ = 0;
        refused_ = 0;
        result.error = message;
        return result;
    };

    // Which velocities each key has a recording for: two words per key, 128 keys, 2 KB on a load
    // path that already holds 100 MB of audio. It buys one true sentence about velocity layers -
    // whether the layers cover the keyboard or leave velocities nothing was recorded for - which a
    // count of regions cannot tell, because a 66 region two layer instrument that covers everything
    // and a one layer instrument with a band on it are not the same problem.
    struct Coverage {
        std::uint64_t low = 0;  // velocities 0-63
        std::uint64_t high = 0; // velocities 64-127
        void set(int from, int to) {
            for (int velocity = from; velocity <= to; ++velocity)
                if (velocity < 64)
                    low |= std::uint64_t{1} << velocity;
                else
                    high |= std::uint64_t{1} << (velocity - 64);
        }
        bool whole_range() const { return low == ~std::uint64_t{0} && high == ~std::uint64_t{0}; }
        bool empty() const { return (low | high) == 0; }
    };
    std::array<Coverage, 128> covered;
    for (const boreaz::sfz::Region& region : mapping_.regions) {
        // A `trigger=release` region is decoded and held exactly like a note region; what differs is
        // which list it lands in, and that it does not claim velocity coverage. It is not a place to
        // start a note, so `find()` cannot answer with it and `find_release()` cannot answer with a
        // note - the two lookups are separate precisely because the recordings are different things.
        const bool release = region.release_trigger;

        const std::string path = join(root_dir, region.sample);
        std::size_t file_index = 0;
        bool found = false;
        for (std::size_t i = 0; i < files_.size(); ++i) {
            if (lower(files_[i].path) == lower(path)) {
                file_index = i;
                found = true;
                break;
            }
        }
        if (!found) {
            if (bytes_ >= kMaxResidentBytes) {
                return fail("refusing to load more than " +
                            std::to_string(kMaxResidentBytes / (1024u * 1024u)) +
                            " MB of samples; " + path + " would be next");
            }
            File file;
            file.path = path;
            std::string error;
            if (!load_wav(path, file.pcm, error)) {
                // A missing recording is a broken instrument, not a silent key.
                return fail(error);
            }
            bytes_ += file.pcm.data.size() * sizeof(float);
            ++result.files;
            files_.push_back(std::move(file));
            file_index = files_.size() - 1;
        }
        // `deque`, so the buffer a region points at cannot move when the next one is read.
        const File& file = files_[file_index];
        const Pcm& pcm = file.pcm;

        Entry entry;
        entry.sample.data = pcm.data.data();
        entry.sample.channels = pcm.channels;
        entry.sample.frames = pcm.frames;
        entry.sample.file_sample_rate = pcm.sample_rate;
        entry.sample.key_center = region.key_center;
        entry.sample.gain = static_cast<float>(db_to_gain(region.gain_db));
        entry.sample.tune_cents = region.tune_cents;
        entry.sample.velocity_tracking = std::clamp(region.velocity_tracking / 100.0f, 0.0f, 1.0f);

        // `offset`: seconds below 100, samples at or above it, which is the SFZ rule.
        double offset = region.offset;
        if (offset < 0.0)
            offset = 0.0;
        const double offset_frames = offset < 100.0 ? offset * pcm.sample_rate : offset;
        std::int64_t start = static_cast<std::int64_t>(offset_frames + 0.5);
        const std::int64_t last = pcm.frames > 0 ? pcm.frames - 1 : 0;
        if (start > last)
            start = last; // an offset past the end plays the last frame, and never reads past it
        entry.sample.start_frame = start;

        // Loop points, in whichever unit the mapping spelled them, then sanity: a loop whose ends
        // are the wrong way round is refused, because a voice that wraps backwards is a voice that
        // reads the file in reverse at an arbitrary speed.
        std::int64_t loop_start = -1;
        std::int64_t loop_end = -1;
        if (region.loop == boreaz::sfz::Loop::kContinuous && region.loop_start >= 0.0 &&
            region.loop_end >= 0.0) {
            const double scale = region.loop_in_seconds ? pcm.sample_rate : 1.0;
            loop_start = static_cast<std::int64_t>(region.loop_start * scale);
            loop_end = static_cast<std::int64_t>(region.loop_end * scale);
            if (loop_end <= loop_start) {
                result.warnings.push_back("region " + region.sample +
                                          " has loop points the wrong way round and is played once");
                loop_start = loop_end = -1;
            }
        } else if (region.loop == boreaz::sfz::Loop::kContinuous && pcm.smpl_loop_end > pcm.smpl_loop_start) {
            loop_start = pcm.smpl_loop_start; // the file says where its own loop is
            loop_end = pcm.smpl_loop_end;
        }
        if (loop_start >= 0 && loop_end > loop_start && loop_end < pcm.frames) {
            entry.sample.loops = true;
            entry.sample.loop_start = loop_start;
            entry.sample.loop_end = loop_end;
        }
        if (region.loop == boreaz::sfz::Loop::kSustain) {
            result.warnings.push_back("region " + region.sample +
                                      " asks for loop_mode=sustain and is played once");
        }
        if (release) {
            // One shot, whatever the file or the mapping says: a key being let go happens once.
            entry.sample.loops = false;
            entry.sample.loop_start = 0;
            entry.sample.loop_end = 0;
        }

        entry.key = region.key_center;
        entry.lo_key = region.lo_key;
        entry.hi_key = region.hi_key;
        entry.lo_velocity = region.lo_velocity;
        entry.hi_velocity = region.hi_velocity;
        if (!release) {
            for (int key = std::max(region.lo_key, 0); key <= std::min(region.hi_key, 127); ++key)
                covered[static_cast<std::size_t>(key)].set(region.lo_velocity, region.hi_velocity);
        }
        (release ? releases_ : entries_).push_back(std::move(entry));
        ++(release ? result.releases : result.regions);
    }

    std::size_t gapped = 0;
    for (const Coverage& key : covered)
        if (!key.empty() && !key.whole_range())
            ++gapped;
    if (gapped != 0) {
        // The recordings are all loaded; what is missing is a choice, and a velocity no layer covers
        // plays whichever recording the mapping's own precedence put last. That is a missing feature
        // that would otherwise arrive as a bad piano.
        result.warnings.push_back(
            std::to_string(gapped) +
            " keys have a velocity layer that does not cover the whole range; a velocity outside it "
            "plays whichever recording the mapping put last");
    }

    if (entries_.empty())
        return fail("no region in this mapping can be played as a note");

    result.ok = true;
    result.refused = refused_;
    result.bytes = bytes_;
    result.load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                               started)
                        .count();
    return result;
}

const boreaz::engine::VoiceSample* SampleStore::lookup(const std::deque<Entry>& list, int key,
                                                       int velocity, int low, int high) {
    if (list.empty())
        return nullptr;
    // Outside the span the caller names: clamp to the edge recording. The plan wants those keys to
    // sound wrong rather than silent, and a recording shifted by a few semitones is that.
    const int clamped = std::clamp(key, low, high);

    for (std::size_t i = list.size(); i > 0; --i) {
        const Entry& entry = list[i - 1];
        if (clamped >= entry.lo_key && clamped <= entry.hi_key && velocity >= entry.lo_velocity &&
            velocity <= entry.hi_velocity) {
            return &entry.sample;
        }
    }
    // Nothing in this velocity band. The key's own recording, at whatever velocity the mapping
    // happens to hold, is a better answer than silence: a piano with no soft layer is still a piano,
    // and a piano that only plays at velocity 100 is not.
    for (std::size_t i = list.size(); i > 0; --i) {
        const Entry& entry = list[i - 1];
        if (clamped >= entry.lo_key && clamped <= entry.hi_key)
            return &entry.sample;
    }
    // A key the mapping leaves out altogether - the shape of a library put together from two
    // releases, where C0 to A0 has recordings and the keys below it do not. The nearest recording,
    // played at the pitch the key asked for, is the same piano; silence in the bottom octave reads
    // as a broken instrument and gets blamed on the synth.
    const Entry* nearest = nullptr;
    int best = 0;
    for (const Entry& entry : list) {
        const int distance = key < entry.lo_key      ? entry.lo_key - key
                             : key > entry.hi_key    ? key - entry.hi_key
                                                     : 0;
        // Ties go to the later region, which is the precedence rule the mapping itself follows.
        if (nearest == nullptr || distance <= best) {
            nearest = &entry;
            best = distance;
        }
    }
    return nearest == nullptr ? nullptr : &nearest->sample;
}

const boreaz::engine::VoiceSample* SampleStore::find(int key, int velocity) const {
    return lookup(entries_, key, velocity, mapping_.low_key, mapping_.high_key);
}

const boreaz::engine::VoiceSample* SampleStore::find_release(int key, int velocity) const {
    // No clamp to the mapping's span: that span describes what the *attacks* cover, which is often
    // wider than a release library (chromatic hammer noise over the middle octave is one shape real
    // libraries ship). The nearest-key scan already answers an uncovered key with the edge release.
    return lookup(releases_, key, velocity, 0, 127);
}

} // namespace boreaz::samples
