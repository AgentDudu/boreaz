#include "boreaz/midifile/midifile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

#include "boreaz/engine/engine.h"
#include "boreaz/engine/message.h"

namespace boreaz::midifile {
namespace {

constexpr std::uint8_t kMeta = 0xFF;
constexpr std::uint8_t kMetaTempo = 0x51;
constexpr std::uint8_t kMetaTimeSignature = 0x58;
constexpr std::uint8_t kMetaEndOfTrack = 0x2F;

bool is_text_meta(std::uint8_t type) {
    // Sequence number, the text family, copyright, sequence name, instrument name, lyrics,
    // marker, cue point. Parsed so the bytes are consumed correctly, then dropped: nothing in a
    // synthesizer reads them.
    switch (type) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
            return true;
        default:
            return false;
    }
}

// A cursor over bytes that cannot read past the end. Every read in this file goes through it,
// because the input is a file somebody else wrote and may have truncated on purpose.
class Reader {
  public:
    Reader(const std::uint8_t* bytes, std::size_t size) : bytes_(bytes), size_(size) {}

    std::size_t remaining() const { return size_ - at_; }
    std::size_t position() const { return at_; }

    bool byte(std::uint8_t& out) {
        if (at_ >= size_)
            return false;
        out = bytes_[at_++];
        return true;
    }

    bool u16(std::uint16_t& out) {
        std::uint8_t high = 0;
        std::uint8_t low = 0;
        if (!byte(high) || !byte(low))
            return false;
        out = static_cast<std::uint16_t>((static_cast<int>(high) << 8) | static_cast<int>(low));
        return true;
    }

    bool u32(std::uint32_t& out) {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t byte_value = 0;
            if (!byte(byte_value))
                return false;
            value = (value << 8) | static_cast<std::uint32_t>(byte_value);
        }
        out = value;
        return true;
    }

    // Variable length quantity: one to four bytes, seven bits each. A fifth continuation bit is
    // refused instead of silently truncated, because a 2 GB delta time is a corrupt file and
    // treating it as 128 would play the rest of the song in the wrong place.
    bool quantity(std::int64_t& out) {
        std::int64_t value = 0;
        for (int i = 0; i < 4; ++i) {
            std::uint8_t byte_value = 0;
            if (!byte(byte_value))
                return false;
            value = (value << 7) | static_cast<std::int64_t>(byte_value & 0x7F);
            if ((byte_value & 0x80) == 0) {
                out = value;
                return true;
            }
        }
        return false;
    }

    // Borrow the next `n` bytes without copying. Invalidates nothing; the caller is done with a
    // previous borrow before asking again.
    bool take(std::size_t n, const std::uint8_t*& out) {
        if (remaining() < n)
            return false;
        out = bytes_ + at_;
        at_ += n;
        return true;
    }

    bool skip(std::int64_t n) {
        if (n < 0 || remaining() < static_cast<std::size_t>(n))
            return false;
        at_ += static_cast<std::size_t>(n);
        return true;
    }

  private:
    const std::uint8_t* bytes_ = nullptr;
    std::size_t size_ = 0;
    std::size_t at_ = 0;
};

struct RawEvent {
    std::int64_t tick = 0;
    boreaz::engine::Event event{};
};

std::string locate(int track, std::size_t offset) {
    std::ostringstream text;
    text << " (track " << track << ", byte " << offset << ")";
    return text.str();
}

// One file's worth of parsing state. Kept in the .cpp: nothing outside this translation unit
// needs to know that a parse has a state at all.
class Parser {
  public:
    Parser(Song& song, std::vector<std::string>& warnings) : song_(song), warnings_(warnings) {}

    bool run(Reader& in, std::string& error);

  private:
    bool fail(std::string message) {
        if (error_.empty())
            error_ = std::move(message);
        return false;
    }
    bool fail_at(std::string message, int track, std::size_t offset) {
        return fail(std::move(message) + locate(track, offset));
    }
    // Said once per kind. A file with 400 unknown meta events should report one line, not 400,
    // and a warning nobody reads is the same as silence.
    void warn_once(const std::string& key, const std::string& text) {
        if (said_.insert(key).second)
            warnings_.push_back(text);
    }

    bool header(Reader& in, int& track_count);
    bool track(Reader& in, int index, std::int64_t length);
    bool meta(Reader& in, int index, std::int64_t tick, std::size_t offset, bool& end_of_track);
    bool tempo(Reader& in, int index, std::int64_t tick, std::size_t offset, std::int64_t length);
    bool time_signature(Reader& in, int index, std::int64_t tick, std::size_t offset,
                        std::int64_t length);
    bool channel_event(Reader& in, int index, std::int64_t tick, std::size_t offset,
                       std::uint8_t status, std::int32_t first_byte = -1);

    Song& song_;
    std::vector<std::string>& warnings_;
    std::string error_;
    std::set<std::string> said_;
    std::vector<RawEvent> raw_;
    std::uint64_t sysex_events_ = 0;
};

bool Parser::run(Reader& in, std::string& error) {
    int track_count = 0;
    if (!header(in, track_count)) {
        error = error_;
        return false;
    }

    // The header counts track chunks, not chunks: a file with a RIFF or a "fact" chunk in front of
    // its tracks still has the tracks it promised, and treating every chunk as a track would lose
    // the ones that matter. Running out of bytes early is an error rather than a warning, because
    // the alternative is to play a file that is missing its ending and call that a success.
    constexpr int kMaxChunks = 8192; // junk chunks do not count as tracks, so they need their own bound
    int tracks_read = 0;
    for (int chunks = 0; tracks_read < track_count; ++chunks) {
        if (chunks >= kMaxChunks) {
            warn_once("chunk_many",
                      "gave up after " + std::to_string(kMaxChunks) + " chunks in one MIDI file");
            break;
        }
        const std::uint8_t* id = nullptr;
        std::uint32_t length = 0;
        const std::size_t offset = in.position();
        if (!in.take(4, id) || !in.u32(length)) {
            fail_at("the file ends after " + std::to_string(tracks_read) + " of the " +
                        std::to_string(track_count) + " track chunks it promised", 0, offset);
            // The copy is not ceremony: error_ is the parser's and error is the caller's, and a
            // failure that forgets it exits with a code and no message to show anyone.
            error = error_;
            return false;
        }
        if (id[0] == 'M' && id[1] == 'T' && id[2] == 'r' && id[3] == 'k') {
            if (!track(in, tracks_read, static_cast<std::int64_t>(length))) {
                error = error_;
                return false;
            }
            ++tracks_read;
            continue;
        }
        // The specification allows chunks we do not know. Skipping one is honest as long as we
        // say so, because "it played but lost a third of the notes" is not.
        warn_once("chunk", "skipped an unknown chunk in the MIDI file");
        if (!in.skip(static_cast<std::int64_t>(length))) {
            fail_at("the file ends inside an unknown chunk", 0, offset);
            error = error_;
            return false;
        }
    }

    if (sysex_events_ > 0)
        warn_once("sysex", "ignored " + std::to_string(sysex_events_) +
                               " system exclusive event(s): a synthesizer with no "
                               "patch standard cannot use them");

    // The tempo map has to be complete before any tick becomes a sample, since a tempo event can
    // appear in any track and at any point in the file.
    std::stable_sort(song_.tempos.begin(), song_.tempos.end(),
                     [](const TempoPoint& a, const TempoPoint& b) { return a.tick < b.tick; });

    std::vector<std::int64_t> samples;
    samples.reserve(raw_.size());
    for (const RawEvent& raw : raw_)
        samples.push_back(song_.tick_to_sample(raw.tick));

    // Stable, so events landing on the same sample keep the order the tracks were read in: that
    // is what makes two renders of one file byte identical rather than merely similar.
    std::vector<std::size_t> order(raw_.size());
    for (std::size_t i = 0; i < raw_.size(); ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(),
                     [&samples](std::size_t a, std::size_t b) { return samples[a] < samples[b]; });

    song_.events.reserve(raw_.size());
    for (const std::size_t i : order) {
        boreaz::engine::Event event = raw_[i].event;
        event.tick = samples[i];
        song_.events.push_back(event);
    }

    const std::int64_t tail =
        static_cast<std::int64_t>(kReleaseTailSeconds * song_.sample_rate);
    song_.end_sample = song_.events.empty()
                           ? tail
                           : song_.events.back().tick + tail; // ascending, so the last is the last
    return true;
}

bool Parser::header(Reader& in, int& track_count) {
    const std::uint8_t* id = nullptr;
    std::uint32_t length = 0;
    if (!in.take(4, id))
        return fail("not a MIDI file: too short to hold a header");
    if (!(id[0] == 'M' && id[1] == 'T' && id[2] == 'h' && id[3] == 'd'))
        return fail("not a MIDI file: expected 'MThd' at byte 0");
    if (!in.u32(length) || length != 6)
        return fail("MThd chunk length is " + std::to_string(length) + ", expected 6");

    std::uint16_t format = 0;
    std::uint16_t tracks = 0;
    std::uint16_t division = 0;
    if (!in.u16(format) || !in.u16(tracks) || !in.u16(division))
        return fail("MThd ends mid-header");

    if (format > 1)
        return fail("MIDI format " + std::to_string(format) +
                    " is not supported. Re-export as format 0 or 1: format 2 interleaves parts "
                    "across tracks and nothing here can decide which one is playing");
    if (tracks == 0)
        return fail("the MIDI file declares no tracks");
    if ((division & 0x8000) != 0) {
        const int ticks_per_second = -static_cast<int>(static_cast<std::int16_t>(division));
        return fail(
            "the MIDI file is timed in SMPTE frames (" + std::to_string(ticks_per_second) +
            " ticks per second), which nothing here can play. Re-export it timed in ticks per "
            "quarter note");
    }
    if (division == 0)
        return fail("the MIDI file says 0 ticks per quarter note");

    song_.format = format;
    song_.ticks_per_quarter = static_cast<int>(division);
    track_count = static_cast<int>(tracks);
    return true;
}

bool Parser::track(Reader& in, int index, std::int64_t length) {
    const std::size_t start = in.position();
    std::int64_t tick = 0;
    std::uint8_t running = 0;
    bool ended = false;

    while (static_cast<std::int64_t>(in.position() - start) < length) {
        const std::size_t offset = in.position();
        std::int64_t delta = 0;
        if (!in.quantity(delta))
            return fail_at("the file ends inside a delta time", index, offset);
        if (delta > (1LL << 40))
            return fail_at("a delta time too large to be real, so the file is corrupt", index,
                           offset);
        tick += delta;

        std::uint8_t byte = 0;
        if (!in.byte(byte))
            return fail_at("the file ends inside a track", index, offset);
        std::uint8_t status = byte;

        // With running status the byte just read is not a status at all: it is the message's first
        // data byte, and has to be handed on rather than read twice. Getting that wrong shifts every
        // later event by a byte, which is how one running status note can corrupt a whole track.
        std::int32_t inherited = -1;
        if (status & 0x80u) {
            // 0xF8-0xFE are the real-time messages of a live cable - clock, sensing, machine stop -
            // and have no meaning in a file. 0xFF is not one of them here: in a MIDI file it
            // introduces a meta event, and it is the most common byte above 0xF0 in any file ever
            // written. Refusing it would refuse every real file, which is what this line first did.
            if (status >= 0xF8u && status != kMeta)
                return fail_at("a real-time message cannot appear inside a MIDI file", index,
                               offset);
            // Running status is a channel message affair: a 0xFF or a 0xF0 must not become the
            // status a later stray data byte inherits, which is how a corrupt file turns into a
            // mystery note rather than an error.
            if (status < 0xF0u)
                running = status;
        } else {
            if (running == 0)
                return fail_at(
                    "a data byte appeared where a status byte was expected. Some encoders "
                    "truncate a file mid-message; this one did",
                    index, offset);
            status = running; // running status: the encoder left the status out, not the message
            inherited = static_cast<std::int32_t>(byte); // ...and this byte is the message's first data byte
        }

        if (status == kMeta) {
            if (!meta(in, index, tick, offset, ended))
                return false;
        } else if (status == 0xF0u || status == 0xF7u) {
            std::int64_t size = 0;
            if (!in.quantity(size))
                return fail_at("the file ends inside a system exclusive length", index, offset);
            // A 0xF7 inside a 0xF0 payload ends it, whatever the declared length claimed - the
            // same rule phase 3 learned from a live cable, where the alternative was a parser
            // that swallowed every note after a sysex.
            std::int64_t consumed = 0;
            while (consumed < size) {
                std::uint8_t byte_value = 0;
                if (!in.byte(byte_value))
                    return fail_at("the file ends inside a system exclusive event", index, offset);
                ++consumed;
                if (byte_value == 0xF7u)
                    break;
            }
            ++sysex_events_;
        } else if (status >= 0xF1u) {
            return fail_at("a system common message cannot appear inside a MIDI file", index,
                           offset);
        } else if (!channel_event(in, index, tick, offset, status, inherited)) {
            return false;
        }

        if (static_cast<std::int64_t>(in.position() - start) > length)
            return fail_at("the track's events run past its declared length", index, offset);
    }

    if (!ended && static_cast<std::int64_t>(in.position() - start) == length)
        warn_once("eot", "a track ended without an end of track event, which is tolerated but "
                         "means the file was written by something in a hurry");
    return true;
}

bool Parser::meta(Reader& in, int index, std::int64_t tick, std::size_t offset,
                  bool& end_of_track) {
    std::uint8_t type = 0;
    std::int64_t length = 0;
    if (!in.byte(type) || !in.quantity(length))
        return fail_at("the file ends inside a meta event", index, offset);

    if (type == kMetaEndOfTrack) {
        if (length != 0)
            return fail_at("an end of track event claims data bytes", index, offset);
        end_of_track = true;
        return true;
    }
    if (type == kMetaTempo)
        return tempo(in, index, tick, offset, length);
    if (type == kMetaTimeSignature)
        return time_signature(in, index, tick, offset, length);

    if (!in.skip(length))
        return fail_at("the file ends inside a meta event", index, offset);
    if (!is_text_meta(type))
        warn_once("meta" + std::to_string(type),
                  "ignored meta event 0x" + [&] {
                      char text[8];
                      std::snprintf(text, sizeof(text), "%02X", static_cast<unsigned>(type));
                      return std::string(text);
                  }() + " at tick " + std::to_string(tick));
    return true;
}

bool Parser::tempo(Reader& in, int index, std::int64_t tick, std::size_t offset,
                   std::int64_t length) {
    const std::uint8_t* bytes = nullptr;
    if (length != 3 || !in.take(3, bytes)) {
        if (!in.skip(length))
            return fail_at("the file ends inside a tempo event", index, offset);
        warn_once("tempolength", "ignored a tempo event whose length is not 3 bytes");
        return true;
    }
    const double microseconds = static_cast<double>((static_cast<std::uint32_t>(bytes[0]) << 16) |
                                                    (static_cast<std::uint32_t>(bytes[1]) << 8) |
                                                    static_cast<std::uint32_t>(bytes[2]));
    // 10 microseconds a quarter is 6000 bpm and 100000000 is 0.6 bpm. Outside that, whatever the
    // file meant, it is not a tempo, and playing it would put the rest of the song in the wrong
    // place while looking like it worked.
    if (microseconds < 10.0 || microseconds > 1.0e8) {
        warn_once("temporange", "ignored a tempo of " + std::to_string(microseconds) +
                                    " microseconds per quarter note");
        return true;
    }
    song_.tempos.push_back(TempoPoint{tick, microseconds});
    return true;
}

bool Parser::time_signature(Reader& in, int index, std::int64_t tick, std::size_t offset,
                            std::int64_t length) {
    const std::uint8_t* bytes = nullptr;
    if (length < 4 || !in.take(4, bytes)) {
        if (!in.skip(length))
            return fail_at("the file ends inside a time signature event", index, offset);
        warn_once("time_sig_length", "ignored a time signature event shorter than 4 bytes");
        return true;
    }
    if (!in.skip(length - 4))
        return fail_at("the file ends inside a time signature event", index, offset);

    // The denominator byte is an exponent, not a number: 2 means four to the beat, and reading it
    // as the number 2 would report 4/4 as 4/2, which is a different piece of music.
    const unsigned denominator = bytes[1] < 16u ? (1u << bytes[1]) : 0u;

    // Read, reported, and used by nothing: Boreaz has no bar-relative behaviour yet, and a time
    // signature that quietly changed how notes sound would be the worst kind of surprise.
    if (song_.time_signatures.size() < 32)
        song_.time_signatures.push_back(std::to_string(static_cast<unsigned>(bytes[0])) + "/" +
                                        (denominator != 0u ? std::to_string(denominator)
                                                           : "?") +
                                        " at tick " + std::to_string(tick));
    else
        warn_once("time_sig_many", "many time signature changes: only the first 32 are reported");
    if (denominator == 0u)
        warn_once("time_sig_denominator",
                  "a time signature denominator byte above 15 cannot be a note value; reported as ?");
    return true;
}

bool Parser::channel_event(Reader& in, int index, std::int64_t tick, std::size_t offset,
                           std::uint8_t status, std::int32_t first_byte) {
    const std::uint8_t kind = status & 0xF0u;
    const int data_bytes = (kind == 0xC0u || kind == 0xD0u) ? 1 : 2;

    // A running status message arrives with its first data byte already read by the event loop.
    // Reading it again would shift every later event in the track by one byte, which is the single
    // most destructive thing a parser of this format can do: the notes still look like notes.
    std::uint8_t first = static_cast<std::uint8_t>(first_byte < 0 ? 0 : first_byte);
    std::uint8_t second = 0;
    if ((first_byte < 0 && !in.byte(first)) || (data_bytes == 2 && !in.byte(second)))
        return fail_at("the file ends inside a message", index, offset);

    // Said here, decided elsewhere. This layer reports which parts of a file it had to leave out;
    // the decoder below owns what a message means.
    switch (kind) {
        case 0xA0u: // polyphonic aftertouch: the engine has one bend and no per-key aftertouch
            warn_once("polyaftertouch",
                      "ignored polyphonic aftertouch: nothing in the engine can use it yet");
            break;
        case 0xC0u:
            warn_once("program", "ignored program changes: patches arrive with the preset phase");
            break;
        case 0xD0u:
            warn_once("pressure", "ignored channel pressure: nothing in the engine can use it yet");
            break;
        default:
            break;
    }

    // Decoded by the same function a live keyboard's bytes go through, so a file and a keyboard
    // cannot drift apart on what a message means. This layer's own first attempt is the proof: it
    // tested the note number where the velocity belongs, so the velocity zero note off that half of
    // all encoders write never happened, and every one of those notes hung until the allocator stole
    // it from a note that was still wanted.
    const std::optional<boreaz::engine::Event> event = boreaz::engine::decode_message(
        boreaz::engine::MidiMessage{status, first, second}, tick);
    if (!event)
        return true; // read for length, then dropped, because the engine has no use for it

    raw_.push_back(RawEvent{tick, *event});
    return true;
}

} // namespace

static double samples_per_tick(double microseconds_per_quarter, int ticks_per_quarter,
                               double sample_rate) {
    return microseconds_per_quarter / static_cast<double>(ticks_per_quarter) * sample_rate * 1e-6;
}

std::int64_t Song::tick_to_sample(std::int64_t tick) const {
    if (ticks_per_quarter <= 0 || sample_rate <= 0.0)
        return 0;
    double per_tick = samples_per_tick(500000.0, ticks_per_quarter, sample_rate); // 120 bpm
    std::int64_t base_tick = 0;
    double base_sample = 0.0;
    for (const TempoPoint& tempo : tempos) {
        if (tempo.tick >= tick)
            break;
        base_sample += static_cast<double>(tempo.tick - base_tick) * per_tick;
        base_tick = tempo.tick;
        per_tick = samples_per_tick(tempo.microseconds_per_quarter, ticks_per_quarter, sample_rate);
    }
    base_sample += static_cast<double>(tick - base_tick) * per_tick;
    return static_cast<std::int64_t>(std::llround(base_sample));
}

std::int64_t Song::sample_to_tick(std::int64_t sample) const {
    if (ticks_per_quarter <= 0 || sample_rate <= 0.0)
        return 0;
    double per_tick = samples_per_tick(500000.0, ticks_per_quarter, sample_rate);
    std::int64_t base_tick = 0;
    double base_sample = 0.0;
    for (const TempoPoint& tempo : tempos) {
        const double next =
            base_sample + static_cast<double>(tempo.tick - base_tick) * per_tick;
        if (static_cast<double>(sample) < next)
            break;
        base_sample = next;
        base_tick = tempo.tick;
        per_tick = samples_per_tick(tempo.microseconds_per_quarter, ticks_per_quarter, sample_rate);
    }
    if (per_tick <= 0.0)
        return base_tick;
    const double ticks = (static_cast<double>(sample) - base_sample) / per_tick;
    return base_tick + static_cast<std::int64_t>(std::llround(ticks));
}

LoadResult parse(const std::uint8_t* bytes, std::size_t size, double sample_rate) {
    LoadResult result;
    if (sample_rate <= 0.0) {
        result.error = "a sample rate above 0 is required to place events in time";
        return result;
    }
    result.song.sample_rate = sample_rate;

    Reader in(bytes, size);
    Parser parser(result.song, result.warnings);
    std::string error;
    if (!parser.run(in, error)) {
        result.error = error;
        result.song = Song{}; // a failed parse hands back nothing half built
        result.song.sample_rate = sample_rate;
        return result;
    }
    result.ok = true;
    return result;
}

LoadResult load_file(const std::string& path, double sample_rate) {
    const auto refuse = [sample_rate](std::string message) {
        LoadResult result;
        result.error = std::move(message);
        result.song.sample_rate = sample_rate;
        return result;
    };

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
        return refuse("cannot open '" + path + "' for reading. Is it a file, and is it there?");

    const std::streamoff size = file.tellg();
    if (size <= 0)
        return refuse("'" + path + "' has nothing in it" +
                      (size == 0 ? std::string() : std::string(" or could not be measured")));
    if (static_cast<std::uint64_t>(size) > kMaxFileBytes)
        return refuse("'" + path + "' is " +
                      std::to_string(static_cast<std::int64_t>(size) / (1024 * 1024)) +
                      " MB, over the " + std::to_string(kMaxFileBytes / (1024 * 1024)) +
                      " MB limit. A song that size is an archive, not a file to play");

    file.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
        return refuse("'" + path + "' could not be read all the way through");

    LoadResult result = parse(bytes.data(), bytes.size(), sample_rate);
    if (!result.ok)
        result.error = "'" + path + "': " + result.error;
    return result;
}

FeedResult feed_and_render(boreaz::engine::Engine& engine, const Song& song, std::size_t cursor,
                           float* output, int frames) {
    // Push a little ahead of the block, then render it. The engine applies an event on the sample
    // it names, so being early costs nothing and being late would move a note.
    FeedResult result;
    const std::int64_t through = engine.sample_position() + frames;
    while (cursor < song.events.size() && song.events[cursor].tick <= through) {
        // A full ring means an event was lost. Count it and carry on: stopping a four minute
        // render over one note is worse than the note, and reporting it is the caller's job.
        if (!engine.push_event(song.events[cursor]))
            ++result.dropped;
        ++cursor;
    }
    engine.process(output, frames);
    result.cursor = cursor;
    return result;
}

std::int64_t render_length(const Song& song) { return song.end_sample; }

} // namespace boreaz::midifile
